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
    imageRawData.value = data
    mapWidth.value     = width
    mapHeight.value    = height
    seedPoint.value    = null
    pathColor.value    = null
    startPoint.value   = null
    endPoint.value     = null
    pathNodes.value    = []
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
    const data      = imageRawData.value!
    const width     = mapWidth.value
    const height    = mapHeight.value
    const freq      = new Map<number, number>()
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

      // 用 DataView 逐 byte 讀取，完全避開 Int32Array 的對齊限制
      const heapBytes: Uint8Array = wasm.HEAPU8
      const nodes: Point[] = []
      for (let i = 0; i < nodeCount; i++) {
        const base = ptr + i * 8  // 每對 (x, y) 各 4 bytes = 8 bytes
        const x = heapBytes[base]!     | (heapBytes[base+1]! << 8)
                | (heapBytes[base+2]! << 16) | (heapBytes[base+3]! << 24)
        const y = heapBytes[base+4]!   | (heapBytes[base+5]! << 8)
                | (heapBytes[base+6]! << 16) | (heapBytes[base+7]! << 24)
        nodes.push({ x, y })
      }
      pathNodes.value = nodes
      return nodeCount

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
  }
})