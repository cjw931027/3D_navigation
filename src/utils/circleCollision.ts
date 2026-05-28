// 圓形玩家碰撞：圓周不得與任何牆 cell（含界外）重疊。
// 座標單位為網格 cell（連續實數，整數為 cell 邊緣）。

export interface GridMask {
  data: ArrayLike<number>
  width: number
  height: number
}

export function isCellPassable(mask: GridMask, cx: number, cy: number): boolean {
  if (cx < 0 || cx >= mask.width || cy < 0 || cy >= mask.height) return false
  return mask.data[cy * mask.width + cx] === 1
}

// 用嚴格小於並扣 epsilon，使「距離 = R 的相切」算可走，避免浮點誤判
const OVERLAP_EPS = 1e-6
function circleOverlapsCell(cu: number, cv: number, r: number, cx: number, cy: number): boolean {
  const nx = Math.max(cx, Math.min(cu, cx + 1))
  const ny = Math.max(cy, Math.min(cv, cy + 1))
  const dx = cu - nx
  const dy = cv - ny
  return dx * dx + dy * dy < r * r - OVERLAP_EPS
}

export function canStandAt(mask: GridMask, cu: number, cv: number, r: number): boolean {
  const minCx = Math.floor(cu - r)
  const maxCx = Math.floor(cu + r)
  const minCy = Math.floor(cv - r)
  const maxCy = Math.floor(cv + r)
  for (let cy = minCy; cy <= maxCy; cy++) {
    for (let cx = minCx; cx <= maxCx; cx++) {
      if (isCellPassable(mask, cx, cy)) continue
      if (circleOverlapsCell(cu, cv, r, cx, cy)) return false
    }
  }
  return true
}

export interface StepResult {
  cu: number
  cv: number
  blocked: boolean
}

// 二分逼近：(cu, cv) 為合法起點，沿 (du, dv) 推到最遠仍合法的位置
function advanceUntilBlocked(
  mask: GridMask,
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
    if (canStandAt(mask, cu + du * mid, cv + dv * mid, r)) lo = mid
    else hi = mid
  }
  return { cu: cu + du * lo, cv: cv + dv * lo }
}

// 對角優先；若對角受阻則嘗試只走 X、只走 Y（沿牆滑行）。
// blocked = true 代表所有方向都被擋；呼叫端通常結束本幀位移。
export function stepCircle(
  mask: GridMask,
  cu: number,
  cv: number,
  dcu: number,
  dcv: number,
  r: number,
): StepResult {
  const nu = cu + dcu
  const nv = cv + dcv
  if (canStandAt(mask, nu, nv, r)) {
    return { cu: nu, cv: nv, blocked: false }
  }
  const refined = advanceUntilBlocked(mask, cu, cv, dcu, dcv, r)
  if (dcu !== 0 && canStandAt(mask, nu, cv, r)) {
    return { cu: nu, cv: cv, blocked: true }
  }
  if (dcv !== 0 && canStandAt(mask, cu, nv, r)) {
    return { cu: cu, cv: nv, blocked: true }
  }
  return { cu: refined.cu, cv: refined.cv, blocked: true }
}

// subStepLen ≤ 0.3 避免 tunneling。任一 substep blocked 即停止後續。
export function moveCircle(
  mask: GridMask,
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
  let fu = cu
  let fv = cv
  for (let i = 0; i < n; i++) {
    const res = stepCircle(mask, fu, fv, stepU, stepV, r)
    fu = res.cu
    fv = res.cv
    if (res.blocked) return { cu: fu, cv: fv, stoppedEarly: true }
  }
  return { cu: fu, cv: fv, stoppedEarly: false }
}
