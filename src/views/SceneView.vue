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
// 3D 牆/碰撞網格的長邊上限 cell 數。mask 超過此值會被降採樣(多數決)。
// 設太小(如 200 → 大圖 step 6)會把細走道糊斷：2D mask 全連通的終點區窄通道
// (北車捷運區寬僅 10–30px)降採樣後咽喉被吃斷 → 3D 起訖不連通、最短路徑看似穿牆。
// 400(大圖 step 3)可保住細走道連通；再高(600)幾何量近 9 倍、手機效能負擔大。
const MAX_CELLS_LONG_SIDE = 400

const viewMode = ref<'overview' | 'first-person'>('overview')
// 第一人稱移動/轉向輸入。移動與視角朝向「解耦」（方案 A）：
//   fwd    : 前進(+)/後退(-)，沿視角前方。範圍 [-1,1]（搖桿類比；鍵盤給 ±1）。
//   strafe : 右側移(+)/左側移(-)，沿視角右方，「不」改變視角朝向。範圍 [-1,1]。
//   rot    : 轉視角(+右/-左)，僅鍵盤 A/D/←/→ 使用（離散 ±1）。觸控轉視角不走這裡，
//            而是直接累加 userHeading（見 onLookMove），兩條路徑共用 userHeading 不互相覆蓋。
// animation loop 依 dt 把 fwd/strafe 積分成位移、把 rot 積分成 heading 增量。
//
// 鍵盤與搖桿「不互相覆蓋」：分別存到 keyInput 與 joystick，每幀在 syncMoveInput() 合併成
// moveInput。fwd/strafe 取「絕對值較大者」（讓任一輸入源都能單獨驅動，且搖桿類比量不會被
// 鍵盤的 ±1 永遠壓過、鍵盤也不會被靜止的搖桿歸零）；rot 只有鍵盤會用。
const moveInput = reactive({ fwd: 0, strafe: 0, rot: 0 })
// 鍵盤輸入（離散）：fwd ∈ {-1,0,1}（W/S），rot ∈ {-1,0,1}（A/D 轉視角）。鍵盤不出 strafe。
const keyInput = reactive({ fwd: 0, rot: 0 })
const offPath = ref(false)
const OFF_PATH_THRESHOLD_PX = 18
const MOVE_SPEED_PX = 30
const ROT_SPEED = Math.PI * 0.9

// === 動態指引路徑 + minimap 參數 ===
// 偏離超過門檻 → 自動從當前位置重跑 A*（取代手動按鈕）；節流避免每幀重算。
const REPLAN_MIN_INTERVAL_SEC = 0.8
let lastReplanTs = 0
// 3D 路徑（trimmed）重建門檻：角色移動超過此 px 才重建 tube，避免逐幀重建過重。
const PATH_REBUILD_MOVE_PX = 3
let lastPathBuildPos: { x: number; y: number } | null = null
// minimap：以角色為中心的局部視窗。
const MINIMAP_SIZE = 160 // canvas 像素邊長（CSS 也同尺寸）
const MINIMAP_RADIUS_PX = 130 // 視窗半徑（原始 px），決定看多大範圍
const minimapCanvas = ref<HTMLCanvasElement | null>(null)
let minimapBaseTmp: { canvas: HTMLCanvasElement; ctx: CanvasRenderingContext2D } | null = null
let minimapBaseDirty = true // floodFillResultData 變動時設 true，下次 drawMinimap 重建 offscreen 底圖
// 路徑統一鮮紅。
const PATH_COLOR_HEX = 0xff3b30
const PATH_COLOR_CSS = '#FF3B30'
// 觸控滑屏轉視角靈敏度：每 1 CSS px 的水平位移 → userHeading 增加多少弧度。
// 調大 = 轉更快。0.005 rad/px ≈ 滑半個 360px 寬手機螢幕轉約 100°，手感適中。
const LOOK_SENSITIVITY = 0.005
// 搖桿死區：正規化半徑 < 此值時視為回中（0），避免手指輕觸造成漂移。
const JOYSTICK_DEADZONE = 0.15
// 觸控分區：畫面「下方」此比例為搖桿生效區，其餘上方為轉視角區（上下分區，不綁左右手）。
// 0.30 = 下方 30%。比例相對 touch-layer（場景區）高度，非整個視窗。調大 = 搖桿區更寬。
const JOYSTICK_ZONE_RATIO = 0.30

// === 虛擬搖桿狀態（浮動式：按下即在手指處生成）===
// jx/jy ∈ [-1,1]：以底座中心為原點、正規化到單位圓的搖桿頭偏移（已夾住、未套死區）。
//   jx：右(+)/左(-)；jy：上(+,前進)/下(-,後退)（畫面 y 往下為正，故取負後對應前進）。
// active：搖桿是否被按住（true=浮動在手指處且不透明；false=回到 idle 預設位置半透明）。
// knobX/knobY：搖桿頭相對底座中心的「像素」位移，純視覺，給 template 綁 transform。
// baseX/baseY：底座中心的螢幕座標（active 時 = 手指按下處）。idle 時走 CSS 預設位置，
//   故 baseX/baseY 僅在 active 時用於 inline 定位（見 template 的 :style 條件）。
const joystick = reactive({ jx: 0, jy: 0, active: false, knobX: 0, knobY: 0, baseX: 0, baseY: 0 })
// debug 疊層：顯示目前 (jx, jy) 與 userHeading。預設關閉（正式使用不需要、手機尺寸會被底部按鈕擋）；
// 將來除錯可手動把此值改 true 或在 console 設定。.touch-debug 元件本身保留。
const showDebug = ref(false)
const debugHeading = ref(0)
// template 不可直接用 Math，這裡把 heading 轉成度數字串供 debug 疊層顯示。
const debugHeadingDeg = computed(() => ((debugHeading.value * 180) / Math.PI).toFixed(0))

// 搖桿底座半徑（CSS px）——視覺底座尺寸（直徑 = 2×此值），須與 style 的 .joystick-base 尺寸一致。
const JOYSTICK_BASE_RADIUS = 60
// 搖桿頭最大可離開中心的像素（略小於底座半徑，留邊）。pointermove 正規化以此為準。
const JOYSTICK_KNOB_RANGE = 44

// 多點觸控分流：分別記錄「搖桿手指」與「轉視角手指」的 pointerId，允許左手搖桿 + 右手滑屏同時。
// null 代表該角色目前沒有手指佔用。絕不可用單一全域 pointer 狀態，否則第二指會搶走第一指。
let joystickPointerId: number | null = null
let lookPointerId: number | null = null
// 轉視角手指的上一個 clientX，用來算每次 move 的水平位移量（delta）。
let lookLastX = 0
// 搖桿底座中心的螢幕座標。浮動式：pointerdown 當下記錄「手指按下處」，move 時據此算偏移。
let joystickCenterX = 0
let joystickCenterY = 0

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
// 綠色起點標記：角色一旦實際移動就隱藏（見 tryMove）。保留引用讓 buildPath 重建後沿用可見狀態。
let startMarker: THREE.Mesh | null = null
let hasMoved = false
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

// (px,py) 換算後的 ds-cell 是否落在「可走區內部」（smoothedMask = 輪廓多邊形的柵格化，
// 即視覺地板覆蓋的格子）。這是 canStandAtPoly 缺的那一半判定：
//   canStandAtPoly 只檢查「圓心到線段距離 ≥ R」，對「地圖外的空曠處」（附近沒有任何線段）
//   會誤判為可站（距離無限大 ≥ R）。加上「必須在 smoothedMask 內」才能排除界外點。
function isInsideWalkable(px: number, py: number): boolean {
  if (!collisionMask || collisionW <= 0 || collisionH <= 0) return false
  const cx = Math.floor(px * pxToColl)
  const cy = Math.floor(py * pxToColl)
  if (cx < 0 || cx >= collisionW || cy < 0 || cy >= collisionH) return false
  return collisionMask[cy * collisionW + cx] === 1
}

// 玩家中心是否可以站在 (px, py)。
// 輪廓模式：兩個條件都要過 —
//   (a) 圓心落在可走區內部（isInsideWalkable，排除地圖外空曠處被誤判可站）；
//   (b) 圓心到任一輪廓線段距離 ≥ R（canStandAtPoly，碰撞 = 視覺牆邊界、不穿牆）。
// box fallback：退回網格版 canStandAt（其網格判定本身已含界外=牆，不需 (a)）。
function isPassableAtPixel(px: number, py: number): boolean {
  if (collisionSegs) {
    if (!isInsideWalkable(px, py)) return false
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

// 動態指引路徑：找 userPosition 在 pathNodes 折線上的最近投影點，回傳「從腳下到終點」的節點序列
//（原始 px 座標）。沿路走時前段被裁掉 → 路徑視覺上縮短。第一人稱外（無 userPosition）回傳完整 pathNodes。
function computeTrimmedPath(): Array<{ x: number; y: number }> {
  const nodes = mapStore.pathNodes as Array<{ x: number; y: number }>
  if (!nodes || nodes.length < 2) return nodes ?? []
  const u = mapStore.userPosition
  if (!u || viewMode.value !== 'first-person') return nodes.slice()

  // 找最近段 i 與其投影參數 t（0..1）
  let bestSeg = 0
  let bestT = 0
  let bestD2 = Infinity
  let bestProj = { x: nodes[0]!.x, y: nodes[0]!.y }
  for (let i = 0; i < nodes.length - 1; i++) {
    const a = nodes[i]!
    const b = nodes[i + 1]!
    const abx = b.x - a.x
    const aby = b.y - a.y
    const ab2 = abx * abx + aby * aby
    let t = ab2 === 0 ? 0 : ((u.x - a.x) * abx + (u.y - a.y) * aby) / ab2
    if (t < 0) t = 0
    else if (t > 1) t = 1
    const px = a.x + t * abx
    const py = a.y + t * aby
    const dx = u.x - px
    const dy = u.y - py
    const d2 = dx * dx + dy * dy
    if (d2 < bestD2) {
      bestD2 = d2
      bestSeg = i
      bestT = t
      bestProj = { x: px, y: py }
    }
  }
  // trimmed = [投影點, 投影所在段之後的所有節點...]
  const out: Array<{ x: number; y: number }> = [bestProj]
  const startIdx = bestT >= 1 ? bestSeg + 2 : bestSeg + 1
  for (let i = startIdx; i < nodes.length; i++) out.push(nodes[i]!)
  if (out.length < 2) out.push(nodes[nodes.length - 1]!)
  return out
}

// 依「目前動態(trimmed)路徑」重建 3D 路徑幾何。呼叫端做節流（角色移動 > 門檻或 pathNodes 變才呼叫），
// 避免逐幀重建 tube 過重。路徑統一鮮紅；起點 marker = 腳下、終點 marker = 終點。
function buildPath() {
  disposeGroup(pathGroup)
  startMarker = null // 舊 marker 已隨 pathGroup dispose，清引用避免懸空（下方會重建）。
  if (!hasMask.value) return
  if (!mapStore.pathNodes || mapStore.pathNodes.length < 2) return

  const yLift = sceneCellSize * 0.3
  const radius = sceneCellSize * 0.45
  const tubeMat = new THREE.MeshStandardMaterial({
    color: PATH_COLOR_HEX,
    emissive: PATH_COLOR_HEX,
    emissiveIntensity: 0.9,
    roughness: 0.3,
    metalness: 0.2,
  })

  // 動態裁切後的路徑（第一人稱沿路走會縮短）。非第一人稱為完整路徑。
  // 再經 DP + Bresenham 驗證簡化（清軸向抖動，真實轉角保留）。
  // pixelToWorld 採零偏移公式，這裡加半個 mask cell 偏移讓視覺路徑落在 A* 走過的 cell 正中央。
  const trimmed = computeTrimmedPath()
  if (trimmed.length < 2) return
  const simplified = dpSimplifyPath(trimmed, 3)
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
  startMarker = new THREE.Mesh(markerGeo, startMat)
  startMarker.position.copy(points[0]!).setY(sceneCellSize * 0.9)
  // 角色已開始移動 → 起點標記不再顯示（buildPath 會在移動後重建，沿用隱藏狀態）。
  startMarker.visible = !hasMoved
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

// 找離 (px, py) 最近、且玩家圓 (R=PLAYER_RADIUS_CELLS) 可整個塞進去的合法位置，回原始像素座標。
//
// 修法（取代舊版「BFS 取第一個 cell 中心」）：
//   舊版只取每個 cell 的「中心」驗證、且回「BFS 順序第一個」通過者。問題：
//     - 半徑 0.35 cell 在貼牆走道中，cell 中心常剛好離牆 < R → 該 cell 整顆被否決，
//       BFS 可能略過真正最近的合法落點（其實只需在 cell 內偏一點點）。
//     - 全找不到時回傳原始點（可能在地圖外）→ 玩家被丟到界外、走不動。
//   新版：
//     1. 候選來源 = A* pathNodes（保證在路線上、可走）優先，再加 smoothedMask 可走 cell；
//        smoothedMask 是輪廓多邊形柵格化，必在可走區內部 → 不會選到界外。
//     2. 每個可走 cell 做 3×3 子取樣（不只中心），用 isPassableAtPixel 驗證（內部 + 圓不撞牆）。
//     3. 取「離目標最近」的合法點（全域最小距離），不是 BFS 第一個。
//     4. 完全找不到合法落點時，退回「最近的可走 cell 中心」（至少在地圖內），並 warn。
function snapToNearestPassable(px: number, py: number): { x: number; y: number } {
  // 原地已合法就直接回傳
  if (isPassableAtPixel(px, py)) return { x: px, y: py }
  if (!collisionMask || collisionW <= 0 || collisionH <= 0) return { x: px, y: py }

  const tx = px * pxToColl // 目標（ds-cell 座標），用來比距離
  const ty = py * pxToColl

  let bestValid: { x: number; y: number } | null = null
  let bestValidD2 = Infinity
  let bestInside: { x: number; y: number } | null = null // 退路：最近的可走 cell 中心
  let bestInsideD2 = Infinity

  const consider = (cellCx: number, cellCy: number) => {
    if (cellCx < 0 || cellCx >= collisionW || cellCy < 0 || cellCy >= collisionH) return
    if (collisionMask![cellCy * collisionW + cellCx] !== 1) return
    // 記錄最近可走 cell 中心當退路
    {
      const dcx = cellCx + 0.5 - tx
      const dcy = cellCy + 0.5 - ty
      const d2 = dcx * dcx + dcy * dcy
      if (d2 < bestInsideD2) {
        bestInsideD2 = d2
        bestInside = { x: (cellCx + 0.5) / pxToColl, y: (cellCy + 0.5) / pxToColl }
      }
    }
    // 3×3 子取樣（cell 內 0.2/0.5/0.8 處），逐一驗證並取最近合法
    for (let sy = 0; sy < 3; sy++) {
      for (let sx = 0; sx < 3; sx++) {
        const cu = cellCx + 0.2 + 0.3 * sx
        const cv = cellCy + 0.2 + 0.3 * sy
        const candPx = cu / pxToColl
        const candPy = cv / pxToColl
        if (!isPassableAtPixel(candPx, candPy)) continue
        const d2 = (cu - tx) * (cu - tx) + (cv - ty) * (cv - ty)
        if (d2 < bestValidD2) {
          bestValidD2 = d2
          bestValid = { x: candPx, y: candPy }
        }
      }
    }
  }

  // 1) A* pathNodes 優先（這些點在路線上、必在可走區）。轉成 cell 後納入考量。
  const nodes = mapStore.pathNodes
  if (nodes && nodes.length > 0) {
    for (const nd of nodes) {
      consider(Math.floor(nd.x * pxToColl), Math.floor(nd.y * pxToColl))
    }
    // pathNodes 本身也直接當候選驗證（不限 cell 中心）
    for (const nd of nodes) {
      if (isPassableAtPixel(nd.x, nd.y)) {
        const cu = nd.x * pxToColl
        const cv = nd.y * pxToColl
        const d2 = (cu - tx) * (cu - tx) + (cv - ty) * (cv - ty)
        if (d2 < bestValidD2) {
          bestValidD2 = d2
          bestValid = { x: nd.x, y: nd.y }
        }
      }
    }
  }

  // 2) BFS 掃 smoothedMask 可走 cell（由目標 clamp 後出發），逐一 consider。
  //    不在找到第一個就停 —— 掃完一個「足夠大」的環以確保拿到全域最近合法點。
  const cx0 = Math.max(0, Math.min(collisionW - 1, Math.floor(px * pxToColl)))
  const cy0 = Math.max(0, Math.min(collisionH - 1, Math.floor(py * pxToColl)))
  const seen = new Uint8Array(collisionW * collisionH)
  const queue: number[] = []
  const enqueue = (cx: number, cy: number) => {
    if (cx < 0 || cx >= collisionW || cy < 0 || cy >= collisionH) return
    const i = cy * collisionW + cx
    if (seen[i]) return
    seen[i] = 1
    queue.push(i)
  }
  enqueue(cx0, cy0)
  // BFS 以「曼哈頓層數」推進；找到第一個合法點後，再多掃 2 層以涵蓋對角更近者，然後停。
  let foundAtSteps = -1
  let steps = 0
  // 用 (idx<<16 | step) 太脆弱，改用平行陣列存步數
  const stepOf = new Map<number, number>()
  stepOf.set(cy0 * collisionW + cx0, 0)
  while (queue.length > 0) {
    const idx = queue.shift()!
    const cx = idx % collisionW
    const cy = Math.floor(idx / collisionW)
    steps = stepOf.get(idx) ?? 0
    if (foundAtSteps >= 0 && steps > foundAtSteps + 2) break // 已找到合法點且多掃 2 層 → 收斂
    consider(cx, cy)
    if (bestValid && foundAtSteps < 0) foundAtSteps = steps
    for (const [nx, ny] of [
      [cx + 1, cy],
      [cx - 1, cy],
      [cx, cy + 1],
      [cx, cy - 1],
    ] as const) {
      if (nx < 0 || nx >= collisionW || ny < 0 || ny >= collisionH) continue
      const ni = ny * collisionW + nx
      if (seen[ni]) continue
      stepOf.set(ni, steps + 1)
      enqueue(nx, ny)
    }
  }

  if (bestValid) return bestValid
  if (bestInside) {
    console.warn(
      '[SceneView] snapToNearestPassable: 找不到「圓可完整塞入」的點（走道可能比玩家直徑窄），退回最近可走 cell 中心。',
    )
    return bestInside
  }
  console.warn('[SceneView] snapToNearestPassable: 完全找不到可走 cell（smoothedMask 全牆？），回傳原點。')
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

  // 轉視角：只有鍵盤 rot 走這裡（觸控轉視角在 onLookMove 直接累加 userHeading）。
  if (moveInput.rot !== 0) {
    mapStore.userHeading = heading + moveInput.rot * ROT_SPEED * dtSec
  }

  // 移動向量（strafe 模型，相對目前視角）：
  //   fwd    沿「前方向」=(cos h, sin h)
  //   strafe 沿「右方向」=(cos(h+π/2), sin(h+π/2)) = (-sin h, cos h)
  //   世界移動方向 = fwd × 前方向 + strafe × 右方向
  // 兩軸合成後再夾住長度 ≤ 1（避免斜推時 fwd² + strafe² > 1 造成對角線比直線快）。
  let fwd = moveInput.fwd
  let strafe = moveInput.strafe
  const inputMag = Math.hypot(fwd, strafe)
  if (inputMag > 1) {
    fwd /= inputMag
    strafe /= inputMag
  }
  if (fwd !== 0 || strafe !== 0) {
    const h = mapStore.userHeading ?? 0
    const cosH = Math.cos(h)
    const sinH = Math.sin(h)
    // 前方向 (cosH, sinH)、右方向 (-sinH, cosH)
    const dirX = fwd * cosH + strafe * -sinH
    const dirY = fwd * sinH + strafe * cosH
    const totalStepPx = MOVE_SPEED_PX * dtSec
    // 全部換算到 ds-cell 座標系，子步長 0.3 cell（防穿牆 tunneling 門檻）。
    // moveCirclePoly/moveCircle 已接受任意 (du,dv)，斜向移動不需改碰撞，沿牆滑行照舊。
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

    // 角色一旦實際移動，隱藏綠色起點標記（只需觸發一次）。
    if (!hasMoved) {
      hasMoved = true
      if (startMarker) startMarker.visible = false
    }
  }

  // debug 疊層讀值（heading 正規化到 [-π,π] 方便閱讀）。
  if (showDebug.value) {
    const hh = mapStore.userHeading ?? 0
    debugHeading.value = Math.atan2(Math.sin(hh), Math.cos(hh))
  }
}

// 每幀檢查偏離。偏離超過門檻 → 自動從當前位置重跑 A*（節流 REPLAN_MIN_INTERVAL_SEC），
// 取代原本的手動按鈕。replan 成功則 offPath 回 false；失敗（A* 回 0，例如終點不可達）才把
// offPath 維持 true 顯示提示 banner（fallback）。
function updateOffPath(nowSec: number) {
  if (!mapStore.userPosition) {
    offPath.value = false
    return
  }
  const d = minDistToPath(mapStore.userPosition.x, mapStore.userPosition.y)
  if (d <= OFF_PATH_THRESHOLD_PX) {
    offPath.value = false
    return
  }
  // 已偏離：節流內不重複重算
  if (nowSec - lastReplanTs < REPLAN_MIN_INTERVAL_SEC) {
    return
  }
  lastReplanTs = nowSec
  const ok = replanFromCurrent()
  offPath.value = !ok // 重算成功 → 路徑已修正、清旗標；失敗 → 顯示提示
}

// 從目前位置重跑 A* 到終點，回傳是否成功。pathNodes 變動會觸發 watch → buildPath 重建 3D 路徑。
function replanFromCurrent(): boolean {
  if (!mapStore.userPosition || !mapStore.endPoint) return false
  mapStore.setPoints({ ...mapStore.userPosition }, { ...mapStore.endPoint })
  const n = mapStore.runAStar()
  if (n > 0) {
    offPath.value = false
    return true
  }
  return false
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

// 把整張 floodFillResultData（原始圖尺寸 RGBA 染色 buffer）畫進一張 offscreen canvas，
// 之後 drawMinimap 用 drawImage 做局部裁切縮放（比逐像素取樣快）。資料變時重建。
function ensureMinimapBase() {
  const buf = mapStore.floodFillResultData
  const w = mapStore.mapWidth
  const h = mapStore.mapHeight
  if (!buf || w <= 0 || h <= 0) {
    minimapBaseTmp = null
    return
  }
  // 已建且尺寸相符就沿用（floodFill 重跑時 buffer 參考會換，這裡用尺寸 + 旗標判斷簡單重建）。
  if (minimapBaseTmp && minimapBaseTmp.canvas.width === w && minimapBaseTmp.canvas.height === h
      && minimapBaseDirty === false) {
    return
  }
  const canvas = minimapBaseTmp?.canvas ?? document.createElement('canvas')
  canvas.width = w
  canvas.height = h
  const ctx = canvas.getContext('2d')
  if (!ctx) { minimapBaseTmp = null; return }
  ctx.putImageData(new ImageData(new Uint8ClampedArray(buf), w, h), 0, 0)
  minimapBaseTmp = { canvas, ctx }
  minimapBaseDirty = false
}

// 繪製右上角 minimap：以角色為中心、半徑 MINIMAP_RADIUS_PX 的局部視窗，固定北上（不旋轉）。
// 底圖 = floodFill 染色 + trimmed 路徑（鮮紅）+ 終點 + 角色箭頭（中心）。
function drawMinimap() {
  const cv = minimapCanvas.value
  if (!cv) return
  const ctx = cv.getContext('2d')
  if (!ctx) return
  const u = mapStore.userPosition
  ctx.clearRect(0, 0, MINIMAP_SIZE, MINIMAP_SIZE)
  if (!u) return

  ensureMinimapBase()
  const R = MINIMAP_RADIUS_PX
  const scale = MINIMAP_SIZE / (2 * R) // 原始 px → minimap px
  // 世界 px → minimap canvas px（以角色為中心、北上）
  const toCv = (px: number, py: number): [number, number] => [
    (px - u.x) * scale + MINIMAP_SIZE / 2,
    (py - u.y) * scale + MINIMAP_SIZE / 2,
  ]

  ctx.save()
  // 圓形裁切（雷達感）
  ctx.beginPath()
  ctx.arc(MINIMAP_SIZE / 2, MINIMAP_SIZE / 2, MINIMAP_SIZE / 2, 0, Math.PI * 2)
  ctx.clip()

  // 底圖：把以角色為中心 2R×2R 的原始區域畫到整個 minimap
  ctx.fillStyle = '#1a2433'
  ctx.fillRect(0, 0, MINIMAP_SIZE, MINIMAP_SIZE)
  if (minimapBaseTmp) {
    ctx.imageSmoothingEnabled = false
    ctx.drawImage(minimapBaseTmp.canvas, u.x - R, u.y - R, 2 * R, 2 * R, 0, 0, MINIMAP_SIZE, MINIMAP_SIZE)
  }

  // trimmed 路徑（鮮紅 + 白外框）
  const trimmed = computeTrimmedPath()
  if (trimmed.length >= 2) {
    const trace = () => {
      ctx.beginPath()
      const [x0, y0] = toCv(trimmed[0]!.x, trimmed[0]!.y)
      ctx.moveTo(x0, y0)
      for (let i = 1; i < trimmed.length; i++) {
        const [x, y] = toCv(trimmed[i]!.x, trimmed[i]!.y)
        ctx.lineTo(x, y)
      }
    }
    ctx.lineJoin = 'round'
    ctx.lineCap = 'round'
    trace()
    ctx.strokeStyle = 'rgba(255,255,255,0.9)'
    ctx.lineWidth = 5
    ctx.stroke()
    trace()
    ctx.strokeStyle = PATH_COLOR_CSS
    ctx.lineWidth = 3
    ctx.stroke()
  }

  // 終點（若在視窗內）
  if (mapStore.endPoint) {
    const [ex, ey] = toCv(mapStore.endPoint.x, mapStore.endPoint.y)
    ctx.beginPath()
    ctx.arc(ex, ey, 5, 0, Math.PI * 2)
    ctx.fillStyle = '#ffffff'
    ctx.fill()
    ctx.beginPath()
    ctx.arc(ex, ey, 3.5, 0, Math.PI * 2)
    ctx.fillStyle = '#ff4466'
    ctx.fill()
  }

  ctx.restore() // 解除圓形裁切

  // 角色箭頭（畫在中心，依 userHeading 朝向；地圖北上，故箭頭旋轉）
  const cx = MINIMAP_SIZE / 2
  const cy = MINIMAP_SIZE / 2
  const h = mapStore.userHeading ?? 0
  ctx.save()
  ctx.translate(cx, cy)
  ctx.rotate(h + Math.PI / 2) // heading=0 指向 +x；畫的箭頭預設朝上(-y)，故 +90°
  ctx.beginPath()
  ctx.moveTo(0, -8)
  ctx.lineTo(6, 7)
  ctx.lineTo(0, 4)
  ctx.lineTo(-6, 7)
  ctx.closePath()
  ctx.fillStyle = '#ffd600'
  ctx.strokeStyle = 'rgba(0,0,0,0.6)'
  ctx.lineWidth = 1.5
  ctx.fill()
  ctx.stroke()
  ctx.restore()

  // 外圈框
  ctx.beginPath()
  ctx.arc(MINIMAP_SIZE / 2, MINIMAP_SIZE / 2, MINIMAP_SIZE / 2 - 1, 0, Math.PI * 2)
  ctx.strokeStyle = 'rgba(255,255,255,0.5)'
  ctx.lineWidth = 2
  ctx.stroke()
}

// 角色移動超過門檻才重建 trimmed 3D 路徑（節流，避免逐幀重建 tube）。
function maybeRebuildPath() {
  const u = mapStore.userPosition
  if (!u) return
  if (lastPathBuildPos) {
    const dx = u.x - lastPathBuildPos.x
    const dy = u.y - lastPathBuildPos.y
    if (dx * dx + dy * dy < PATH_REBUILD_MOVE_PX * PATH_REBUILD_MOVE_PX) return
  }
  lastPathBuildPos = { x: u.x, y: u.y }
  buildPath()
}

function animate(ts?: number) {
  frameId = requestAnimationFrame(animate)
  const now = ts ?? performance.now()
  const nowSec = now / 1000
  const dt = lastTs === 0 ? 0 : Math.min(0.05, (now - lastTs) / 1000)
  lastTs = now

  if (viewMode.value === 'first-person') {
    ensureUserState()
    syncMoveInput() // 合併鍵盤 + 搖桿成 moveInput（每幀，tryMove 前）
    tryMove(dt)
    updateOffPath(nowSec) // 偏離自動 replan（節流）
    maybeRebuildPath() // 沿路走 → 動態裁切重建 3D 路徑（節流）
    drawMinimap() // 右上角 2D minimap 同步角色/路徑
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
  // 進第一人稱：重置動態路徑/重算節流狀態，並立即依目前位置重建一次 trimmed 路徑。
  lastPathBuildPos = null
  lastReplanTs = 0
  buildPath()
  // === Bug1 診斷：起點 / 起點是否可走 / snap 結果 / snap 結果是否可走 ===
  // 用來判別問題是「起點被判不可走」還是「snap 找錯點」。實測排查後可移除。
  if (mapStore.startPoint) {
    const sp = mapStore.startPoint
    const startOk = isPassableAtPixel(sp.x, sp.y)
    const startInside = isInsideWalkable(sp.x, sp.y)
    const snapped = snapToNearestPassable(sp.x, sp.y)
    const snapOk = isPassableAtPixel(snapped.x, snapped.y)
    const snapInside = isInsideWalkable(snapped.x, snapped.y)
    console.log(
      '[SceneView][enterFP] startPoint=(%d,%d) passable=%s inside=%s | snap=(%.1f,%.1f) passable=%s inside=%s | userPos=(%.1f,%.1f) | mode=%s',
      sp.x, sp.y, String(startOk), String(startInside),
      snapped.x, snapped.y, String(snapOk), String(snapInside),
      mapStore.userPosition?.x ?? NaN, mapStore.userPosition?.y ?? NaN,
      collisionSegs ? 'poly' : 'grid',
    )
  }
  mapGroup.rotation.y = 0
  pathGroup.rotation.y = 0
  // 第一人稱由 updateCamera 直接控制相機，停用 OrbitControls 以免互搶。
  if (controls) controls.enabled = false
}

function exitFirstPerson() {
  viewMode.value = 'overview'
  // 離開第一人稱：清掉鍵盤/搖桿/轉視角殘留輸入，避免回俯瞰後還在動或下次進來漂移。
  keyInput.fwd = 0
  keyInput.rot = 0
  moveInput.fwd = 0
  moveInput.strafe = 0
  moveInput.rot = 0
  clearTouchState()
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
  // 俯瞰顯示完整路徑（computeTrimmedPath 在非第一人稱回傳完整 pathNodes）。
  buildPath()
}

// 把鍵盤(keyInput)與搖桿(joystick)合併成本幀的 moveInput。每幀呼叫一次（tryMove 前）。
//   fwd/strafe：取「絕對值較大」的來源 —— 搖桿推到底(±1)等同鍵盤，搖桿類比小量也能生效，
//               鍵盤按住時不會被靜止搖桿(0)歸零。strafe 只有搖桿會出（鍵盤不側移）。
//   rot       ：只有鍵盤（A/D/←/→ 轉視角）；觸控轉視角直接改 userHeading，不經此。
function syncMoveInput() {
  const jx = Math.abs(joystick.jx) >= JOYSTICK_DEADZONE ? joystick.jx : 0
  const jy = Math.abs(joystick.jy) >= JOYSTICK_DEADZONE ? joystick.jy : 0
  // jy 已在 updateJoystickFromPointer 取負，故「上=前進(+)」可直接當 fwd；jx「右=側移(+)」當 strafe。
  const joyFwd = jy
  const joyStrafe = jx
  moveInput.fwd = Math.abs(joyFwd) >= Math.abs(keyInput.fwd) ? joyFwd : keyInput.fwd
  moveInput.strafe = joyStrafe
  moveInput.rot = keyInput.rot
}

function onKey(down: boolean, e: KeyboardEvent) {
  if (viewMode.value !== 'first-person') return
  const v = down ? 1 : 0
  switch (e.key) {
    case 'w':
    case 'W':
    case 'ArrowUp':
      keyInput.fwd = v
      break
    case 's':
    case 'S':
    case 'ArrowDown':
      keyInput.fwd = -v
      break
    case 'a':
    case 'A':
    case 'ArrowLeft':
      keyInput.rot = -v // 桌機沿用習慣：A/← = 左轉視角（非側移）
      break
    case 'd':
    case 'D':
    case 'ArrowRight':
      keyInput.rot = v // D/→ = 右轉視角
      break
    default:
      return
  }
  e.preventDefault()
}
const onKeyDown = (e: KeyboardEvent) => onKey(true, e)
const onKeyUp = (e: KeyboardEvent) => onKey(false, e)

// ============================================================================
// 觸控操控（第一人稱）：虛擬搖桿 + 滑屏轉視角，多點觸控分流
// ============================================================================
//
// 【pointerId 分流】所有觸控事件掛在覆蓋全螢幕的 .touch-layer 上，pointerdown 依「落點」
//   決定這根手指是「搖桿」還是「轉視角」，並把它的 pointerId 記到 joystickPointerId 或
//   lookPointerId。之後 pointermove / pointerup 一律先比對 e.pointerId，只處理屬於自己
//   角色的那根手指 → 搖桿與轉視角可同時、互不干擾（不綁左右手）。絕不用單一全域 pointer 狀態。
//
// 【區域劃分（浮動式）】落點在「畫面下方 JOYSTICK_ZONE_RATIO 比例」→ 搖桿（底座浮動生成在手指按下處）；
//   落點在「上方其餘」→ 轉視角。手指一旦歸搖桿，後續滑到上方也仍算搖桿（用 pointerId 鎖定），
//   不會半途變成轉視角；反之亦然。
//
// 【搖桿向量】updateJoystickFromPointer 以「按下處（=底座中心）」為原點算 (dx,dy) 像素位移，
//   除以 JOYSTICK_KNOB_RANGE 正規化，長度 >1 夾回 1。jx=右(+)/左(-)；螢幕 y 往下為正，
//   前進要正，故 jy = -dy/range（手指往上 → dy<0 → jy>0 → 前進）。死區在 syncMoveInput 套用。
//
// 【轉視角】onLookMove 取本次 clientX - 上次 clientX 的水平 delta × LOOK_SENSITIVITY，
//   累加到 userHeading。垂直方向忽略（目前只做 yaw，相機維持平視）。

// 落點是否在「搖桿生效區」（touch-layer 下方 JOYSTICK_ZONE_RATIO 比例）。
// 用 touch-layer 自身的 rect（非 window.innerHeight）算相對位置，因為場景上方有 header/步驟條，
// 用視窗高度會對不準。relativeY=0 為頂、1 為底；落在底部 ratio 帶內 → 搖桿區。
function isInJoystickZone(clientY: number, layer: HTMLElement): boolean {
  const rect = layer.getBoundingClientRect()
  if (rect.height <= 0) return false
  const relativeY = (clientY - rect.top) / rect.height
  return relativeY >= 1 - JOYSTICK_ZONE_RATIO
}

function updateJoystickFromPointer(clientX: number, clientY: number) {
  let dx = clientX - joystickCenterX
  let dy = clientY - joystickCenterY
  const len = Math.hypot(dx, dy)
  // 夾住在搖桿頭可移動範圍內（單位圓 = JOYSTICK_KNOB_RANGE 像素）。
  if (len > JOYSTICK_KNOB_RANGE) {
    dx = (dx / len) * JOYSTICK_KNOB_RANGE
    dy = (dy / len) * JOYSTICK_KNOB_RANGE
  }
  joystick.knobX = dx
  joystick.knobY = dy
  joystick.jx = dx / JOYSTICK_KNOB_RANGE // 右(+)/左(-)
  joystick.jy = -dy / JOYSTICK_KNOB_RANGE // 上(+,前進)/下(-,後退)
}

function resetJoystick() {
  joystick.jx = 0
  joystick.jy = 0
  joystick.knobX = 0
  joystick.knobY = 0
  joystick.active = false
  joystickPointerId = null
}

// 觸控事件是否落在 UI 控制項（按鈕/連結）上。雖然這些 UI 的 z-index 已高於 touch-layer
// → 正常情況 pointerdown 會直接命中按鈕、不會進到這裡；但為防呆（瀏覽器差異 / 事件冒泡），
// 仍在此判斷 e.target 是否在互動控制項內，是的話讓事件穿透（不轉成搖桿/轉視角）。
function isUiControlTarget(e: PointerEvent): boolean {
  const t = e.target as HTMLElement | null
  if (!t || typeof t.closest !== 'function') return false
  return !!t.closest('button, a, .scene-flow-btn, .mode-btn, .replan-btn')
}

function onTouchPointerDown(e: PointerEvent) {
  if (viewMode.value !== 'first-person') return
  // 落在 UI 按鈕上 → 不處理，讓點擊穿透到按鈕（俯瞰切換/流程/重新規劃皆可點）。
  if (isUiControlTarget(e)) return
  // 搖桿落點（浮動式）：手指落在「畫面下方搖桿區」且搖桿尚未被別的手指佔用 → 歸搖桿，
  // 底座中心 = 手指按下處（不是固定 DOM 中心），knob 從該點起算 → 手指永遠在搖桿中心附近、不會滑出。
  const layer = e.currentTarget as HTMLElement // = .touch-layer，用來算下方比例
  // setPointerCapture：把這根 pointer 綁定到 touch-layer，之後 pointermove/up 即使手指滑出
  // canvas 邊界（甚至滑出視窗）仍持續送到此元素 → 不會「滑出去就失去操控/卡住不歸零」。
  const capture = () => {
    try { layer.setPointerCapture(e.pointerId) } catch { /* 某些環境不支援，忽略 */ }
  }
  if (joystickPointerId === null && isInJoystickZone(e.clientY, layer)) {
    joystickCenterX = e.clientX
    joystickCenterY = e.clientY
    joystick.baseX = e.clientX // 給 template 把底座浮動定位到手指處
    joystick.baseY = e.clientY
    joystickPointerId = e.pointerId
    joystick.active = true
    capture()
    updateJoystickFromPointer(e.clientX, e.clientY)
    e.preventDefault()
    return
  }
  // 上方（或搖桿區已被佔用）→ 轉視角：若轉視角尚未被佔用 → 歸轉視角。
  if (lookPointerId === null) {
    lookPointerId = e.pointerId
    lookLastX = e.clientX
    capture()
    e.preventDefault()
  }
}

function onTouchPointerMove(e: PointerEvent) {
  if (viewMode.value !== 'first-person') return
  if (e.pointerId === joystickPointerId) {
    updateJoystickFromPointer(e.clientX, e.clientY)
    e.preventDefault()
  } else if (e.pointerId === lookPointerId) {
    const dx = e.clientX - lookLastX
    lookLastX = e.clientX
    const h = mapStore.userHeading ?? 0
    mapStore.userHeading = h + dx * LOOK_SENSITIVITY
    e.preventDefault()
  }
}

function onTouchPointerUp(e: PointerEvent) {
  if (e.pointerId === joystickPointerId) {
    resetJoystick() // 放開回中，不漂移
    e.preventDefault()
  } else if (e.pointerId === lookPointerId) {
    lookPointerId = null
    e.preventDefault()
  }
}

// 把目前已分流的手指狀態全部歸零（退出第一人稱 / 卸載時呼叫，避免殘留輸入）。
function clearTouchState() {
  resetJoystick()
  lookPointerId = null
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
    hasMoved = false // 換地圖 / 重跑 flood fill → 新路線，重新顯示起點標記
    buildGeometry()
    lastPathBuildPos = null // 強制下次 maybeRebuildPath 重建
    buildPath()
    buildAvatar()
  },
)

// floodFill 重跑 → minimap 底圖需重建。
watch(
  () => mapStore.floodFillResultData,
  () => {
    minimapBaseDirty = true
  },
)

watch(
  () => mapStore.pathNodes,
  () => {
    lastPathBuildPos = null // replan 換了 pathNodes → 立即重建 trimmed 路徑
    buildPath()
  },
  { deep: true },
)

onBeforeUnmount(() => {
  cancelAnimationFrame(frameId)
  resizeObserver?.disconnect()
  window.removeEventListener('keydown', onKeyDown)
  window.removeEventListener('keyup', onKeyUp)
  clearTouchState()
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

    <!-- 第一人稱右上角 2D minimap：以角色為中心的局部地圖（固定北上），每幀於 drawMinimap 更新。 -->
    <canvas
      v-if="viewMode === 'first-person' && hasMask"
      ref="minimapCanvas"
      class="minimap"
      :width="MINIMAP_SIZE"
      :height="MINIMAP_SIZE"
    />

    <!-- 偏離路徑時可從目前位置重新跑 A*，不重新做 flood fill。 -->
    <div v-if="offPath && viewMode === 'first-person'" class="offpath-banner">
      偵測到偏離路徑
      <button class="replan-btn" @click="replanFromCurrent">重新規劃</button>
    </div>

    <!--
      第一人稱觸控層：覆蓋全螢幕，承接所有 pointer 事件並依落點分流（搖桿 / 轉視角）。
      只在 first-person 模式渲染 → 俯瞰模式時此層不存在，OrbitControls 直接吃 canvas 事件，不受影響。
      touch-action:none（見 style）防瀏覽器手勢；事件掛同一層且用 pointerId 過濾。
      區域劃分（浮動搖桿）：畫面下方比例 pointerdown 歸搖桿（底座浮動生成在手指處），上方歸轉視角。
    -->
    <div
      v-if="viewMode === 'first-person' && hasMask"
      class="touch-layer"
      @pointerdown="onTouchPointerDown"
      @pointermove="onTouchPointerMove"
      @pointerup="onTouchPointerUp"
      @pointercancel="onTouchPointerUp"
    >
      <!--
        浮動虛擬搖桿：底座 + 可拖曳搖桿頭（knob 用 transform 跟手）。pointer 事件由外層 touch-layer 統一處理。
        active 時 → 用 inline left/top 把底座中心定位到手指按下處（fixed 定位，translate(-50%,-50%) 置中）；
        idle（未按）→ 移除 inline 定位，改吃 CSS 預設位置（水平置中、垂直約 62%）並半透明淡隱。
      -->
      <div
        class="joystick-base"
        :class="{ active: joystick.active }"
        :style="joystick.active ? { left: `${joystick.baseX}px`, top: `${joystick.baseY}px` } : undefined"
      >
        <div
          class="joystick-knob"
          :style="{ transform: `translate(-50%, -50%) translate(${joystick.knobX}px, ${joystick.knobY}px)` }"
        />
      </div>

      <!-- debug：顯示搖桿 (jx,jy) 與目前視角 heading（度），確認移動與轉視角各自獨立。 -->
      <div v-if="showDebug" class="touch-debug">
        jx {{ joystick.jx.toFixed(2) }} · jy {{ joystick.jy.toFixed(2) }}<br />
        heading {{ debugHeadingDeg }}°
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
  z-index: 5; /* 高於 .touch-layer(3)：第一人稱時流程按鈕仍可點 */
  display: flex;
  gap: var(--space-2);
  flex-wrap: wrap;
  justify-content: flex-end;
}

/* 右上角 minimap：在流程按鈕下方。z-index 高於 touch-layer，但 pointer-events:none 讓觸控穿透
   （minimap 純顯示、不吃事件，下方仍可轉視角）。圓形雷達樣式。 */
.minimap {
  position: absolute;
  top: 72px;
  right: 16px;
  z-index: 4;
  width: 160px;
  height: 160px;
  border-radius: var(--radius-circle);
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
  pointer-events: none;
  background: var(--color-scene-panel);
}

@media (max-width: 480px) {
  .minimap {
    width: 120px;
    height: 120px;
    top: 64px;
  }
}

.scene-flow-btn {
  display: inline-flex;
  align-items: center;
  min-height: 40px;
  padding: var(--space-2) var(--space-3);
  border: 1px solid rgba(255, 255, 255, 0.18);
  border-radius: var(--radius-md);
  background: var(--color-scene-panel);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  color: var(--color-scene-text);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  text-decoration: none;
  cursor: pointer;
  box-shadow: var(--shadow-md);
  transition:
    background var(--dur-fast) var(--ease-out),
    color var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out),
    box-shadow var(--dur-fast) var(--ease-out);
}

.scene-flow-btn:hover {
  background: var(--gradient-primary);
  color: var(--color-white);
  transform: translateY(-1px);
  box-shadow: var(--shadow-lg);
}
.scene-flow-btn:active {
  transform: translateY(0) scale(0.98);
}

.scene-flow-btn--primary {
  border-color: transparent;
  background: var(--gradient-primary);
  color: var(--color-white);
}

.top-bar {
  position: absolute;
  top: 16px;
  left: 16px;
  z-index: 5; /* 高於 .touch-layer(3)：第一人稱時「俯瞰」切換鈕仍可點 */
  display: flex;
  gap: 8px;
}
.mode-btn {
  padding: var(--space-2) var(--space-4);
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: var(--radius-md);
  background: var(--color-scene-panel);
  backdrop-filter: blur(8px);
  -webkit-backdrop-filter: blur(8px);
  color: var(--color-scene-text);
  cursor: pointer;
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  box-shadow: var(--shadow-md);
  transition:
    background-color var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out),
    box-shadow var(--dur-fast) var(--ease-out);
}
.mode-btn:hover {
  transform: translateY(-1px);
  box-shadow: var(--shadow-lg);
  border-color: var(--color-scene-accent);
}
.mode-btn:active {
  transform: translateY(0) scale(0.97);
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
  z-index: 5; /* 高於 .touch-layer(3)：偏離路徑的「重新規劃」鈕仍可點 */
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
  transition: transform var(--dur-fast) var(--ease-out);
}
.replan-btn:hover {
  transform: scale(1.04);
}
.replan-btn:active {
  transform: scale(0.96);
}

/* 觸控層：覆蓋整個場景、承接所有 pointer 事件。touch-action:none 禁用瀏覽器手勢
   （捲動/縮放/雙擊放大），確保搖桿拖曳與滑屏轉視角不被攔截。user-select:none 防選字。 */
.touch-layer {
  position: absolute;
  inset: 0;
  z-index: 3;
  touch-action: none;
  user-select: none;
  -webkit-user-select: none;
}

/* 浮動虛擬搖桿底座。尺寸 = JOYSTICK_BASE_RADIUS×2（120px），須與 script 常數一致。
   position:fixed + translate(-50%,-50%)：以「中心」對齊定位點（idle=CSS 預設、active=inline 手指座標）。
   用 fixed（視口座標）讓 active 時的 inline left/top（= pointer clientX/Y，視口座標）精準跟手。
   idle（未按）：水平置中、垂直約 62%（中下、比正中略高），半透明淡隱、不擋視線、提示搖桿在左半可用。 */
.joystick-base {
  position: fixed;
  left: 50%;
  top: 72%;
  width: 120px;
  height: 120px;
  border-radius: var(--radius-circle);
  background: var(--color-scene-panel);
  border: 2px solid rgba(255, 255, 255, 0.18);
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.35);
  transform: translate(-50%, -50%);
  opacity: 0.35; /* idle：淡 */
  transition: opacity 0.15s ease;
}
/* active：浮動到手指處（inline left/top 覆蓋上面的預設值），加深、邊框點亮。 */
.joystick-base.active {
  opacity: 0.85;
  border-color: var(--color-scene-accent);
}

/* 搖桿頭：底座中心起算，knobX/knobY（px）跟手。範圍夾在 JOYSTICK_KNOB_RANGE（44px）內。 */
.joystick-knob {
  position: absolute;
  left: 50%;
  top: 50%;
  width: 52px;
  height: 52px;
  border-radius: var(--radius-circle);
  background: var(--color-scene-accent);
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.4);
  transform: translate(-50%, -50%);
  pointer-events: none; /* 事件由 touch-layer 統一處理，knob 不自己吃 pointer */
}

/* debug 疊層：右下角顯示搖桿值與 heading，純資訊不吃事件。 */
.touch-debug {
  position: absolute;
  right: 12px;
  bottom: 12px;
  padding: var(--space-2) var(--space-3);
  background: var(--color-scene-panel);
  color: var(--color-scene-text);
  border-radius: var(--radius-md);
  font-size: var(--text-sm);
  line-height: 1.4;
  font-variant-numeric: tabular-nums;
  pointer-events: none;
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
}

@media (max-height: 620px) and (orientation: landscape) {
  .scene-view {
    height: calc(100dvh - 84px);
    min-height: 360px;
  }

  .scene-flow-actions {
    left: auto;
  }

  /* 橫向矮螢幕：idle 搖桿略上移，避開底部、不擋畫面（active 仍跟手不受影響）。 */
  .joystick-base {
    top: 66%;
  }

  .offpath-banner {
    top: var(--space-3);
  }
}

/* 桌機/寬螢幕（非手機）：畫面較高，idle 搖桿再往下一點，更貼近底部、不擋視線。
   active 跟手不受影響（inline 座標）。 */
@media (min-width: 721px) {
  .joystick-base {
    top: 82%;
  }
}
</style>
