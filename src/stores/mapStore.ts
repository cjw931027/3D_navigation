import { defineStore } from 'pinia'
import { ref } from 'vue'

// @ts-expect-error — core.js 為 Emscripten 產物,無型別定義
import loadWasm from '@/wasm/core.js'

export interface Point {
  x: number
  y: number
}
export interface ColorRGB {
  r: number
  g: number
  b: number
}

export interface FloodFillParams {
  pathColorTolerance: number
  closingKernelSize: number
  wallThicken: number
  sampleRadius: number
  spanThreshold: number
  smoothClosingSize: number
  smoothMinWallArea: number
}

export type MapMode = 'indoor'

export interface Landmark {
  id: string
  text: string
  pixelX: number
  pixelY: number
  source: 'ocr' | 'manual'
  confidence?: number
}

export type PositioningMode = 'manual' | 'inertial' | 'ar'

// Emscripten 模組介面:只列出本專案實際呼叫的 API(core.js 為建置產物,無型別定義)。
export interface WasmEngine {
  HEAPU8: Uint8Array
  allocateMemory(size: number): number
  freeMemory(): void
  intelligentFloodFill(
    width: number,
    height: number,
    seedXs: number[],
    seedYs: number[],
    pathColorTolerance: number,
    closingKernelSize: number,
    wallThicken: number,
    sampleRadius: number,
    denoiseMinArea: number,
    spanThreshold: number,
    smoothClosingSize: number,
    smoothMinWallArea: number,
    clipMode: number,
  ): void
  runAStar(startX: number, startY: number, endX: number, endY: number): number
  getPathBuffer(): number
  getPassableMaskBuffer(): number
  getPassableMaskSize(): number
  getPassableMaskWidth(): number
  getPassableMaskHeight(): number
}

export type MapType = 'color-block'

interface ModeRange {
  defaultSensitivity: number
  pathColorTolerance: [number, number]
  closingKernelSize: [number, number]
  wallThicken: [number, number]
  sampleRadius: [number, number]
  spanThreshold: [number, number]
}

const MODE_RANGE: Record<MapMode, ModeRange> = {
  indoor: {
    defaultSensitivity: 4,
    pathColorTolerance: [8, 60],
    closingKernelSize: [3, 9],
    wallThicken: [0, 2],
    sampleRadius: [5, 12],
    spanThreshold: [20, 40], // 可辨識為牆壁之長度range
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
// const LINE_ART_UPSCALE = 2

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
    spanThreshold: lerp(r.spanThreshold[0], r.spanThreshold[1], t),
    // 平滑參數獨立於靈敏度：smoothMinWallArea 把「小面積、不接觸真牆、被可走包圍」的洞翻為可走。
    smoothClosingSize: 3,
    smoothMinWallArea: 600,
  }
}

// 驗證節點序列每一條相鄰段在 mask 上的 Bresenham 連線全為 passable。
// 任一格在牆內或出界即視為無效，用於在後處理失敗時降級。
function pathAllPassable(nodes: Point[], mask: Uint8Array, width: number, height: number): boolean {
  for (let i = 0; i < nodes.length - 1; i++) {
    const a = nodes[i]!,
      b = nodes[i + 1]!
    let cx = Math.round(a.x),
      cy = Math.round(a.y)
    const tx = Math.round(b.x),
      ty = Math.round(b.y)
    const dx = Math.abs(tx - cx),
      dy = Math.abs(ty - cy)
    const sx = cx < tx ? 1 : -1,
      sy = cy < ty ? 1 : -1
    let err = dx - dy
    while (true) {
      if (cx < 0 || cx >= width || cy < 0 || cy >= height) return false
      if (mask[cy * width + cx] === 0) return false
      if (cx === tx && cy === ty) break
      const e2 = err * 2
      if (e2 > -dy) {
        err -= dy
        cx += sx
      }
      if (e2 < dx) {
        err += dx
        cy += sy
      }
    }
  }
  return true
}

// Visibility-graph greedy shortcut：保留起點、每次跳至最遠可直達節點。
function straightenPath(nodes: Point[], mask: Uint8Array, width: number, height: number): Point[] {
  if (nodes.length <= 2) return nodes

  function linePassable(x0: number, y0: number, x1: number, y1: number): boolean {
    const dx = Math.abs(x1 - x0),
      dy = Math.abs(y1 - y0)
    const sx = x0 < x1 ? 1 : -1,
      sy = y0 < y1 ? 1 : -1
    let err = dx - dy
    let cx = x0,
      cy = y0
    while (true) {
      if (cx < 0 || cx >= width || cy < 0 || cy >= height) return false
      if (mask[cy * width + cx] === 0) return false
      if (cx === x1 && cy === y1) break
      const e2 = err * 2
      if (e2 > -dy) {
        err -= dy
        cx += sx
      }
      if (e2 < dx) {
        err += dx
        cy += sy
      }
    }
    return true
  }

  const result: Point[] = [nodes[0]!]
  let cur = 0

  while (cur < nodes.length - 1) {
    let far = cur + 1
    for (let j = nodes.length - 1; j > cur + 1; j--) {
      if (
        linePassable(
          Math.round(nodes[cur]!.x),
          Math.round(nodes[cur]!.y),
          Math.round(nodes[j]!.x),
          Math.round(nodes[j]!.y),
        )
      ) {
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
  const mapWidth = ref<number>(0)
  const mapHeight = ref<number>(0)

  const startPoint = ref<Point | null>(null)
  const endPoint = ref<Point | null>(null)
  // 多種子：使用者可點多個路色種子（多色底圖如三樓需點多種走廊色）。
  const seedPoints = ref<Point[]>([])
  const pathColors = ref<ColorRGB[]>([])
  // 過度捕捉對策：0=不裁、1=裁到所有牆包圍盒(場地/戶外白底圖)、2=裁到灰色圖框內(hospital 院區外框)。
  const clipMode = ref<0 | 1 | 2>(0)

  const mapMode = ref<MapMode>('indoor')
  const sensitivity = ref<number>(MODE_RANGE.indoor.defaultSensitivity)

  const floodFillParams = ref<FloodFillParams>(
    computeParams('indoor', MODE_RANGE.indoor.defaultSensitivity),
  )

  const showAdvanced = ref<boolean>(false)

  const wasmModule = ref<WasmEngine | null>(null)
  const isEngineReady = ref<boolean>(false)

  const denoiseMinArea = ref<number>(80)

  const upscaleFactor = ref<number>(1)

  const floodFillResultData = ref<Uint8ClampedArray | null>(null)

  // 0/1 可通行遮罩，尺寸為上採樣後的 maskWidth × maskHeight。
  const passableMask = ref<Uint8Array | null>(null)
  const maskWidth = ref<number>(0)
  const maskHeight = ref<number>(0)

  function setPassableMask(mask: Uint8Array, w: number, h: number) {
    passableMask.value = mask
    maskWidth.value = w
    maskHeight.value = h
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
    landmarks.value = landmarks.value.filter((l) => l.id !== id)
  }

  function updateLandmark(id: string, patch: Partial<Landmark>) {
    landmarks.value = landmarks.value.map((l) => (l.id === id ? { ...l, ...patch } : l))
  }

  // Phase 3 定位狀態，目前尚未啟用。
  const userPosition = ref<Point | null>(null)
  const userHeading = ref<number | null>(null)
  const positioningMode = ref<PositioningMode>('manual')

  function setMapType(type: MapType) {
    mapType.value = type
    upscaleFactor.value = 1
    clearDerivedResults()
    applyComputed()
  }

  function applyComputed() {
    const p = computeParams(mapMode.value, sensitivity.value)
    floodFillParams.value = p
  }

  function setMapMode(mode: MapMode) {
    mapMode.value = mode
    sensitivity.value = MODE_RANGE[mode].defaultSensitivity
    clearDerivedResults()
    applyComputed()
  }

  // 改動會使既有 flood fill 結果與路徑座標失效時，統一清理下游狀態。
  function clearDerivedResults() {
    pathNodes.value = []
    passableMask.value = null
    maskWidth.value = 0
    maskHeight.value = 0
    floodFillResultData.value = null
  }

  function setSensitivity(val: number) {
    sensitivity.value = Math.min(10, Math.max(1, val))
    applyComputed()
  }

  function setFloodFillParams(params: Partial<FloodFillParams>) {
    floodFillParams.value = { ...floodFillParams.value, ...params }
    if (params.sampleRadius !== undefined && seedPoints.value.length) {
      resampleAllColors()
    }
  }

  // 依目前 sampleRadius，對所有種子重新取主色，更新 pathColors。
  function resampleAllColors() {
    if (!imageRawData.value || mapWidth.value === 0) {
      pathColors.value = []
      return
    }
    pathColors.value = seedPoints.value.map((s) =>
      sampleDominantColor(s, floodFillParams.value.sampleRadius),
    )
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
    mapWidth.value = width
    mapHeight.value = height
    seedPoints.value = []
    pathColors.value = []
    clipMode.value = 0
    startPoint.value = null
    endPoint.value = null
    pathNodes.value = []
    floodFillResultData.value = null
    passableMask.value = null
    maskWidth.value = 0
    maskHeight.value = 0
    landmarks.value = []
    userPosition.value = null
    userHeading.value = null

    mapType.value = 'color-block'
    upscaleFactor.value = 1
    applyComputed()
  }

  // 多種子：新增 / 移除 / 清空。每次變動都重新取所有種子的主色。
  function addSeedPoint(seed: Point) {
    seedPoints.value = [...seedPoints.value, seed]
    resampleAllColors()
  }

  function removeSeedPoint(index: number) {
    seedPoints.value = seedPoints.value.filter((_, i) => i !== index)
    resampleAllColors()
  }

  function clearSeedPoints() {
    seedPoints.value = []
    pathColors.value = []
  }

  function setClipMode(m: 0 | 1 | 2) {
    clipMode.value = m
    clearDerivedResults()
  }

  function setPoints(start: Point | null, end: Point | null) {
    startPoint.value = start
    endPoint.value = end
  }

  function sampleDominantColor(point: Point, radius: number): ColorRGB {
    const data = imageRawData.value!
    const width = mapWidth.value
    const height = mapHeight.value
    const freq = new Map<number, number>()
    const quantShift = 3

    for (let dy = -radius; dy <= radius; dy++) {
      for (let dx = -radius; dx <= radius; dx++) {
        const nx = point.x + dx
        const ny = point.y + dy
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue
        const i = (ny * width + nx) * 4
        const r = (data[i]! >> quantShift) << quantShift
        const g = (data[i + 1]! >> quantShift) << quantShift
        const b = (data[i + 2]! >> quantShift) << quantShift
        const key = (r << 16) | (g << 8) | b
        freq.set(key, (freq.get(key) ?? 0) + 1)
      }
    }

    let bestKey = 0,
      bestCount = 0
    freq.forEach((count, key) => {
      if (count > bestCount) {
        bestCount = count
        bestKey = key
      }
    })

    return {
      r: (bestKey >> 16) & 0xff,
      g: (bestKey >> 8) & 0xff,
      b: bestKey & 0xff,
    }
  }

  const pathNodes = ref<Point[]>([])

  function runAStar(): number {
    const wasm = wasmModule.value
    const start = startPoint.value
    const end = endPoint.value

    if (!wasm || !isEngineReady.value) return 0
    if (!start || !end) return 0
    if (!passableMask.value || maskWidth.value === 0 || maskHeight.value === 0) {
      console.warn('runAStar: passableMask 尚未建立，請先執行路徑識別')
      return 0
    }

    try {
      const up = upscaleFactor.value
      const nodeCount: number = wasm.runAStar(
        Math.round(start.x * up),
        Math.round(start.y * up),
        Math.round(end.x * up),
        Math.round(end.y * up),
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
        const x =
          heapBytes[base]! |
          (heapBytes[base + 1]! << 8) |
          (heapBytes[base + 2]! << 16) |
          (heapBytes[base + 3]! << 24)
        const y =
          heapBytes[base + 4]! |
          (heapBytes[base + 5]! << 8) |
          (heapBytes[base + 6]! << 16) |
          (heapBytes[base + 7]! << 24)
        raw.push({ x, y })
      }

      // straighten 在放大座標系進行，最後除回原尺寸。
      const maskPtr = wasm.getPassableMaskBuffer() as number
      const maskLen = wasm.getPassableMaskSize() as number
      const maskW = wasm.getPassableMaskWidth() as number
      const maskH = wasm.getPassableMaskHeight() as number
      const maskBytes = heapBytes.subarray(maskPtr, maskPtr + maskLen)

      const straightened = straightenPath(raw, maskBytes, maskW, maskH)
      // straightened 每段都經 Bresenham 驗證；若仍含穿牆段則退回原始 A* 路徑，確保最終 path 在 passable 內。
      const final = pathAllPassable(straightened, maskBytes, maskW, maskH) ? straightened : raw

      pathNodes.value = up === 1 ? final : final.map((p) => ({ x: p.x / up, y: p.y / up }))
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
    imageRawData,
    mapWidth,
    mapHeight,
    startPoint,
    endPoint,
    seedPoints,
    pathColors,
    clipMode,
    mapMode,
    sensitivity,
    floodFillParams,
    showAdvanced,
    setMapMode,
    setSensitivity,
    setFloodFillParams,
    setMapData,
    setPoints,
    addSeedPoint,
    removeSeedPoint,
    clearSeedPoints,
    setClipMode,
    wasmModule,
    isEngineReady,
    initEngine,
    pathNodes,
    runAStar,
    clearPath,
    floodFillResultData,
    passableMask,
    maskWidth,
    maskHeight,
    setPassableMask,
    denoiseMinArea,
    upscaleFactor,
    mapType,
    setMapType,
    landmarks,
    setLandmarks,
    addLandmark,
    removeLandmark,
    updateLandmark,
    userPosition,
    userHeading,
    positioningMode,
  }
})
