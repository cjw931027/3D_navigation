<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue'
import * as THREE from 'three'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const container = ref<HTMLDivElement | null>(null)

let renderer: THREE.WebGLRenderer | null = null
let scene: THREE.Scene | null = null
let camera: THREE.PerspectiveCamera | null = null
let frameId = 0
let resizeObserver: ResizeObserver | null = null
let lastTs = 0

const mapGroup = new THREE.Group()
const pathGroup = new THREE.Group()
const avatarGroup = new THREE.Group()

const hasMask = computed(
  () => !!mapStore.passableMask && mapStore.maskWidth > 0 && mapStore.maskHeight > 0,
)

const MAP_EXTENT = 20
const WALL_HEIGHT_RATIO = 2.0
const MAX_CELLS_LONG_SIDE = 200

const viewMode = ref<'overview' | 'first-person'>('overview')
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
let pxToColl = 1

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
        let total  = 0
        if (x > 0)     { total++; if (data[i - 1] === 1) passNb++ }
        if (x < w - 1) { total++; if (data[i + 1] === 1) passNb++ }
        if (y > 0)     { total++; if (data[i - w] === 1) passNb++ }
        if (y < h - 1) { total++; if (data[i + w] === 1) passNb++ }
        if (total >= 3 && passNb >= 3) {
          data[i] = 1
          changed = true
        }
      }
    }
    if (!changed) break
  }
}

type Pt = { x: number; y: number }

// 外圍補 1 格 0 再做 N 次 3x3 box blur；把二值遮罩轉為 0..1 連續場，讓 iso-contour 可以吃到斜線特徵。
// passes 加大會更平滑但會吃掉 1 格寬的薄牆；1 pass 對 2+ 格的牆保留良好。
function blurToField(
  data: Uint8Array, w: number, h: number, passes: number,
): { field: Float32Array; pw: number; ph: number } {
  const pw = w + 2
  const ph = h + 2
  let field = new Float32Array(pw * ph)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (data[y * w + x] === 1) field[(y + 1) * pw + (x + 1)] = 1
    }
  }
  for (let p = 0; p < passes; p++) {
    const next = new Float32Array(pw * ph)
    for (let y = 0; y < ph; y++) {
      for (let x = 0; x < pw; x++) {
        let sum = 0, n = 0
        for (let dy = -1; dy <= 1; dy++) {
          const yy = y + dy
          if (yy < 0 || yy >= ph) continue
          for (let dx = -1; dx <= 1; dx++) {
            const xx = x + dx
            if (xx < 0 || xx >= pw) continue
            sum += field[yy * pw + xx]!
            n++
          }
        }
        next[y * pw + x] = sum / n
      }
    }
    field = next
  }
  return { field, pw, ph }
}

// Blur → Marching Squares (帶線性內插) 抽 iso-contour，在 cell edge 上找精確 0.5 交點。
// 同一條 edge 在相鄰 cell 中用相同兩端值計算 t，兩側算出的座標完全相同，方便以 edge id 串接。
// 輸出座標已扣除 padding 偏移，與原始 (w, h) grid corner 對齊。
function traceContours(data: Uint8Array, w: number, h: number): Pt[][] {
  const BLUR_PASSES = 2
  const THRESHOLD  = 0.5
  const { field, pw, ph } = blurToField(data, w, h, BLUR_PASSES)

  // Edge id：水平邊 (y, xl) in [0..ph] x [0..pw-1]；垂直邊 (x, yt) in [0..pw] x [0..ph-1]。
  const HORZ_BASE = 0
  const VERT_BASE = (ph + 1) * pw
  const horzId = (y: number, xl: number) => HORZ_BASE + y * pw + xl
  const vertId = (x: number, yt: number) => VERT_BASE + x * ph + yt

  const edgePos   = new Map<number, Pt>()
  const nextEdge  = new Map<number, number[]>()
  const addSeg = (from: number, to: number) => {
    const arr = nextEdge.get(from)
    if (arr) arr.push(to)
    else nextEdge.set(from, [to])
  }
  const hcross = (Vl: number, Vr: number, xl: number, y: number): Pt => {
    const t = (THRESHOLD - Vl) / (Vr - Vl)
    return { x: xl + t, y }
  }
  const vcross = (Vt: number, Vb: number, x: number, yt: number): Pt => {
    const t = (THRESHOLD - Vt) / (Vb - Vt)
    return { x, y: yt + t }
  }

  for (let cy = 0; cy < ph - 1; cy++) {
    for (let cx = 0; cx < pw - 1; cx++) {
      const tl = field[cy * pw + cx]!
      const tr = field[cy * pw + cx + 1]!
      const br = field[(cy + 1) * pw + cx + 1]!
      const bl = field[(cy + 1) * pw + cx]!
      const code =
        (tl >= THRESHOLD ? 8 : 0) |
        (tr >= THRESHOLD ? 4 : 0) |
        (br >= THRESHOLD ? 2 : 0) |
        (bl >= THRESHOLD ? 1 : 0)
      if (code === 0 || code === 15) continue

      const eT = horzId(cy, cx)
      const eB = horzId(cy + 1, cx)
      const eL = vertId(cx, cy)
      const eR = vertId(cx + 1, cy)
      const setT = () => { if (!edgePos.has(eT)) edgePos.set(eT, hcross(tl, tr, cx, cy)) }
      const setB = () => { if (!edgePos.has(eB)) edgePos.set(eB, hcross(bl, br, cx, cy + 1)) }
      const setL = () => { if (!edgePos.has(eL)) edgePos.set(eL, vcross(tl, bl, cx, cy)) }
      const setR = () => { if (!edgePos.has(eR)) edgePos.set(eR, vcross(tr, br, cx + 1, cy)) }

      // 方向取「passable 位於行進方向右側」= CW 繞 passable 區，與舊版 signedArea 分類相容。
      switch (code) {
        case 1:  setL(); setB(); addSeg(eL, eB); break
        case 2:  setB(); setR(); addSeg(eB, eR); break
        case 3:  setL(); setR(); addSeg(eL, eR); break
        case 4:  setR(); setT(); addSeg(eR, eT); break
        case 5: {
          setL(); setT(); setR(); setB()
          const ctr = (tl + tr + br + bl) * 0.25
          if (ctr >= THRESHOLD) { addSeg(eL, eT); addSeg(eB, eR) }
          else                  { addSeg(eL, eB); addSeg(eR, eT) }
          break
        }
        case 6:  setB(); setT(); addSeg(eB, eT); break
        case 7:  setL(); setT(); addSeg(eL, eT); break
        case 8:  setT(); setL(); addSeg(eT, eL); break
        case 9:  setT(); setB(); addSeg(eT, eB); break
        case 10: {
          setL(); setT(); setR(); setB()
          const ctr = (tl + tr + br + bl) * 0.25
          if (ctr >= THRESHOLD) { addSeg(eT, eR); addSeg(eB, eL) }
          else                  { addSeg(eT, eL); addSeg(eB, eR) }
          break
        }
        case 11: setT(); setR(); addSeg(eT, eR); break
        case 12: setR(); setL(); addSeg(eR, eL); break
        case 13: setR(); setB(); addSeg(eR, eB); break
        case 14: setB(); setL(); addSeg(eB, eL); break
      }
    }
  }

  const out: Pt[][] = []
  while (nextEdge.size > 0) {
    const it = nextEdge.keys().next()
    if (it.done) break
    const startId = it.value
    const startPos = edgePos.get(startId)
    if (!startPos) { nextEdge.delete(startId); continue }
    const contour: Pt[] = [{ x: startPos.x, y: startPos.y }]
    let cur = startId
    while (true) {
      const list = nextEdge.get(cur)
      if (!list || list.length === 0) { nextEdge.delete(cur); break }
      const nxt = list.shift()!
      if (list.length === 0) nextEdge.delete(cur)
      cur = nxt
      if (cur === startId) break
      const pos = edgePos.get(cur)
      if (!pos) break
      contour.push({ x: pos.x, y: pos.y })
    }
    if (contour.length >= 3) out.push(contour)
  }

  // 扣除 1 格 padding 偏移，回到原始 (w, h) grid-corner 座標系。
  for (const c of out) for (const p of c) { p.x -= 1; p.y -= 1 }
  return out
}

function signedArea(poly: Pt[]): number {
  let s = 0
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i]!
    const b = poly[(i + 1) % poly.length]!
    s += a.x * b.y - b.x * a.y
  }
  return s * 0.5
}

function perpDist2(p: Pt, a: Pt, b: Pt): number {
  const dx = b.x - a.x, dy = b.y - a.y
  const L2 = dx * dx + dy * dy
  if (L2 === 0) {
    const ex = p.x - a.x, ey = p.y - a.y
    return ex * ex + ey * ey
  }
  const cross = dx * (p.y - a.y) - dy * (p.x - a.x)
  return (cross * cross) / L2
}

function dpOpen(pts: Pt[], eps2: number): Pt[] {
  if (pts.length < 3) return pts.slice()
  const keep = new Uint8Array(pts.length)
  keep[0] = 1
  keep[pts.length - 1] = 1
  const stack: Array<[number, number]> = [[0, pts.length - 1]]
  while (stack.length > 0) {
    const [lo, hi] = stack.pop()!
    let maxD = 0, idx = -1
    for (let i = lo + 1; i < hi; i++) {
      const d = perpDist2(pts[i]!, pts[lo]!, pts[hi]!)
      if (d > maxD) { maxD = d; idx = i }
    }
    if (maxD > eps2 && idx !== -1) {
      keep[idx] = 1
      stack.push([lo, idx], [idx, hi])
    }
  }
  const r: Pt[] = []
  for (let i = 0; i < pts.length; i++) if (keep[i]) r.push(pts[i]!)
  return r
}

// 封閉 DP：先挑兩個互為最遠點的頂點作支點，避免起點偏置。
function dpClosed(pts: Pt[], eps: number): Pt[] {
  if (pts.length < 4) return pts.slice()
  const eps2 = eps * eps
  let i1 = 0, maxD = -1
  for (let i = 1; i < pts.length; i++) {
    const dx = pts[i]!.x - pts[0]!.x, dy = pts[i]!.y - pts[0]!.y
    const d = dx * dx + dy * dy
    if (d > maxD) { maxD = d; i1 = i }
  }
  let i0 = 0
  maxD = -1
  for (let i = 0; i < pts.length; i++) {
    if (i === i1) continue
    const dx = pts[i]!.x - pts[i1]!.x, dy = pts[i]!.y - pts[i1]!.y
    const d = dx * dx + dy * dy
    if (d > maxD) { maxD = d; i0 = i }
  }
  if (i0 > i1) { const t = i0; i0 = i1; i1 = t }
  const s1 = pts.slice(i0, i1 + 1)
  const s2 = pts.slice(i1).concat(pts.slice(0, i0 + 1))
  const r1 = dpOpen(s1, eps2)
  const r2 = dpOpen(s2, eps2)
  return r1.concat(r2.slice(1, -1))
}

function pointInPolygon(p: Pt, poly: Pt[]): boolean {
  let inside = false
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const xi = poly[i]!.x, yi = poly[i]!.y
    const xj = poly[j]!.x, yj = poly[j]!.y
    const hit = ((yi > p.y) !== (yj > p.y)) &&
      (p.x < ((xj - xi) * (p.y - yi)) / (yj - yi) + xi)
    if (hit) inside = !inside
  }
  return inside
}

// 在 store 上採樣後的 passableMask 上做 Bresenham 可視度檢查，用於把 axisAlignPath
// 打進 pathNodes 的階梯形節點重新簡化回最長直線段。
function linePassableOnMask(x0: number, y0: number, x1: number, y1: number): boolean {
  const mask = mapStore.passableMask
  if (!mask) return false
  const w = mapStore.maskWidth
  const h = mapStore.maskHeight
  const up = mapStore.upscaleFactor || 1
  let cx = Math.round(x0 * up), cy = Math.round(y0 * up)
  const tx = Math.round(x1 * up), ty = Math.round(y1 * up)
  const dx = Math.abs(tx - cx), dy = Math.abs(ty - cy)
  const sx = cx < tx ? 1 : -1, sy = cy < ty ? 1 : -1
  let err = dx - dy
  while (true) {
    if (cx < 0 || cx >= w || cy < 0 || cy >= h) return false
    if (mask[cy * w + cx] === 0) return false
    if (cx === tx && cy === ty) break
    const e2 = err * 2
    if (e2 > -dy) { err -= dy; cx += sx }
    if (e2 <  dx) { err += dx; cy += sy }
  }
  return true
}

function straightenPathNodes(nodes: Array<{ x: number; y: number }>): Array<{ x: number; y: number }> {
  if (nodes.length <= 2) return nodes.slice()
  const out: Array<{ x: number; y: number }> = [nodes[0]!]
  let cur = 0
  while (cur < nodes.length - 1) {
    let far = cur + 1
    for (let j = nodes.length - 1; j > cur + 1; j--) {
      if (linePassableOnMask(nodes[cur]!.x, nodes[cur]!.y, nodes[j]!.x, nodes[j]!.y)) {
        far = j
        break
      }
    }
    out.push(nodes[far]!)
    cur = far
  }
  return out
}

function pixelToWorld(px: number, py: number): { x: number; z: number } {
  const cx = px * pxToColl
  const cy = py * pxToColl
  return {
    x: (cx + 0.5) * sceneCellSize - sceneHalfW,
    z: (cy + 0.5) * sceneCellSize - sceneHalfH,
  }
}

// 與牆體相鄰的貼牆緩衝；保留比 camera.near 大的距離避免近裁面切穿牆面。
const COLLISION_MARGIN = 0.18
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
  if (fx < COLLISION_MARGIN       && !isCollCellPassable(cx - 1, cy)) return false
  if (fx > 1 - COLLISION_MARGIN   && !isCollCellPassable(cx + 1, cy)) return false
  if (fy < COLLISION_MARGIN       && !isCollCellPassable(cx, cy - 1)) return false
  if (fy > 1 - COLLISION_MARGIN   && !isCollCellPassable(cx, cy + 1)) return false
  return true
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

  // 視覺網格與世界座標共用同一套 cellSize，並用於碰撞檢查。
  const dispCell   = MAP_EXTENT / Math.max(width, height)
  const wallHeight = dispCell * WALL_HEIGHT_RATIO
  const halfW      = (width  * dispCell) / 2
  const halfH      = (height * dispCell) / 2

  sceneCellSize   = dispCell
  sceneHalfW      = halfW
  sceneHalfH      = halfH
  sceneUp         = mapStore.upscaleFactor || 1
  sceneWallHeight = wallHeight
  sceneEyeHeight  = wallHeight * 0.75

  collisionMask = data
  collisionW    = width
  collisionH    = height
  pxToColl      = (sceneUp * width) / mapStore.maskWidth

  // Grid-corner 座標轉世界 XZ。
  const toWX = (gx: number) => gx * dispCell - halfW
  const toWZ = (gy: number) => gy * dispCell - halfH

  // DP eps 單位為 cell 寬。真正的牆角 perpDist 遠大於數格永遠保留；小於 1 格的離散雜訊
  // （MS 量化誤差、1 像素缺口等）會被吞掉，避免一段直牆被切成多個微角度不同的小段、
  // 讓 Gouraud 著色呈現肉眼可見的亮度凹凸。
  const DP_EPS = 1.0
  const rawContours = traceContours(data, width, height)
  const simplified = rawContours
    .map((c) => dpClosed(c, DP_EPS))
    .filter((c) => c.length >= 3)

  // 有向面積分類：正 = passable 區外輪廓，負 = passable 區內的障礙物（洞）。
  const outers: Pt[][] = []
  const holes:  Pt[][] = []
  for (const c of simplified) {
    if (signedArea(c) > 0) outers.push(c)
    else                   holes.push(c)
  }

  if (outers.length > 0) {
    const floorMat = new THREE.MeshStandardMaterial({
      color: 0x7fb8ff, roughness: 0.85, metalness: 0.05,
      side: THREE.DoubleSide,
    })
    const shapes: THREE.Shape[] = []
    for (const outer of outers) {
      const shape = new THREE.Shape(
        outer.map((p) => new THREE.Vector2(toWX(p.x), toWZ(p.y))),
      )
      for (const hole of holes) {
        if (pointInPolygon(hole[0]!, outer)) {
          shape.holes.push(new THREE.Path(
            hole.map((p) => new THREE.Vector2(toWX(p.x), toWZ(p.y))),
          ))
        }
      }
      shapes.push(shape)
    }
    const floorGeo = new THREE.ShapeGeometry(shapes)
    floorGeo.rotateX(Math.PI / 2)
    mapGroup.add(new THREE.Mesh(floorGeo, floorMat))
  }

  // 牆面：沿 outer 與 hole 走一圈，每個輪廓點共用給相鄰兩段；法線取兩段段法線平均，
  // 讓平滑段（斜線）連續著色、真正的直角（段法線夾角大）則退化為獨立頂點保留硬邊。
  const wallPolys: Pt[][] = [...outers, ...holes]
  if (wallPolys.length > 0) {
    const SHARP_DOT = 0.5   // 相鄰段法線 dot 低於此值視為硬邊（約 60 度以上轉折）
    const positions: number[] = []
    const normals:   number[] = []
    const indices:   number[] = []
    let vbase = 0
    for (const poly of wallPolys) {
      const n = poly.length
      if (n < 2) continue
      const sx = new Float32Array(n)
      const sz = new Float32Array(n)
      const snx = new Float32Array(n)
      const snz = new Float32Array(n)
      for (let i = 0; i < n; i++) {
        const a = poly[i]!
        const b = poly[(i + 1) % n]!
        const ax = toWX(a.x), az = toWZ(a.y)
        const bx = toWX(b.x), bz = toWZ(b.y)
        sx[i] = ax; sz[i] = az
        const dx = bx - ax, dz = bz - az
        const L = Math.hypot(dx, dz) || 1
        snx[i] = -dz / L
        snz[i] =  dx / L
      }
      // 每個輪廓點視轉折角度決定 1 組頂點（平滑）或 2 組頂點（硬邊）。
      const startBot: number[] = new Array(n)   // 指向該點「作為下一段起點」時的 bottom vertex index
      const endBot:   number[] = new Array(n)   // 指向該點「作為上一段終點」時的 bottom vertex index
      let cur = vbase
      for (let i = 0; i < n; i++) {
        const prev = (i - 1 + n) % n
        const dot = snx[prev]! * snx[i]! + snz[prev]! * snz[i]!
        const x = sx[i]!, z = sz[i]!
        if (dot >= SHARP_DOT) {
          // 平滑：單一共用頂點，法線取兩段平均並正規化。
          let nx = snx[prev]! + snx[i]!
          let nz = snz[prev]! + snz[i]!
          const L = Math.hypot(nx, nz) || 1
          nx /= L; nz /= L
          positions.push(x, 0, z, x, wallHeight, z)
          normals.push(nx, 0, nz, nx, 0, nz)
          endBot[i] = cur
          startBot[i] = cur
          cur += 2
        } else {
          // 硬邊：兩組頂點位置相同、法線分別取自上一段與下一段。
          positions.push(x, 0, z, x, wallHeight, z)
          normals.push(snx[prev]!, 0, snz[prev]!, snx[prev]!, 0, snz[prev]!)
          endBot[i] = cur
          cur += 2
          positions.push(x, 0, z, x, wallHeight, z)
          normals.push(snx[i]!, 0, snz[i]!, snx[i]!, 0, snz[i]!)
          startBot[i] = cur
          cur += 2
        }
      }
      for (let i = 0; i < n; i++) {
        const j = (i + 1) % n
        const a0 = startBot[i]!,   a1 = a0 + 1
        const b0 = endBot[j]!,     b1 = b0 + 1
        indices.push(a0, b0, b1, a0, b1, a1)
      }
      vbase = cur
    }
    const wallGeo = new THREE.BufferGeometry()
    wallGeo.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3))
    wallGeo.setAttribute('normal',   new THREE.Float32BufferAttribute(normals, 3))
    wallGeo.setIndex(indices)
    const wallMat = new THREE.MeshStandardMaterial({
      color: 0xf0f3f7, roughness: 0.6, metalness: 0.05,
      side: THREE.DoubleSide,
    })
    mapGroup.add(new THREE.Mesh(wallGeo, wallMat))
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
    color: 0x00ffaa, emissive: 0x00ffaa, emissiveIntensity: 0.9,
    roughness: 0.3, metalness: 0.2,
  })

  // store 的 runAStar 會在 straightenPath 之後再跑 axisAlignPath，把斜線改寫為軸向 L 形，
  // 所以 pathNodes 到這裡其實是階梯狀。這裡用 Bresenham 再做一次最長可視線段簡化，
  // 恢復為直線捷徑，視覺上與平滑牆面一致；僅限 3D 場景，不動 HomeView 的 2D 路徑。
  const straight = straightenPathNodes(mapStore.pathNodes as Array<{ x: number; y: number }>)
  const points = straight.map((p) => {
    const w = pixelToWorld(p.x, p.y)
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
    color: 0x00ff66, emissive: 0x00ff66, emissiveIntensity: 0.8,
  })
  const endMat = new THREE.MeshStandardMaterial({
    color: 0xff4466, emissive: 0xff4466, emissiveIntensity: 0.8,
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
      color: 0xffcc00, emissive: 0xffaa00, emissiveIntensity: 0.6,
    }),
  )
  body.rotation.x = Math.PI / 2
  body.position.y = sceneCellSize * 1.1
  avatarGroup.add(body)
}

function ensureUserState() {
  if (!mapStore.startPoint) return
  if (!mapStore.userPosition) {
    mapStore.userPosition = { ...mapStore.startPoint }
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
  px: number, py: number, ax: number, ay: number, bx: number, by: number,
): number {
  const abx = bx - ax, aby = by - ay
  const apx = px - ax, apy = py - ay
  const abLen2 = abx * abx + aby * aby
  let t = abLen2 === 0 ? 0 : (apx * abx + apy * aby) / abLen2
  t = Math.max(0, Math.min(1, t))
  const cx = ax + t * abx, cy = ay + t * aby
  const dx = px - cx, dy = py - cy
  return Math.sqrt(dx * dx + dy * dy)
}

function minDistToPath(px: number, py: number): number {
  const nodes = mapStore.pathNodes
  if (!nodes || nodes.length < 2) return Infinity
  let best = Infinity
  for (let i = 0; i < nodes.length - 1; i++) {
    const d = distPointToSegment(
      px, py, nodes[i]!.x, nodes[i]!.y, nodes[i+1]!.x, nodes[i+1]!.y,
    )
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
    const step = moveInput.fwd * MOVE_SPEED_PX * dtSec
    const ox = mapStore.userPosition.x
    const oy = mapStore.userPosition.y
    const nx = ox + Math.cos(h) * step
    const ny = oy + Math.sin(h) * step
    // 先嘗試合成對角，再退化為單軸滑行，避免兩軸各自通過卻合成踩進牆角的漏洞。
    let fx = ox
    let fy = oy
    if (isPassableAtPixel(nx, ny)) {
      fx = nx; fy = ny
    } else if (isPassableAtPixel(nx, oy)) {
      fx = nx
    } else if (isPassableAtPixel(ox, ny)) {
      fy = ny
    }
    mapStore.userPosition = { x: fx, y: fy }
  }
}

function updateOffPath() {
  if (!mapStore.userPosition) { offPath.value = false; return }
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
    case 'w': case 'W': case 'ArrowUp':    moveInput.fwd =  v; break
    case 's': case 'S': case 'ArrowDown':  moveInput.fwd = -v; break
    case 'a': case 'A': case 'ArrowLeft':  moveInput.rot = -v; break
    case 'd': case 'D': case 'ArrowRight': moveInput.rot =  v; break
    default: return
  }
  e.preventDefault()
}
const onKeyDown = (e: KeyboardEvent) => onKey(true, e)
const onKeyUp   = (e: KeyboardEvent) => onKey(false, e)

function holdFwd(v: number)   { moveInput.fwd = v }
function holdRot(v: number)   { moveInput.rot = v }
function releaseFwd()         { moveInput.fwd = 0 }
function releaseRot()         { moveInput.rot = 0 }

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
    <div ref="container" class="scene-canvas" />

    <div v-if="!hasMask" class="hint">
      尚未產生可通行遮罩，請先到「首頁」上傳地圖並執行一次路徑識別。
    </div>

    <div v-if="hasMask" class="top-bar">
      <button
        class="mode-btn"
        :class="{ active: viewMode === 'overview' }"
        @click="exitFirstPerson"
      >俯瞰</button>
      <button
        class="mode-btn"
        :class="{ active: viewMode === 'first-person' }"
        @click="enterFirstPerson"
      >第一人稱</button>
    </div>

    <div v-if="offPath && viewMode === 'first-person'" class="offpath-banner">
      偵測到偏離路徑
      <button class="replan-btn" @click="replanFromCurrent">重新規劃</button>
    </div>

    <div v-if="viewMode === 'first-person' && hasMask" class="controls">
      <div class="pad">
        <button
          class="pad-btn up"
          @pointerdown.prevent="holdFwd(1)"
          @pointerup="releaseFwd"
          @pointerleave="releaseFwd"
          @pointercancel="releaseFwd"
        >前進</button>
        <button
          class="pad-btn left"
          @pointerdown.prevent="holdRot(-1)"
          @pointerup="releaseRot"
          @pointerleave="releaseRot"
          @pointercancel="releaseRot"
        >左轉</button>
        <button
          class="pad-btn right"
          @pointerdown.prevent="holdRot(1)"
          @pointerup="releaseRot"
          @pointerleave="releaseRot"
          @pointercancel="releaseRot"
        >右轉</button>
        <button
          class="pad-btn down"
          @pointerdown.prevent="holdFwd(-1)"
          @pointerup="releaseFwd"
          @pointerleave="releaseFwd"
          @pointercancel="releaseFwd"
        >後退</button>
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

.top-bar {
  position: absolute;
  top: 16px;
  left: 16px;
  display: flex;
  gap: 8px;
}
.mode-btn {
  padding: 8px 14px;
  border: none;
  border-radius: 6px;
  background: rgba(22, 33, 62, 0.85);
  color: #e0e0e0;
  cursor: pointer;
  font-size: 14px;
}
.mode-btn.active {
  background: #00c8ff;
  color: #1a1a2e;
  font-weight: bold;
}

.offpath-banner {
  position: absolute;
  top: 16px;
  left: 50%;
  transform: translateX(-50%);
  padding: 10px 16px;
  background: rgba(180, 40, 60, 0.9);
  color: #fff;
  border-radius: 6px;
  font-size: 14px;
  display: flex;
  align-items: center;
  gap: 10px;
}
.replan-btn {
  padding: 6px 12px;
  border: none;
  border-radius: 4px;
  background: #fff;
  color: #b4283c;
  cursor: pointer;
  font-weight: bold;
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
  border-radius: 10px;
  background: rgba(22, 33, 62, 0.85);
  color: #e0e0e0;
  font-size: 14px;
  cursor: pointer;
  touch-action: none;
}
.pad-btn:active { background: #00c8ff; color: #1a1a2e; }
.pad-btn.up    { grid-column: 2; grid-row: 1; }
.pad-btn.left  { grid-column: 1; grid-row: 2; }
.pad-btn.right { grid-column: 3; grid-row: 2; }
.pad-btn.down  { grid-column: 2; grid-row: 3; }
</style>
