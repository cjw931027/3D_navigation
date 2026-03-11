<script setup lang="ts">
import { ref, shallowRef, onMounted, toRaw } from 'vue'
import { useRouter } from 'vue-router'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const router = useRouter()

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
    if (seedPoint.value) mapStore.setSeedPoint(seedPoint.value)

  } else if (selectionStep.value === 2) {
    startPoint.value = { x, y }
    selectionStep.value = 3
    clickMessage.value = '步驟 3/3：請點擊「終點」'

  } else if (selectionStep.value === 3) {
    endPoint.value = { x, y }
    selectionStep.value = 4
    clickMessage.value = '🎉 標記完成！請點擊下方按鈕執行路徑識別'

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

  const drawDot = (point: {x: number, y: number}, color: string, label: string) => {
    // 外圈
    ctx.beginPath()
    ctx.arc(point.x, point.y, 8, 0, Math.PI * 2)
    ctx.fillStyle = 'white'
    ctx.fill()
    // 內點
    ctx.beginPath()
    ctx.arc(point.x, point.y, 6, 0, Math.PI * 2)
    ctx.fillStyle = color
    ctx.fill()
    // 文字標籤
    ctx.fillStyle = color
    ctx.font = 'bold 12px sans-serif'
    ctx.fillText(label, point.x + 10, point.y - 5)
  }

  if (seedPoint.value) drawDot(seedPoint.value, '#2196F3', '種子')
  if (startPoint.value) drawDot(startPoint.value, '#4CAF50', '起點')
  if (endPoint.value) drawDot(endPoint.value, '#F44336', '終點')
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
    if (ctx) redrawCanvas(ctx)
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

// 完成標記後，前往首頁執行演算法
const goToProcess = () => {
  router.push('/')
}
</script>

<template>
  <main class="upload-container">
    <h2>📤 上傳 2D 平面圖</h2>

    <p class="status">{{ statusMessage }}</p>
    <p class="time-status" v-if="timeMessage">⏱ {{ timeMessage }}</p>

    <!-- 步驟說明 -->
    <div class="steps-bar">
      <div class="step" :class="{ active: selectionStep >= 1, done: selectionStep > 1 }">
        <span class="step-num">1</span> 種子點
      </div>
      <div class="step-arrow">→</div>
      <div class="step" :class="{ active: selectionStep >= 2, done: selectionStep > 2 }">
        <span class="step-num">2</span> 起點
      </div>
      <div class="step-arrow">→</div>
      <div class="step" :class="{ active: selectionStep >= 3, done: selectionStep > 3 }">
        <span class="step-num">3</span> 終點
      </div>
    </div>

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

    <!-- 座標顯示 -->
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

    <!-- ✅ 新增：完成後的下一步按鈕 -->
    <div v-if="selectionStep === 4" class="next-step-section">
      <p class="next-hint">✅ 三個點都標記完成，可以執行路徑識別了！</p>
      <button class="btn-next" @click="goToProcess">
        ▶ 下一步：執行 Flood Fill 路徑識別
      </button>
    </div>
  </main>
</template>

<style scoped>
.upload-container { max-width: 800px; margin: 0 auto; padding: 20px; text-align: center; }
.status { color: #666; margin-bottom: 5px; font-weight: bold; }
.time-status { color: #e91e63; font-size: 0.9em; margin-bottom: 10px; font-weight: bold; }
.input-section { margin-bottom: 20px; }
input[type="file"] { padding: 10px; border: 1px solid #ccc; border-radius: 5px; cursor: pointer; }

/* 步驟條 */
.steps-bar {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  margin: 15px 0;
}
.step {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 14px;
  border-radius: 20px;
  background: #eee;
  color: #999;
  font-weight: bold;
  font-size: 0.9em;
  transition: 0.3s;
}
.step.active { background: #e3f2fd; color: #1565c0; }
.step.done { background: #e8f5e9; color: #2e7d32; }
.step-num {
  width: 20px; height: 20px;
  border-radius: 50%;
  background: currentColor;
  color: white;
  display: flex; align-items: center; justify-content: center;
  font-size: 0.8em;
}
.step-arrow { color: #bbb; font-size: 1.2em; }

.canvas-container {
  margin-top: 20px; border: 2px dashed #aaa; padding: 10px;
  background-color: #f9f9f9; border-radius: 8px; overflow: auto;
  max-height: 60vh; cursor: crosshair; transition: 0.3s;
}
.canvas-container.disabled { cursor: default; opacity: 0.85; }

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

/* 下一步區塊 */
.next-step-section {
  margin-top: 30px;
  padding: 20px;
  background: linear-gradient(135deg, #e8f5e9, #e3f2fd);
  border-radius: 12px;
  border: 2px solid #a5d6a7;
}
.next-hint {
  color: #2e7d32;
  font-weight: bold;
  margin-bottom: 12px;
}
.btn-next {
  padding: 14px 32px;
  font-size: 1.1em;
  font-weight: bold;
  background: linear-gradient(135deg, #43a047, #1e88e5);
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  transition: 0.2s;
  box-shadow: 0 4px 12px rgba(0,0,0,0.2);
}
.btn-next:hover {
  transform: translateY(-2px);
  box-shadow: 0 6px 16px rgba(0,0,0,0.25);
}
</style>