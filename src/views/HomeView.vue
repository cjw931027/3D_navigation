<script setup lang="ts">
// 1. 把我們剛剛建好的資料大腦 (Store) 引入進來
import { useMapStore } from '@/stores/mapStore'

// 2. 啟動大腦！我們把它存進一個叫 mapStore 的常數中
const mapStore = useMapStore()

// 3. 寫一個給按鈕觸發的測試功能
function testPinia() {
  // Pinia 最棒的地方在於：你可以直接修改裡面的資料！
  // 這裡我們假裝使用者上傳了一張圖片，把寬高改掉
  mapStore.mapWidth = 1024
  mapStore.mapHeight = 768
}
</script>

<template>
  <main>
    
    <div class="info-box">
      <!-- 這裡直接去 mapStore 拿出 mapWidth 跟 mapHeight 顯示在畫面上 -->
      <p>現在地圖寬度是： <strong>{{ mapStore.mapWidth }}</strong></p>
      <p>現在地圖高度是： <strong>{{ mapStore.mapHeight }}</strong></p>
    </div>
    
    <button @click="testPinia">
      點我！把地圖尺寸變成 1024 x 768
    </button>
  </main>
</template>

<style scoped>
/* 下面這些只是讓畫面稍微好看一點的 CSS，不用太在意 */
main {
  padding: 2rem;
  text-align: center;
  font-family: sans-serif;
}

.info-box {
  margin: 20px auto;
  padding: 20px;
  border: 2px dashed #ccc;
  border-radius: 8px;
  max-width: 300px;
  background-color: #f9f9f9;
  color: #333;
}

button {
  padding: 10px 20px;
  font-size: 16px;
  cursor: pointer;
  background-color: #4CAF50;
  color: white;
  border: none;
  border-radius: 5px;
  font-weight: bold;
  transition: 0.3s;
}

button:hover {
  background-color: #45a049;
}
</style>