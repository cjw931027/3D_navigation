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

// ============================================================
//  地圖類型：色塊圖 (BFS) vs 線稿圖 (YOLO)
// ============================================================
export type MapType = 'color-block' | 'line-art'

/** 偵測閾值：彩色像素佔比超過此值 → 色塊圖 */
const COLOR_PIXEL_THRESHOLD = 0.05

/**
 * 分析 imageRawData 中彩色像素（有色相、非黑非白）佔全圖的比例。
 *
 * 判定標準（HSV 近似）：
 *   max(R,G,B) - min(R,G,B) > 30  → 有足夠色差（排除灰色）
 *   max(R,G,B) > 30               → 非近黑
 *
 * 效能：stride=4 對每 4 個像素取 1 樣，1200×1200 圖約掃 360K 次，
 * 純整數運算，實測 < 10ms。
 *
 * @returns 0–1 之間的彩色像素比例
 */
function calcColorPixelRatio(
  data: Uint8ClampedArray,
  pixelCount: number
): number {
  const CHROMA_MIN = 30  // max-min 差值下限（排除灰階）
  const VALUE_MIN  = 30  // 亮度下限（排除近黑）
  const STRIDE     = 4   // 每隔幾個像素取樣一次

  let chromatic = 0
  let sampled   = 0

  for (let p = 0; p < pixelCount; p += STRIDE) {
    const i = p * 4
    const r = data[i]!
    const g = data[i + 1]!
    const b = data[i + 2]!

    const maxC = r > g ? (r > b ? r : b) : (g > b ? g : b)
    const minC = r < g ? (r < b ? r : b) : (g < b ? g : b)

    if (maxC > VALUE_MIN && maxC - minC > CHROMA_MIN) chromatic++
    sampled++
  }

  return sampled > 0 ? chromatic / sampled : 0
}

// ============================================================

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
    closingKernelSize:  [1,  7 ],
    wallThicken:        [0,  2 ],
    sampleRadius:       [5,  12],
  },
  outdoor: {
    defaultSensitivity: 4,
    pathColorTolerance: [10, 70],
    closingKernelSize:  [1,  5 ],
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

function computeParams(mode: MapMode, sensitivity: number): FloodFillParams {
  const r = MODE_RANGE[mode]
  const t = (sensitivity - 1) / 9

  let closingKernelSize: number
  if (sensitivity <= 5) {
    closingKernelSize = 1
  } else {
    const t2 = (sensitivity - 5) / 5
    closingKernelSize = lerpOdd(r.closingKernelSize[0], r.closingKernelSize[1], t2)
  }

  let wallThicken: number
  if (mode === 'indoor') {
    if (sensitivity <= 6) {
      wallThicken = 0
    } else {
      const t2 = (sensitivity - 6) / 4
      wallThicken = lerp(0, 2, t2)
    }
  } else {
    if (sensitivity <= 8) {
      wallThicken = 0
    } else {
      const t2 = (sensitivity - 8) / 2
      wallThicken = lerp(0, 2, t2)
    }
  }

  return {
    pathColorTolerance: lerp(r.pathColorTolerance[0], r.pathColorTolerance[1], t),
    closingKernelSize,
    wallThicken,
    sampleRadius: lerp(r.sampleRadius[0], r.sampleRadius[1], t),
  }
}

// ============================================================
//  路徑直線化：Visibility-graph greedy shortcut
//
//  從起點開始，每次盡量往最遠可直達的節點跳，
//  中間用 Bresenham line 逐像素檢查是否都在可通行遮罩內。
//  結果只保留必要的轉折點，其餘全部省略。
// ============================================================
function straightenPath(
  nodes: Point[],
  mask: Uint8Array,
  width: number,
  height: number
): Point[] {
  if (nodes.length <= 2) return nodes

  // Bresenham 直線可通行檢查
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
    // 從 cur 往後找最遠可直達的節點
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

  // 遮罩連通域過濾閾值（0 = 關閉）
  const denoiseMinArea = ref<number>(80)

  // FloodFill 後的像素快取，供「重算路徑」時重繪底圖用
  const floodFillResultData = ref<Uint8ClampedArray | null>(null)

  // ============================================================
  //  地圖類型自動偵測
  // ============================================================

  /** 自動偵測到的地圖類型（不受手動覆蓋影響） */
  const mapTypeAuto = ref<MapType>('color-block')

  /** 目前生效的地圖類型（可被使用者手動覆蓋） */
  const mapType = ref<MapType>('color-block')

  /** 彩色像素佔比（0–1），供 UI 顯示置信度用 */
  const colorPixelRatio = ref<number>(0)

  /** 使用者是否已手動覆蓋自動偵測結果 */
  const mapTypeOverridden = ref<boolean>(false)

  /**
   * 手動設定地圖類型（覆蓋自動偵測）。
   * 傳入 null 可還原為自動偵測結果。
   */
  function setMapType(type: MapType | null) {
    if (type === null) {
      mapType.value          = mapTypeAuto.value
      mapTypeOverridden.value = false
    } else {
      mapType.value          = type
      mapTypeOverridden.value = type !== mapTypeAuto.value
    }
  }

  // ============================================================

  function applyComputed() {
    floodFillParams.value = computeParams(mapMode.value, sensitivity.value)
  }

  function setMapMode(mode: MapMode) {
    mapMode.value     = mode
    sensitivity.value = MODE_RANGE[mode].defaultSensitivity
    applyComputed()
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

    // ── 自動偵測地圖類型 ──────────────────────────────────
    const ratio = calcColorPixelRatio(data, width * height)
    colorPixelRatio.value   = ratio
    const detected: MapType = ratio > COLOR_PIXEL_THRESHOLD ? 'color-block' : 'line-art'
    mapTypeAuto.value       = detected
    mapType.value           = detected       // 重置為自動偵測值
    mapTypeOverridden.value = false
    // ─────────────────────────────────────────────────────
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

  // ============================================================
  //  A* 最短路徑
  // ============================================================

  const pathNodes = ref<Point[]>([])

  function runAStar(): number {
    const wasm  = wasmModule.value
    const start = startPoint.value
    const end   = endPoint.value

    if (!wasm || !isEngineReady.value) return 0
    if (!start || !end) return 0

    try {
      const nodeCount: number = wasm.runAStar(
        Math.round(start.x), Math.round(start.y),
        Math.round(end.x),   Math.round(end.y)
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

      // 用 g_passableMask 對路徑做直線化
      // getPassableMask() 回傳遮罩的 byte offset 與長度
      const maskPtr    = wasm.getPassableMaskBuffer() as number
      const maskLen    = wasm.getPassableMaskSize()   as number
      const maskW      = wasm.getPassableMaskWidth()  as number
      const maskH      = wasm.getPassableMaskHeight() as number
      const maskBytes  = heapBytes.subarray(maskPtr, maskPtr + maskLen)

      pathNodes.value = straightenPath(raw, maskBytes, maskW, maskH)
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
    denoiseMinArea,
    // 地圖類型
    mapType, mapTypeAuto, colorPixelRatio, mapTypeOverridden, setMapType,
  }
})