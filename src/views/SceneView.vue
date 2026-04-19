<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue'
import * as THREE from 'three'

const container = ref<HTMLDivElement | null>(null)

let renderer: THREE.WebGLRenderer | null = null
let scene: THREE.Scene | null = null
let camera: THREE.PerspectiveCamera | null = null
let frameId = 0
let resizeObserver: ResizeObserver | null = null

const cube = new THREE.Mesh(
  new THREE.BoxGeometry(1, 1, 1),
  new THREE.MeshStandardMaterial({ color: 0x00c8ff, roughness: 0.4, metalness: 0.1 }),
)

function resize() {
  if (!container.value || !renderer || !camera) return
  const { clientWidth: w, clientHeight: h } = container.value
  renderer.setSize(w, h)
  camera.aspect = w / Math.max(h, 1)
  camera.updateProjectionMatrix()
}

function animate() {
  frameId = requestAnimationFrame(animate)
  cube.rotation.x += 0.005
  cube.rotation.y += 0.01
  renderer!.render(scene!, camera!)
}

onMounted(() => {
  if (!container.value) return

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x1a1a2e)

  camera = new THREE.PerspectiveCamera(60, 1, 0.1, 1000)
  camera.position.set(3, 3, 5)
  camera.lookAt(0, 0, 0)

  renderer = new THREE.WebGLRenderer({ antialias: true })
  renderer.setPixelRatio(window.devicePixelRatio)
  container.value.appendChild(renderer.domElement)

  const ambient = new THREE.AmbientLight(0xffffff, 0.4)
  const dir = new THREE.DirectionalLight(0xffffff, 0.9)
  dir.position.set(5, 10, 7)
  scene.add(ambient, dir)

  const grid = new THREE.GridHelper(10, 10, 0x00c8ff, 0x16213e)
  scene.add(grid)

  scene.add(cube)
  cube.position.y = 0.5

  resize()
  resizeObserver = new ResizeObserver(resize)
  resizeObserver.observe(container.value)

  animate()
})

onBeforeUnmount(() => {
  cancelAnimationFrame(frameId)
  resizeObserver?.disconnect()
  renderer?.dispose()
  if (renderer?.domElement.parentElement) {
    renderer.domElement.parentElement.removeChild(renderer.domElement)
  }
  cube.geometry.dispose()
  ;(cube.material as THREE.Material).dispose()
})
</script>

<template>
  <div class="scene-view">
    <div ref="container" class="scene-canvas" />
  </div>
</template>

<style scoped>
.scene-view {
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
</style>
