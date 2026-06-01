<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import * as THREE from 'three'
import { mergeGeometries } from 'three/examples/jsm/utils/BufferGeometryUtils.js'
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js'
import { useMapStore } from '@/stores/mapStore'
import { canStandAt, moveCircle, type GridMask } from '@/utils/circleCollision'
import { extractAndRasterize, type Ring, type Pt } from '@/utils/contourUtils'
import {
  buildSegGrid,
  canStandAtPoly,
  moveCirclePoly,
  ringsToSegments,
  type SegGrid,
} from '@/utils/polyCollision'

const mapStore = useMapStore()
const container = ref<HTMLDivElement | null>(null)

// Three.js 物件由本頁手動建立與釋放，避免切換頁面後殘留 WebGL 資源。
let renderer: THREE.WebGLRenderer | null = null
let scene: THREE.Scene | null = null
let camera: THREE.PerspectiveCamera | null = null
// 俯瞰模式的手動相機控制（拖曳旋轉 / 滾輪縮放 / 右鍵平移）。第一人稱時停用。
let controls: OrbitControls | null = null
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
// 碰撞遮罩（網格版）：USE_CONTOUR_WALLS=false 的 box fallback 仍用它；輪廓模式改用 collisionSegs。
let collisionMask: Uint8Array | null = null
let collisionW = 0
let collisionH = 0
// px→碰撞格 / px→顯示格：值相等（都是「原始 px → ds 網格」係數），保留兩個變數只是語意分組。
let pxToColl = 1
let pxToDisplay = 1
// 輪廓線段碰撞（主路徑）：把視覺牆用的「簡化+內偏後輪廓」轉成線段並建空間網格，
// 圓形碰撞直接吃這些線段 → 碰撞邊界 = 視覺牆邊界，貼牆順滑無階梯、不穿牆。
// null 代表走 box fallback（網格碰撞）。
let collisionSegs: SegGrid | null = null

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
// 保留理由（greedy meshing 後）：仍能讓走廊裡 1 cell 寬的雜訊牆消失，減少切割大地板矩形的機會、
// 順便降 wall rect 數。動作只翻 ds.data，不會破壞「視覺 = 碰撞同源」（兩者都讀 ds.data）。
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

// === 玩家圓形碰撞 ===
// 把玩家視為「以位置為圓心、半徑 PLAYER_RADIUS_CELLS 的圓」，圓周不得與任何牆 cell 重疊。
// 單位是 ds 網格 cell（= 視覺幾何 cell = collision cell；pxToColl == pxToDisplay）。
//
// 數值選擇：
//   camera.near = 0.01 world units（見 PerspectiveCamera）。
//   一個 ds cell 的世界尺寸 = sceneCellSize = MAP_EXTENT / max(width, height)
//     典型平面圖 (downsample 後 maxDim ≤ 200, MAP_EXTENT=20) ⇒ ≈ 0.1 world／cell
//     ⇒ camera.near ≈ 0.1 cell
//   半徑 0.35 cell（≈ camera.near 的 3.5 倍）：
//     - 第一人稱近裁面與牆面之間永遠有 0.25 cell 的緩衝，畫面不會穿牆。
//     - 走廊最窄為 1 cell；圓直徑 0.7 < 1，正常走廊不會卡住。
//
// 重要：此常數固定以「ds cell」為單位，不受原始 mask 解析度或 upscaleFactor 影響
//   （因為 pxToColl 同步把 px → ds cell，玩家半徑相對於世界尺寸維持 0.35 × sceneCellSize）。
const PLAYER_RADIUS_CELLS = 0.35

function collisionGrid(): GridMask | null {
  if (!collisionMask) return null
  return { data: collisionMask, width: collisionW, height: collisionH }
}

// 玩家中心是否可以站在 (px, py)。
// 輪廓模式：圓心到任一輪廓線段距離 ≥ R（碰撞 = 視覺牆邊界）。
// box fallback：退回網格版 canStandAt。
function isPassableAtPixel(px: number, py: number): boolean {
  if (collisionSegs) {
    return canStandAtPoly(collisionSegs, px * pxToColl, py * pxToColl, PLAYER_RADIUS_CELLS)
  }
  const g = collisionGrid()
  if (!g) return false
  return canStandAt(g, px * pxToColl, py * pxToColl, PLAYER_RADIUS_CELLS)
}

function buildGeometry() {
  disposeGroup(mapGroup)
  if (!mapStore.passableMask) return

  // 使用原始 passableMask 經 downsampleMask（+ smoothIsolatedWalls）後的 ds.data 建幾何。
  // 不做 contour 簡化；改用 greedy meshing 合併同類連續 cell 為大矩形，再 mergeGeometries 成單一 Mesh，
  // 兼顧「視覺＝碰撞同源」與長牆光滑、低頂點數。
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

  // pxToColl 與 pxToDisplay 完全相同（兩者都是「原始 px 座標 → ds 網格座標」的係數）
  pxToDisplay = (sceneUp * width) / mapStore.maskWidth
  pxToColl = pxToDisplay
  // collisionMask 暫設為 data，等 smoothedMask 烤好後會覆蓋
  collisionW = width
  collisionH = height

  // 走廊寬度健檢：downsample 後 1 cell 對應 step 個原始 mask cell；
  // PLAYER_RADIUS_CELLS=0.35 換算後直徑 0.7 ds-cell，1 cell 寬走廊在 ds 上仍可塞下。
  // 若原圖的窄走廊在 downsample 多數決下被吃成 0 cell，玩家無法進入 — 屬於 downsample
  // 本身的限制，需藉由提高 MAX_CELLS_LONG_SIDE 或調整 PLAYER_RADIUS_CELLS 解決。
  if (width < 3 || height < 3) {
    console.warn(
      '[SceneView] downsampled grid is very small (%dx%d); 0.35-cell player may not fit narrow corridors. Consider raising MAX_CELLS_LONG_SIDE.',
      width,
      height,
    )
  }

  // === 視覺幾何：輪廓抽取 + Douglas-Peucker 簡化 + smoothedMask 烤回 ===
  //
  // 使用 contourUtils 的 marching-squares 邊界抽取 + 鄰接表多邊匯集 + 最右轉串環，
  // 取代舊版 Map<number,number> 一對一串環（後者在多邊匯集頂點覆蓋前寫 → 斜線區串斷）。
  //
  // 管線：extractBoundaryEdges → traceRings → classify → DP 簡化 → inwardBias
  //       → scanlineFill（烤回 smoothedMask）→ 視覺牆面 + 地板幾何。
  //
  // 碰撞遮罩（smoothedMask）與視覺牆面由同一組簡化後輪廓衍生，形狀一致。
  // circleCollision.ts 完全不改（仍吃 Uint8Array 網格）。
  //
  // 開關：USE_CONTOUR_WALLS = false 時回退到 box-per-cell 舊版（debug 用）。
  const USE_CONTOUR_WALLS = true
  const DP_EPSILON_CELLS = 0.8   // DP 簡化容差（cell 單位）；偏小保守，>1 會吃掉窄門
  const INWARD_BIAS_CELLS = 0.05 // 簡化後內偏量（cell 單位）；確保視覺可走 ⊆ 碰撞可走

  const latticeToWorld = (p: Pt) => new THREE.Vector2(
    p[0] * dispCell - halfW,
    p[1] * dispCell - halfH,
  )

  // ---- Fallback：保留 box-per-cell 路徑作為 USE_CONTOUR_WALLS=false 的 debug 開關 ----
  function buildBoxFallback() {
    collisionMask = data
    collisionSegs = null // box fallback 用網格碰撞
    const wallMat = new THREE.MeshStandardMaterial({ color: 0xf0f3f7, roughness: 0.6, metalness: 0.05 })
    const floorMat = new THREE.MeshStandardMaterial({ color: 0x7fb8ff, roughness: 0.85, metalness: 0.05, side: THREE.DoubleSide })
    const wallParts: THREE.BufferGeometry[] = []
    const floorParts: THREE.BufferGeometry[] = []
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
      const cx = (x + 0.5) * dispCell - halfW
      const cz = (y + 0.5) * dispCell - halfH
      if (data[y * width + x] === 1) {
        const g = new THREE.PlaneGeometry(dispCell, dispCell)
        g.rotateX(-Math.PI / 2); g.translate(cx, 0, cz); floorParts.push(g)
      } else {
        const g = new THREE.BoxGeometry(dispCell, wallHeight, dispCell)
        g.translate(cx, wallHeight / 2, cz); wallParts.push(g)
      }
    }
    const fm = mergeGeometries(floorParts, false); floorParts.forEach(g => g.dispose())
    const wm = mergeGeometries(wallParts, false);  wallParts.forEach(g => g.dispose())
    if (fm) mapGroup.add(new THREE.Mesh(fm, floorMat))
    if (wm) mapGroup.add(new THREE.Mesh(wm, wallMat))
  }

  if (!USE_CONTOUR_WALLS) { buildBoxFallback(); return }

  // -- 抽輪廓 → 分類 → 簡化 → 內偏 → 烤回 smoothedMask（一次完成） --
  const contour = extractAndRasterize(data, width, height, DP_EPSILON_CELLS, INWARD_BIAS_CELLS)
  const { simpOuters, simpHolesByOuter, allRings, smoothedMask, stats } = contour

  // === 碰撞改用「輪廓線段」（與視覺牆完全同一條多邊形）===
  // allRings = 簡化+內偏後的所有輪廓環（外環+holes），即視覺牆 quad 用的同一串線。
  // 轉成線段 + 空間網格，圓形碰撞直接吃線段 → 碰撞邊界 = 視覺斜線，無階梯、不穿牆。
  // 座標為 ds-cell 單位，與 pxToColl 一致（px * pxToColl = ds-cell）。
  collisionSegs = buildSegGrid(ringsToSegments(allRings))
  // smoothedMask 仍保留給 snapToNearestPassable 當「找初始可走點」的粗定位用（見該函式）。
  collisionMask = smoothedMask

  const toWorldX = (p: Pt) => p[0] * dispCell - halfW
  const toWorldZ = (p: Pt) => p[1] * dispCell - halfH

  // ---- 地板：每個可走元件 = 一個 Shape（outer=外環, holes=內牆塊環），平鋪在 y=0 ----
  const floorMat = new THREE.MeshStandardMaterial({
    color: 0x7fb8ff, roughness: 0.85, metalness: 0.05, side: THREE.DoubleSide,
  })
  const floorParts: THREE.BufferGeometry[] = []
  for (let i = 0; i < simpOuters.length; i++) {
    const outer = simpOuters[i]!
    if (outer.length < 3) continue
    const shape = new THREE.Shape(outer.map(latticeToWorld))
    const holes = simpHolesByOuter.get(i) ?? []
    for (const h of holes) if (h.length >= 3) shape.holes.push(new THREE.Path(h.map(latticeToWorld)))
    const g = new THREE.ShapeGeometry(shape)
    // Shape 在 XY 平面、Y=0；rotateX(+π/2) 使 shape-y → world-z（不翻轉），與牆 quad 對齊。
    // 法線旋轉後朝 -Y，但 floorMat 是 DoubleSide，從上方仍可見。
    g.rotateX(Math.PI / 2)
    floorParts.push(g)
  }
  const floorMerged = mergeGeometries(floorParts, false)
  floorParts.forEach(g => g.dispose())
  if (floorMerged) mapGroup.add(new THREE.Mesh(floorMerged, floorMat))

  // ---- 牆：對每條 ring 的每條邊手動建垂直 quad（避開 ExtrudeGeometry 軸向陷阱） ----
  const wallMat = new THREE.MeshStandardMaterial({
    color: 0xf0f3f7, roughness: 0.6, metalness: 0.05,
  })

  let edgeCount = 0
  for (const r of allRings) if (r.length >= 2) edgeCount += r.length

  let wallMerged: THREE.BufferGeometry | null = null
  let wallQuads = 0
  if (edgeCount > 0) {
    const positions = new Float32Array(edgeCount * 6 * 3)
    let o = 0
    const pushV = (x: number, y: number, z: number) => {
      positions[o++] = x; positions[o++] = y; positions[o++] = z
    }
    for (const ring of allRings) {
      const n = ring.length
      if (n < 2) continue
      for (let i = 0; i < n; i++) {
        const p1 = ring[i]!
        const p2 = ring[(i + 1) % n]!
        const x1 = toWorldX(p1), z1 = toWorldZ(p1)
        const x2 = toWorldX(p2), z2 = toWorldZ(p2)
        pushV(x1, 0, z1); pushV(x2, wallHeight, z2); pushV(x2, 0, z2)
        pushV(x1, 0, z1); pushV(x1, wallHeight, z1); pushV(x2, wallHeight, z2)
        wallQuads++
      }
    }
    wallMerged = new THREE.BufferGeometry()
    wallMerged.setAttribute('position', new THREE.BufferAttribute(positions, 3))
    wallMerged.computeVertexNormals()
    mapGroup.add(new THREE.Mesh(wallMerged, wallMat))
  }

  // 統計輸出（含 bounding box 自查 + smoothedMask 差異）
  const triCount = (g: THREE.BufferGeometry | null) => {
    if (!g) return 0
    if (g.index) return g.index.count / 3
    return (g.attributes.position?.count ?? 0) / 3
  }
  const bboxY = (g: THREE.BufferGeometry | null): [number, number] => {
    if (!g) return [0, 0]
    g.computeBoundingBox()
    const b = g.boundingBox
    return b ? [b.min.y, b.max.y] : [0, 0]
  }
  const wallTris = triCount(wallMerged)
  const floorTris = triCount(floorMerged)
  const [wMinY, wMaxY] = bboxY(wallMerged)
  const [fMinY, fMaxY] = bboxY(floorMerged)
  console.log(
    '[SceneView] contour walls: %dx%d grid, rings raw=%d (outer=%d, holes=%d), verts %d → %d (%.1f%% after DP+bias)',
    width, height,
    stats.rawRingCount, stats.outerCount, stats.holeCount,
    stats.rawVerts, stats.simpVerts,
    stats.rawVerts > 0 ? (100 * stats.simpVerts) / stats.rawVerts : 0,
  )
  console.log(
    '[SceneView] smoothedMask: passable cells %d → %d (diff %.2f%%)',
    stats.originalPassable, stats.smoothedPassable, stats.diffPercent,
  )
  console.log(
    '[SceneView] poly collision: %d segments; start point passable=%s',
    collisionSegs ? collisionSegs.segs.length : 0,
    mapStore.startPoint ? String(isPassableAtPixel(mapStore.startPoint.x, mapStore.startPoint.y)) : 'n/a',
  )
  console.log(
    '[SceneView] geometry: wall quads=%d (tris=%d), floor tris=%d, wallHeight=%.3f | wall bbox.y=[%.3f, %.3f] (expect [0, %.3f]), floor bbox.y=[%.3f, %.3f] (expect ~0)',
    wallQuads, wallTris, floorTris, wallHeight,
    wMinY, wMaxY, wallHeight,
    fMinY, fMaxY,
  )
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

// 找離 (px, py) 最近、且玩家圓 (R=PLAYER_RADIUS_CELLS) 可整個塞進去的位置，回傳原始像素座標。
// 用 smoothedMask（粗網格）做 BFS 取候選 cell 中心，但「是否可站」一律用 isPassableAtPixel
// 判定（輪廓模式 → 線段碰撞、box fallback → 網格碰撞），確保 snap 與移動碰撞同一套標準。
function snapToNearestPassable(px: number, py: number): { x: number; y: number } {
  // 原地已合法就直接回傳
  if (isPassableAtPixel(px, py)) return { x: px, y: py }
  // 沒有粗網格可 BFS（理論上不會發生）→ 原地回傳
  if (!collisionMask || collisionW <= 0 || collisionH <= 0) return { x: px, y: py }

  const cx0 = Math.floor(px * pxToColl)
  const cy0 = Math.floor(py * pxToColl)
  const seen = new Uint8Array(collisionW * collisionH)
  const queue: number[] = []
  const enqueue = (cx: number, cy: number) => {
    if (cx < 0 || cx >= collisionW || cy < 0 || cy >= collisionH) return
    const i = cy * collisionW + cx
    if (seen[i]) return
    seen[i] = 1
    queue.push(i)
  }
  // 起點可能在界外，clamp 後入列
  enqueue(Math.max(0, Math.min(collisionW - 1, cx0)), Math.max(0, Math.min(collisionH - 1, cy0)))
  while (queue.length > 0) {
    const idx = queue.shift()!
    const cx = idx % collisionW
    const cy = Math.floor(idx / collisionW)
    if (collisionMask[idx] === 1) {
      // 候選：cell 中心，轉回原始像素座標後用統一碰撞標準驗證
      const candPx = (cx + 0.5) / pxToColl
      const candPy = (cy + 0.5) / pxToColl
      if (isPassableAtPixel(candPx, candPy)) {
        return { x: candPx, y: candPy }
      }
    }
    enqueue(cx + 1, cy)
    enqueue(cx - 1, cy)
    enqueue(cx, cy + 1)
    enqueue(cx, cy - 1)
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
    const totalStepPx = moveInput.fwd * MOVE_SPEED_PX * dtSec
    const dirX = Math.cos(h)
    const dirY = Math.sin(h)
    // 全部換算到 ds-cell 座標系，子步長 0.3 cell（防穿牆 tunneling 門檻）
    const cu = mapStore.userPosition.x * pxToColl
    const cv = mapStore.userPosition.y * pxToColl
    const duTotal = dirX * totalStepPx * pxToColl
    const dvTotal = dirY * totalStepPx * pxToColl
    // 輪廓模式：圓 vs 線段碰撞（沿斜線切線滑行）；box fallback：網格版 moveCircle。
    if (collisionSegs) {
      const res = moveCirclePoly(collisionSegs, cu, cv, duTotal, dvTotal, PLAYER_RADIUS_CELLS, 0.3)
      mapStore.userPosition = { x: res.cu / pxToColl, y: res.cv / pxToColl }
    } else {
      const g = collisionGrid()
      if (!g) return
      const res = moveCircle(g, cu, cv, duTotal, dvTotal, PLAYER_RADIUS_CELLS, 0.3)
      mapStore.userPosition = { x: res.cu / pxToColl, y: res.cv / pxToColl }
    }
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
    // 俯瞰模式不再自動旋轉，改由使用者透過 OrbitControls 手動操作。
    controls?.update()
  }

  updateCamera()
  renderer!.render(scene!, camera!)
}

function enterFirstPerson() {
  viewMode.value = 'first-person'
  ensureUserState()
  mapGroup.rotation.y = 0
  pathGroup.rotation.y = 0
  // 第一人稱由 updateCamera 直接控制相機，停用 OrbitControls 以免互搶。
  if (controls) controls.enabled = false
}

function exitFirstPerson() {
  viewMode.value = 'overview'
  if (camera) {
    camera.position.set(0, MAP_EXTENT * 0.9, MAP_EXTENT * 1.1)
    camera.lookAt(0, 0, 0)
  }
  // 回到俯瞰：重設控制器的對焦中心並重新啟用手動操作。
  if (controls) {
    controls.target.set(0, 0, 0)
    controls.enabled = true
    controls.update()
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

  // 俯瞰模式的手動相機控制：左鍵拖曳旋轉、滾輪/雙指縮放、右鍵或雙指平移。
  // autoRotate 預設關閉 → 切到 3D 後畫面靜止，由使用者自行操作。
  controls = new OrbitControls(camera, renderer.domElement)
  controls.enableDamping = true
  controls.dampingFactor = 0.08
  controls.autoRotate = false
  controls.target.set(0, 0, 0)
  controls.enabled = viewMode.value === 'overview'
  controls.update()

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
  controls?.dispose()
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
