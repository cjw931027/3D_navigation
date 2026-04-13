<script setup lang="ts">
import { ref, toRaw } from 'vue'
import { useMapStore } from '@/stores/mapStore'
import type { MapMode } from '@/stores/mapStore'

const mapStore    = useMapStore()
const processTime = ref<number | null>(null)
const isRunning   = ref(false)
const previewCanvas = ref<HTMLCanvasElement | null>(null)

const activeTooltip = ref<string | null>(null)
const pathStatus    = ref<string | null>(null)

const tooltips: Record<string, { problem: string; fix: string }> = {
  pathColorTolerance: {
    problem: '識別到的區域太小、路徑顏色不均勻',
    fix: '往右（數值調高）；若識別到太多不相關區域則往左',
  },
  closingKernelSize: {
    problem: '路上有文字或圖示造成路徑中斷',
    fix: '往右（數值調高）；若路徑邊緣模糊則往左',
  },
  wallThicken: {
    problem: '路徑從牆壁細縫漏出、蔓延到不該去的區域',
    fix: '往右（數值調高）；若路徑被截斷、範圍偏小則往左',
  },
  sampleRadius: {
    problem: '種子點採到錯誤顏色（點到邊緣或雜色位置）',
    fix: '往右（數值調高）；若路面顏色不均勻採色不準則往左',
  },
}

const paramLabels: Record<string, string> = {
  pathColorTolerance: '路色容差',
  closingKernelSize:  '斷點填補',
  wallThicken:        '牆壁加厚',
  sampleRadius:       '採色半徑',
}

const paramConfig: Record<string, { min: number; max: number; step: number }> = {
  pathColorTolerance: { min: 5,  max: 80,  step: 1  },
  closingKernelSize:  { min: 1,  max: 9,   step: 2  },
  wallThicken:        { min: 0,  max: 5,   step: 1  },
  sampleRadius:       { min: 3,  max: 18,  step: 1  },
}

const modeOptions: { value: MapMode; label: string; desc: string }[] = [
  { value: 'indoor',  label: '室內', desc: '走廊、地鐵站、辦公室等有明確路徑顏色' },
  { value: 'outdoor', label: '室外', desc: '院區、校園、停車場等戶外路面' },
]

function selectMode(mode: MapMode) {
  mapStore.setMapMode(mode)
}

function drawPath(ctx: CanvasRenderingContext2D) {
  const nodes = mapStore.pathNodes
  if (nodes.length < 2) return

  // 黑色外框壓底
  ctx.save()
  ctx.globalCompositeOperation = 'destination-over'
  ctx.beginPath()
  ctx.moveTo(nodes[0]!.x, nodes[0]!.y)
  for (let i = 1; i < nodes.length; i++) ctx.lineTo(nodes[i]!.x, nodes[i]!.y)
  ctx.strokeStyle = 'rgba(0,0,0,0.4)'
  ctx.lineWidth   = 5
  ctx.lineJoin    = 'round'
  ctx.lineCap     = 'round'
  ctx.stroke()
  ctx.restore()

  // 黃色主線
  ctx.beginPath()
  ctx.moveTo(nodes[0]!.x, nodes[0]!.y)
  for (let i = 1; i < nodes.length; i++) ctx.lineTo(nodes[i]!.x, nodes[i]!.y)
  ctx.strokeStyle = '#FFD600'
  ctx.lineWidth   = 3
  ctx.lineJoin    = 'round'
  ctx.lineCap     = 'round'
  ctx.stroke()
}

function drawDot(ctx: CanvasRenderingContext2D, point: { x: number; y: number }, color: string, label: string) {
  ctx.beginPath()
  ctx.arc(point.x, point.y, 9, 0, Math.PI * 2)
  ctx.fillStyle = 'white'
  ctx.fill()
  ctx.beginPath()
  ctx.arc(point.x, point.y, 6, 0, Math.PI * 2)
  ctx.fillStyle = color
  ctx.fill()
  ctx.fillStyle = color
  ctx.font      = 'bold 13px sans-serif'
  ctx.fillText(label, point.x + 11, point.y - 5)
}

// 將路徑與起訖點標記疊加到 canvas
function drawOverlay(ctx: CanvasRenderingContext2D) {
  if (mapStore.pathNodes.length >= 2) drawPath(ctx)
  if (mapStore.startPoint) drawDot(ctx, mapStore.startPoint, '#4CAF50', '起點')
  if (mapStore.endPoint)   drawDot(ctx, mapStore.endPoint,   '#F44336', '終點')
}

// 把指定像素陣列渲染到 canvas，回傳 ctx（canvas 尺寸同時更新）
function renderToCanvas(buffer: Uint8ClampedArray, width: number, height: number): CanvasRenderingContext2D | null {
  const canvas = previewCanvas.value
  if (!canvas) return null
  canvas.width  = width
  canvas.height = height
  const ctx = canvas.getContext('2d')
  if (!ctx) return null
  ctx.putImageData(new ImageData(buffer as any, width, height), 0, 0)
  return ctx
}

const runFloodFill = () => {
  if (!mapStore.wasmModule) return alert('引擎尚未就緒')
  const rawData = toRaw(mapStore.imageRawData)
  if (!rawData || mapStore.mapWidth === 0) return alert('尚未載入地圖')
  if (!mapStore.seedPoint) return alert('缺少種子點，請回上傳頁重新標記')

  const width  = mapStore.mapWidth
  const height = mapStore.mapHeight
  const size   = rawData.length
  const p      = mapStore.floodFillParams
  const seed   = mapStore.seedPoint

  isRunning.value  = true
  pathStatus.value = null

  try {
    const t0 = performance.now()

    const pointer = mapStore.wasmModule.allocateMemory(size) as number
    mapStore.wasmModule.HEAPU8.set(rawData, pointer)

    const modeInt      = mapStore.mapMode === 'outdoor' ? 1 : 0
    const normInt      = 0
    const denoiseArea  = mapStore.denoiseMinArea
    mapStore.wasmModule.intelligentFloodFill(
      width, height,
      seed.x, seed.y,
      p.pathColorTolerance,
      p.closingKernelSize,
      p.wallThicken,
      p.sampleRadius,
      modeInt,
      normInt,
      denoiseArea
    )

    // freeMemory 前先把 FloodFill 結果完整複製出來並存入 store
    const resultBuffer = new Uint8ClampedArray(size)
    resultBuffer.set(new Uint8ClampedArray(
      mapStore.wasmModule.HEAPU8.buffer as ArrayBuffer, pointer, size
    ))
    mapStore.floodFillResultData = resultBuffer

    mapStore.wasmModule.freeMemory()
    processTime.value = Math.round(performance.now() - t0)

    // freeMemory 後才執行 A*（g_passableMask 仍在 WASM 全域）
    let nodeCount = 0
    if (mapStore.startPoint && mapStore.endPoint) {
      nodeCount = mapStore.runAStar()
    }

    const ctx = renderToCanvas(resultBuffer, width, height)
    if (ctx) drawOverlay(ctx)

    if (nodeCount > 0) {
      pathStatus.value = `路徑長度 ${nodeCount} 段`
    } else if (mapStore.startPoint && mapStore.endPoint) {
      pathStatus.value = '找不到路徑，請確認起訖點位於可通行區域'
    }

  } catch (err) {
    console.error('runFloodFill 失敗:', err)
    pathStatus.value = '運算發生錯誤，請查看 console'
  } finally {
    isRunning.value = false
  }
}

// 重算路徑：不重跑 FloodFill，底圖使用上次 FloodFill 的快取結果
const runAStarOnly = () => {
  if (!mapStore.wasmModule || !mapStore.isEngineReady) return
  if (!mapStore.startPoint || !mapStore.endPoint) return alert('請先設定起點與終點')
  if (!mapStore.floodFillResultData) return alert('請先執行一次路徑識別，再使用重算路徑')

  isRunning.value  = true
  pathStatus.value = null

  try {
    const nodeCount = mapStore.runAStar()

    const ctx = renderToCanvas(
      mapStore.floodFillResultData,
      mapStore.mapWidth,
      mapStore.mapHeight
    )
    if (ctx) drawOverlay(ctx)

    if (nodeCount > 0) {
      pathStatus.value = `路徑長度 ${nodeCount} 段`
    } else {
      pathStatus.value = '找不到路徑，請確認起訖點位於可通行區域'
    }

  } catch (err) {
    console.error('runAStarOnly 失敗:', err)
    pathStatus.value = '運算發生錯誤，請查看 console'
  } finally {
    isRunning.value = false
  }
}
</script>

<template>
  <main class="home-container">
    <h1>路徑識別</h1>

    <div class="warn-panel" v-if="mapStore.mapWidth === 0">
      尚未載入地圖，請先前往
      <router-link to="/upload">上傳地圖</router-link>
      並完成標記。
    </div>

    <template v-else>
      <div class="panel">

        <!-- 路色預覽 -->
        <div class="color-row">
          <div class="color-chip" v-if="mapStore.pathColor">
            <span class="swatch" :style="{
              background: `rgb(${mapStore.pathColor.r},${mapStore.pathColor.g},${mapStore.pathColor.b})`
            }"></span>
            路色　rgb({{ mapStore.pathColor.r }}, {{ mapStore.pathColor.g }}, {{ mapStore.pathColor.b }})
          </div>
          <div class="color-chip muted" v-else>路色未採樣</div>
        </div>

        <!-- 地圖類型 -->
        <div class="section-label">地圖類型</div>
        <div class="mode-row">
          <button
            v-for="opt in modeOptions"
            :key="opt.value"
            class="mode-btn"
            :class="{ active: mapStore.mapMode === opt.value }"
            @click="selectMode(opt.value)"
          >
            {{ opt.label }}
            <span class="mode-desc">{{ opt.desc }}</span>
          </button>
        </div>

        <!-- 靈敏度 -->
        <div class="section-label" style="margin-top:20px">
          辨識靈敏度
          <span class="sens-badge">{{ mapStore.sensitivity }}</span>
        </div>

        <div class="sensitivity-wrap">
          <div class="sensitivity-row">
            <span class="sens-end-label">精細</span>
            <div class="slider-with-ticks">
              <input
                type="range"
                :value="mapStore.sensitivity"
                @input="mapStore.setSensitivity(Number(($event.target as HTMLInputElement).value))"
                min="1" max="10" step="1"
                class="sensitivity-slider"
              />
              <div class="tick-row">
                <span
                  v-for="n in 10" :key="n"
                  class="tick"
                  :class="{ active: mapStore.sensitivity === n }"
                >{{ n }}</span>
              </div>
            </div>
            <span class="sens-end-label">寬鬆</span>
          </div>
          <div class="sensitivity-guide">
            <span>識別範圍太小 → 往右</span>
            <span>識別到多餘區域 → 往左</span>
          </div>
        </div>

        <!-- 進階參數（折疊） -->
        <div
          class="advanced-toggle"
          @click="mapStore.showAdvanced = !mapStore.showAdvanced"
        >
          進階參數
          <span class="toggle-arrow">{{ mapStore.showAdvanced ? '▲' : '▼' }}</span>
        </div>

        <div class="advanced-panel" v-if="mapStore.showAdvanced">
          <div
            v-for="key in (Object.keys(paramLabels) as (keyof typeof paramLabels)[])"
            :key="key"
            class="param-row"
          >
            <div class="param-label">
              <span class="param-name">
                {{ paramLabels[key] }}
                <span
                  class="tooltip-trigger"
                  @mouseenter="activeTooltip = key"
                  @mouseleave="activeTooltip = null"
                  @touchstart.prevent="activeTooltip = activeTooltip === key ? null : key"
                >?</span>
                <div class="tooltip-box" v-if="activeTooltip === key">
                  <p><strong>什麼情況調整：</strong>{{ tooltips[key]!.problem }}</p>
                  <p><strong>如何調整：</strong>{{ tooltips[key]!.fix }}</p>
                </div>
              </span>
              <strong>{{ mapStore.floodFillParams[key as keyof typeof mapStore.floodFillParams] }}</strong>
            </div>
            <input
              type="range"
              :value="mapStore.floodFillParams[key as keyof typeof mapStore.floodFillParams]"
              @input="mapStore.setFloodFillParams({
                [key]: Number(($event.target as HTMLInputElement).value)
              })"
              :min="paramConfig[key]!.min"
              :max="paramConfig[key]!.max"
              :step="paramConfig[key]!.step"
            />
          </div>

          <!-- 遮罩過濾 -->
          <div class="preprocess-section">
            <div class="preprocess-label">遮罩過濾</div>
            <div class="param-row">
              <div class="param-label">
                <span class="param-name">破碎區塊門檻（像素數）</span>
                <strong>{{ mapStore.denoiseMinArea }}</strong>
              </div>
              <input
                type="range"
                :value="mapStore.denoiseMinArea"
                @input="mapStore.denoiseMinArea = Number(($event.target as HTMLInputElement).value)"
                min="0" max="400" step="10"
              />
              <span class="toggle-hint">BFS 後自動清除遮罩中面積過小的孤立連通域，設為 0 可關閉</span>
            </div>
          </div>
        </div>

        <!-- 按鈕列 -->
        <div class="btn-row">
          <button
            class="btn-run"
            @click="runFloodFill"
            :disabled="!mapStore.isEngineReady || isRunning"
          >
            {{ isRunning ? '運算中...' : '執行路徑識別' }}
          </button>
          <button
            class="btn-astar"
            @click="runAStarOnly"
            :disabled="!mapStore.isEngineReady || isRunning
                       || !mapStore.startPoint || !mapStore.endPoint
                       || !mapStore.floodFillResultData"
            title="保留目前識別結果，重新計算起訖點之間的最短路徑"
          >
            重算路徑
          </button>
        </div>

        <!-- 結果資訊列 -->
        <div class="result-row" v-if="processTime !== null || pathStatus !== null">
          <span class="result-time" v-if="processTime !== null">
            耗時 <strong>{{ processTime }}</strong> 毫秒
          </span>
          <span
            class="result-path"
            :class="{ error: pathStatus?.startsWith('找不到') || pathStatus?.startsWith('運算') }"
            v-if="pathStatus"
          >
            {{ pathStatus }}
          </span>
        </div>
      </div>

      <div class="canvas-container">
        <canvas ref="previewCanvas"></canvas>
      </div>
    </template>

  </main>
</template>

<style scoped>
.home-container {
  padding: 2rem;
  max-width: 900px;
  margin: 0 auto;
  text-align: center;
}

h1 {
  font-size: 1.4rem;
  font-weight: 700;
  color: #1a1a2e;
  margin-bottom: 1.2rem;
}

.warn-panel {
  margin: 40px auto;
  max-width: 480px;
  padding: 20px;
  background: #fff8e1;
  border: 1px solid #ffe082;
  border-radius: 8px;
  color: #6d4c00;
  font-size: 0.92em;
}
.warn-panel a { color: #1565c0; font-weight: 600; }

.panel {
  margin: 0 auto 20px;
  padding: 22px 24px;
  border: 1px solid #ddd;
  border-radius: 10px;
  max-width: 600px;
  background: #f8f9ff;
  text-align: left;
}

.color-row { display: flex; margin-bottom: 18px; }

.color-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: 0.8em;
  color: #444;
  background: white;
  border: 1px solid #ddd;
  border-radius: 20px;
  padding: 4px 12px;
}
.color-chip.muted { color: #aaa; }

.swatch {
  display: inline-block;
  width: 14px; height: 14px;
  border-radius: 3px;
  border: 1px solid #aaa;
  flex-shrink: 0;
}

.section-label {
  font-size: 0.8em;
  font-weight: 700;
  color: #888;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  margin-bottom: 8px;
  display: flex;
  align-items: center;
  gap: 8px;
}

.mode-row { display: flex; gap: 10px; }

.mode-btn {
  flex: 1;
  padding: 10px 14px;
  border: 2px solid #ddd;
  border-radius: 8px;
  background: white;
  cursor: pointer;
  text-align: left;
  font-size: 0.9em;
  font-weight: 700;
  color: #444;
  transition: border-color 0.15s, background 0.15s;
  display: flex;
  flex-direction: column;
  gap: 4px;
  touch-action: manipulation;
}
.mode-btn.active {
  border-color: #1565c0;
  background: #e3f2fd;
  color: #1565c0;
}
.mode-desc {
  font-size: 0.74em;
  font-weight: 400;
  color: #999;
  line-height: 1.4;
}
.mode-btn.active .mode-desc { color: #5c8fc7; }

.sens-badge {
  background: #1565c0;
  color: white;
  border-radius: 10px;
  padding: 1px 10px;
  font-size: 0.88em;
  letter-spacing: 0;
  text-transform: none;
  min-width: 26px;
  text-align: center;
}

.sensitivity-wrap { margin-bottom: 4px; }

.sensitivity-row {
  display: flex;
  align-items: center;
  gap: 8px;
}

.sens-end-label {
  font-size: 0.74em;
  color: #aaa;
  white-space: nowrap;
  width: 26px;
  text-align: center;
}

.slider-with-ticks {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.sensitivity-slider {
  width: 100%;
  cursor: pointer;
  accent-color: #1565c0;
}

.tick-row {
  display: flex;
  justify-content: space-between;
  padding: 0 1px;
}

.tick {
  font-size: 0.68em;
  color: #ccc;
  width: 18px;
  text-align: center;
  line-height: 1;
  transition: color 0.15s, font-weight 0.15s;
}

.tick.active {
  color: #1565c0;
  font-weight: 700;
}

.sensitivity-guide {
  display: flex;
  justify-content: space-between;
  font-size: 0.72em;
  color: #bbb;
  margin-top: 4px;
  margin-bottom: 4px;
  padding: 0 34px;
}

.advanced-toggle {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 0.8em;
  font-weight: 700;
  color: #aaa;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  cursor: pointer;
  padding: 10px 0 6px;
  border-top: 1px solid #e8e8e8;
  margin-top: 14px;
  user-select: none;
  touch-action: manipulation;
}
.toggle-arrow { font-size: 0.85em; }

.advanced-panel { padding-top: 8px; margin-bottom: 14px; }

.param-row { margin-bottom: 14px; }

.param-label {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: 0.85em;
  color: #333;
  margin-bottom: 4px;
}

.param-name {
  position: relative;
  display: inline-flex;
  align-items: center;
  gap: 5px;
}

.param-label strong {
  background: #1565c0;
  color: white;
  border-radius: 4px;
  padding: 1px 8px;
  font-size: 0.88em;
  min-width: 28px;
  text-align: center;
}

.param-row input[type="range"] {
  width: 100%;
  cursor: pointer;
  accent-color: #1565c0;
}

.tooltip-trigger {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px; height: 16px;
  border-radius: 50%;
  background: #e0e0e0;
  color: #666;
  font-size: 0.74em;
  font-weight: 700;
  cursor: help;
  flex-shrink: 0;
  touch-action: manipulation;
}

.tooltip-box {
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  z-index: 100;
  background: #1a1a2e;
  color: white;
  font-size: 0.78em;
  font-weight: 400;
  padding: 10px 13px;
  border-radius: 7px;
  width: 260px;
  line-height: 1.55;
  box-shadow: 0 4px 14px rgba(0,0,0,0.25);
  pointer-events: none;
}
.tooltip-box p { margin: 0 0 5px; }
.tooltip-box p:last-child { margin: 0; }

.btn-row {
  display: flex;
  gap: 8px;
  margin-top: 6px;
}

.btn-run {
  flex: 1;
  padding: 12px;
  font-size: 0.95em;
  font-weight: 700;
  background: #00bcd4;
  color: white;
  border: none;
  border-radius: 7px;
  cursor: pointer;
  transition: background 0.2s;
  touch-action: manipulation;
}
.btn-run:hover:not(:disabled) { background: #0097a7; }
.btn-run:disabled { opacity: 0.4; cursor: not-allowed; }

.btn-astar {
  padding: 12px 16px;
  font-size: 0.88em;
  font-weight: 700;
  background: #1565c0;
  color: white;
  border: none;
  border-radius: 7px;
  cursor: pointer;
  transition: background 0.2s;
  touch-action: manipulation;
  white-space: nowrap;
}
.btn-astar:hover:not(:disabled) { background: #0d47a1; }
.btn-astar:disabled { opacity: 0.4; cursor: not-allowed; }

.result-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-top: 10px;
  font-size: 0.85em;
  flex-wrap: wrap;
}

.result-time { color: #006064; }

.result-path {
  color: #1565c0;
  font-weight: 500;
}

.result-path.error { color: #c62828; }

.canvas-container { margin-top: 16px; }

canvas {
  max-width: 100%;
  border: 1px solid #ddd;
  border-radius: 8px;
  background: white;
  box-shadow: 0 2px 8px rgba(0,0,0,0.08);
}

/* 前處理區 */
.preprocess-section {
  border-top: 1px solid #e8e8e8;
  padding-top: 12px;
  margin-top: 4px;
}

.preprocess-label {
  font-size: 0.8em;
  font-weight: 700;
  color: #aaa;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  margin-bottom: 10px;
}

.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 3px;
  margin-bottom: 12px;
}

.toggle-label {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  font-size: 0.88em;
  font-weight: 600;
  color: #333;
  user-select: none;
}

/* 隱藏原生 checkbox */
.toggle-label input[type="checkbox"] {
  position: absolute;
  opacity: 0;
  width: 0;
  height: 0;
}

.toggle-track {
  position: relative;
  display: inline-block;
  width: 34px;
  height: 20px;
  background: #ccc;
  border-radius: 10px;
  transition: background 0.2s;
  flex-shrink: 0;
}

.toggle-label input:checked + .toggle-track {
  background: #1565c0;
}

.toggle-thumb {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 16px;
  height: 16px;
  background: white;
  border-radius: 50%;
  transition: transform 0.2s;
  box-shadow: 0 1px 3px rgba(0,0,0,0.2);
}

.toggle-label input:checked + .toggle-track .toggle-thumb {
  transform: translateX(14px);
}

.toggle-hint {
  font-size: 0.74em;
  color: #999;
  line-height: 1.4;
  padding-left: 42px;
}
</style>