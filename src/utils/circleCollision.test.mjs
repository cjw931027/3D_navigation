// 執行：node src/utils/circleCollision.test.mjs
// 內嵌 circleCollision.ts 的等價 JS 版以免打包依賴；修改 .ts 後請同步此檔。

function isCellPassable(mask, cx, cy) {
  if (cx < 0 || cx >= mask.width || cy < 0 || cy >= mask.height) return false
  return mask.data[cy * mask.width + cx] === 1
}
const OVERLAP_EPS = 1e-6
function circleOverlapsCell(cu, cv, r, cx, cy) {
  const nx = Math.max(cx, Math.min(cu, cx + 1))
  const ny = Math.max(cy, Math.min(cv, cy + 1))
  const dx = cu - nx, dy = cv - ny
  return dx * dx + dy * dy < r * r - OVERLAP_EPS
}
function _canStandAt(mask, cu, cv, r) {
  const minCx = Math.floor(cu - r), maxCx = Math.floor(cu + r)
  const minCy = Math.floor(cv - r), maxCy = Math.floor(cv + r)
  for (let cy = minCy; cy <= maxCy; cy++)
    for (let cx = minCx; cx <= maxCx; cx++) {
      if (isCellPassable(mask, cx, cy)) continue
      if (circleOverlapsCell(cu, cv, r, cx, cy)) return false
    }
  return true
}
function _advanceUntilBlocked(mask, cu, cv, du, dv, r) {
  let lo = 0, hi = 1
  for (let i = 0; i < 8; i++) {
    const mid = (lo + hi) / 2
    if (_canStandAt(mask, cu + du * mid, cv + dv * mid, r)) lo = mid; else hi = mid
  }
  return { cu: cu + du * lo, cv: cv + dv * lo }
}
function _stepCircle(mask, cu, cv, dcu, dcv, r) {
  const nu = cu + dcu, nv = cv + dcv
  if (_canStandAt(mask, nu, nv, r)) return { cu: nu, cv: nv, blocked: false }
  const refined = _advanceUntilBlocked(mask, cu, cv, dcu, dcv, r)
  if (dcu !== 0 && _canStandAt(mask, nu, cv, r)) return { cu: nu, cv, blocked: true }
  if (dcv !== 0 && _canStandAt(mask, cu, nv, r)) return { cu, cv: nv, blocked: true }
  return { cu: refined.cu, cv: refined.cv, blocked: true }
}
function _moveCircle(mask, cu, cv, du, dv, r, subStepLen = 0.3) {
  const total = Math.hypot(du, dv)
  const n = Math.max(1, Math.ceil(total / subStepLen))
  const su = du / n, sv = dv / n
  let fu = cu, fv = cv
  for (let i = 0; i < n; i++) {
    const res = _stepCircle(mask, fu, fv, su, sv, r)
    fu = res.cu; fv = res.cv
    if (res.blocked) return { cu: fu, cv: fv, stoppedEarly: true }
  }
  return { cu: fu, cv: fv, stoppedEarly: false }
}

const R = 0.35
let passed = 0, failed = 0
function assert(cond, label) {
  if (cond) { passed++; console.log(`  PASS ${label}`) }
  else { failed++; console.log(`  FAIL ${label}`) }
}
function near(actual, expected, eps, label) {
  const ok = Math.abs(actual - expected) <= eps
  if (ok) { passed++; console.log(`  PASS ${label} (${actual.toFixed(4)} ≈ ${expected})`) }
  else { failed++; console.log(`  FAIL ${label}: expected ≈ ${expected} ±${eps}, got ${actual}`) }
}
function makeMask(rows) {
  const h = rows.length, w = rows[0].length
  const data = new Uint8Array(w * h)
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++)
      data[y * w + x] = rows[y][x] === '.' ? 1 : 0
  return { data, width: w, height: h }
}

console.log('\n== Test 1: 單面牆 ==')
{
  const rows = []
  for (let y = 0; y < 5; y++) {
    let row = ''
    for (let x = 0; x < 10; x++) row += (x >= 5) ? '#' : '.'
    rows.push(row)
  }
  const mask = makeMask(rows)
  const res = _moveCircle(mask, 1, 2.5, 8, 0, R)
  near(res.cu, 5 - R, 0.005, 'stop near wall distance R')
  assert(res.stoppedEarly, 'movement stopped early at wall')
  assert(_canStandAt(mask, res.cu, res.cv, R), 'circle does not overlap wall')
  const res2 = _moveCircle(mask, res.cu, res.cv, 5, 0, R)
  near(res2.cu, res.cu, 0.005, 'cannot push past wall')
}

console.log('\n== Test 2: 內角 ==')
{
  const rows = []
  for (let y = 0; y < 10; y++) {
    let row = ''
    for (let x = 0; x < 10; x++) row += (x >= 5 || y >= 5) ? '#' : '.'
    rows.push(row)
  }
  const mask = makeMask(rows)
  const res = _moveCircle(mask, 2, 2, 8, 8, R)
  assert(res.cu <= 5 - R + 1e-9, 'x stops at corner wall')
  assert(res.cv <= 5 - R + 1e-9, 'y stops at corner wall')
  assert(_canStandAt(mask, res.cu, res.cv, R), 'no overlap at inner corner')
}

console.log('\n== Test 3: 沿牆滑行 ==')
{
  const rows = []
  for (let y = 0; y < 10; y++) {
    let row = ''
    for (let x = 0; x < 10; x++) row += (x >= 5) ? '#' : '.'
    rows.push(row)
  }
  const mask = makeMask(rows)
  const res = _moveCircle(mask, 5 - R, 2, 2, 2, R)
  near(res.cu, 5 - R, 0.005, 'x stays near wall')
  assert(res.cv > 2 + 0.1, 'y advances along wall')
  assert(_canStandAt(mask, res.cu, res.cv, R), 'no overlap while sliding')
}

console.log('\n== Test 4: 1-cell 走廊 ==')
{
  const rows = []
  for (let y = 0; y < 9; y++) {
    let row = ''
    for (let x = 0; x < 20; x++) row += (y === 4) ? '.' : '#'
    rows.push(row)
  }
  const mask = makeMask(rows)
  const res = _moveCircle(mask, 1, 4.5, 18, 0, R)
  near(res.cu, 19, 1e-6, 'reaches end of 1-cell corridor')
  assert(!res.stoppedEarly, 'corridor did not lock player')
}

console.log('\n== Test 5: 走廊內偏離中央 ==')
{
  const rows = []
  for (let y = 0; y < 9; y++) {
    let row = ''
    for (let x = 0; x < 5; x++) row += (y === 4) ? '.' : '#'
    rows.push(row)
  }
  const mask = makeMask(rows)
  assert(_canStandAt(mask, 1, 4 + R, R), 'can stand glued to top wall of corridor')
  const res = _moveCircle(mask, 1, 4 + R, 0, -1, R)
  near(res.cv, 4 + R, 1e-6, 'cannot move into top wall')
}

console.log('\n== Test 6: 對角縫不應穿透 ==')
{
  // (0,0) 與 (1,1) 可走、(1,0) 與 (0,1) 是牆
  const mask = makeMask(['.#', '#.'])
  const res = _moveCircle(mask, 1.5, 1.5, -1, -1, R)
  assert(_canStandAt(mask, res.cu, res.cv, R), 'no overlap near diagonal gap')
  assert(!(res.cu < 1 && res.cv < 1), 'did not squeeze through diagonal gap')
}

console.log('\n== Test 7: 圓緊貼牆邊 ==')
{
  const mask = makeMask(['..#', '..#', '..#'])
  assert(_canStandAt(mask, 2 - R, 1.5, R), 'circle exactly tangent to wall is allowed')
  assert(!_canStandAt(mask, 2 - R + 1e-4, 1.5, R), 'circle slightly inside wall is blocked')
}

console.log(`\n=== ${passed} passed, ${failed} failed ===`)
process.exit(failed > 0 ? 1 : 0)
