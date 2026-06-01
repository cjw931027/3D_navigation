// 執行：node --experimental-strip-types src/utils/polyCollision.test.mjs
import {
  buildSegGrid,
  canStandAtPoly,
  moveCirclePoly,
  ringsToSegments,
} from './polyCollision.ts'

let pass = 0
let fail = 0
const assert = (c, l) => {
  if (c) { pass++; console.log('  PASS ' + l) }
  else { fail++; console.log('  FAIL ' + l) }
}
const near = (a, b, eps, l) =>
  assert(Math.abs(a - b) <= eps, `${l} (got ${a.toFixed(4)}, want ≈${b} ±${eps})`)

const R = 0.35

// ── Case 1：單一垂直線段 x=5，圓從左貼牆 ──
console.log('\n== Case 1: single vertical segment ==')
{
  const segs = [{ ax: 5, ay: -10, bx: 5, by: 10 }]
  const grid = buildSegGrid(segs)
  // 從 (1,0) 往右走 8，應停在 x = 5 - R
  const res = moveCirclePoly(grid, 1, 0, 8, 0, R)
  near(res.cu, 5 - R, 0.01, 'stop at distance R from wall')
  assert(res.stoppedEarly, 'blocked')
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap')
  // 再推不過去
  const res2 = moveCirclePoly(grid, res.cu, res.cv, 5, 0, R)
  near(res2.cu, res.cu, 0.01, 'cannot push past')
}

// ── Case 2：斜線段，圓貼斜線走、停在距線 R 處、不越線 ──
console.log('\n== Case 2: diagonal segment ==')
{
  // 線：(0,0)→(10,10)，法線方向 (1,-1)/√2。圓在線右下側往左上推，應被擋在距線 R。
  const segs = [{ ax: 0, ay: 0, bx: 10, by: 10 }]
  const grid = buildSegGrid(segs)
  // 起點 (6,4)（在線下方），往 (-2,+2) 方向（朝線推）
  const res = moveCirclePoly(grid, 6, 4, -3, 3, R)
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap with diagonal')
  // 與線的距離應 ≈ R（垂直距離 = |x - y| / √2）
  const perpDist = Math.abs(res.cu - res.cv) / Math.SQRT2
  assert(perpDist >= R - 0.02, 'kept at >= R from diagonal (' + perpDist.toFixed(3) + ')')
}

// ── Case 3：沿斜線滑行 — 斜撞線後沿切線仍前進 ──
console.log('\n== Case 3: slide along diagonal ==')
{
  const segs = [{ ax: 0, ay: 0, bx: 20, by: 20 }]
  const grid = buildSegGrid(segs)
  // 貼在線下方 (5 + R*√2 偏移)，往右（+x）推：會撞線 → 沿線 (1,1) 方向滑行
  const start = { x: 5 + 0.5, y: 5 - 0.5 } // 線下方一點
  const res = moveCirclePoly(grid, start.x, start.y, 4, 0, R)
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap while sliding')
  assert(res.cu > start.x + 0.3, 'advanced in x along wall (' + res.cu.toFixed(2) + ')')
}

// ── Case 4：內角（兩線段夾直角），圓推入角內兩方向都被擋 ──
console.log('\n== Case 4: inner corner ==')
{
  // 牆：x=5 (右牆) 與 y=5 (下牆) 形成左上開口的內角；可走區在左上 (x<5, y<5)
  const segs = [
    { ax: 5, ay: -5, bx: 5, by: 5 }, // 垂直牆
    { ax: -5, ay: 5, bx: 5, by: 5 }, // 水平牆
  ]
  const grid = buildSegGrid(segs)
  // 從 (2,2) 往右下 (+4,+4) 推（朝角落）
  const res = moveCirclePoly(grid, 2, 2, 4, 4, R)
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap at inner corner')
  assert(res.cu <= 5 - R + 0.02, 'x blocked by vertical wall (' + res.cu.toFixed(3) + ')')
  assert(res.cv <= 5 - R + 0.02, 'y blocked by horizontal wall (' + res.cv.toFixed(3) + ')')
}

// ── Case 5：封閉多邊形內，圓可自由走、貼邊不出界 ──
console.log('\n== Case 5: inside closed polygon ==')
{
  // 10x10 方框（ring），可走在內。
  const ring = [
    [0, 0],
    [10, 0],
    [10, 10],
    [0, 10],
  ]
  const segs = ringsToSegments([ring])
  const grid = buildSegGrid(segs)
  assert(canStandAtPoly(grid, 5, 5, R), 'center is free')
  // 往右推到貼右牆
  const res = moveCirclePoly(grid, 5, 5, 10, 0, R)
  near(res.cu, 10 - R, 0.02, 'stops at right wall inside box')
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap, stays inside')
  // 往左上角推，兩牆都擋
  const res2 = moveCirclePoly(grid, 5, 5, -10, -10, R)
  near(res2.cu, R, 0.05, 'left wall stop')
  near(res2.cv, R, 0.05, 'top wall stop')
}

// ── Case 6：上百條線段，加速結構不退化 ──
console.log('\n== Case 6: many segments perf ==')
{
  const segs = []
  for (let i = 0; i < 400; i++) {
    const x = (i % 20) * 5
    const y = Math.floor(i / 20) * 5
    segs.push({ ax: x, ay: y, bx: x + 3, by: y + 1 })
  }
  const grid = buildSegGrid(segs)
  const t0 = performance.now()
  let calls = 0
  for (let k = 0; k < 5000; k++) {
    canStandAtPoly(grid, Math.random() * 100, Math.random() * 100, R)
    calls++
  }
  const dt = performance.now() - t0
  console.log(`  ${calls} canStandAtPoly calls over 400 segs in ${dt.toFixed(1)}ms`)
  assert(dt < 200, 'fast enough (<200ms for 5000 calls)')
}

// ── Case 7：穿牆防護 — 大位移一次跨過薄牆不應穿過 ──
console.log('\n== Case 7: no tunneling through thin wall ==')
{
  const segs = [{ ax: 5, ay: -10, bx: 5, by: 10 }]
  const grid = buildSegGrid(segs)
  // 從 (1,0) 一次想衝到 (9,0)，跨過 x=5 的牆；subStep 應防穿
  const res = moveCirclePoly(grid, 1, 0, 8, 0, R, 0.3)
  assert(res.cu <= 5 - R + 0.05, 'did not tunnel through (' + res.cu.toFixed(3) + ')')
}

// ── Case 8：斜 45° 撞垂直牆 → 沿牆滑行前進，不停在原地 ──
console.log('\n== Case 8: 45° hit vertical wall → slide ==')
{
  const segs = [{ ax: 5, ay: -50, bx: 5, by: 50 }] // 垂直牆 x=5
  const grid = buildSegGrid(segs)
  // 從 (4, 0) 已貼近牆，往右上 45° (du=dv=3)：x 被擋在 5-R，y 應沿牆前進
  const start = { x: 5 - R, y: 0 }
  const res = moveCirclePoly(grid, start.x, start.y, 3, 3, R)
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'no overlap after slide')
  near(res.cu, 5 - R, 0.02, 'x stays blocked at R from wall')
  assert(res.cv > start.y + 1.5, 'y advanced along wall (' + res.cv.toFixed(2) + ')')
}

// ── Case 9：沿牆連續滑行多 substep → 持續前進不卡死 ──
console.log('\n== Case 9: continuous slide many substeps ==')
{
  const segs = [{ ax: 5, ay: -100, bx: 5, by: 100 }]
  const grid = buildSegGrid(segs)
  let p = { x: 5 - R, y: 0 }
  let lastY = p.y
  let stuck = false
  for (let k = 0; k < 20; k++) {
    const res = moveCirclePoly(grid, p.x, p.y, 2, 2, R) // 每幀往右上推
    p = { x: res.cu, y: res.cv }
    if (p.y <= lastY + 0.1) { stuck = true; break } // 某幀沒前進 = 卡死
    lastY = p.y
  }
  assert(!stuck, 'never stuck over 20 frames (reached y=' + p.y.toFixed(1) + ')')
  assert(canStandAtPoly(grid, p.x, p.y, R), 'still no overlap')
  near(p.x, 5 - R, 0.05, 'still glued to wall')
}

// ── Case 10：內角 — 推入角內會停；但沿其中一面牆平行移動仍前進 ──
console.log('\n== Case 10: inner corner — dead into corner vs slide along one wall ==')
{
  // 右牆 x=5、下牆 y=5，內角開口朝左上
  const segs = [
    { ax: 5, ay: -50, bx: 5, by: 5 },
    { ax: -50, ay: 5, bx: 5, by: 5 },
  ]
  const grid = buildSegGrid(segs)
  // 10a：從角內附近往右下推（朝角）→ 兩牆都擋，幾乎不動
  {
    const start = { x: 5 - R, y: 5 - R }
    const res = moveCirclePoly(grid, start.x, start.y, 3, 3, R)
    near(res.cu, 5 - R, 0.05, 'corner: x stays')
    near(res.cv, 5 - R, 0.05, 'corner: y stays')
    assert(canStandAtPoly(grid, res.cu, res.cv, R), 'corner: no overlap')
  }
  // 10b：貼右牆、往上（平行右牆、遠離下牆）→ 應沿右牆順利前進
  {
    const start = { x: 5 - R, y: 2 }
    const res = moveCirclePoly(grid, start.x, start.y, 0, -3, R)
    assert(res.cv < start.y - 2, 'slide up along right wall (' + res.cv.toFixed(2) + ')')
    near(res.cu, 5 - R, 0.02, 'stays glued to right wall')
  }
}

// ── Case 11：平行貼牆直走 → 完全不被阻擋 ──
console.log('\n== Case 11: parallel to wall → unobstructed ==')
{
  const segs = [{ ax: 5, ay: -50, bx: 5, by: 50 }]
  const grid = buildSegGrid(segs)
  const start = { x: 5 - R, y: 0 }
  const res = moveCirclePoly(grid, start.x, start.y, 0, 5, R) // 完全平行牆面
  near(res.cv, 5, 0.02, 'moved full distance parallel to wall')
  near(res.cu, 5 - R, 0.02, 'x unchanged')
  assert(!res.stoppedEarly, 'not flagged blocked when moving parallel')
}

// ── Case 12：push-out — 從輕微重疊狀態出發仍能滑行、不抖死 ──
console.log('\n== Case 12: push-out from slight overlap ==')
{
  const segs = [{ ax: 5, ay: -50, bx: 5, by: 50 }]
  const grid = buildSegGrid(segs)
  // 故意起點略微插進牆（距牆 R-0.05 < R）
  const start = { x: 5 - (R - 0.05), y: 0 }
  const res = moveCirclePoly(grid, start.x, start.y, 0, 3, R)
  assert(canStandAtPoly(grid, res.cu, res.cv, R), 'push-out resolved overlap')
  assert(res.cv > start.y + 1.5, 'still advanced along wall despite初始重疊')
}

console.log(`\n=== ${pass} passed, ${fail} failed ===`)
process.exit(fail > 0 ? 1 : 0)
