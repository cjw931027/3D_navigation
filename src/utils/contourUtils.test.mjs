// 執行：node --experimental-strip-types src/utils/contourUtils.test.mjs
// 直接 import .ts（Node 22 strip-types），不複製實作，避免 drift。
import {
  extractBoundaryEdges,
  traceRings,
  signedArea,
  classifyRings,
  simplifyRing,
  extractAndRasterize,
} from './contourUtils.ts'

let pass = 0
let fail = 0
const assert = (c, l) => {
  if (c) {
    pass++
    console.log('  PASS ' + l)
  } else {
    fail++
    console.log('  FAIL ' + l)
  }
}
const eq = (a, b, l) => assert(a === b, `${l} (got ${a}, want ${b})`)

const mask = (rows) => {
  const h = rows.length
  const w = rows[0].length
  const d = new Uint8Array(w * h)
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) d[y * w + x] = rows[y][x] === '.' ? 1 : 0
  return { d, w, h }
}
const passCount = (m) => {
  let c = 0
  for (let i = 0; i < m.length; i++) if (m[i] === 1) c++
  return c
}
const overlap = (a, b) => {
  let inter = 0
  let uni = 0
  for (let i = 0; i < a.length; i++) {
    if (a[i] === 1 || b[i] === 1) uni++
    if (a[i] === 1 && b[i] === 1) inter++
  }
  return uni === 0 ? 1 : inter / uni
}

console.log('\n== Case 1: single rectangle ==')
{
  const { d, w, h } = mask(['.....', '.....', '.....'])
  const { outers, holes } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  eq(outers.length, 1, 'one outer')
  eq(holes.length, 0, 'no holes')
  assert(signedArea(outers[0]) < 0, 'outer signedArea < 0')
}

console.log('\n== Case 2: L-shape ==')
{
  const { d, w, h } = mask(['...##', '...##', '.....', '.....'])
  const { outers, holes } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  eq(outers.length, 1, 'one outer')
  eq(holes.length, 0, 'no holes')
}

console.log('\n== Case 3: hole inside ==')
{
  const { d, w, h } = mask(['.....', '..#..', '.....'])
  const { outers, holes, holesByOuter } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  eq(outers.length, 1, 'one outer')
  eq(holes.length, 1, 'one hole')
  eq(holesByOuter.get(0).length, 1, 'hole assigned to outer 0')
}

console.log('\n== Case 4: two components ==')
{
  const { d, w, h } = mask(['..#..', '..#..', '..#..'])
  const { outers, holes } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  eq(outers.length, 2, 'two outers')
  eq(holes.length, 0, 'no holes')
}

console.log('\n== Case 5: diagonal staircase (no region eaten) ==')
{
  const { d, w, h } = mask([
    '#####....',
    '####.....',
    '###......',
    '##.......',
    '#........',
    '.........',
  ])
  const { outers } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  assert(outers.length >= 1, 'at least one outer (' + outers.length + ')')
  const r = extractAndRasterize(d, w, h, 0.9, 0.05)
  const ov = overlap(d, r.smoothedMask)
  assert(ov > 0.8, 'staircase recover overlap > 0.8 (got ' + ov.toFixed(3) + ')')
}

console.log('\n== Case 6: diagonal touch pinch ==')
{
  // (0,0) 與 (1,1) 可走、(1,0) 與 (0,1) 牆：只對角接觸，應視為 2 個獨立可走環。
  const { d, w, h } = mask(['.#', '#.'])
  const { outers } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  eq(outers.length, 2, 'diagonal-touch yields 2 separate outers')
}

console.log('\n== Case 7: rasterize recoverage (visual = collision) ==')
{
  const { d, w, h } = mask([
    '..........',
    '..####....',
    '..#..#....',
    '..####....',
    '..........',
    '....######',
    '....#....#',
    '....######',
  ])
  const r = extractAndRasterize(d, w, h, 0.9, 0.05)
  const ov = overlap(d, r.smoothedMask)
  assert(ov > 0.7, 'overlap > 0.7 (got ' + ov.toFixed(3) + ')')
  assert(r.stats.diffPercent < 40, 'diff% reasonable (got ' + r.stats.diffPercent.toFixed(1) + ')')
  assert(r.smoothedMask.length === w * h, 'mask size matches')
  assert(passCount(r.smoothedMask) > passCount(d) * 0.5, 'smoothed keeps >50% passable')
}

console.log('\n== Case 8: DP simplify diagonal ==')
{
  const { d, w, h } = mask(['#######', '.######', '..#####', '...####', '....###'])
  const { outers } = classifyRings(traceRings(extractBoundaryEdges(d, w, h)))
  assert(outers.length >= 1, 'has outer')
  const before = outers[0].length
  const after = simplifyRing(outers[0], 1.0).length
  assert(after >= 3, 'simplified still polygon (' + after + ')')
  assert(after <= before, 'simplified not larger (' + before + '→' + after + ')')
}

console.log('\n== Case 9: complex crossing (taipei-like slants) ==')
{
  // 模擬右上斜線交叉密集區：多條斜邊匯集，檢查不崩潰、可走區大致保留。
  const { d, w, h } = mask([
    '..........',
    '.#..#..#..',
    '..#..#..#.',
    '...#..#..#',
    '..#..#..#.',
    '.#..#..#..',
    '..........',
  ])
  const r = extractAndRasterize(d, w, h, 0.9, 0.05)
  assert(r.stats.rawRingCount >= 1, 'extracted rings (' + r.stats.rawRingCount + ')')
  const ov = overlap(d, r.smoothedMask)
  assert(ov > 0.55, 'complex-crossing overlap > 0.55 (got ' + ov.toFixed(3) + ')')
  assert(passCount(r.smoothedMask) > passCount(d) * 0.4, 'not整片缺失 (>40%)')
}

console.log(`\n=== ${pass} passed, ${fail} failed ===`)
process.exit(fail > 0 ? 1 : 0)
