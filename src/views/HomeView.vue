<script setup lang="ts">
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()

// 測試用：手動塞假資料進冰箱
function testPinia() {
  mapStore.mapWidth = 1024
  mapStore.mapHeight = 768
}

// 測試用：檢查冰箱裡的像素資料
function checkPixelData() {
  if (!mapStore.imageRawData) {
    alert('冰箱裡沒有圖片資料！請先去「上傳頁」傳一張圖。')
    return
  }
  
  // 印出前 20 個像素值來看看
  // Uint8ClampedArray 裡面存的是 [R, G, B, A, R, G, B, A...] 這樣每 4 個數字代表一個像素
  console.log('--- 冰箱裡的原始像素資料 ---')
  console.log('資料總長度 (寬x高x4):', mapStore.imageRawData.length)
  console.log('前 20 個數字 (5個像素):', mapStore.imageRawData.slice(0, 20))
  alert(`成功讀取！這張圖總共有 ${mapStore.imageRawData.length / 4} 個像素。請按 F12 打開 Console 看詳細數字。`)
}
</script>

<template>
  <main class="home-container">
    <h1>首頁 / Pinia 測試</h1>
    
    <div class="info-box">
      <p>目前地圖寬度： <strong>{{ mapStore.mapWidth }}</strong> px</p>
      <p>目前地圖高度： <strong>{{ mapStore.mapHeight }}</strong> px</p>
      <p>是否已有圖片資料： <strong>{{ mapStore.imageRawData ? '✅ 是' : '❌ 否' }}</strong></p>
    </div>
    
    <div class="button-group">
      <button @click="testPinia" class="btn btn-blue">
        測試：手動變更寬高
      </button>
      
      <button @click="checkPixelData" class="btn btn-green">
        檢查冰箱裡的像素資料 (F12)
      </button>
    </div>
  </main>
</template>

<style scoped>
.home-container {
  padding: 2rem;
  text-align: center;
}

.info-box {
  margin: 20px auto;
  padding: 20px;
  border: 2px dashed #ccc;
  border-radius: 8px;
  max-width: 400px;
  background-color: #f9f9f9;
  text-align: left;
}

.button-group {
  display: flex;
  gap: 10px;
  justify-content: center;
  margin-top: 20px;
}

.btn {
  padding: 10px 20px;
  font-size: 16px;
  cursor: pointer;
  border: none;
  border-radius: 5px;
  font-weight: bold;
  transition: 0.3s;
}

.btn-blue {
  background-color: #2196F3;
  color: white;
}
.btn-blue:hover { background-color: #0b7dda; }

.btn-green {
  background-color: #4CAF50;
  color: white;
}
.btn-green:hover { background-color: #45a049; }
</style>