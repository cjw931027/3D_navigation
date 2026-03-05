<script setup lang="ts">
import { ref, shallowRef, onMounted, toRaw } from 'vue'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const mapCanvas = ref<HTMLCanvasElement | null>(null)
const statusMessage = ref<string>('請選擇一張室內平面圖 (建議 PNG 或 JPG)')
const timeMessage = ref<string>('') 

const selectionStep = ref<number>(0)
const clickMessage = ref<string>('尚未上傳圖片') 
const seedPoint = ref<{x: number, y: number} | null>(null)
const startPoint = ref<{x: number, y: number} | null>(null)
const endPoint = ref<{x: number, y: number} | null>(null)

const originalImageData = shallowRef<ImageData | null>(null)

  // 自動還原
onMounted(() => {
  // 檢查Pinia內是否有資料
  if (mapStore.imageRawData && mapStore.mapWidth > 0 && mapStore.mapHeight > 0) {
    try {
      const canvas = mapCanvas.value
      if (!canvas) return
      const ctx = canvas.getContext('2d')
      if (!ctx) return

      canvas.width = mapStore.mapWidth
      canvas.height = mapStore.mapHeight

      const rawPixelData = toRaw(mapStore.imageRawData)
      
      const restoredImageData = new ImageData(
        new Uint8ClampedArray(rawPixelData),
        mapStore.mapWidth,
        mapStore.mapHeight
      )

      originalImageData.value = restoredImageData

      // 還原所有座標點
      if (mapStore.seedPoint) seedPoint.value = { ...mapStore.seedPoint }
      if (mapStore.startPoint) startPoint.value = { ...mapStore.startPoint }
      if (mapStore.endPoint) endPoint.value = { ...mapStore.endPoint }

      // 判斷目前的步驟狀態
      if (startPoint.value && endPoint.value) {
        selectionStep.value = 4
        clickMessage.value = '🎉 標記完成！(已從暫存恢復資料)'
      } else if (startPoint.value) {
        selectionStep.value = 3
        clickMessage.value = '步驟 3/3：請點擊「終點」'
      } else if (seedPoint.value) {
        selectionStep.value = 2
        clickMessage.value = '步驟 2/3：請點擊「起點」'
      } else {
        selectionStep.value = 1
        clickMessage.value = '已恢復圖片，請點擊「種子點」'
      }

      // 重新把地圖跟點畫上去
      redrawCanvas(ctx)
      statusMessage.value = `已從快取恢復地圖！尺寸：${canvas.width} x ${canvas.height}`
      
    } catch (error) {
      console.error("還原地圖時發生錯誤:", error)
      statusMessage.value = '地圖還原失敗，請重新上傳'
    }
  }
})

const handleFileUpload = (event: Event) => {
  const target = event.target as HTMLInputElement
  if (!target.files || target.files.length === 0) return
  const file = target.files[0]
  if (!file) return 
  
  statusMessage.value = '圖片讀取中...'
  timeMessage.value = ''
  
  selectionStep.value = 1
  clickMessage.value = '步驟 1/3：請點擊「種子點」(代表可通行的走廊區域)'
  resetPoints() 

  const reader = new FileReader()
  reader.onload = (e) => {
    const img = new Image()
    img.onload = () => {
      const canvas = mapCanvas.value
      if (!canvas) return
      const ctx = canvas.getContext('2d')
      if (!ctx) return

      const startTime = performance.now()
      const MAX_SIZE = 1200
      let targetWidth = img.width
      let targetHeight = img.height

      if (targetWidth > MAX_SIZE || targetHeight > MAX_SIZE) {
        const ratio = Math.min(MAX_SIZE / targetWidth, MAX_SIZE / targetHeight)
        targetWidth = Math.round(targetWidth * ratio)
        targetHeight = Math.round(targetHeight * ratio)
      }

      canvas.width = targetWidth
      canvas.height = targetHeight
      
      ctx.drawImage(img, 0, 0, targetWidth, targetHeight)

      const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height)
      originalImageData.value = imageData
      mapStore.setMapData(imageData.data, canvas.width, canvas.height)

      const endTime = performance.now()
      statusMessage.value = `上傳成功！最終地圖尺寸：${canvas.width} x ${canvas.height}`
      timeMessage.value = `影像處理耗時：${Math.round(endTime - startTime)} 毫秒`
    }
    img.src = e.target?.result as string
  }
  reader.readAsDataURL(file)
}

const handleCanvasClick = (event: MouseEvent) => {
  const canvas = mapCanvas.value
  if (!canvas || selectionStep.value === 0 || selectionStep.value === 4) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return

  const rect = canvas.getBoundingClientRect()
  const scaleX = canvas.width / rect.width
  const scaleY = canvas.height / rect.height

  const x = Math.round((event.clientX - rect.left) * scaleX)
  const y = Math.round((event.clientY - rect.top) * scaleY)

  if (selectionStep.value === 1) {
    seedPoint.value = { x, y }
    selectionStep.value = 2
    clickMessage.value = '步驟 2/3：請點擊「起點」'

    if(seedPoint.value) mapStore.setSeedPoint(seedPoint.value)
  } else if (selectionStep.value === 2) {
    startPoint.value = { x, y }
    selectionStep.value = 3
    clickMessage.value = '步驟 3/3：請點擊「終點」'
  } else if (selectionStep.value === 3) {
    endPoint.value = { x, y }
    selectionStep.value = 4
    clickMessage.value = '🎉 標記完成！(種子點、起點、終點皆已記錄)'
    
    if (startPoint.value && endPoint.value) {
      mapStore.setPoints(startPoint.value, endPoint.value)
    }
  }

  redrawCanvas(ctx)
}

const redrawCanvas = (ctx: CanvasRenderingContext2D) => {
  if (originalImageData.value) {
    ctx.putImageData(originalImageData.value, 0, 0)
  }

  const drawDot = (point: {x: number, y: number}, color: string) => {
    ctx.fillStyle = color
    ctx.beginPath()
    ctx.arc(point.x, point.y, 6, 0, Math.PI * 2) 
    ctx.fill()
    ctx.lineWidth = 2
    ctx.strokeStyle = 'white'
    ctx.stroke()
  }

  if (seedPoint.value) drawDot(seedPoint.value, '#2196F3') 
  if (startPoint.value) drawDot(startPoint.value, '#4CAF50')
  if (endPoint.value) drawDot(endPoint.value, '#F44336')  
}

const resetStartEndPoints = () => {
  if (selectionStep.value < 2) return
  startPoint.value = null
  endPoint.value = null
  selectionStep.value = 2
  clickMessage.value = '步驟 2/3：請點擊「起點」'
  
  mapStore.setPoints(null, null)

  const canvas = mapCanvas.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx) {
      redrawCanvas(ctx)
    }
  }
}

const resetPoints = () => {
  if (selectionStep.value === 0) return
  seedPoint.value = null
  startPoint.value = null
  endPoint.value = null
  selectionStep.value = 1
  clickMessage.value = '步驟 1/3：請點擊「種子點」(代表可通行的走廊區域)'
  
  mapStore.setSeedPoint(null)
  mapStore.setPoints(null, null)
  
  const canvas = mapCanvas.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx && originalImageData.value) {
      ctx.putImageData(originalImageData.value, 0, 0)
    }
  }
}
</script>

<template>
  <main class="upload-container">
    <h2>上傳 2D 平面圖</h2>
    
    <p class="status">{{ statusMessage }}</p>
    <p class="time-status" v-if="timeMessage">{{ timeMessage }}</p>
    
    <div class="coordinate-box" :class="{ 'done': selectionStep === 4 }">
      <strong>{{ clickMessage }}</strong>
      
      <div class="action-buttons">
        <button 
          v-if="selectionStep > 2" 
          class="reset-btn" 
          @click="resetStartEndPoints"
        >
          ⏪ 重設起訖點
        </button>
        
        <button 
          v-if="selectionStep > 1" 
          class="reset-btn" 
          @click="resetPoints"
        >
          🔄 全部重設
        </button>
      </div>
    </div>

    <div class="points-debug" v-if="selectionStep > 1">
      <span v-if="seedPoint">🔵 種子: ({{ seedPoint.x }}, {{ seedPoint.y }}) </span>
      <span v-if="startPoint">🟢 起點: ({{ startPoint.x }}, {{ startPoint.y }}) </span>
      <span v-if="endPoint">🔴 終點: ({{ endPoint.x }}, {{ endPoint.y }}) </span>
    </div>

    <div class="input-section">
      <input type="file" accept="image/png, image/jpeg" @change="handleFileUpload" />
    </div>

    <div class="canvas-container" :class="{ 'disabled': selectionStep === 4 || selectionStep === 0 }">
      <canvas ref="mapCanvas" @click="handleCanvasClick"></canvas>
    </div>
  </main>
</template>

<style scoped>
.upload-container { max-width: 800px; margin: 0 auto; padding: 20px; text-align: center; }
.status { color: #666; margin-bottom: 10px; font-weight: bold; }
.time-status { color: #e91e63; font-size: 0.9em; margin-bottom: 10px; font-weight: bold; }
.input-section { margin-bottom: 20px; }
input[type="file"] { padding: 10px; border: 1px solid #ccc; border-radius: 5px; cursor: pointer; }

.canvas-container { 
  margin-top: 20px; border: 2px dashed #aaa; padding: 10px; 
  background-color: #f9f9f9; border-radius: 8px; overflow: auto; 
  max-height: 60vh; cursor: crosshair; transition: 0.3s;
}
.canvas-container.disabled { cursor: default; }

canvas { max-width: 100%; height: auto; background-color: white; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }

.coordinate-box {
  background-color: #e3f2fd; color: #1565c0; padding: 15px; 
  border-radius: 5px; margin-bottom: 10px; display: inline-flex; 
  align-items: center; gap: 15px; border: 1px solid #90caf9;
  transition: 0.3s;
}
.coordinate-box.done { background-color: #e8f5e9; color: #2e7d32; border-color: #a5d6a7; }

.action-buttons { display: flex; gap: 8px; }

.reset-btn {
  background-color: white; border: 1px solid currentColor; color: inherit;
  padding: 4px 8px; border-radius: 4px; cursor: pointer; font-size: 0.85em;
  font-weight: bold; transition: 0.2s;
}
.reset-btn:hover { background-color: rgba(0,0,0,0.05); }

.points-debug { font-size: 0.9em; color: #555; margin-bottom: 20px; font-family: monospace; }
</style>