<script setup lang="ts">
import { ref } from 'vue'
import { useMapStore } from '@/stores/mapStore'

// 1. 取得 Pinia 冰箱的遙控器
const mapStore = useMapStore()

// 2. 準備一個變數來連接 HTML 裡的 <canvas> 標籤
const mapCanvas = ref<HTMLCanvasElement | null>(null)

// 3. 準備一個狀態來顯示訊息給使用者看
const statusMessage = ref<string>('請選擇一張室內平面圖 (建議 PNG 或 JPG)')
// 【新增】準備一個變數來顯示處理耗時
const timeMessage = ref<string>('') 

// 4. 當使用者選好檔案後，會觸發這個函數
const handleFileUpload = (event: Event) => {
  const target = event.target as HTMLInputElement
  if (!target.files || target.files.length === 0) return
  
  const file = target.files[0]
  if (!file) return 
  
  statusMessage.value = '圖片讀取中...'
  timeMessage.value = '' // 清空之前的時間

  const reader = new FileReader()
  
  reader.onload = (e) => {
    const img = new Image()
    
    img.onload = () => {
      const canvas = mapCanvas.value
      if (!canvas) return
      
      const ctx = canvas.getContext('2d')
      if (!ctx) return

      const startTime = performance.now() // 開始計時

      // 設定我們容許的「最大邊長」安全值
      const MAX_SIZE = 1200
      let targetWidth = img.width
      let targetHeight = img.height

      // 如果圖片太大，計算縮放比例
      if (targetWidth > MAX_SIZE || targetHeight > MAX_SIZE) {
        const ratio = Math.min(MAX_SIZE / targetWidth, MAX_SIZE / targetHeight)
        targetWidth = Math.round(targetWidth * ratio)
        targetHeight = Math.round(targetHeight * ratio)
        console.log(`[系統提示] 圖片過大 (${img.width}x${img.height})，已自動縮放為 ${targetWidth}x${targetHeight}`)
      }

      // 將畫布設定為「安全」的尺寸
      canvas.width = targetWidth
      canvas.height = targetHeight

      // 把圖片畫到 Canvas 上 (傳入 targetWidth/Height 瀏覽器就會幫我們自動壓縮)
      ctx.drawImage(img, 0, 0, targetWidth, targetHeight)

      const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height)
      mapStore.setMapData(imageData.data, canvas.width, canvas.height)

      const endTime = performance.now() // 結束計時
      const processTime = Math.round(endTime - startTime)

      statusMessage.value = `上傳成功！最終地圖尺寸：${canvas.width} x ${canvas.height}`
      timeMessage.value = `影像防護與處理耗時：${processTime} 毫秒`
      // ==========================================
    }
    
    img.src = e.target?.result as string
  }
  
  reader.readAsDataURL(file)
}
</script>

<template>
  <main class="upload-container">
    <h2>上傳 2D 平面圖</h2>
    
    <!-- 提示訊息 -->
    <p class="status">{{ statusMessage }}</p>
    <!-- 【新增】顯示處理時間 -->
    <p class="time-status" v-if="timeMessage">{{ timeMessage }}</p>

    <!-- 檔案上傳按鈕，限制只能選圖片 -->
    <div class="input-section">
      <input 
        type="file" 
        accept="image/png, image/jpeg" 
        @change="handleFileUpload" 
      />
    </div>

    <!-- 顯示圖片的畫布，加上 ref="mapCanvas" 讓 JS 可以控制它 -->
    <div class="canvas-container">
      <canvas ref="mapCanvas"></canvas>
    </div>
  </main>
</template>

<style scoped>
/* 這裡只做最基礎的排版，確保畫面乾淨 */
.upload-container {
  max-width: 800px;
  margin: 0 auto;
  padding: 20px;
  text-align: center;
}

.status {
  color: #666;
  margin-bottom: 10px;
  font-weight: bold;
}

/* 【新增】時間訊息的樣式 */
.time-status {
  color: #e91e63;
  font-size: 0.9em;
  margin-bottom: 20px;
  font-weight: bold;
}

.input-section {
  margin-bottom: 20px;
}

input[type="file"] {
  padding: 10px;
  border: 1px solid #ccc;
  border-radius: 5px;
  background-color: white;
  cursor: pointer;
}

.canvas-container {
  margin-top: 20px;
  border: 2px dashed #aaa;
  padding: 10px;
  background-color: #f9f9f9;
  border-radius: 8px;
  /* 讓畫布太大的時候可以在框框內捲動，不會撐爆網頁 */
  overflow: auto; 
  max-height: 60vh; 
}

/* 確保畫布不會超出容器，但維持比例 */
canvas {
  max-width: 100%;
  height: auto;
  background-color: white;
  box-shadow: 0 4px 6px rgba(0,0,0,0.1);
}
</style>