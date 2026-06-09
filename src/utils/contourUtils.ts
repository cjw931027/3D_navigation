// contourUtils.ts — 輪廓抽取、DP 簡化、內偏、scanline 柵格化。純函式、可在 Node 測試。
// marching squares 產生有向邊 → 鄰接表（允許頂點多條邊）→「進邊方向 + 最右轉」串封閉環。

export type Pt = [number, number]
export type Ring = Pt[]

// ─────────────────────────────────────────────────
// 1. Marching-squares 邊界邊抽取
// ─────────────────────────────────────────────────
// 掃描每個 cell 的 4 邊，與不同類型相鄰處產生有向邊（方向保證「可走在邊左側」、lattice 整數座標）。

export interface DirectedEdge {
  from: Pt
  to: Pt
}

export function extractBoundaryEdges(
  data: ArrayLike<number>,
  w: number,
  h: number,
): DirectedEdge[] {
  const edges: DirectedEdge[] = []
  const cell = (cx: number, cy: number): number => {
    if (cx < 0 || cx >= w || cy < 0 || cy >= h) return 0
    return data[cy * w + cx]!
  }
  for (let cy = 0; cy < h; cy++) {
    for (let cx = 0; cx < w; cx++) {
      if (cell(cx, cy) !== 1) continue
      // 上：鄰格是牆 → 邊 (cx+1, cy) → (cx, cy)   可走在下(左)
      if (cell(cx, cy - 1) === 0) edges.push({ from: [cx + 1, cy], to: [cx, cy] })
      // 下：鄰格是牆 → 邊 (cx, cy+1) → (cx+1, cy+1)
      if (cell(cx, cy + 1) === 0) edges.push({ from: [cx, cy + 1], to: [cx + 1, cy + 1] })
      // 左：鄰格是牆 → 邊 (cx, cy) → (cx, cy+1)
      if (cell(cx - 1, cy) === 0) edges.push({ from: [cx, cy], to: [cx, cy + 1] })
      // 右：鄰格是牆 → 邊 (cx+1, cy+1) → (cx+1, cy)
      if (cell(cx + 1, cy) === 0) edges.push({ from: [cx + 1, cy + 1], to: [cx + 1, cy] })
    }
  }
  return edges
}

// ─────────────────────────────────────────────────
// 2. 建鄰接表（允許一個頂點多條出邊）
// ─────────────────────────────────────────────────

const ptKey = (p: Pt) => `${p[0]},${p[1]}`

interface AdjEntry {
  to: Pt
  edgeIdx: number
}

function buildAdjacency(edges: DirectedEdge[]): Map<string, AdjEntry[]> {
  const adj = new Map<string, AdjEntry[]>()
  for (let i = 0; i < edges.length; i++) {
    const e = edges[i]!
    const k = ptKey(e.from)
    let list = adj.get(k)
    if (!list) {
      list = []
      adj.set(k, list)
    }
    list.push({ to: e.to, edgeIdx: i })
  }
  return adj
}

// ─────────────────────────────────────────────────
// 3. 用「進邊方向 + 最右轉」規則串封閉環
// ─────────────────────────────────────────────────
// 每條有向邊只用一次；到頂點時選「相對進邊方向最右轉」的邊繼續，正確分離外環與內環。

export function traceRings(edges: DirectedEdge[]): Ring[] {
  const adj = buildAdjacency(edges)
  const used = new Uint8Array(edges.length)
  const rings: Ring[] = []

  for (let ei = 0; ei < edges.length; ei++) {
    if (used[ei]) continue
    const ring: Pt[] = []
    let curEdgeIdx = ei
    let safety = edges.length + 10

    while (safety-- > 0) {
      if (used[curEdgeIdx]) break
      used[curEdgeIdx] = 1

      const curEdge = edges[curEdgeIdx]!
      ring.push(curEdge.from)

      const curTo = curEdge.to
      const toKey = ptKey(curTo)
      const outgoing = adj.get(toKey)
      if (!outgoing) break

      // 進邊「行進方向」din = from → to
      const inDx = curTo[0] - curEdge.from[0]
      const inDy = curTo[1] - curEdge.from[1]

      // 轉向規則：用 cross/dot 分類挑「保持可走在左的最銳轉彎」（取代 atan2，避免對角 saddle 誤拆環）。
      // 優先序：region-preserving 轉彎(cross<0) > 直行 > 反向轉彎 > 回頭。
      let bestIdx = -1
      let bestScore = 99

      for (const entry of outgoing) {
        if (used[entry.edgeIdx]) continue
        const outDx = entry.to[0] - curTo[0]
        const outDy = entry.to[1] - curTo[1]
        const cross = inDx * outDy - inDy * outDx
        const dot = inDx * outDx + inDy * outDy
        let score: number
        if (dot < 0) score = 3 // 回頭（180°）
        else if (cross > 0) score = 2 // 反向轉彎
        else if (cross === 0) score = 1 // 直行
        else score = 0 // cross < 0：保持可走在左的最銳轉彎（最優先）
        if (score < bestScore) {
          bestScore = score
          bestIdx = entry.edgeIdx
        }
      }

      if (bestIdx < 0) break
      curEdgeIdx = bestIdx
      // 如果回到起始邊，結束
      if (bestIdx === ei) break
    }

    if (ring.length >= 3) rings.push(ring)
  }

  return rings
}

// ─────────────────────────────────────────────────
// 4. Signed area (Shoelace) — y-down: <0 = 外環, >0 = 內環（牆塊洞）
// ─────────────────────────────────────────────────

export function signedArea(ring: Ring): number {
  let s = 0
  for (let i = 0; i < ring.length; i++) {
    const a = ring[i]!
    const b = ring[(i + 1) % ring.length]!
    s += a[0] * b[1] - b[0] * a[1]
  }
  return s * 0.5
}

export interface ClassifiedRings {
  outers: Ring[]
  holes: Ring[]
  holesByOuter: Map<number, Ring[]>
}

export function classifyRings(rings: Ring[]): ClassifiedRings {
  const outers: Ring[] = []
  const holes: Ring[] = []
  for (const r of rings) {
    const a = signedArea(r)
    if (a < 0) outers.push(r)
    else if (a > 0) holes.push(r)
    // area === 0 的退化環丟棄
  }

  // 每個 hole 找它屬於哪個 outer（point-in-polygon）
  const holesByOuter = new Map<number, Ring[]>()
  for (let i = 0; i < outers.length; i++) holesByOuter.set(i, [])

  for (const hole of holes) {
    // 取環的第一條邊中點作為測試點
    const a = hole[0]!
    const b = hole[1] ?? hole[0]!
    const mx = (a[0] + b[0]) / 2 + 0.001
    const my = (a[1] + b[1]) / 2 + 0.001
    let owner = -1
    for (let i = 0; i < outers.length; i++) {
      if (pointInRing(mx, my, outers[i]!)) { owner = i; break }
    }
    if (owner >= 0) holesByOuter.get(owner)!.push(hole)
  }

  return { outers, holes, holesByOuter }
}

function pointInRing(px: number, py: number, ring: Ring): boolean {
  let inside = false
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const xi = ring[i]![0], yi = ring[i]![1]
    const xj = ring[j]![0], yj = ring[j]![1]
    const hit =
      yi > py !== yj > py &&
      px < ((xj - xi) * (py - yi)) / (yj - yi + 1e-12) + xi
    if (hit) inside = !inside
  }
  return inside
}

// ─────────────────────────────────────────────────
// 5. Douglas-Peucker 簡化
// ─────────────────────────────────────────────────

export function dpSimplify(pts: Pt[], eps: number): Pt[] {
  if (pts.length <= 2) return pts.slice()
  const n = pts.length
  const keep = new Uint8Array(n)
  keep[0] = 1
  keep[n - 1] = 1
  const stack: Array<[number, number]> = [[0, n - 1]]
  while (stack.length) {
    const [lo, hi] = stack.pop()!
    let maxD = -1
    let maxI = -1
    const ax = pts[lo]![0],
      ay = pts[lo]![1]
    const bx = pts[hi]![0],
      by = pts[hi]![1]
    const dx = bx - ax,
      dy = by - ay
    const len2 = dx * dx + dy * dy
    for (let i = lo + 1; i < hi; i++) {
      const px = pts[i]![0],
        py = pts[i]![1]
      let d2
      if (len2 === 0) {
        const ex = px - ax,
          ey = py - ay
        d2 = ex * ex + ey * ey
      } else {
        const cross = (px - ax) * dy - (py - ay) * dx
        d2 = (cross * cross) / len2
      }
      if (d2 > maxD) {
        maxD = d2
        maxI = i
      }
    }
    if (maxD > eps * eps && maxI > 0) {
      keep[maxI] = 1
      stack.push([lo, maxI], [maxI, hi])
    }
  }
  const out: Pt[] = []
  for (let i = 0; i < n; i++) if (keep[i]) out.push(pts[i]!)
  return out
}

export function simplifyRing(ring: Ring, eps: number): Ring {
  const n = ring.length
  if (n <= 3) return ring.slice()

  // 封閉環：取距頂點 0 最遠者為第二錨點，切兩半各跑 DP 再合併（避免首尾同點的退化）。
  const p0 = ring[0]!
  let maxD = -1
  let anchor = Math.floor(n / 2)
  for (let i = 1; i < n; i++) {
    const dx = ring[i]![0] - p0[0]
    const dy = ring[i]![1] - p0[1]
    const d2 = dx * dx + dy * dy
    if (d2 > maxD) {
      maxD = d2
      anchor = i
    }
  }

  // 切成兩條開放折線：[0 .. anchor] 和 [anchor .. n-1, 0]
  const half1: Pt[] = ring.slice(0, anchor + 1)
  const half2: Pt[] = [...ring.slice(anchor), ring[0]!]

  const s1 = dpSimplify(half1, eps)
  const s2 = dpSimplify(half2, eps)

  // 合併：s1 的末尾 = s2 的開頭（都是 anchor），s2 的末尾 = s1 的開頭（都是 ring[0]）
  // 去掉 s2 的首尾避免重複
  const result: Pt[] = [...s1]
  for (let i = 1; i < s2.length - 1; i++) result.push(s2[i]!)
  // 若退化到只剩不足 3 點，回傳原環
  return result.length >= 3 ? result : ring.slice()
}

// ─────────────────────────────────────────────────
// 6. 安全內偏
// ─────────────────────────────────────────────────

export function inwardBias(ring: Ring, bias: number): Ring {
  const n = ring.length
  if (n < 3 || bias === 0) return ring.slice()
  const out: Ring = []
  for (let i = 0; i < n; i++) {
    const prev = ring[(i - 1 + n) % n]!
    const cur = ring[i]!
    const next = ring[(i + 1) % n]!
    const e1x = cur[0] - prev[0],
      e1y = cur[1] - prev[1]
    const e2x = next[0] - cur[0],
      e2y = next[1] - cur[1]
    // (dy, -dx) 把外環往內推（y-down 座標）
    const n1x = e1y,
      n1y = -e1x
    const n2x = e2y,
      n2y = -e2x
    let nx = n1x + n2x,
      ny = n1y + n2y
    const m = Math.hypot(nx, ny)
    if (m > 1e-9) {
      nx /= m
      ny /= m
    }
    out.push([cur[0] + nx * bias, cur[1] + ny * bias])
  }
  return out
}

// ─────────────────────────────────────────────────
// 7. Scanline polygon fill — 把簡化後的輪廓烤回網格
// ─────────────────────────────────────────────────
// even-odd rule：每掃描行收集邊交點，排序後奇偶配對填 1（外環填可走、hole 自動挖掉）。

export function scanlineFill(
  allRings: Ring[],
  w: number,
  h: number,
): Uint8Array {
  const mask = new Uint8Array(w * h)

  // 收集所有邊（所有 ring 的邊合在一起）
  interface Edge {
    x0: number
    y0: number
    x1: number
    y1: number
  }
  const polyEdges: Edge[] = []
  for (const ring of allRings) {
    const n = ring.length
    for (let i = 0; i < n; i++) {
      const a = ring[i]!
      const b = ring[(i + 1) % n]!
      // 跳過水平邊（不產生交點）
      if (a[1] === b[1]) continue
      polyEdges.push({ x0: a[0], y0: a[1], x1: b[0], y1: b[1] })
    }
  }

  // 對每個 cell row，掃描線 y = row + 0.5（cell 中心）
  for (let row = 0; row < h; row++) {
    const scanY = row + 0.5
    const intersections: number[] = []

    for (const e of polyEdges) {
      const yMin = Math.min(e.y0, e.y1)
      const yMax = Math.max(e.y0, e.y1)
      // 掃描線必須嚴格在邊的 y 範圍內（不含頂端，避免重複計數）
      if (scanY <= yMin || scanY > yMax) continue
      // 線性插值求交點 x
      const t = (scanY - e.y0) / (e.y1 - e.y0)
      const x = e.x0 + t * (e.x1 - e.x0)
      intersections.push(x)
    }

    intersections.sort((a, b) => a - b)

    // 奇偶填充：每對交點之間的 cell 為可走
    for (let p = 0; p + 1 < intersections.length; p += 2) {
      const xLeft = intersections[p]!
      const xRight = intersections[p + 1]!
      // cell cx 的中心是 cx + 0.5；若 cx+0.5 落在 [xLeft, xRight] 內則填 1
      const cxMin = Math.max(0, Math.ceil(xLeft - 0.5))
      const cxMax = Math.min(w - 1, Math.floor(xRight - 0.5))
      for (let cx = cxMin; cx <= cxMax; cx++) {
        mask[row * w + cx] = 1
      }
    }
  }

  return mask
}

// ─────────────────────────────────────────────────
// 8. 整合管線：抽輪廓 → 分類 → 簡化 → 內偏 → 烤回 smoothedMask
// ─────────────────────────────────────────────────

export interface ContourResult {
  /** 簡化+內偏後的外環（用於視覺地板） */
  simpOuters: Ring[]
  /** 簡化+內偏後的牆塊環，按所屬 outer 索引分組 */
  simpHolesByOuter: Map<number, Ring[]>
  /** 所有簡化+內偏後的 ring（外環+hole，用於視覺牆面） */
  allRings: Ring[]
  /** 烤回的 smoothed 碰撞遮罩（1=可走, 0=牆） */
  smoothedMask: Uint8Array
  /** 統計資訊 */
  stats: {
    rawRingCount: number
    outerCount: number
    holeCount: number
    rawVerts: number
    simpVerts: number
    originalPassable: number
    smoothedPassable: number
    diffPercent: number
  }
}

export function extractAndRasterize(
  data: ArrayLike<number>,
  w: number,
  h: number,
  dpEpsilon: number,
  bias: number,
): ContourResult {
  // 1. 抽邊界邊
  const edges = extractBoundaryEdges(data, w, h)

  // 2. 串環
  const rawRings = traceRings(edges)

  let rawVerts = 0
  for (const r of rawRings) rawVerts += r.length

  // 3. 分類
  const { outers, holesByOuter } = classifyRings(rawRings)

  // 4. 簡化 + 內偏
  const simpOuters = outers.map((r) => inwardBias(simplifyRing(r, dpEpsilon), bias))
  const simpHolesByOuter = new Map<number, Ring[]>()
  for (const [k, arr] of holesByOuter)
    simpHolesByOuter.set(
      k,
      arr.map((r) => inwardBias(simplifyRing(r, dpEpsilon), bias)),
    )

  let simpVerts = 0
  for (const r of simpOuters) simpVerts += r.length
  for (const arr of simpHolesByOuter.values()) for (const r of arr) simpVerts += r.length

  // 收集所有 ring
  const allRings: Ring[] = [...simpOuters]
  for (const arr of simpHolesByOuter.values()) for (const r of arr) allRings.push(r)

  // 5. Scanline fill 烤回 smoothedMask
  const smoothedMask = scanlineFill(allRings, w, h)

  // 統計
  let originalPassable = 0
  let smoothedPassable = 0
  for (let i = 0; i < w * h; i++) {
    if (data[i] === 1) originalPassable++
    if (smoothedMask[i] === 1) smoothedPassable++
  }
  const diffPercent =
    originalPassable > 0
      ? (Math.abs(originalPassable - smoothedPassable) / originalPassable) * 100
      : 0

  return {
    simpOuters,
    simpHolesByOuter,
    allRings,
    smoothedMask,
    stats: {
      rawRingCount: rawRings.length,
      outerCount: outers.length,
      holeCount: holesByOuter.size
        ? Array.from(holesByOuter.values()).reduce((s, a) => s + a.length, 0)
        : 0,
      rawVerts,
      simpVerts,
      originalPassable,
      smoothedPassable,
      diffPercent,
    },
  }
}
