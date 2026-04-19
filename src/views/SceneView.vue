<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import * as THREE from 'three'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const container = ref<HTMLDivElement | null>(null)

let renderer: THREE.WebGLRenderer | null = null
let scene: THREE.Scene | null = null
let camera: THREE.PerspectiveCamera | null = null
let frameId = 0
let resizeObserver: ResizeObserver | null = null

const mapGroup = new THREE.Group()
const pathGroup = new THREE.Group()

const hasMask = computed(
  () => !!mapStore.passableMask && mapStore.maskWidth > 0 && mapStore.maskHeight > 0,
)

const MAP_EXTENT = 20
const WALL_HEIGHT_RATIO = 2.0
const MAX_CELLS_LONG_SIDE = 200

function disposeGroup(group: THREE.Group) {
  group.traverse((obj) => {
    if ((obj as THREE.Mesh).isMesh || (obj as THREE.InstancedMesh).isInstancedMesh) {
      const mesh = obj as THREE.Mesh
      mesh.geometry.dispose()
      const mat = mesh.material
      if (Array.isArray(mat)) mat.forEach((m) => m.dispose())
      else (mat as THREE.Material).dispose()
    }
  })
  group.clear()
}

function downsampleMask(
  mask: Uint8Array,
  w: number,
  h: number,
): { data: Uint8Array; width: number; height: number } {
  const longSide = Math.max(w, h)
  if (longSide <= MAX_CELLS_LONG_SIDE) return { data: mask, width: w, height: h }
  const step = Math.ceil(longSide / MAX_CELLS_LONG_SIDE)
  const dw = Math.ceil(w / step)
  const dh = Math.ceil(h / step)
  const out = new Uint8Array(dw * dh)
  for (let y = 0; y < dh; y++) {
    for (let x = 0; x < dw; x++) {
      let sum = 0
      let total = 0
      const sy0 = y * step
      const sx0 = x * step
      const sy1 = Math.min(sy0 + step, h)
      const sx1 = Math.min(sx0 + step, w)
      for (let sy = sy0; sy < sy1; sy++) {
        for (let sx = sx0; sx < sx1; sx++) {
          sum += mask[sy * w + sx]!
          total++
        }
      }
      // 過半通行就算通行。
      out[y * dw + x] = sum * 2 >= total ? 1 : 0
    }
  }
  return { data: out, width: dw, height: dh }
}

function buildGeometry() {
  disposeGroup(mapGroup)
  if (!mapStore.passableMask) return

  const ds = downsampleMask(
    mapStore.passableMask,
    mapStore.maskWidth,
    mapStore.maskHeight,
  )
  const { data, width, height } = ds

  const cellSize = MAP_EXTENT / Math.max(width, height)
  const wallHeight = cellSize * WALL_HEIGHT_RATIO
  const halfW = (width  * cellSize) / 2
  const halfH = (height * cellSize) / 2

  const floorCells: number[] = []
  const wallCells: number[] = []
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x
      if (data[i] === 1) {
        floorCells.push(i)
      } else {
        // 只保留有相鄰通行格的牆，減少看不見的實心填充。
        let borders = false
        if (x > 0         && data[i - 1]     === 1) borders = true
        else if (x < width - 1  && data[i + 1]     === 1) borders = true
        else if (y > 0          && data[i - width] === 1) borders = true
        else if (y < height - 1 && data[i + width] === 1) borders = true
        if (borders) wallCells.push(i)
      }
    }
  }

  const cellToWorld = (idx: number) => {
    const cx = idx % width
    const cy = Math.floor(idx / width)
    // 圖片 Y 軸向下，3D 世界 Z 以南北對應，翻轉 cy 讓視覺方向自然。
    const wx = (cx + 0.5) * cellSize - halfW
    const wz = (cy + 0.5) * cellSize - halfH
    return { x: wx, z: wz }
  }

  const dummy = new THREE.Object3D()

  if (floorCells.length > 0) {
    const floorGeo = new THREE.BoxGeometry(cellSize, cellSize * 0.1, cellSize)
    const floorMat = new THREE.MeshStandardMaterial({
      color: 0x2d7dd2,
      roughness: 0.85,
      metalness: 0.05,
    })
    const floorMesh = new THREE.InstancedMesh(floorGeo, floorMat, floorCells.length)
    floorCells.forEach((idx, i) => {
      const { x, z } = cellToWorld(idx)
      dummy.position.set(x, 0, z)
      dummy.rotation.set(0, 0, 0)
      dummy.scale.set(1, 1, 1)
      dummy.updateMatrix()
      floorMesh.setMatrixAt(i, dummy.matrix)
    })
    floorMesh.instanceMatrix.needsUpdate = true
    mapGroup.add(floorMesh)
  }

  if (wallCells.length > 0) {
    const wallGeo = new THREE.BoxGeometry(cellSize, wallHeight, cellSize)
    const wallMat = new THREE.MeshStandardMaterial({
      color: 0xcfd8dc,
      roughness: 0.7,
      metalness: 0.05,
    })
    const wallMesh = new THREE.InstancedMesh(wallGeo, wallMat, wallCells.length)
    wallCells.forEach((idx, i) => {
      const { x, z } = cellToWorld(idx)
      dummy.position.set(x, wallHeight / 2, z)
      dummy.rotation.set(0, 0, 0)
      dummy.scale.set(1, 1, 1)
      dummy.updateMatrix()
      wallMesh.setMatrixAt(i, dummy.matrix)
    })
    wallMesh.instanceMatrix.needsUpdate = true
    mapGroup.add(wallMesh)
  }
}

function buildPath() {
  disposeGroup(pathGroup)
  if (!mapStore.passableMask) return
  if (!mapStore.pathNodes || mapStore.pathNodes.length < 2) return

  const width  = mapStore.maskWidth
  const height = mapStore.maskHeight
  if (width <= 0 || height <= 0) return

  const cellSize = MAP_EXTENT / Math.max(width, height)
  const halfW = (width  * cellSize) / 2
  const halfH = (height * cellSize) / 2
  const up = mapStore.upscaleFactor || 1
  // 路徑節點為原始影像座標，需換算回遮罩座標系。
  const yLift = cellSize * 0.3

  const points = mapStore.pathNodes.map((p) => {
    const mx = p.x * up
    const my = p.y * up
    return new THREE.Vector3(
      (mx + 0.5) * cellSize - halfW,
      yLift,
      (my + 0.5) * cellSize - halfH,
    )
  })

  const curve = new THREE.CatmullRomCurve3(points, false, 'catmullrom', 0.1)
  const tubeSegments = Math.max(32, points.length * 8)
  const radius = cellSize * 0.45
  const tubeGeo = new THREE.TubeGeometry(curve, tubeSegments, radius, 8, false)
  const tubeMat = new THREE.MeshStandardMaterial({
    color: 0x00ffaa,
    emissive: 0x00ffaa,
    emissiveIntensity: 0.9,
    roughness: 0.3,
    metalness: 0.2,
  })
  pathGroup.add(new THREE.Mesh(tubeGeo, tubeMat))

  const markerGeo = new THREE.SphereGeometry(cellSize * 0.9, 20, 20)
  const startMat = new THREE.MeshStandardMaterial({
    color: 0x00ff66, emissive: 0x00ff66, emissiveIntensity: 0.8,
  })
  const endMat = new THREE.MeshStandardMaterial({
    color: 0xff4466, emissive: 0xff4466, emissiveIntensity: 0.8,
  })
  const startMarker = new THREE.Mesh(markerGeo, startMat)
  startMarker.position.copy(points[0]!).setY(cellSize * 0.9)
  const endMarker = new THREE.Mesh(markerGeo.clone(), endMat)
  endMarker.position.copy(points[points.length - 1]!).setY(cellSize * 0.9)
  pathGroup.add(startMarker, endMarker)
}

function resize() {
  if (!container.value || !renderer || !camera) return
  const { clientWidth: w, clientHeight: h } = container.value
  renderer.setSize(w, h)
  camera.aspect = w / Math.max(h, 1)
  camera.updateProjectionMatrix()
}

function animate() {
  frameId = requestAnimationFrame(animate)
  mapGroup.rotation.y += 0.0015
  pathGroup.rotation.y = mapGroup.rotation.y
  renderer!.render(scene!, camera!)
}

onMounted(() => {
  if (!container.value) return

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x1a1a2e)

  camera = new THREE.PerspectiveCamera(55, 1, 0.1, 1000)
  camera.position.set(0, MAP_EXTENT * 0.9, MAP_EXTENT * 1.1)
  camera.lookAt(0, 0, 0)

  renderer = new THREE.WebGLRenderer({ antialias: true })
  renderer.setPixelRatio(window.devicePixelRatio)
  container.value.appendChild(renderer.domElement)

  const ambient = new THREE.AmbientLight(0xffffff, 0.5)
  const dir = new THREE.DirectionalLight(0xffffff, 1.0)
  dir.position.set(10, 20, 15)
  scene.add(ambient, dir)

  const grid = new THREE.GridHelper(MAP_EXTENT * 1.5, 30, 0x00c8ff, 0x16213e)
  grid.position.y = -0.05
  scene.add(grid)

  scene.add(mapGroup)
  scene.add(pathGroup)
  buildGeometry()
  buildPath()

  resize()
  resizeObserver = new ResizeObserver(resize)
  resizeObserver.observe(container.value)

  animate()
})

watch(
  () => [mapStore.passableMask, mapStore.maskWidth, mapStore.maskHeight],
  () => {
    buildGeometry()
    buildPath()
  },
)

watch(
  () => mapStore.pathNodes,
  () => buildPath(),
  { deep: true },
)

onBeforeUnmount(() => {
  cancelAnimationFrame(frameId)
  resizeObserver?.disconnect()
  disposeGroup(mapGroup)
  disposeGroup(pathGroup)
  renderer?.dispose()
  if (renderer?.domElement.parentElement) {
    renderer.domElement.parentElement.removeChild(renderer.domElement)
  }
})
</script>

<template>
  <div class="scene-view">
    <div ref="container" class="scene-canvas" />
    <div v-if="!hasMask" class="hint">
      尚未產生可通行遮罩，請先到「首頁」上傳地圖並執行一次路徑識別。
    </div>
  </div>
</template>

<style scoped>
.scene-view {
  position: relative;
  width: 100%;
  height: calc(100vh - 100px);
}
.scene-canvas {
  width: 100%;
  height: 100%;
  border-radius: 8px;
  overflow: hidden;
  background: #1a1a2e;
}
.scene-canvas :deep(canvas) {
  display: block;
  width: 100% !important;
  height: 100% !important;
}
.hint {
  position: absolute;
  top: 16px;
  left: 50%;
  transform: translateX(-50%);
  padding: 10px 16px;
  background: rgba(22, 33, 62, 0.85);
  color: #e0e0e0;
  border-radius: 6px;
  font-size: 14px;
  pointer-events: none;
}
</style>
