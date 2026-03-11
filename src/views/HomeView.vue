<script setup lang="ts">
import { ref, toRaw } from 'vue'
import { useMapStore } from '@/stores/mapStore'

// @ts-ignore
import loadWasm from '@/wasm/core.js'

const mapStore = useMapStore()
const isEngineReady = ref(false)
const processTime = ref<number | null>(null)
const statusMessage = ref<string>('請先啟動 C++ 引擎')

// tolerance 讓使用者可以調整，預設 30
const tolerance = ref<number>(30)

let wasmModule: any = null

const previewCanvas = ref<HTMLCanvasElement | null>(null)

// 1. 啟動 C++ 引擎
const initEngine = async () => {
  try {
    statusMessage.value = '引擎載入中...'
    wasmModule = await loadWasm()
    isEngineReady.value = true
    statusMessage.value = '✅ C++ 引擎已就緒，可以執行運算'
    console.log('C++ WebAssembly 引擎啟動成功', wasmModule)
  } catch (error) {
    console.error('引擎啟動失敗:', error)
    statusMessage.value = '❌ 引擎啟動失敗，請檢查 F12 Console'
  }
}

// 2. 呼叫 Flood Fill，把結果畫到 canvas
const runFloodFill = () => {
  if (!wasmModule) {
    return alert('請先啟動 C++ 引擎！')
  }

  const rawData = toRaw(mapStore.imageRawData)
  if (!rawData || mapStore.mapWidth === 0) {
    return alert('請先到「上傳地圖」頁面上傳一張平面圖！')
  }

  const seed = mapStore.seedPoint
  if (!seed) {
    return alert('找不到種子點！請回到上傳頁重新點選。')
  }

  const width = mapStore.mapWidth
  const height = mapStore.mapHeight
  const size = rawData.length

  statusMessage.value = '⏳ 執行 Flood Fill 運算中...'
  const startTime = performance.now()

  // 步驟 A: 請 C++ 配置記憶體，取得指標
  const pointer = wasmModule.allocateMemory(size)

  // 步驟 B: 把 JS 的圖片像素資料倒進 C++ 記憶體
  wasmModule.HEAPU8.set(rawData, pointer)

  // 步驟 C: 呼叫 Flood Fill！
  // floodFill(width, height, startX, startY, tolerance)
  wasmModule.floodFill(width, height, seed.x, seed.y, tolerance.value)

  // 步驟 D: 把 C++ 處理完的結果從記憶體取出
  const resultData = new Uint8ClampedArray(
    wasmModule.HEAPU8.buffer,
    pointer,
    size
  )

  const endTime = performance.now()
  processTime.value = Math.round(endTime - startTime)

  // 步驟 E: 釋放 C++ 記憶體
  wasmModule.freeMemory()

  // 步驟 F: 把結果畫到 canvas 上預覽
  const canvas = previewCanvas.value
  if (!canvas) return
  canvas.width = width
  canvas.height = height
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  const newImageData = new ImageData(resultData, width, height)
  ctx.putImageData(newImageData, 0, 0)

  // 畫上起點/終點標記
  const drawDot = (point: {x: number, y: number}, color: string, label: string) => {
    ctx.beginPath()
    ctx.arc(point.x, point.y, 8, 0, Math.PI * 2)
    ctx.fillStyle = 'white'
    ctx.fill()
    ctx.beginPath()
    ctx.arc(point.x, point.y, 6, 0, Math.PI * 2)
    ctx.fillStyle = color
    ctx.fill()
    ctx.fillStyle = color
    ctx.font = 'bold 13px sans-serif'
    ctx.fillText(label, point.x + 10, point.y - 5)
  }

  if (mapStore.startPoint) drawDot(mapStore.startPoint, '#4CAF50', '起點')
  if (mapStore.endPoint) drawDot(mapStore.endPoint, '#F44336', '終點')

  statusMessage.value = `✅ Flood Fill 完成！青藍色區域 = 可通行路徑`
}
</script>

<template>
  <main class="home-container">
    <h1>🗺️ 路徑識別 — Flood Fill</h1>

    <div class="engine-panel">
      <p class="status-line">
        引擎狀態：
        <span :class="isEngineReady ? 'status-on' : 'status-off'">
          {{ isEngineReady ? '✅ 已啟動' : '⭕ 未啟動' }}
        </span>
      </p>

      <p class="status-msg">{{ statusMessage }}</p>

      <!-- 容差調整 -->
      <div class="tolerance-row" v-if="isEngineReady">
        <label>顏色容差 (tolerance)：<strong>{{ tolerance }}</strong></label>
        <input type="range" v-model="tolerance" min="5" max="100" step="5" />
        <span class="tolerance-hint">數值越大，越容易把相近顏色視為同一區域</span>
      </div>

      <div class="button-group">
        <button @click="initEngine" class="btn btn-blue" :disabled="isEngineReady">
          1. 啟動 C++ 引擎
        </button>

        <button @click="runFloodFill" class="btn btn-cyan" :disabled="!isEngineReady">
          2. 執行 Flood Fill 路徑識別
        </button>
      </div>

      <div class="result-box" v-if="processTime !== null">
        C++ Flood Fill 耗時：<strong>{{ processTime }} 毫秒</strong>
      </div>
    </div>

    <!-- 目前 Pinia 裡的資料摘要 -->
    <div class="info-panel" v-if="mapStore.mapWidth > 0">
      <h3>📋 目前地圖資料</h3>
      <div class="info-grid">
        <div class="info-item">🖼️ 尺寸：{{ mapStore.mapWidth }} × {{ mapStore.mapHeight }}</div>
        <div class="info-item" v-if="mapStore.seedPoint">🔵 種子點：({{ mapStore.seedPoint.x }}, {{ mapStore.seedPoint.y }})</div>
        <div class="info-item" v-if="mapStore.startPoint">🟢 起點：({{ mapStore.startPoint.x }}, {{ mapStore.startPoint.y }})</div>
        <div class="info-item" v-if="mapStore.endPoint">🔴 終點：({{ mapStore.endPoint.x }}, {{ mapStore.endPoint.y }})</div>
      </div>
    </div>
    <div class="info-panel warn" v-else>
      ⚠️ 尚未載入地圖，請先到「<router-link to="/upload">上傳地圖</router-link>」頁面上傳並標記座標點。
    </div>

    <!-- 預覽 Canvas -->
    <div class="canvas-container" v-if="mapStore.mapWidth > 0">
      <h3>🖼️ Flood Fill 結果預覽</h3>
      <p class="canvas-hint">青藍色 (RGB 0,200,255) = 演算法識別出的可通行區域</p>
      <canvas ref="previewCanvas"></canvas>
    </div>
  </main>
</template>

<style scoped>
.home-container {
  padding: 2rem;
  max-width: 900px;
  margin: 0 auto;
  text-align: center;
}

h1 { color: #1a1a2e; margin-bottom: 1.5rem; }

.engine-panel {
  margin: 20px auto;
  padding: 30px;
  border: 2px solid #ccc;
  border-radius: 12px;
  max-width: 600px;
  background-color: #f8f9ff;
  box-shadow: 0 2px 10px rgba(0,0,0,0.08);
}

.status-line { margin-bottom: 5px; font-size: 1em; }
.status-on { color: #4CAF50; font-weight: bold; }
.status-off { color: #F44336; font-weight: bold; }
.status-msg { color: #555; font-size: 0.95em; margin-bottom: 15px; }

/* 容差滑桿 */
.tolerance-row {
  background: #e8f4fd;
  border-radius: 8px;
  padding: 12px 16px;
  margin-bottom: 16px;
  text-align: left;
}
.tolerance-row label { font-weight: bold; color: #1565c0; display: block; margin-bottom: 6px; }
.tolerance-row input[type="range"] { width: 100%; cursor: pointer; }
.tolerance-hint { font-size: 0.8em; color: #888; display: block; margin-top: 4px; }

.button-group {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin: 16px 0;
}

.btn {
  padding: 12px 20px;
  font-size: 1em;
  cursor: pointer;
  border: none;
  border-radius: 8px;
  font-weight: bold;
  transition: 0.2s;
}
.btn:disabled { opacity: 0.45; cursor: not-allowed; }

.btn-blue { background-color: #2196F3; color: white; }
.btn-blue:not(:disabled):hover { background-color: #0b7dda; }

.btn-cyan { background-color: #00bcd4; color: white; }
.btn-cyan:not(:disabled):hover { background-color: #0097a7; }

.result-box {
  margin-top: 16px;
  padding: 12px;
  background-color: #e0f7fa;
  border: 2px dashed #00bcd4;
  border-radius: 8px;
  font-size: 1.05em;
  color: #006064;
}

/* 地圖資訊摘要 */
.info-panel {
  margin: 20px auto;
  max-width: 600px;
  padding: 16px 20px;
  background: #f1f8e9;
  border: 1px solid #c5e1a5;
  border-radius: 10px;
  text-align: left;
}
.info-panel.warn {
  background: #fff8e1;
  border-color: #ffe082;
  color: #6d4c00;
  text-align: center;
}
.info-panel h3 { margin: 0 0 10px 0; color: #33691e; }
.info-grid { display: flex; flex-wrap: wrap; gap: 8px; }
.info-item {
  background: white;
  border-radius: 6px;
  padding: 5px 12px;
  font-size: 0.9em;
  font-family: monospace;
  border: 1px solid #ddd;
}

/* 預覽 Canvas */
.canvas-container { margin-top: 30px; }
.canvas-container h3 { color: #1a1a2e; }
.canvas-hint { color: #00838f; font-size: 0.9em; margin-bottom: 10px; }

canvas {
  max-width: 100%;
  border: 2px solid #00bcd4;
  border-radius: 8px;
  background-color: white;
  box-shadow: 0 4px 12px rgba(0,188,212,0.2);
}
</style>