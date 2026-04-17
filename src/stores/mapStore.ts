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
      pathNodes.value = up === 1
        ? straightened
        : straightened.map(p => ({ x: p.x / up, y: p.y / up }))
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
    upscaleFactor,
    mapType, setMapType,
    landmarks, setLandmarks, addLandmark, removeLandmark, updateLandmark,
    userPosition, userHeading, positioningMode,
  }
})