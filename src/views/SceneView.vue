<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import * as THREE from 'three'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const container = ref<HTMLDivElement | null>(null)

// Three.js 物件由本頁手動建立與釋放，避免切換頁面後殘留 WebGL 資源。
let renderer: THREE.WebGLRenderer | null = null
let scene: THREE.Scene | null = null
let camera: THREE.PerspectiveCamera | null = null
let frameId = 0
let resizeObserver: ResizeObserver | null = null
let lastTs = 0

const mapGroup = new THREE.Group()
const pathGroup = new THREE.Group()
const avatarGroup = new THREE.Group()

// 3D 場景依賴路徑識別頁輸出的 passableMask，沒有遮罩時只顯示提示。
const hasMask = computed(
  () => !!mapStore.passableMask && mapStore.maskWidth > 0 && mapStore.maskHeight > 0,
)

const MAP_EXTENT = 20
const WALL_HEIGHT_RATIO = 2.0
const MAX_CELLS_LONG_SIDE = 200

const viewMode = ref<'overview' | 'first-person'>('overview')
// 第一人稱移動輸入以 -1/0/1 表示方向，animation loop 依 dt 積分成位移。
const moveInput = reactive({ fwd: 0, rot: 0 })
const offPath = ref(false)
const OFF_PATH_THRESHOLD_PX = 18
const MOVE_SPEED_PX = 30
const ROT_SPEED = Math.PI * 0.9

let sceneCellSize = 0
let sceneHalfW = 0
let sceneHalfH = 0
let sceneUp = 1
let sceneEyeHeight = 1.2
let sceneWallHeight = 2
// 碰撞用降採樣遮罩，與視覺牆體網格一致，避免半格穿牆。
let collisionMask: Uint8Array | null = null
let collisionW = 0
let collisionH = 0
// px→碰撞格（原始 passableMask，= sceneUp）；供 isPassableAtPixel / snap 使用。
let pxToColl = 1
// px→顯示格（下採樣後的 data 網格）；供 pixelToWorld 把像素座標映射回 3D 世界 XZ。
let pxToDisplay = 1

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
  let data: Uint8Array
  let dw: number
  let dh: number
  if (longSide <= MAX_CELLS_LONG_SIDE) {
    data = new Uint8Array(mask)
    dw = w
    dh = h
  } else {
    const step = Math.ceil(longSide / MAX_CELLS_LONG_SIDE)
    dw = Math.ceil(w / step)
    dh = Math.ceil(h / step)
    data = new Uint8Array(dw * dh)
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
        data[y * dw + x] = sum * 2 >= total ? 1 : 0
      }
    }
  }

  smoothIsolatedWalls(data, dw, dh)
  return { data, width: dw, height: dh }
}

// 移除與通行區相接超過 3 邊的孤立牆塊，避免 3D 牆面出現鋸齒突起。
function smoothIsolatedWalls(data: Uint8Array, w: number, h: number) {
  for (let pass = 0; pass < 3; pass++) {
    let changed = false
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = y * w + x
        if (data[i] !== 0) continue
        let passNb = 0
        let total = 0
        if (x > 0) {
          total++
          if (data[i - 1] === 1) passNb++
        }
        if (x < w - 1) {
          total++
          if (data[i + 1] === 1) passNb++
        }
        if (y > 0) {
          total++
          if (data[i - w] === 1) passNb++
        }
        if (y < h - 1) {
          total++
          if (data[i + w] === 1) passNb++
        }
        if (total >= 3 && passNb >= 3) {
          data[i] = 1
          changed = true
        }
      }
    }
    if (!changed) break
  }
}


// DP 簡化 pathNodes：若中間節點到 chord 的 perpDist 在 eps 之內、且 chord 在原始
// passableMask 上 Bresenham 可通行，就丟棄。用於清除 store 端 axisAlignPath 產生的
// 小範圍軸向抖動；有真實轉彎（perpDist > eps）時會保留，不會把路徑拉去貼牆。
function dpSimplifyPath(
  nodes: Array<{ x: number; y: number }>,
  epsPx: number,
): Array<{ x: number; y: number }> {
  const n = nodes.length
  if (n <= 2) return nodes.slice()
  const mask = mapStore.passableMask
  if (!mask) return nodes.slice()
  const W = mapStore.maskWidth
  const H = mapStore.maskHeight
  const up = mapStore.upscaleFactor || 1
  const linePass = (ax: number, ay: number, bx: number, by: number): boolean => {
    let cx = Math.round(ax * up),
      cy = Math.round(ay * up)
    const tx = Math.round(bx * up),
      ty = Math.round(by * up)
    const dx = Math.abs(tx - cx),
      dy = Math.abs(ty - cy)
    const sx = cx < tx ? 1 : -1,
      sy = cy < ty ? 1 : -1
    let err = dx - dy
    while (true) {
      if (cx < 0 || cx >= W || cy < 0 || cy >= H) return false
      if (mask[cy * W + cx] === 0) return false
      if (cx === tx && cy === ty) break
      const e2 = err * 2
      if (e2 > -dy) {
        err -= dy
        cx += sx
      }
      if (e2 < dx) {
        err += dx
        cy += sy
      }
    }
    return true
  }
  const eps2 = epsPx * epsPx
  const keep = new Uint8Array(n)
  keep[0] = 1
  keep[n - 1] = 1
  const stack: Array<[number, number]> = [[0, n - 1]]
  while (stack.length > 0) {
    const [lo, hi] = stack.pop()!
    if (hi - lo < 2) continue
    const a = nodes[lo]!,
      b = nodes[hi]!
    const dx = b.x - a.x,
      dy = b.y - a.y
    const L2 = dx * dx + dy * dy
    let maxD = 0,
      idx = -1
    for (let i = lo + 1; i < hi; i++) {
      const p = nodes[i]!
      let d
      if (L2 < 1e-9) {
        const ex = p.x - a.x,
          ey = p.y - a.y
        d = ex * ex + ey * ey
      } else {
        const cross = dx * (p.y - a.y) - dy * (p.x - a.x)
        d = (cross * cross) / L2
      }
      if (d > maxD) {
        maxD = d
        idx = i
      }
    }
    const canSkip = maxD <= eps2 && linePass(a.x, a.y, b.x, b.y)
    if (!canSkip && idx !== -1) {
      keep[idx] = 1
      stack.push([lo, idx], [idx, hi])
    }
  }
  const out: Array<{ x: number; y: number }> = []
  for (let i = 0; i < n; i++) if (keep[i]) out.push(nodes[i]!)
  return out
}

function pixelToWorld(px: number, py: number): { x: number; z: number } {
  // isPassableAtPixel 用 floor(px × sceneUp) 直接映射 mask cell，隱含 px 是連續的 mask 座標
  // ÷ sceneUp：px=0 對應 mask 座標 0（最左 cell 左邊緣），不是格中央。渲染必須採同一個
  // 公式、不加任何 +0.5 偏移，否則玩家 render 位置會比 collision 判定系統性地推偏半格，
  // 在牆邊就會出現「畫面穿牆但還沒撞到／已撞到卻看起來還沒到牆」。
  return {
    x: px * pxToDisplay * sceneCellSize - sceneHalfW,
    z: py * pxToDisplay * sceneCellSize - sceneHalfH,
  }
}

// 與牆體相鄰的貼牆緩衝；需大於 camera.near 折算回 cell 的比例，避免近裁面切穿牆面，
// 同時要夠大以防玩家沿牆滑行時從薄牆之間的對角縫隙鑽過去。
const COLLISION_MARGIN = 0.3
function isPassableAtPixel(px: number, py: number): boolean {
  if (!collisionMask) return false
  const cu = px * pxToColl
  const cv = py * pxToColl
  const cx = Math.floor(cu)
  const cy = Math.floor(cv)
  if (cx < 0 || cx >= collisionW) return false
  if (cy < 0 || cy >= collisionH) return false
  if (collisionMask[cy * collisionW + cx] !== 1) return false
  const fx = cu - cx
  const fy = cv - cy
  if (fx < COLLISION_MARGIN && !isCollCellPassable(cx - 1, cy)) return false
  if (fx > 1 - COLLISION_MARGIN && !isCollCellPassable(cx + 1, cy)) return false
  if (fy < COLLISION_MARGIN && !isCollCellPassable(cx, cy - 1)) return false
  if (fy > 1 - COLLISION_MARGIN && !isCollCellPassable(cx, cy + 1)) return false
  return true
}

function buildGeometry() {
  disposeGroup(mapGroup)
  if (!mapStore.passableMask) return

  // 直接使用原始 passableMask 建構幾何：每個 cell 一格 box（牆）或地板片。
  // 不做 contour 簡化，3D 顯示結果與 2D 路徑識別預覽完全一致。
  const ds = downsampleMask(mapStore.passableMask, mapStore.maskWidth, mapStore.maskHeight)
  const { data, width, height } = ds

  const dispCell = MAP_EXTENT / Math.max(width, height)
  const wallHeight = dispCell * WALL_HEIGHT_RATIO
  const halfW = (width * dispCell) / 2
  const halfH = (height * dispCell) / 2

  sceneCellSize = dispCell
  sceneHalfW = halfW
  sceneHalfH = halfH
  sceneUp = mapStore.upscaleFactor || 1
  sceneWallHeight = wallHeight
  sceneEyeHeight = wallHeight * 0.75

  // 碰撞使用原始 passableMask（與視覺幾何完全同源，只是 downsample 比例不同）。
  collisionMask = new Uint8Array(mapStore.passableMask)
  collisionW = mapStore.maskWidth
  collisionH = mapStore.maskHeight
  pxToColl = sceneUp
  pxToDisplay = (sceneUp * width) / mapStore.maskWidth

  // 統計可走 / 牆塊數，預先配置 InstancedMesh。
  let passCount = 0
  let wallCount = 0
  for (let i = 0; i < data.length; i++) {
    if (data[i] === 1) passCount++
    else wallCount++
  }

  const cellCenterX = (gx: number) => (gx + 0.5) * dispCell - halfW
  const cellCenterZ = (gy: number) => (gy + 0.5) * dispCell - halfH

  if (passCount > 0) {
    const floorGeo = new THREE.PlaneGeometry(dispCell, dispCell)
    floorGeo.rotateX(-Math.PI / 2)
    const floorMat = new THREE.MeshStandardMaterial({
      color: 0x7fb8ff,
      roughness: 0.85,
      metalness: 0.05,
      side: THREE.DoubleSide,
    })
    const floorInst = new THREE.InstancedMesh(floorGeo, floorMat, passCount)
    const m = new THREE.Matrix4()
    let idx = 0
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        if (data[y * width + x] !== 1) continue
        m.makeTranslation(cellCenterX(x), 0, cellCenterZ(y))
        floorInst.setMatrixAt(idx++, m)
      }
    }
    floorInst.instanceMatrix.needsUpdate = true
    mapGroup.add(floorInst)
  }

  if (wallCount > 0) {
    const wallGeo = new THREE.BoxGeometry(dispCell, wallHeight, dispCell)
    const wallMat = new THREE.MeshStandardMaterial({
      color: 0xf0f3f7,
      roughness: 0.6,
      metalness: 0.05,
    })
    const wallInst = new THREE.InstancedMesh(wallGeo, wallMat, wallCount)
    const m = new THREE.Matrix4()
    let idx = 0
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        if (data[y * width + x] === 1) continue
        m.makeTranslation(cellCenterX(x), wallHeight / 2, cellCenterZ(y))
        wallInst.setMatrixAt(idx++, m)
      }
    }
    wallInst.instanceMatrix.needsUpdate = true
    mapGroup.add(wallInst)
  }
}

function isCollCellPassable(cx: number, cy: number): boolean {
  if (!collisionMask) return false
  if (cx < 0 || cx >= collisionW) return false
  if (cy < 0 || cy >= collisionH) return false
  return collisionMask[cy * collisionW + cx] === 1
}

function buildPath() {
  disposeGroup(pathGroup)
  if (!hasMask.value) return
  if (!mapStore.pathNodes || mapStore.pathNodes.length < 2) return

  const yLift = sceneCellSize * 0.3
  const radius = sceneCellSize * 0.45
  const tubeMat = new THREE.MeshStandardMaterial({
    color: 0x00ffaa,
    emissive: 0x00ffaa,
    emissiveIntensity: 0.9,
    roughness: 0.3,
    metalness: 0.2,
  })

  // pathNodes 來自 centered → aligned → straightened 的降級。軸向 L 形輸出常產生小幅抖動，
  // 這裡用 DP + Bresenham 驗證簡化：只合併真的抖動（perpDist 小 + chord 可通行），真實轉角保留。
  // eps 用 3 原始像素；小於典型走廊寬的一半，不會把路徑拉去貼牆。
  // pixelToWorld 採零偏移公式（px=整數 = mask cell 左邊緣），這裡加半個 mask cell 的偏移
  // 讓視覺路徑落在 A* 走過的 cell 正中央（僅影響路徑顯示、不動碰撞與玩家渲染）。
  const raw = mapStore.pathNodes as Array<{ x: number; y: number }>
  const simplified = dpSimplifyPath(raw, 3)
  const half = 0.5 / Math.max(sceneUp, 1e-9)
  const points = simplified.map((p) => {
    const w = pixelToWorld(p.x + half, p.y + half)
    return new THREE.Vector3(w.x, yLift, w.z)
  })

  for (let i = 0; i < points.length - 1; i++) {
    const a = points[i]!
    const b = points[i + 1]!
    if (a.distanceToSquared(b) < 1e-8) continue
    const seg = new THREE.TubeGeometry(new THREE.LineCurve3(a, b), 1, radius, 8, false)
    pathGroup.add(new THREE.Mesh(seg, tubeMat))
  }
  // 轉角補球以平滑接合兩條直線 tube。
  const jointGeo = new THREE.SphereGeometry(radius, 10, 10)
  for (const p of points) {
    const s = new THREE.Mesh(jointGeo, tubeMat)
    s.position.copy(p)
    pathGroup.add(s)
  }

  const markerGeo = new THREE.SphereGeometry(sceneCellSize * 0.9, 20, 20)
  const startMat = new THREE.MeshStandardMaterial({
    color: 0x00ff66,
    emissive: 0x00ff66,
    emissiveIntensity: 0.8,
  })
  const endMat = new THREE.MeshStandardMaterial({
    color: 0xff4466,
    emissive: 0xff4466,
    emissiveIntensity: 0.8,
  })
  const startMarker = new THREE.Mesh(markerGeo, startMat)
  startMarker.position.copy(points[0]!).setY(sceneCellSize * 0.9)
  const endMarker = new THREE.Mesh(markerGeo, endMat)
  endMarker.position.copy(points[points.length - 1]!).setY(sceneCellSize * 0.9)
  pathGroup.add(startMarker, endMarker)
}

function buildAvatar() {
  disposeGroup(avatarGroup)
  if (!hasMask.value) return
  const r = sceneCellSize * 0.5
  const body = new THREE.Mesh(
    new THREE.ConeGeometry(r, r * 2.5, 16),
    new THREE.MeshStandardMaterial({
      color: 0xffcc00,
      emissive: 0xffaa00,
      emissiveIntensity: 0.6,
    }),
  )
  body.rotation.x = Math.PI / 2
  body.position.y = sceneCellSize * 1.1
  avatarGroup.add(body)
}

// 在碰撞遮罩上 BFS 找離 (px, py) 最近的 passable cell，轉回原始像素座標。
function snapToNearestPassable(px: number, py: number): { x: number; y: number } {
  if (!collisionMask) return { x: px, y: py }
  const cx0 = Math.floor(px * pxToColl)
  const cy0 = Math.floor(py * pxToColl)
  if (
    cx0 >= 0 &&
    cx0 < collisionW &&
    cy0 >= 0 &&
    cy0 < collisionH &&
    collisionMask[cy0 * collisionW + cx0] === 1
  ) {
    return { x: px, y: py }
  }
  const seen = new Uint8Array(collisionW * collisionH)
  const queue: number[] = [cy0 * collisionW + cx0]
  if (cx0 >= 0 && cx0 < collisionW && cy0 >= 0 && cy0 < collisionH) {
    seen[cy0 * collisionW + cx0] = 1
  }
  while (queue.length > 0) {
    const idx = queue.shift()!
    const cx = idx % collisionW
    const cy = Math.floor(idx / collisionW)
    if (cx >= 0 && cx < collisionW && cy >= 0 && cy < collisionH && collisionMask[idx] === 1) {
      return { x: (cx + 0.5) / pxToColl, y: (cy + 0.5) / pxToColl }
    }
    for (const [dx, dy] of [
      [1, 0],
      [-1, 0],
      [0, 1],
      [0, -1],
    ] as const) {
      const nx = cx + dx,
        ny = cy + dy
      if (nx < 0 || nx >= collisionW || ny < 0 || ny >= collisionH) continue
      const ni = ny * collisionW + nx
      if (seen[ni]) continue
      seen[ni] = 1
      queue.push(ni)
    }
  }
  return { x: px, y: py }
}

function ensureUserState() {
  if (!mapStore.startPoint) return
  if (!mapStore.userPosition) {
    const sp = mapStore.startPoint
    // 若 startPoint 落在（膨脹 / 碰撞定義下的）牆內，吸附到最近 passable，避免進第一人稱就在牆裡。
    mapStore.userPosition = isPassableAtPixel(sp.x, sp.y)
      ? { x: sp.x, y: sp.y }
      : snapToNearestPassable(sp.x, sp.y)
  }
  if (mapStore.userHeading === null) {
    const nodes = mapStore.pathNodes
    if (nodes && nodes.length >= 2) {
      const dx = nodes[1]!.x - nodes[0]!.x
      const dy = nodes[1]!.y - nodes[0]!.y
      mapStore.userHeading = Math.atan2(dy, dx)
    } else {
      mapStore.userHeading = 0
    }
  }
}

function distPointToSegment(
  px: number,
  py: number,
  ax: number,
  ay: number,
  bx: number,
  by: number,
): number {
  const abx = bx - ax,
    aby = by - ay
  const apx = px - ax,
    apy = py - ay
  const abLen2 = abx * abx + aby * aby
  let t = abLen2 === 0 ? 0 : (apx * abx + apy * aby) / abLen2
  t = Math.max(0, Math.min(1, t))
  const cx = ax + t * abx,
    cy = ay + t * aby
  const dx = px - cx,
    dy = py - cy
  return Math.sqrt(dx * dx + dy * dy)
}

function minDistToPath(px: number, py: number): number {
  const nodes = mapStore.pathNodes
  if (!nodes || nodes.length < 2) return Infinity
  let best = Infinity
  for (let i = 0; i < nodes.length - 1; i++) {
    const d = distPointToSegment(px, py, nodes[i]!.x, nodes[i]!.y, nodes[i + 1]!.x, nodes[i + 1]!.y)
    if (d < best) best = d
  }
  return best
}

function tryMove(dtSec: number) {
  if (!mapStore.userPosition) return
  const heading = mapStore.userHeading ?? 0

  if (moveInput.rot !== 0) {
    mapStore.userHeading = heading + moveInput.rot * ROT_SPEED * dtSec
  }

  if (moveInput.fwd !== 0) {
    const h = mapStore.userHeading ?? 0
    const totalStep = moveInput.fwd * MOVE_SPEED_PX * dtSec
    const dirX = Math.cos(h)
    const dirY = Math.sin(h)
    // 以 ≤ 0.3 collision cell 為單位切 substep，避免單幀位移大於薄牆厚度時直接穿過去（tunneling）。
    const maxSubPx = 0.3 / Math.max(pxToColl, 1e-6)
    const nSub = Math.max(1, Math.ceil(Math.abs(totalStep) / maxSubPx))
    const subStep = totalStep / nSub
    let fx = mapStore.userPosition.x
    let fy = mapStore.userPosition.y
    for (let i = 0; i < nSub; i++) {
      const nx = fx + dirX * subStep
      const ny = fy + dirY * subStep
      // 對角優先；若只有單軸通則沿該軸滑行並於本幀停住。
      if (isPassableAtPixel(nx, ny)) {
        fx = nx
        fy = ny
        continue
      }
      if (isPassableAtPixel(nx, fy)) {
        fx = nx
        break
      }
      if (isPassableAtPixel(fx, ny)) {
        fy = ny
        break
      }
      break
    }
    mapStore.userPosition = { x: fx, y: fy }
  }
}

function updateOffPath() {
  if (!mapStore.userPosition) {
    offPath.value = false
    return
  }
  const d = minDistToPath(mapStore.userPosition.x, mapStore.userPosition.y)
  offPath.value = d > OFF_PATH_THRESHOLD_PX
}

function replanFromCurrent() {
  if (!mapStore.userPosition || !mapStore.endPoint) return
  mapStore.setPoints({ ...mapStore.userPosition }, { ...mapStore.endPoint })
  const n = mapStore.runAStar()
  if (n > 0) offPath.value = false
}

function updateCamera() {
  if (!camera) return
  if (viewMode.value === 'first-person' && mapStore.userPosition) {
    const h = mapStore.userHeading ?? 0
    const w = pixelToWorld(mapStore.userPosition.x, mapStore.userPosition.y)
    const eyeY = sceneEyeHeight
    camera.position.set(w.x, eyeY, w.z)
    camera.lookAt(w.x + Math.cos(h), eyeY, w.z + Math.sin(h))
    avatarGroup.visible = false
    mapGroup.rotation.y = 0
    pathGroup.rotation.y = 0
  } else {
    avatarGroup.visible = true
    if (mapStore.userPosition) {
      const h = mapStore.userHeading ?? 0
      const w = pixelToWorld(mapStore.userPosition.x, mapStore.userPosition.y)
      avatarGroup.position.set(w.x, 0, w.z)
      avatarGroup.rotation.y = -h + Math.PI / 2
    }
  }
}

function animate(ts?: number) {
  frameId = requestAnimationFrame(animate)
  const now = ts ?? performance.now()
  const dt = lastTs === 0 ? 0 : Math.min(0.05, (now - lastTs) / 1000)
  lastTs = now

  if (viewMode.value === 'first-person') {
    ensureUserState()
    tryMove(dt)
    updateOffPath()
  } else {
    mapGroup.rotation.y += 0.0015
    pathGroup.rotation.y = mapGroup.rotation.y
  }

  updateCamera()
  renderer!.render(scene!, camera!)
}

function enterFirstPerson() {
  viewMode.value = 'first-person'
  ensureUserState()
  mapGroup.rotation.y = 0
  pathGroup.rotation.y = 0
}

function exitFirstPerson() {
  viewMode.value = 'overview'
  if (camera) {
    camera.position.set(0, MAP_EXTENT * 0.9, MAP_EXTENT * 1.1)
    camera.lookAt(0, 0, 0)
  }
}

function onKey(down: boolean, e: KeyboardEvent) {
  if (viewMode.value !== 'first-person') return
  const v = down ? 1 : 0
  switch (e.key) {
    case 'w':
    case 'W':
    case 'ArrowUp':
      moveInput.fwd = v
      break
    case 's':
    case 'S':
    case 'ArrowDown':
      moveInput.fwd = -v
      break
    case 'a':
    case 'A':
    case 'ArrowLeft':
      moveInput.rot = -v
      break
    case 'd':
    case 'D':
    case 'ArrowRight':
      moveInput.rot = v
      break
    default:
      return
  }
  e.preventDefault()
}
const onKeyDown = (e: KeyboardEvent) => onKey(true, e)
const onKeyUp = (e: KeyboardEvent) => onKey(false, e)

function holdFwd(v: number) {
  moveInput.fwd = v
}
function holdRot(v: number) {
  moveInput.rot = v
}
function releaseFwd() {
  moveInput.fwd = 0
}
function releaseRot() {
  moveInput.rot = 0
}

function resize() {
  if (!container.value || !renderer || !camera) return
  const { clientWidth: w, clientHeight: h } = container.value
  renderer.setSize(w, h)
  camera.aspect = w / Math.max(h, 1)
  camera.updateProjectionMatrix()
}

onMounted(() => {
  if (!container.value) return

  scene = new THREE.Scene()
  scene.background = new THREE.Color(0x4a6a8a)

  // near 設小以避免第一人稱貼牆時 near plane 切穿牆面導致見到牆體內部。
  camera = new THREE.PerspectiveCamera(55, 1, 0.01, 1000)
  camera.position.set(0, MAP_EXTENT * 0.9, MAP_EXTENT * 1.1)
  camera.lookAt(0, 0, 0)

  renderer = new THREE.WebGLRenderer({ antialias: true })
  renderer.setPixelRatio(window.devicePixelRatio)
  container.value.appendChild(renderer.domElement)

  const ambient = new THREE.AmbientLight(0xffffff, 1.1)
  const hemi = new THREE.HemisphereLight(0xffffff, 0x8899aa, 0.9)
  const dir = new THREE.DirectionalLight(0xffffff, 1.4)
  dir.position.set(10, 20, 15)
  scene.add(ambient, hemi, dir)

  const grid = new THREE.GridHelper(MAP_EXTENT * 1.5, 30, 0x00c8ff, 0x16213e)
  grid.position.y = -0.05
  scene.add(grid)

  scene.add(mapGroup)
  scene.add(pathGroup)
  // avatar 掛進 mapGroup 讓俯瞰自轉時位置與牆面同步。
  mapGroup.add(avatarGroup)
  buildGeometry()
  buildPath()
  buildAvatar()

  resize()
  resizeObserver = new ResizeObserver(resize)
  resizeObserver.observe(container.value)

  window.addEventListener('keydown', onKeyDown)
  window.addEventListener('keyup', onKeyUp)

  animate()
})

watch(
  () => [mapStore.passableMask, mapStore.maskWidth, mapStore.maskHeight],
  () => {
    buildGeometry()
    buildPath()
    buildAvatar()
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
  window.removeEventListener('keydown', onKeyDown)
  window.removeEventListener('keyup', onKeyUp)
  disposeGroup(mapGroup)
  disposeGroup(pathGroup)
  disposeGroup(avatarGroup)
  renderer?.dispose()
  if (renderer?.domElement.parentElement) {
    renderer.domElement.parentElement.removeChild(renderer.domElement)
  }
})
</script>

<template>
  <div class="scene-view">
    <!-- Three.js renderer 會掛到這個容器，大小由 ResizeObserver 同步。 -->
    <div ref="container" class="scene-canvas" />

    <!-- 沒有遮罩時無法建立牆面與碰撞資料，只提示使用者回到前一步。 -->
    <div v-if="!hasMask" class="hint">尚未產生可通行遮罩，請先上傳地圖並執行一次路徑識別。</div>

    <!-- 流程導覽保留在場景上層，避免使用者只能依賴頂部步驟條切頁。 -->
    <div class="scene-flow-actions">
      <RouterLink to="/path" class="scene-flow-btn">上一步：路徑識別</RouterLink>
      <RouterLink to="/upload" class="scene-flow-btn scene-flow-btn--primary">
        回到第一步：上傳地圖
      </RouterLink>
    </div>

    <!-- 俯瞰與第一人稱共用同一套場景，以單一切換鈕切換模式；按鈕文字顯示按下後會進入的模式。 -->
    <div v-if="hasMask" class="top-bar">
      <button
        class="mode-btn"
        @click="viewMode === 'first-person' ? exitFirstPerson() : enterFirstPerson()"
      >
        {{ viewMode === 'first-person' ? '俯瞰' : '第一人稱' }}
      </button>
    </div>

    <!-- 偏離路徑時可從目前位置重新跑 A*，不重新做 flood fill。 -->
    <div v-if="offPath && viewMode === 'first-person'" class="offpath-banner">
      偵測到偏離路徑
      <button class="replan-btn" @click="replanFromCurrent">重新規劃</button>
    </div>

    <!-- 手機觸控方向盤，輸入值會在 animation loop 轉成前進與旋轉速度。 -->
    <div v-if="viewMode === 'first-person' && hasMask" class="controls">
      <div class="pad">
        <button
          class="pad-btn up"
          @pointerdown.prevent="holdFwd(1)"
          @pointerup="releaseFwd"
          @pointerleave="releaseFwd"
          @pointercancel="releaseFwd"
        >
          前進
        </button>
        <button
          class="pad-btn left"
          @pointerdown.prevent="holdRot(-1)"
          @pointerup="releaseRot"
          @pointerleave="releaseRot"
          @pointercancel="releaseRot"
        >
          左轉
        </button>
        <button
          class="pad-btn right"
          @pointerdown.prevent="holdRot(1)"
          @pointerup="releaseRot"
          @pointerleave="releaseRot"
          @pointercancel="releaseRot"
        >
          右轉
        </button>
        <button
          class="pad-btn down"
          @pointerdown.prevent="holdFwd(-1)"
          @pointerup="releaseFwd"
          @pointerleave="releaseFwd"
          @pointercancel="releaseFwd"
        >
          後退
        </button>
      </div>
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
  border-radius: var(--radius-md);
  overflow: hidden;
  background: var(--color-text);
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
  padding: var(--space-3) var(--space-4);
  background: var(--color-scene-panel);
  color: var(--color-scene-text);
  border-radius: var(--radius-md);
  font-size: var(--text-base);
  pointer-events: none;
}

.scene-flow-actions {
  position: absolute;
  top: 16px;
  right: 16px;
  z-index: 2;
  display: flex;
  gap: var(--space-2);
  flex-wrap: wrap;
  justify-content: flex-end;
}

.scene-flow-btn {
  display: inline-flex;
  align-items: center;
  min-height: 38px;
  padding: var(--space-2) var(--space-3);
  border: 1px solid rgba(255, 255, 255, 0.18);
  border-radius: var(--radius-md);
  background: var(--color-scene-panel);
  color: var(--color-scene-text);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  text-decoration: none;
  cursor: pointer;
}

.scene-flow-btn:hover {
  background: var(--color-primary-hover);
  color: var(--color-white);
}

.scene-flow-btn--primary {
  border-color: var(--color-primary);
  background: var(--color-primary);
  color: var(--color-white);
}

.top-bar {
  position: absolute;
  top: 16px;
  left: 16px;
  display: flex;
  gap: 8px;
}
.mode-btn {
  padding: var(--space-2) var(--space-4);
  border: none;
  border-radius: var(--radius-md);
  background: var(--color-scene-panel);
  color: var(--color-scene-text);
  cursor: pointer;
  font-size: var(--text-base);
}
.mode-btn.active {
  background: var(--color-scene-accent);
  color: var(--color-text);
  font-weight: var(--font-bold);
}

.offpath-banner {
  position: absolute;
  top: 16px;
  left: 50%;
  transform: translateX(-50%);
  padding: var(--space-3) var(--space-4);
  background: var(--color-scene-warning);
  color: var(--color-white);
  border-radius: var(--radius-md);
  font-size: var(--text-base);
  display: flex;
  align-items: center;
  gap: 10px;
}
.replan-btn {
  padding: var(--space-2) var(--space-3);
  border: none;
  border-radius: var(--radius-sm);
  background: var(--color-white);
  color: var(--color-scene-warning-text);
  cursor: pointer;
  font-weight: var(--font-bold);
}

.controls {
  position: absolute;
  bottom: 20px;
  right: 20px;
  user-select: none;
}
.pad {
  display: grid;
  grid-template-columns: repeat(3, 64px);
  grid-template-rows: repeat(3, 64px);
  gap: 6px;
}
.pad-btn {
  border: none;
  border-radius: var(--radius-md);
  background: var(--color-scene-panel);
  color: var(--color-scene-text);
  font-size: var(--text-base);
  cursor: pointer;
  touch-action: none;
}
.pad-btn:active {
  background: var(--color-scene-accent);
  color: var(--color-text);
}
.pad-btn.up {
  grid-column: 2;
  grid-row: 1;
}
.pad-btn.left {
  grid-column: 1;
  grid-row: 2;
}
.pad-btn.right {
  grid-column: 3;
  grid-row: 2;
}
.pad-btn.down {
  grid-column: 2;
  grid-row: 3;
}

@media (max-width: 720px) {
  .scene-view {
    height: calc(100dvh - 112px);
    min-height: 520px;
  }

  .scene-canvas {
    border-radius: var(--radius-md);
  }

  .hint {
    top: 50%;
    width: min(320px, calc(100% - 32px));
    text-align: center;
  }

  .top-bar {
    top: var(--space-3);
    left: var(--space-3);
  }

  .mode-btn,
  .scene-flow-btn,
  .replan-btn {
    min-height: 40px;
  }

  .scene-flow-actions {
    top: var(--space-3);
    right: var(--space-3);
    left: auto;
    justify-content: flex-end;
  }

  .scene-flow-btn {
    padding: var(--space-2) var(--space-3);
    font-size: var(--text-sm);
  }

  .offpath-banner {
    top: 64px;
    width: min(340px, calc(100% - 32px));
    justify-content: center;
    flex-wrap: wrap;
    text-align: center;
  }

  .controls {
    right: var(--space-3);
    bottom: var(--space-3);
  }

  .pad {
    grid-template-columns: repeat(3, 56px);
    grid-template-rows: repeat(3, 56px);
    gap: var(--space-1);
  }

  .pad-btn {
    font-size: var(--text-sm);
  }
}

@media (max-width: 480px) {
  .scene-view {
    display: flex;
    flex-direction: column;
    height: calc(100dvh - 104px);
  }

  .scene-canvas {
    flex: 1;
    min-height: 0;
  }

  .top-bar {
    right: var(--space-3);
  }

  .mode-btn {
    flex: 1;
    padding: var(--space-2);
  }

  .scene-flow-actions {
    position: static;
    display: grid;
    grid-template-columns: 1fr;
    gap: var(--space-2);
    padding: var(--space-2) var(--space-3) var(--space-3);
    flex-shrink: 0;
  }

  .scene-flow-btn {
    justify-content: center;
    min-height: 32px;
    padding: var(--space-1) var(--space-2);
    font-size: var(--text-xs);
  }

  .offpath-banner {
    top: 150px;
  }

  .controls {
    left: 50%;
    right: auto;
    bottom: 110px;
    transform: translateX(-50%);
  }
}

@media (max-height: 620px) and (orientation: landscape) {
  .scene-view {
    height: calc(100dvh - 84px);
    min-height: 360px;
  }

  .scene-flow-actions {
    left: auto;
  }

  .pad {
    grid-template-columns: repeat(3, 48px);
    grid-template-rows: repeat(3, 48px);
  }

  .offpath-banner {
    top: var(--space-3);
  }
}
</style>
