<script setup lang="ts">
import { ref } from 'vue'
import { useMapStore } from '@/stores/mapStore'

// 直接把wasm編譯出來的 JS 當成模組載入
// @ts-ignore
import loadWasm from '@/wasm/core.js'

const mapStore = useMapStore()
const calculationResult = ref<number | string>('尚未計算')
const isEngineReady = ref(false)

// 讓使用者輸入的兩個數字變數
const inputA = ref<number>(0)
const inputB = ref<number>(0)

// 準備一個變數來存放 C++ 引擎實體
let wasmModule: any = null

// 啟動 C++ 引擎的函數
const initEngine = async () => {
  try {
    // 呼叫 loadWasm()，瀏覽器會在背景自動去抓 core.wasm 檔案
    wasmModule = await loadWasm()
    isEngineReady.value = true
    console.log('C++ WebAssembly 引擎啟動成功', wasmModule)
  } catch (error) {
    console.error('引擎啟動失敗:', error)
    alert('引擎啟動失敗，請檢查 F12 Console')
  }
}

// 測試呼叫 C++ 的 add 函數
const testCppAdd = () => {
  if (!wasmModule) {
    alert('請先啟動 C++ 引擎！')
    return
  }
  // 這裡直接呼叫我們在 C++ 用 Embind 綁定的 "add" 函數，並傳入使用者輸入的值
  const result = wasmModule.add(inputA.value, inputB.value)
  calculationResult.value = result
}
</script>

<template>
  <main class="home-container">
    <h1>首頁 / C++ 測試區</h1>
    
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
        
        <!-- 新增的數字輸入區塊 -->
        <div class="input-area">
          <input type="number" v-model="inputA" class="num-input" />
          <span class="plus-sign">+</span>
          <input type="number" v-model="inputB" class="num-input" />
        </div>

        <button @click="testCppAdd" class="btn btn-purple" :disabled="!isEngineReady">
          2. 讓 C++ 計算
        </button>
      </div>

      <div class="result-box">
        C++ 計算結果：<strong>{{ calculationResult }}</strong>
      </div>
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

/* 輸入區塊的樣式 */
.input-area {
  display: flex;
  justify-content: center;
  align-items: center;
  gap: 10px;
  margin: 10px 0;
}

.num-input {
  width: 80px;
  padding: 8px;
  font-size: 16px;
  text-align: center;
  border: 1px solid #ccc;
  border-radius: 4px;
}

.plus-sign {
  font-size: 20px;
  font-weight: bold;
  color: #555;
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
  font-size: 1.2em; 
  color: #333;
}
</style>