// polyCollision.ts — 圓 vs 多邊形線段碰撞（純函式，可離線測試）。讓碰撞直接吃輪廓線段，
// 碰撞邊界 = 視覺牆邊界、不穿牆。座標同 pxToColl(ds-cell)；「可站」= 圓心到所有線段距離 ≥ R。

export interface Seg {
  ax: number
  ay: number
  bx: number
  by: number
}

const OVERLAP_EPS = 1e-6

// ── 空間網格加速結構：把線段依其包圍盒登記到 bucket，查詢時只取附近 bucket 的線段 ──
export interface SegGrid {
  segs: Seg[]
  cell: number // bucket 邊長（cell 單位）
  cols: number
  rows: number
  minX: number
  minY: number
  buckets: number[][] // 每個 bucket 存 seg index 陣列
}

export function buildSegGrid(segs: Seg[], cell = 4): SegGrid {
  let minX = Infinity
  let minY = Infinity
  let maxX = -Infinity
  let maxY = -Infinity
  for (const s of segs) {
    minX = Math.min(minX, s.ax, s.bx)
    minY = Math.min(minY, s.ay, s.by)
    maxX = Math.max(maxX, s.ax, s.bx)
    maxY = Math.max(maxY, s.ay, s.by)
  }
  if (!isFinite(minX)) {
    minX = 0
    minY = 0
    maxX = 0
    maxY = 0
  }
  const cols = Math.max(1, Math.ceil((maxX - minX) / cell) + 1)
  const rows = Math.max(1, Math.ceil((maxY - minY) / cell) + 1)
  const buckets: number[][] = new Array(cols * rows)
  for (let i = 0; i < buckets.length; i++) buckets[i] = []
  const clampCol = (c: number) => Math.max(0, Math.min(cols - 1, c))
  const clampRow = (r: number) => Math.max(0, Math.min(rows - 1, r))
  for (let i = 0; i < segs.length; i++) {
    const s = segs[i]!
    const c0 = clampCol(Math.floor((Math.min(s.ax, s.bx) - minX) / cell))
    const c1 = clampCol(Math.floor((Math.max(s.ax, s.bx) - minX) / cell))
    const r0 = clampRow(Math.floor((Math.min(s.ay, s.by) - minY) / cell))
    const r1 = clampRow(Math.floor((Math.max(s.ay, s.by) - minY) / cell))
    for (let r = r0; r <= r1; r++)
      for (let c = c0; c <= c1; c++) buckets[r * cols + c]!.push(i)
  }
  return { segs, cell, cols, rows, minX, minY, buckets }
}

// 圓心 (cu,cv) 半徑 R 附近的候選線段 index（去重）。R 較大時擴一圈 bucket。
function nearbySegIndices(grid: SegGrid, cu: number, cv: number, r: number): number[] {
  const reach = r + grid.cell
  const c0 = Math.max(0, Math.floor((cu - reach - grid.minX) / grid.cell))
  const c1 = Math.min(grid.cols - 1, Math.floor((cu + reach - grid.minX) / grid.cell))
  const r0 = Math.max(0, Math.floor((cv - reach - grid.minY) / grid.cell))
  const r1 = Math.min(grid.rows - 1, Math.floor((cv + reach - grid.minY) / grid.cell))
  const seen = new Set<number>()
  const out: number[] = []
  for (let rr = r0; rr <= r1; rr++)
    for (let cc = c0; cc <= c1; cc++) {
      const b = grid.buckets[rr * grid.cols + cc]!
      for (const i of b)
        if (!seen.has(i)) {
          seen.add(i)
          out.push(i)
        }
    }
  return out
}

// 點到線段最短距離平方 + 最近點（給滑行用）。
function distSqToSeg(
  px: number,
  py: number,
  s: Seg,
): { d2: number; nx: number; ny: number } {
  const abx = s.bx - s.ax
  const aby = s.by - s.ay
  const apx = px - s.ax
  const apy = py - s.ay
  const ab2 = abx * abx + aby * aby
  let t = ab2 === 0 ? 0 : (apx * abx + apy * aby) / ab2
  if (t < 0) t = 0
  else if (t > 1) t = 1
  const nx = s.ax + t * abx
  const ny = s.ay + t * aby
  const dx = px - nx
  const dy = py - ny
  return { d2: dx * dx + dy * dy, nx, ny }
}

// 圓心是否可站：到所有附近線段的距離皆 ≥ R。
export function canStandAtPoly(grid: SegGrid, cu: number, cv: number, r: number): boolean {
  const r2 = r * r - OVERLAP_EPS
  const idxs = nearbySegIndices(grid, cu, cv, r)
  for (const i of idxs) {
    const { d2 } = distSqToSeg(cu, cv, grid.segs[i]!)
    if (d2 < r2) return false
  }
  return true
}

export interface StepResult {
  cu: number
  cv: number
  blocked: boolean
}

// 二分逼近：(cu,cv) 合法起點，沿 (du,dv) 推到最遠仍合法處（最多 8 次切半）。
function advanceUntilBlocked(
  grid: SegGrid,
  cu: number,
  cv: number,
  du: number,
  dv: number,
  r: number,
): { cu: number; cv: number } {
  let lo = 0
  let hi = 1
  for (let i = 0; i < 8; i++) {
    const mid = (lo + hi) / 2
    if (canStandAtPoly(grid, cu + du * mid, cv + dv * mid, r)) lo = mid
    else hi = mid
  }
  return { cu: cu + du * lo, cv: cv + dv * lo }
}

// 「皮膚」厚度：把偵測半徑略放大成 R + SKIN，使「剛好貼牆休息在距牆 R」的線段也算受阻，
// 才能在後續 substep 持續抓到該牆的法線做滑行（否則 strict < R 會漏掉貼牆面 → 無法滑行）。
const SKIN = 1e-3

// 收集「目前位置 (R+SKIN) 範圍內、會擋住的線段」之單位外推法線（由線段最近點指向圓心）。
// 用 R+SKIN 而非 R：包含「正貼著牆面」的線段，確保沿牆滑行時法線抓得到。
function blockingNormals(
  grid: SegGrid,
  cu: number,
  cv: number,
  r: number,
): Array<[number, number]> {
  const reach = r + SKIN
  const reach2 = reach * reach
  const out: Array<[number, number]> = []
  for (const i of nearbySegIndices(grid, cu, cv, reach)) {
    const { d2, nx, ny } = distSqToSeg(cu, cv, grid.segs[i]!)
    if (d2 < reach2) {
      let dx = cu - nx
      let dy = cv - ny
      const m = Math.hypot(dx, dy)
      if (m > 1e-9) {
        out.push([dx / m, dy / m])
      }
    }
  }
  return out
}

// 把位移向量對所有受阻法線做「剔除壓入牆分量」投影（內角=多法線逐一剔除）。
// 反覆 2 趟讓多法線收斂（剔第一條後可能又壓入第二條）。
function slideVector(
  du: number,
  dv: number,
  normals: Array<[number, number]>,
): [number, number] {
  let vx = du
  let vy = dv
  for (let pass = 0; pass < 2; pass++) {
    for (const [nx, ny] of normals) {
      const dot = vx * nx + vy * ny
      if (dot < 0) {
        vx -= dot * nx
        vy -= dot * ny
      }
    }
  }
  return [vx, vy]
}

// push-out：若位置因浮點誤差微微重疊線段（距離 < R），沿外推法線推回到剛好 R，避免反覆觸發受阻抖動。
function pushOut(grid: SegGrid, cu: number, cv: number, r: number): { cu: number; cv: number } {
  let x = cu
  let y = cv
  for (let iter = 0; iter < 4; iter++) {
    let moved = false
    for (const i of nearbySegIndices(grid, x, y, r)) {
      const { d2, nx, ny } = distSqToSeg(x, y, grid.segs[i]!)
      if (d2 < (r - SKIN) * (r - SKIN)) {
        const d = Math.sqrt(d2)
        let dx = x - nx
        let dy = y - ny
        const m = d > 1e-9 ? d : 1
        dx /= m
        dy /= m
        const push = r - d + SKIN
        x += dx * push
        y += dy * push
        moved = true
      }
    }
    if (!moved) break
  }
  return { cu: x, cv: y }
}

// 單步推進：完整位移可走就走；否則沿牆切線滑行（投影剔除法線 + 死角偵測 + push-out）。
// 回傳 blocked=true 代表「這步撞到牆並做了滑行/停住」；呼叫端據此決定是否繼續用滑行方向走完本幀。
export function stepCirclePoly(
  grid: SegGrid,
  cu: number,
  cv: number,
  dcu: number,
  dcv: number,
  r: number,
): StepResult {
  const nu = cu + dcu
  const nv = cv + dcv
  if (canStandAtPoly(grid, nu, nv, r)) return { cu: nu, cv: nv, blocked: false }

  // 受阻：在「起點」取受阻法線（含 R+SKIN 範圍，故貼牆休息也抓得到）
  const normals = blockingNormals(grid, cu, cv, r)
  let [sx, sy] = slideVector(dcu, dcv, normals)

  // 死角：切線分量幾乎為零（兩面牆夾死）→ 停住
  if (sx * sx + sy * sy < 1e-12) {
    return { cu, cv, blocked: true }
  }

  // 切線位移可整段走 → 直接滑行
  if (canStandAtPoly(grid, cu + sx, cv + sy, r)) {
    return { cu: cu + sx, cv: cv + sy, blocked: true }
  }
  // 否則二分逼近沿切線能走多遠，再 push-out 修正浮點重疊
  const slid = advanceUntilBlocked(grid, cu, cv, sx, sy, r)
  const fixed = pushOut(grid, slid.cu, slid.cv, r)
  return { cu: fixed.cu, cv: fixed.cv, blocked: true }
}

// 切 substep 逐次套用 stepCirclePoly：撞牆仍有前進就繼續滑行，只有完全卡死（死角）才提前結束。
export function moveCirclePoly(
  grid: SegGrid,
  cu: number,
  cv: number,
  totalDu: number,
  totalDv: number,
  r: number,
  subStepLen = 0.3,
): { cu: number; cv: number; stoppedEarly: boolean } {
  const total = Math.hypot(totalDu, totalDv)
  const n = Math.max(1, Math.ceil(total / subStepLen))
  const stepU = totalDu / n
  const stepV = totalDv / n
  const PROGRESS_EPS2 = 1e-10
  let fu = cu
  let fv = cv
  let stoppedEarly = false
  for (let i = 0; i < n; i++) {
    const beforeU = fu
    const beforeV = fv
    const res = stepCirclePoly(grid, fu, fv, stepU, stepV, r)
    fu = res.cu
    fv = res.cv
    const moved2 = (fu - beforeU) * (fu - beforeU) + (fv - beforeV) * (fv - beforeV)
    if (moved2 < PROGRESS_EPS2) {
      // 這一 substep 完全卡死（死角）→ 結束本幀
      stoppedEarly = true
      break
    }
    // 有前進（不論是否 blocked/滑行）→ 繼續處理剩餘 substep
    if (res.blocked) stoppedEarly = true
  }
  return { cu: fu, cv: fv, stoppedEarly }
}

// 把一組 ring（每個是 [x,y] 點陣列，封閉）轉成線段陣列（ds-cell 單位）。
export function ringsToSegments(rings: Array<Array<[number, number]>>): Seg[] {
  const segs: Seg[] = []
  for (const ring of rings) {
    const n = ring.length
    if (n < 2) continue
    for (let i = 0; i < n; i++) {
      const a = ring[i]!
      const b = ring[(i + 1) % n]!
      if (a[0] === b[0] && a[1] === b[1]) continue
      segs.push({ ax: a[0], ay: a[1], bx: b[0], by: b[1] })
    }
  }
  return segs
}
