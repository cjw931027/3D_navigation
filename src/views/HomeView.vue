<script setup lang="ts">
import { ref, toRaw } from 'vue'
import { useMapStore } from '@/stores/mapStore'

// @ts-ignore
import loadWasm from '@/wasm/core.js'

const mapStore = useMapStore()
const isEngineReady = ref(false)
const processTime = ref<number | null>(null)

let wasmModule: any = null

// 準備用來顯示 C++ 處理完圖片的 Canvas
const previewCanvas = ref<HTMLCanvasElement | null>(null)

// 1. 啟動 C++ 引擎
const initEngine = async () => {
  try {
    wasmModule = await loadWasm()
    isEngineReady.value = true
    console.log('C++ WebAssembly 引擎啟動成功', wasmModule)
  } catch (error) {
    console.error('引擎啟動失敗:', error)
    alert('引擎啟動失敗，請檢查 F12 Console')
  }
}

// 2. 將 Pinia 裡的圖片傳給 C++ 處理
const testImageProcess = () => {
  if (!wasmModule) {
    return alert('請先啟動 C++ 引擎！')
  }
  
  // 使用 toRaw 脫掉 Vue 的 Proxy 外套，拿到乾淨的像素陣列
  const rawData = toRaw(mapStore.imageRawData)
  if (!rawData || mapStore.mapWidth === 0) {
    return alert('請先到「上傳頁」上傳一張平面圖！')
  }

  const size = rawData.length // 陣列的長度 (像素數量)

  // 開始計時
  const startTime = performance.now()

  // 步驟 A: 叫 C++ 準備一塊夠大的記憶體，並拿到 Pointer
  const pointer = wasmModule.allocateMemory(size)

  // 步驟 B: 把 JS 的圖片陣列，瞬間倒進這塊 C++ 的記憶體pointer裡
  wasmModule.HEAPU8.set(rawData, pointer)

  // 步驟 C: 呼叫 C++ 進行極速運算 (反色濾鏡)
  wasmModule.invertColors(size)

  // 步驟 D: 算完之後，從 C++ 的記憶體把結果抓出來
  const resultData = new Uint8ClampedArray(
    wasmModule.HEAPU8.buffer,
    pointer,
    size
  )

  const endTime = performance.now()
  processTime.value = Math.round(endTime - startTime)

  // 步驟 E: 釋放 C++ 記憶體 
  wasmModule.freeMemory()

  // 步驟 F: 把結果畫到畫面上的 Canvas
  const canvas = previewCanvas.value
  if (!canvas) return
  canvas.width = mapStore.mapWidth
  canvas.height = mapStore.mapHeight
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  const newImageData = new ImageData(resultData, mapStore.mapWidth, mapStore.mapHeight)
  ctx.putImageData(newImageData, 0, 0)
}
</script>

<template>
  <main class="home-container">
    <h1>首頁 / C++ 演算法實驗室</h1>
    
    <div class="engine-panel">
      <p>引擎狀態：
        <span :class="isEngineReady ? 'status-on' : 'status-off'">
          {{ isEngineReady ? '已啟動' : '未啟動' }}
        </span>
      </p>

      <div class="button-group">
        <button @click="initEngine" class="btn btn-blue" :disabled="isEngineReady">
          1. 啟動 C++ 引擎
        </button>

        <button @click="testImageProcess" class="btn btn-purple" :disabled="!isEngineReady">
          2. 執行 C++ 影像反色運算
        </button>
      </div>

      <div class="result-box" v-if="processTime !== null">
        C++ 處理幾百萬個像素耗時：<strong>{{ processTime }} 毫秒</strong>
      </div>
    </div>

    <!-- 顯示 C++ 處理完的結果 -->
    <div class="canvas-container">
      <h3>C++ 運算結果預覽</h3>
      <canvas ref="previewCanvas"></canvas>
    </div>
  </main>
</template>

<style scoped>
.home-container { 
  padding: 2rem; 
  text-align: center; 
}

.engine-panel {
  margin: 20px auto; 
  padding: 30px; 
  border: 2px solid #333;
  border-radius: 12px; 
  max-width: 500px; 
  background-color: #f4f4f9;
}

.status-on { color: #4CAF50; font-weight: bold; }
.status-off { color: #F44336; font-weight: bold; }

.button-group { 
  display: flex; 
  flex-direction: column; 
  gap: 15px; 
  margin: 20px 0; 
}

.btn { 
  padding: 12px 20px; 
  font-size: 16px; 
  cursor: pointer; 
  border: none; 
  border-radius: 5px; 
  font-weight: bold; 
  transition: 0.3s; 
}

.btn:disabled { 
  opacity: 0.5; 
  cursor: not-allowed; 
}

.btn-blue { background-color: #2196F3; color: white; }
.btn-blue:not(:disabled):hover { background-color: #0b7dda; }

.btn-purple { background-color: #9C27B0; color: white; }
.btn-purple:not(:disabled):hover { background-color: #7B1FA2; }

.result-box {
  margin-top: 20px; 
  padding: 15px; 
  background-color: white;
  border: 2px dashed #9C27B0; 
  border-radius: 8px; 
  font-size: 1.1em; 
  color: #333;
}

.canvas-container {
  margin-top: 40px;
}

canvas {
  max-width: 100%;
  border: 2px solid #ccc;
  border-radius: 8px;
  background-color: white;
  box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
</style>