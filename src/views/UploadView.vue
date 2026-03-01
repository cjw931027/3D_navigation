<script setup lang="ts">
import { ref } from 'vue'
import { useMapStore } from '@/stores/mapStore'

// 1. 取得 Pinia 冰箱的遙控器
const mapStore = useMapStore()

// 2. 準備一個變數來連接 HTML 裡的 <canvas> 標籤
const mapCanvas = ref<HTMLCanvasElement | null>(null)

// 3. 準備一個狀態來顯示訊息給使用者看
const statusMessage = ref<string>('請選擇一張室內平面圖 (建議 PNG 或 JPG)')

// 4. 當使用者選好檔案後，會觸發這個函數
const handleFileUpload = (event: Event) => {
  // 取得使用者上傳的檔案
  const target = event.target as HTMLInputElement
  if (!target.files || target.files.length === 0) return
  
  const file = target.files[0]
  
  // 【新增這行】明確告訴 TypeScript 如果沒有檔案就中斷，消除 undefined 的疑慮
  if (!file) return 
  
  statusMessage.value = '圖片讀取中...'

  // 使用 FileReader 來讀取檔案內容
  const reader = new FileReader()
  
  // 當檔案讀取完成後要做的事：
  reader.onload = (e) => {
    // 建立一個隱形的圖片物件
    const img = new Image()
    
    // 當隱形圖片載入完成後要做的事：
    img.onload = () => {
      const canvas = mapCanvas.value
      if (!canvas) return
      
      const ctx = canvas.getContext('2d')
      if (!ctx) return

      // 將畫布的寬高設定成跟圖片一模一樣
      canvas.width = img.width
      canvas.height = img.height

      // 把圖片畫到 Canvas 上！(從座標 0,0 開始畫)
      ctx.drawImage(img, 0, 0)

      // 從 Canvas 擷取所有的像素資料 (Pixel Data)
      const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height)

      // 把資料、寬度、高度存進 Pinia 冰箱裡！
      mapStore.setMapData(imageData.data, canvas.width, canvas.height)

      statusMessage.value = `上傳成功！地圖尺寸：${canvas.width} x ${canvas.height}`
    }
    
    // 把讀取到的檔案內容餵給隱形圖片
    img.src = e.target?.result as string
  }
  
  // 指示 FileReader 以 Data URL (Base64字串) 的格式讀取檔案
  reader.readAsDataURL(file)
}
</script>

<template>
  <main class="upload-container">
    <h2>上傳 2D 平面圖</h2>
    
    <!-- 提示訊息 -->
    <p class="status">{{ statusMessage }}</p>

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