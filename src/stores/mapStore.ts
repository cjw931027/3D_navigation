import { defineStore } from 'pinia'
import { ref } from 'vue'

// @ts-ignore
import loadWasm from '@/wasm/core.js'

export interface Point { x: number; y: number }
export interface ColorRGB { r: number; g: number; b: number }

export interface FloodFillParams {
  pathColorTolerance: number
  closingKernelSize: number
  wallThicken: number
  sampleRadius: number
}

export type MapMode = 'indoor' | 'outdoor'

export interface Landmark {
  id: string
  text: string
  pixelX: number
  pixelY: number
  source: 'ocr' | 'manual'
  confidence?: number
}

export type PositioningMode = 'manual' | 'inertial' | 'ar'

export type MapType = 'color-block' | 'line-art'

interface ModeRange {
  defaultSensitivity: number
  pathColorTolerance: [number, number]
  closingKernelSize:  [number, number]
  wallThicken:        [number, number]
  sampleRadius:       [number, number]
}

const MODE_RANGE: Record<MapMode, ModeRange> = {
  indoor: {
    defaultSensitivity: 4,
    pathColorTolerance: [8,  60],
    closingKernelSize:  [3,  9 ],
    wallThicken:        [0,  2 ],
    sampleRadius:       [5,  12],
  },
  outdoor: {
    defaultSensitivity: 4,
    pathColorTolerance: [10, 70],
    closingKernelSize:  [3,  7 ],
    wallThicken:        [0,  2 ],
    sampleRadius:       [4,  12],
  },
}

function lerp(a: number, b: number, t: number): number {
  return Math.round(a + (b - a) * t)
}

function lerpOdd(a: number, b: number, t: number): number {
  const v = a + (b - a) * t
  const rounded = Math.round(v)
  return rounded % 2 === 0 ? Math.max(1, rounded - 1) : rounded
}

// 線稿圖送 WASM 前先 2x 上採樣，避免窄走廊被 closing/erode 消除。
const LINE_ART_UPSCALE = 2

function computeParams(mode: MapMode, sensitivity: number): FloodFillParams {
  const r = MODE_RANGE[mode]
  const t = (sensitivity - 1) / 9

  const closingKernelSize = lerpOdd(r.closingKernelSize[0], r.closingKernelSize[1], t)

  let wallThicken: number
  if (sensitivity <= 8) {
    wallThicken = 0
  } else {
    const t2 = (sensitivity - 8) / 2
    wallThicken = lerp(0, 2, t2)
  }

  return {
    pathColorTolerance: lerp(r.pathColorTolerance[0], r.pathColorTolerance[1], t),
    closingKernelSize,
    wallThicken,
    sampleRadius: lerp(r.sampleRadius[0], r.sampleRadius[1], t),
  }
}

// 驗證節點序列每一條相鄰段在 mask 上的 Bresenham 連線全為 passable。
// 任一格在牆內或出界即視為無效，用於在後處理失敗時降級。
function pathAllPassable(
  nodes: Point[],
  mask: Uint8Array,
  width: number,
  height: number,
): boolean {
  for (let i = 0; i < nodes.length - 1; i++) {
    const a = nodes[i]!, b = nodes[i + 1]!
    let cx = Math.round(a.x), cy = Math.round(a.y)
    const tx = Math.round(b.x), ty = Math.round(b.y)
    const dx = Math.abs(tx - cx), dy = Math.abs(ty - cy)
    const sx = cx < tx ? 1 : -1, sy = cy < ty ? 1 : -1
    let err = dx - dy
    while (true) {
      if (cx < 0 || cx >= width || cy < 0 || cy >= height) return false
      if (mask[cy * width + cx] === 0) return false
      if (cx === tx && cy === ty) break
      const e2 = err * 2
      if (e2 > -dy) { err -= dy; cx += sx }
      if (e2 <  dx) { err += dx; cy += sy }
    }
  }
  return true
}

// Visibility-graph greedy shortcut：保留起點、每次跳至最遠可直達節點。
function straightenPath(
  nodes: Point[],
  mask: Uint8Array,
  width: number,
  height: number
): Point[] {
  if (nodes.length <= 2) return nodes

  function linePassable(x0: number, y0: number, x1: number, y1: number): boolean {
    let dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0)
    let sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1
    let err = dx - dy
    let cx = x0, cy = y0
    while (true) {
      if (cx < 0 || cx >= width || cy < 0 || cy >= height) return false
      if (mask[cy * width + cx] === 0) return false
      if (cx === x1 && cy === y1) break
      const e2 = err * 2
      if (e2 > -dy) { err -= dy; cx += sx }
      if (e2 <  dx) { err += dx; cy += sy }
    }
    return true
  }

  const result: Point[] = [nodes[0]!]
  let cur = 0

  while (cur < nodes.length - 1) {
    let far = cur + 1
    for (let j = nodes.length - 1; j > cur + 1; j--) {
      if (linePassable(
        Math.round(nodes[cur]!.x), Math.round(nodes[cur]!.y),
        Math.round(nodes[j]!.x),   Math.round(nodes[j]!.y)
      )) {
        far = j
        break
      }
    }
    result.push(nodes[far]!)
    cur = far
  }

  return result
}

// 將斜向節點重寫為純軸向 L 形；兩個 L 皆被擋時遞迴二分尋找可通行的中繼點。
function axisAlignPath(
  nodes: Point[],
  mask: Uint8Array,
  width: number,
  height: number
): Point[] {
  if (nodes.length <= 1) return nodes

  function hOk(x0: number, x1: number, y: number): boolean {
    if (y < 0 || y >= height) return false
    const lo = Math.min(x0, x1), hi = Math.max(x0, x1)
    for (let x = lo; x <= hi; x++) {
      if (x < 0 || x >= width) return false
      if (mask[y * width + x] === 0) return false
    }
    return true
  }
  function vOk(x: number, y0: number, y1: number): boolean {
    if (x < 0 || x >= width) return false
    const lo = Math.min(y0, y1), hi = Math.max(y0, y1)
    for (let y = lo; y <= hi; y++) {
      if (y < 0 || y >= height) return false
      if (mask[y * width + x] === 0) return false
    }
    return true
  }

  function lShape(from: Point, to: Point, depth: number): Point[] {
    if (from.x === to.x && from.y === to.y) return []
    if (from.x === to.x || from.y === to.y) return [to]
    const okH = hOk(from.x, to.x, from.y) && vOk(to.x, from.y, to.y)
    const okV = vOk(from.x, from.y, to.y) && hOk(from.x, to.x, to.y)
    const preferH = Math.abs(to.x - from.x) >= Math.abs(to.y - from.y)
    if (okH && (preferH || !okV)) return [{ x: to.x,   y: from.y }, to]
    if (okV)                      return [{ x: from.x, y: to.y   }, to]
    if (okH)                      return [{ x: to.x,   y: from.y }, to]
    if (depth >= 5) return [to]
    const mx = Math.round((from.x + to.x) / 2)
    const my = Math.round((from.y + to.y) / 2)
    if (mx < 0 || mx >= width || my < 0 || my >= height) return [to]
    if (mask[my * width + mx] === 0) return [to]
    const mid = { x: mx, y: my }
    return [...lShape(from, mid, depth + 1), ...lShape(mid, to, depth + 1)]
  }

  const out: Point[] = [{ x: Math.round(nodes[0]!.x), y: Math.round(nodes[0]!.y) }]
  for (let i = 1; i < nodes.length; i++) {
    const from = out[out.length - 1]!
    const to   = { x: Math.round(nodes[i]!.x), y: Math.round(nodes[i]!.y) }
    for (const p of lShape(from, to, 0)) out.push(p)
  }
  return out
}

// 將「內部」軸向段垂直於自身方向擴張到牆面極限，取中央作為該段新座標，轉角自然落在走廊中線。
function centerlinePath(
  wps: Point[],
  mask: Uint8Array,
  width: number,
  height: number
): Point[] {
  if (wps.length < 4) return wps

  function rowPassable(y: number, xMin: number, xMax: number): boolean {
    if (y < 0 || y >= height) return false
    for (let x = xMin; x <= xMax; x++) {
      if (x < 0 || x >= width) return false
      if (mask[y * width + x] === 0) return false
    }
    return true
  }
  function colPassable(x: number, yMin: number, yMax: number): boolean {
    if (x < 0 || x >= width) return false
    for (let y = yMin; y <= yMax; y++) {
      if (y < 0 || y >= height) return false
      if (mask[y * width + x] === 0) return false
    }
    return true
  }

  interface Seg {
    isH: boolean
    xMin: number; xMax: number
    yMin: number; yMax: number
    perp: number
  }
  const segs: Seg[] = []
  for (let i = 0; i < wps.length - 1; i++) {
    const a = wps[i]!, b = wps[i + 1]!
    if (a.x === b.x && a.y === b.y) continue
    const isH = a.y === b.y
    segs.push({
      isH,
      xMin: Math.min(a.x, b.x), xMax: Math.max(a.x, b.x),
      yMin: Math.min(a.y, b.y), yMax: Math.max(a.y, b.y),
      perp: isH ? a.y : a.x,
    })
  }
  if (segs.length < 2) return wps

  for (let i = 1; i < segs.length - 1; i++) {
    const s = segs[i]!
    if (s.isH) {
      let yUp = s.perp, yDn = s.perp
      while (rowPassable(yUp - 1, s.xMin, s.xMax)) yUp--
      while (rowPassable(yDn + 1, s.xMin, s.xMax)) yDn++
      s.perp = Math.round((yUp + yDn) / 2)
    } else {
      let xL = s.perp, xR = s.perp
      while (colPassable(xL - 1, s.yMin, s.yMax)) xL--
      while (colPassable(xR + 1, s.yMin, s.yMax)) xR++
      s.perp = Math.round((xL + xR) / 2)
    }
  }

  const result: Point[] = [{ x: wps[0]!.x, y: wps[0]!.y }]
  for (let i = 1; i < wps.length - 1; i++) {
    const before = segs[i - 1]
    const after  = segs[i]
    let nx = wps[i]!.x, ny = wps[i]!.y
    if (before) {
      if (before.isH) ny = before.perp
      else            nx = before.perp
    }
    if (after) {
      if (after.isH)  ny = after.perp
      else            nx = after.perp
    }
    result.push({ x: nx, y: ny })
  }
  result.push({ x: wps[wps.length - 1]!.x, y: wps[wps.length - 1]!.y })
  return result
}

export const useMapStore = defineStore('map', () => {

  const imageRawData = ref<Uint8ClampedArray | null>(null)
  const mapWidth     = ref<number>(0)
  const mapHeight    = ref<number>(0)

  const startPoint = ref<Point | null>(null)
  const endPoint   = ref<Point | null>(null)
  const seedPoint  = ref<Point | null>(null)
  const pathColor  = ref<ColorRGB | null>(null)

  const mapMode     = ref<MapMode>('indoor')
  const sensitivity = ref<number>(MODE_RANGE.indoor.defaultSensitivity)

  const floodFillParams = ref<FloodFillParams>(
    computeParams('indoor', MODE_RANGE.indoor.defaultSensitivity)
  )

  const showAdvanced = ref<boolean>(false)

  const wasmModule    = ref<any>(null)
  const isEngineReady = ref<boolean>(false)

  const denoiseMinArea = ref<number>(80)

  const upscaleFactor = ref<number>(1)

  const floodFillResultData = ref<Uint8ClampedArray | null>(null)

  // 0/1 可通行遮罩，尺寸為上採樣後的 maskWidth × maskHeight。
  const passableMask = ref<Uint8Array | null>(null)
  const maskWidth    = ref<number>(0)
  const maskHeight   = ref<number>(0)

  function setPassableMask(mask: Uint8Array, w: number, h: number) {
    passableMask.value = mask
    maskWidth.value    = w
    maskHeight.value   = h
  }

  const mapType = ref<MapType>('color-block')

  const landmarks = ref<Landmark[]>([])

  function setLandmarks(list: Landmark[]) {
    landmarks.value = list
  }

  function addLandmark(l: Landmark) {
    landmarks.value = [...landmarks.value, l]
  }

  function removeLandmark(id: string) {
    landmarks.value = landmarks.value.filter(l => l.id !== id)
  }

  function updateLandmark(id: string, patch: Partial<Landmark>) {
    landmarks.value = landmarks.value.map(l => (l.id === id ? { ...l, ...patch } : l))
  }

  // Phase 3 定位狀態，目前尚未啟用。
  const userPosition   = ref<Point | null>(null)
  const userHeading    = ref<number | null>(null)
  const positioningMode = ref<PositioningMode>('manual')

  function setMapType(type: MapType) {
    mapType.value       = type
    upscaleFactor.value = type === 'line-art' ? LINE_ART_UPSCALE : 1
    clearDerivedResults()
    applyComputed()
  }

  function applyComputed() {
    const p = computeParams(mapMode.value, sensitivity.value)
    // 線稿圖走廊本身就窄，erode 會切斷；強制關閉牆壁加厚。
    if (mapType.value === 'line-art') p.wallThicken = 0
    floodFillParams.value = p
  }

  function setMapMode(mode: MapMode) {
    mapMode.value     = mode
    sensitivity.value = MODE_RANGE[mode].defaultSensitivity
    clearDerivedResults()
    applyComputed()
  }

  // 改動會使既有 flood fill 結果與路徑座標失效時，統一清理下游狀態。
  function clearDerivedResults() {
    pathNodes.value           = []
    passableMask.value        = null
    maskWidth.value           = 0
    maskHeight.value          = 0
    floodFillResultData.value = null
  }

  function setSensitivity(val: number) {
    sensitivity.value = Math.min(10, Math.max(1, val))
    applyComputed()
  }

  function setFloodFillParams(params: Partial<FloodFillParams>) {
    floodFillParams.value = { ...floodFillParams.value, ...params }
    if (params.sampleRadius !== undefined && seedPoint.value) {
      pathColor.value = sampleDominantColor(seedPoint.value, floodFillParams.value.sampleRadius)
    }
  }

  async function initEngine() {
    if (isEngineReady.value) return
    try {
      wasmModule.value = await loadWasm()
      isEngineReady.value = true
    } catch (error) {
      console.error('WASM 載入失敗:', error)
    }
  }

  function setMapData(data: Uint8ClampedArray, width: number, height: number) {
    imageRawData.value        = data
    mapWidth.value            = width
    mapHeight.value           = height
    seedPoint.value           = null
    pathColor.value           = null
    startPoint.value          = null
    endPoint.value            = null
    pathNodes.value           = []
    floodFillResultData.value = null
    passableMask.value        = null
    maskWidth.value           = 0
    maskHeight.value          = 0
    landmarks.value           = []
    userPosition.value        = null
    userHeading.value         = null

    mapType.value       = 'color-block'
    upscaleFactor.value = 1
    applyComputed()
  }

  function setSeedPoint(seed: Point | null) {
    seedPoint.value = seed
    if (seed && imageRawData.value && mapWidth.value > 0) {
      pathColor.value = sampleDominantColor(seed, floodFillParams.value.sampleRadius)
    } else {
      pathColor.value = null
    }
  }

  function setPoints(start: Point | null, end: Point | null) {
    startPoint.value = start
    endPoint.value   = end
  }

  function sampleDominantColor(point: Point, radius: number): ColorRGB {
    const data       = imageRawData.value!
    const width      = mapWidth.value
    const height     = mapHeight.value
    const freq       = new Map<number, number>()
    const quantShift = mapMode.value === 'outdoor' ? 2 : 3

    for (let dy = -radius; dy <= radius; dy++) {
      for (let dx = -radius; dx <= radius; dx++) {
        const nx = point.x + dx
        const ny = point.y + dy
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue
        const i = (ny * width + nx) * 4
        const r = (data[i]!   >> quantShift) << quantShift
        const g = (data[i+1]! >> quantShift) << quantShift
        const b = (data[i+2]! >> quantShift) << quantShift
        const key = (r << 16) | (g << 8) | b
        freq.set(key, (freq.get(key) ?? 0) + 1)
      }
    }

    let bestKey = 0, bestCount = 0
    freq.forEach((count, key) => {
      if (count > bestCount) { bestCount = count; bestKey = key }
    })

    return {
      r: (bestKey >> 16) & 0xFF,
      g: (bestKey >>  8) & 0xFF,
      b:  bestKey        & 0xFF,
    }
  }

  const pathNodes = ref<Point[]>([])

  function runAStar(): number {
    const wasm  = wasmModule.value
    const start = startPoint.value
    const end   = endPoint.value

    if (!wasm || !isEngineReady.value) return 0
    if (!start || !end) return 0
    if (!passableMask.value || maskWidth.value === 0 || maskHeight.value === 0) {
      console.warn('runAStar: passableMask 尚未建立，請先執行路徑識別')
      return 0
    }

    try {
      const up = upscaleFactor.value
      const nodeCount: number = wasm.runAStar(
        Math.round(start.x * up), Math.round(start.y * up),
        Math.round(end.x   * up), Math.round(end.y   * up)
      )

      if (nodeCount === 0) {
        pathNodes.value = []
        return 0
      }

      const ptr = wasm.getPathBuffer() as number
      const heapBytes: Uint8Array = wasm.HEAPU8

      const raw: Point[] = []
      for (let i = 0; i < nodeCount; i++) {
        const base = ptr + i * 8
        const x = heapBytes[base]!     | (heapBytes[base+1]! << 8)
                | (heapBytes[base+2]! << 16) | (heapBytes[base+3]! << 24)
        const y = heapBytes[base+4]!   | (heapBytes[base+5]! << 8)
                | (heapBytes[base+6]! << 16) | (heapBytes[base+7]! << 24)
        raw.push({ x, y })
      }

      // straighten 在放大座標系進行，最後除回原尺寸。
      const maskPtr    = wasm.getPassableMaskBuffer() as number
      const maskLen    = wasm.getPassableMaskSize()   as number
      const maskW      = wasm.getPassableMaskWidth()  as number
      const maskH      = wasm.getPassableMaskHeight() as number
      const maskBytes  = heapBytes.subarray(maskPtr, maskPtr + maskLen)

      const straightened = straightenPath(raw, maskBytes, maskW, maskH)
      const aligned      = axisAlignPath(straightened, maskBytes, maskW, maskH)
      const centered     = centerlinePath(aligned, maskBytes, maskW, maskH)
      // axisAlignPath 的 lShape fallback 與 centerlinePath 的 corner 位移都可能產生穿牆段；
      // 只有 straightened 每段都經 Bresenham 驗證過，依序降級確保最終 path 保證在 passable 內。
      const final = pathAllPassable(centered, maskBytes, maskW, maskH)
        ? centered
        : pathAllPassable(aligned, maskBytes, maskW, maskH)
          ? aligned
          : straightened
      pathNodes.value = up === 1
        ? final
        : final.map(p => ({ x: p.x / up, y: p.y / up }))
      return pathNodes.value.length

    } catch (err) {
      console.error('runAStar 失敗:', err)
      pathNodes.value = []
      return 0
    }
  }

  function clearPath() {
    pathNodes.value = []
  }

  return {
    imageRawData, mapWidth, mapHeight,
    startPoint, endPoint, seedPoint, pathColor,
    mapMode, sensitivity, floodFillParams, showAdvanced,
    setMapMode, setSensitivity, setFloodFillParams,
    setMapData, setPoints, setSeedPoint,
    wasmModule, isEngineReady, initEngine,
    pathNodes, runAStar, clearPath,
    floodFillResultData,
    passableMask, maskWidth, maskHeight, setPassableMask,
    denoiseMinArea,
    upscaleFactor,
    mapType, setMapType,
    landmarks, setLandmarks, addLandmark, removeLandmark, updateLandmark,
    userPosition, userHeading, positioningMode,
  }
})