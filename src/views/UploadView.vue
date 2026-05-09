<script setup lang="ts">
import { ref, shallowRef, onMounted, onUnmounted, toRaw, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const router = useRouter()

const mapCanvas = ref<HTMLCanvasElement | null>(null)
const canvasWrapper = ref<HTMLDivElement | null>(null)
const magnifier = ref<HTMLCanvasElement | null>(null)

const statusMessage = ref<string>('請選擇一張室內平面圖')
const selectionStep = ref<number>(0)

const seedPoint = ref<{ x: number; y: number } | null>(null)
const startPoint = ref<{ x: number; y: number } | null>(null)
const endPoint = ref<{ x: number; y: number } | null>(null)

const originalImageData = shallowRef<ImageData | null>(null)

const showMagnifier = ref(false)
const magnifierPos = ref({ x: 0, y: 0 })

// Pinch-to-zoom / pan 狀態
const scale = ref(1)
const translateX = ref(0)
const translateY = ref(0)
let lastPinchDist = 0
let isPanning = false
let panStart = { x: 0, y: 0 }
let panOrigin = { x: 0, y: 0 }
let touchStartTime = 0
let touchStartX = 0
let touchStartY = 0
let didPinch = false

const stepMessages: Record<number, string> = {
  1: '步驟 1／3　點擊種子點（走廊任意位置）',
  2: '步驟 2／3　點擊起點',
  3: '步驟 3／3　點擊終點',
  4: '標記完成',
}

// ── 座標轉換 ──────────────────────────────────────────────

// 螢幕座標 → canvas 像素座標（考慮 CSS transform 縮放）
function screenToCanvas(clientX: number, clientY: number): { x: number; y: number } {
  const canvas = mapCanvas.value
  if (!canvas) return { x: -1, y: -1 }
  const cRect = canvas.getBoundingClientRect()
  const scaleX = canvas.width / cRect.width
  const scaleY = canvas.height / cRect.height
  return {
    x: Math.round((clientX - cRect.left) * scaleX),
    y: Math.round((clientY - cRect.top) * scaleY),
  }
}

function getPinchDist(touches: TouchList): number {
  const dx = touches[0]!.clientX - touches[1]!.clientX
  const dy = touches[0]!.clientY - touches[1]!.clientY
  return Math.sqrt(dx * dx + dy * dy)
}

// ── 放大鏡 ────────────────────────────────────────────────

function updateMagnifier(canvasX: number, canvasY: number) {
  const src = mapCanvas.value
  const mag = magnifier.value
  if (!src || !mag) return
  const ctx = mag.getContext('2d')
  if (!ctx) return

  const ZOOM = 3
  const SIZE = mag.width
  const half = SIZE / 2

  ctx.clearRect(0, 0, SIZE, SIZE)
  ctx.save()
  ctx.beginPath()
  ctx.arc(half, half, half, 0, Math.PI * 2)
  ctx.clip()
  ctx.imageSmoothingEnabled = false
  ctx.drawImage(
    src,
    canvasX - half / ZOOM,
    canvasY - half / ZOOM,
    SIZE / ZOOM,
    SIZE / ZOOM,
    0,
    0,
    SIZE,
    SIZE,
  )
  // 準心
  ctx.strokeStyle = 'rgba(0,0,0,0.5)'
  ctx.lineWidth = 1
  ctx.beginPath()
  ctx.moveTo(half, half - 8)
  ctx.lineTo(half, half + 8)
  ctx.moveTo(half - 8, half)
  ctx.lineTo(half + 8, half)
  ctx.stroke()
  ctx.restore()
}

// ── 限制平移範圍 ──────────────────────────────────────────

function clampTranslate() {
  const canvas = mapCanvas.value
  const wrapper = canvasWrapper.value
  if (!canvas || !wrapper) return
  const wRect = wrapper.getBoundingClientRect()
  const scaledW = canvas.offsetWidth * scale.value
  const scaledH = canvas.offsetHeight * scale.value
  const maxX = Math.max(0, (scaledW - wRect.width) / 2)
  const maxY = Math.max(0, (scaledH - wRect.height) / 2)
  translateX.value = Math.min(maxX, Math.max(-maxX, translateX.value))
  translateY.value = Math.min(maxY, Math.max(-maxY, translateY.value))
}

// ── 觸控事件（手動 addEventListener 以支援 preventDefault） ──

function onTouchStart(event: TouchEvent) {
  event.preventDefault()

  if (event.touches.length === 2) {
    lastPinchDist = getPinchDist(event.touches)
    didPinch = true
    return
  }

  if (event.touches.length === 1) {
    const t = event.touches[0]!
    touchStartTime = Date.now()
    touchStartX = t.clientX
    touchStartY = t.clientY
    didPinch = false

    if (scale.value > 1.05) {
      isPanning = true
      panStart = { x: t.clientX, y: t.clientY }
      panOrigin = { x: translateX.value, y: translateY.value }
    }
  }
}

function onTouchMove(event: TouchEvent) {
  event.preventDefault()

  if (event.touches.length === 2) {
    const dist = getPinchDist(event.touches)
    const delta = dist / lastPinchDist
    lastPinchDist = dist

    const wrapper = canvasWrapper.value
    if (!wrapper) return
    const wRect = wrapper.getBoundingClientRect()
    const midX = (event.touches[0]!.clientX + event.touches[1]!.clientX) / 2
    const midY = (event.touches[0]!.clientY + event.touches[1]!.clientY) / 2

    const newScale = Math.min(Math.max(scale.value * delta, 1), 5)
    const scaleDiff = newScale - scale.value
    translateX.value -= ((midX - wRect.left - wRect.width / 2) * scaleDiff) / newScale
    translateY.value -= ((midY - wRect.top - wRect.height / 2) * scaleDiff) / newScale
    scale.value = newScale
    clampTranslate()
    didPinch = true
    return
  }

  if (event.touches.length === 1 && isPanning) {
    const t = event.touches[0]!
    translateX.value = panOrigin.x + (t.clientX - panStart.x)
    translateY.value = panOrigin.y + (t.clientY - panStart.y)
    clampTranslate()

    if (selectionStep.value > 0 && selectionStep.value < 4) {
      const canvas = mapCanvas.value
      if (!canvas) return
      const { x, y } = screenToCanvas(t.clientX, t.clientY)
      if (x >= 0 && x < canvas.width && y >= 0 && y < canvas.height) {
        showMagnifier.value = true
        magnifierPos.value = { x: t.clientX - 130, y: t.clientY - 160 }
        updateMagnifier(x, y)
      }
    }
  }
}

function onTouchEnd(event: TouchEvent) {
  event.preventDefault()
  isPanning = false

  if (didPinch) {
    showMagnifier.value = false
    return
  }

  if (event.changedTouches.length === 1) {
    const t = event.changedTouches[0]!
    const moved = Math.abs(t.clientX - touchStartX) + Math.abs(t.clientY - touchStartY)
    const elapsed = Date.now() - touchStartTime

    if (moved < 10 && elapsed < 400) {
      const canvas = mapCanvas.value
      if (!canvas || selectionStep.value === 0 || selectionStep.value === 4) return
      const ctx = canvas.getContext('2d')
      if (!ctx) return
      const { x, y } = screenToCanvas(t.clientX, t.clientY)
      if (x >= 0 && x < canvas.width && y >= 0 && y < canvas.height) {
        commitPoint(x, y, ctx)
      }
    }
  }
  showMagnifier.value = false
}

// ── 生命週期 ──────────────────────────────────────────────

onMounted(async () => {
  // touch 事件需要 { passive: false } 才能 preventDefault，無法用 Vue 修飾符
  const wrapper = canvasWrapper.value
  if (wrapper) {
    wrapper.addEventListener('touchstart', onTouchStart, { passive: false })
    wrapper.addEventListener('touchmove', onTouchMove, { passive: false })
    wrapper.addEventListener('touchend', onTouchEnd, { passive: false })
  }

  // 還原快取地圖
  if (!mapStore.imageRawData || mapStore.mapWidth === 0) return
  try {
    await nextTick()
    const canvas = mapCanvas.value
    if (!canvas) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return

    canvas.width = mapStore.mapWidth
    canvas.height = mapStore.mapHeight

    const restored = new ImageData(
      new Uint8ClampedArray(toRaw(mapStore.imageRawData)),
      mapStore.mapWidth,
      mapStore.mapHeight,
    )
    originalImageData.value = restored

    if (mapStore.seedPoint) seedPoint.value = { ...mapStore.seedPoint }
    if (mapStore.startPoint) startPoint.value = { ...mapStore.startPoint }
    if (mapStore.endPoint) endPoint.value = { ...mapStore.endPoint }

    if (startPoint.value && endPoint.value) selectionStep.value = 4
    else if (startPoint.value) selectionStep.value = 3
    else if (seedPoint.value) selectionStep.value = 2
    else selectionStep.value = 1

    redrawCanvas(ctx)
    statusMessage.value = `${canvas.width} × ${canvas.height}`
  } catch {
    statusMessage.value = '地圖還原失敗，請重新上傳'
  }
})

onUnmounted(() => {
  const wrapper = canvasWrapper.value
  if (wrapper) {
    wrapper.removeEventListener('touchstart', onTouchStart)
    wrapper.removeEventListener('touchmove', onTouchMove)
    wrapper.removeEventListener('touchend', onTouchEnd)
  }
})

// ── 上傳圖片 ──────────────────────────────────────────────

const handleFileUpload = (event: Event) => {
  const target = event.target as HTMLInputElement
  if (!target.files?.length) return

  resetAll()
  statusMessage.value = '讀取中...'

  const reader = new FileReader()
  reader.onload = (e) => {
    const img = new Image()
    img.onload = () => {
      const canvas = mapCanvas.value
      if (!canvas) return
      const ctx = canvas.getContext('2d')
      if (!ctx) return

      // 縮放上限 1200px，保留完整圖片比例
      const MAX = 1200
      let w = img.naturalWidth,
        h = img.naturalHeight
      if (w > MAX || h > MAX) {
        const ratio = Math.min(MAX / w, MAX / h)
        w = Math.round(w * ratio)
        h = Math.round(h * ratio)
      }

      canvas.width = w
      canvas.height = h
      ctx.drawImage(img, 0, 0, w, h)

      const imageData = ctx.getImageData(0, 0, w, h)
      originalImageData.value = imageData
      mapStore.setMapData(imageData.data, w, h)

      selectionStep.value = 1
      statusMessage.value = `${w} × ${h}`
      scale.value = 1
      translateX.value = 0
      translateY.value = 0
    }
    img.src = (e.target as FileReader).result as string
  }
  reader.readAsDataURL(target.files[0]!)
}

// ── 滑鼠事件 ──────────────────────────────────────────────

const handleMouseMove = (event: MouseEvent) => {
  if (selectionStep.value === 0 || selectionStep.value === 4) return
  const canvas = mapCanvas.value
  if (!canvas) return
  const { x, y } = screenToCanvas(event.clientX, event.clientY)
  if (x < 0 || x >= canvas.width || y < 0 || y >= canvas.height) {
    showMagnifier.value = false
    return
  }
  showMagnifier.value = true
  magnifierPos.value = { x: event.clientX - 130, y: event.clientY - 130 }
  updateMagnifier(x, y)
}

const handleMouseLeave = () => {
  showMagnifier.value = false
}

const handleCanvasClick = (event: MouseEvent) => {
  const canvas = mapCanvas.value
  if (!canvas || selectionStep.value === 0 || selectionStep.value === 4) return
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  const { x, y } = screenToCanvas(event.clientX, event.clientY)
  if (x < 0 || x >= canvas.width || y < 0 || y >= canvas.height) return
  commitPoint(x, y, ctx)
}

// ── 標記點 ────────────────────────────────────────────────

function commitPoint(x: number, y: number, ctx: CanvasRenderingContext2D) {
  if (selectionStep.value === 1) {
    seedPoint.value = { x, y }
    mapStore.setSeedPoint(seedPoint.value)
    selectionStep.value = 2
  } else if (selectionStep.value === 2) {
    startPoint.value = { x, y }
    selectionStep.value = 3
  } else if (selectionStep.value === 3) {
    endPoint.value = { x, y }
    mapStore.setPoints(startPoint.value, endPoint.value)
    selectionStep.value = 4
  }
  redrawCanvas(ctx)
}

// ── 繪製 Canvas ───────────────────────────────────────────

function redrawCanvas(ctx: CanvasRenderingContext2D) {
  if (originalImageData.value) ctx.putImageData(originalImageData.value, 0, 0)

  const drawDot = (
    point: { x: number; y: number },
    color: string,
    label: string,
    shape: 'circle' | 'square' = 'circle',
  ) => {
    ctx.beginPath()
    if (shape === 'circle') ctx.arc(point.x, point.y, 9, 0, Math.PI * 2)
    else ctx.rect(point.x - 8, point.y - 8, 16, 16)
    ctx.fillStyle = 'white'
    ctx.fill()

    ctx.beginPath()
    if (shape === 'circle') ctx.arc(point.x, point.y, 6, 0, Math.PI * 2)
    else ctx.rect(point.x - 5, point.y - 5, 10, 10)
    ctx.fillStyle = color
    ctx.fill()

    ctx.fillStyle = color
    ctx.font = 'bold 12px sans-serif'
    ctx.fillText(label, point.x + 11, point.y - 5)
  }

  if (seedPoint.value) drawDot(seedPoint.value, '#2196F3', '種子')
  if (startPoint.value) drawDot(startPoint.value, '#4CAF50', '起點')
  if (endPoint.value) drawDot(endPoint.value, '#F44336', '終點')
}

// ── 重設 ──────────────────────────────────────────────────

function resetAll() {
  seedPoint.value = null
  startPoint.value = null
  endPoint.value = null
  if (selectionStep.value === 0) return
  selectionStep.value = 1
  mapStore.setSeedPoint(null)
  mapStore.setPoints(null, null)
  const canvas = mapCanvas.value
  if (canvas && originalImageData.value) {
    const ctx = canvas.getContext('2d')
    if (ctx) ctx.putImageData(originalImageData.value, 0, 0)
  }
}

function resetStartEnd() {
  if (selectionStep.value < 2) return
  startPoint.value = null
  endPoint.value = null
  selectionStep.value = 2
  mapStore.setPoints(null, null)
  const canvas = mapCanvas.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx) redrawCanvas(ctx)
  }
}

function resetZoom() {
  scale.value = 1
  translateX.value = 0
  translateY.value = 0
}

const goToProcess = () => router.push('/')
</script>

<template>
  <main class="upload-container">
    <h2>上傳地圖</h2>
    <p class="status">{{ statusMessage }}</p>

    <!-- 步驟條 -->
    <div class="steps-bar" v-if="selectionStep > 0">
      <div class="step" :class="{ active: selectionStep >= 1, done: selectionStep > 1 }">
        種子點
      </div>
      <div class="step-sep">—</div>
      <div class="step" :class="{ active: selectionStep >= 2, done: selectionStep > 2 }">起點</div>
      <div class="step-sep">—</div>
      <div class="step" :class="{ active: selectionStep >= 3, done: selectionStep > 3 }">終點</div>
    </div>

    <!-- 步驟提示 + 按鈕列 -->
    <div class="action-bar" v-if="selectionStep > 0">
      <span class="step-msg">{{ stepMessages[selectionStep] }}</span>
      <div class="btn-group">
        <button v-if="selectionStep > 2" class="btn-sm" @click="resetStartEnd">重設起訖點</button>
        <button v-if="selectionStep > 1" class="btn-sm" @click="resetAll">全部重設</button>
        <button v-if="scale > 1.05" class="btn-sm" @click="resetZoom">重置縮放</button>
      </div>
    </div>

    <!-- 路色預覽 -->
    <div class="color-row" v-if="mapStore.pathColor">
      <div class="color-chip">
        <span
          class="swatch"
          :style="{
            background: `rgb(${mapStore.pathColor.r},${mapStore.pathColor.g},${mapStore.pathColor.b})`,
          }"
        ></span>
        路色　rgb({{ mapStore.pathColor.r }}, {{ mapStore.pathColor.g }},
        {{ mapStore.pathColor.b }})
      </div>
    </div>

    <div class="map-type-row" v-if="mapStore.imageRawData">
      <span class="type-label">地圖類型</span>
      <div class="type-toggle">
        <button
          class="btn-type"
          :class="{ active: mapStore.mapType === 'color-block' }"
          @click="mapStore.setMapType('color-block')"
        >
          色塊圖
        </button>
        <button
          class="btn-type"
          :class="{ active: mapStore.mapType === 'line-art' }"
          @click="mapStore.setMapType('line-art')"
        >
          線稿圖
        </button>
      </div>
    </div>

    <div class="input-section">
      <input type="file" accept="image/png, image/jpeg" @change="handleFileUpload" />
    </div>

    <!-- Canvas 區域 -->
    <div
      ref="canvasWrapper"
      class="canvas-wrapper"
      :class="{ locked: selectionStep === 4 || selectionStep === 0 }"
    >
      <canvas
        ref="mapCanvas"
        :style="{
          transform: `translate(${translateX}px, ${translateY}px) scale(${scale})`,
          transformOrigin: 'center center',
          cursor: selectionStep > 0 && selectionStep < 4 ? 'crosshair' : 'default',
        }"
        @click="handleCanvasClick"
        @mousemove="handleMouseMove"
        @mouseleave="handleMouseLeave"
      ></canvas>
    </div>

    <!-- 放大鏡 -->
    <teleport to="body">
      <canvas
        v-show="showMagnifier"
        ref="magnifier"
        width="120"
        height="120"
        class="magnifier"
        :style="{ left: magnifierPos.x + 'px', top: magnifierPos.y + 'px' }"
      ></canvas>
    </teleport>

    <!-- 完成後 -->
    <div v-if="selectionStep === 4" class="next-section">
      <button class="btn-primary" @click="goToProcess">執行路徑識別</button>
    </div>
  </main>
</template>

<style scoped>
.upload-container {
  max-width: 900px;
  margin: 0 auto;
  padding: var(--space-5);
  text-align: center;
}

h2 {
  font-size: var(--text-h2);
  font-weight: var(--font-bold);
  color: var(--color-text);
  margin-bottom: var(--space-2);
}

.status {
  color: var(--color-text-muted);
  font-size: var(--text-base);
  margin-bottom: var(--space-3);
}

/* 步驟條 */
.steps-bar {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  margin: var(--space-3) 0;
}

.step {
  padding: var(--space-1) var(--space-4);
  border-radius: var(--radius-pill);
  background: var(--color-bg-neutral);
  color: var(--color-text-faded);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  transition: 0.2s;
}

.step.active {
  background: var(--color-info-bg);
  color: var(--color-primary);
}
.step.done {
  background: var(--color-success-bg);
  color: var(--color-success-text);
}
.step-sep {
  color: var(--color-text-tick);
  font-size: var(--text-base);
}

/* 操作列 */
.action-bar {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
  flex-wrap: wrap;
  margin: var(--space-2) 0 var(--space-3);
}

.step-msg {
  font-size: var(--text-base);
  color: var(--color-text-tertiary);
  font-weight: var(--font-medium);
}

.btn-group {
  display: flex;
  gap: 5px;
}

.btn-sm {
  padding: var(--space-1) var(--space-3);
  font-size: var(--text-sm);
  font-weight: var(--font-semibold);
  background: white;
  border: 1px solid var(--color-text-hint);
  border-radius: var(--radius-sm);
  cursor: pointer;
  color: var(--color-text-soft);
  transition: background 0.15s;
  touch-action: manipulation;
}
.btn-sm:hover {
  background: var(--color-bg-hover);
}

/* 路色預覽 */
.color-row {
  display: flex;
  justify-content: center;
  margin-bottom: var(--space-3);
}

.color-chip {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  font-size: var(--text-sm);
  color: var(--color-text-soft);
  background: var(--color-bg-soft);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-3);
}

.swatch {
  display: inline-block;
  width: 14px;
  height: 14px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--color-text-placeholder);
  flex-shrink: 0;
}

/* 上傳 */
.input-section {
  margin-bottom: var(--space-3);
}

input[type='file'] {
  padding: var(--space-2);
  border: 1px solid var(--color-text-tick);
  border-radius: var(--radius-sm);
  cursor: pointer;
  font-size: var(--text-base);
  touch-action: manipulation;
}

/* Canvas wrapper
   使用 flex + align-items:center 讓圖片在容器中置中
   不設 max-height 截斷，讓圖片完整顯示後再可捲動 */
.canvas-wrapper {
  position: relative;
  overflow: auto; /* 圖片超出時可捲動，不裁切 */
  border: 1px dashed var(--color-text-hint);
  border-radius: var(--radius-md);
  background: var(--color-bg-soft);
  display: flex;
  align-items: flex-start;
  justify-content: center;
  touch-action: none;
  user-select: none;
  -webkit-user-select: none;
}

.canvas-wrapper.locked {
  opacity: 0.88;
}

canvas {
  display: block;
  /* 寬度自適應容器，高度等比縮放，圖片完整顯示 */
  max-width: 100%;
  height: auto;
  background: white;
  will-change: transform;
}

/* 放大鏡 */
:global(.magnifier) {
  position: fixed;
  pointer-events: none;
  border-radius: var(--radius-circle);
  border: 2px solid var(--color-text-soft);
  box-shadow: var(--shadow-magnifier);
  z-index: 9999;
  background: white;
}

/* 下一步 */
.next-section {
  margin-top: var(--space-5);
}

.btn-primary {
  padding: var(--space-3) var(--space-8);
  font-size: var(--text-md);
  font-weight: var(--font-bold);
  background: var(--color-primary);
  color: white;
  border: none;
  border-radius: var(--radius-md);
  cursor: pointer;
  transition: background 0.2s;
  touch-action: manipulation;
}
.btn-primary:hover {
  background: var(--color-primary-hover);
}

.next-section {
  display: flex;
  gap: 10px;
  justify-content: center;
  flex-wrap: wrap;
}

.landmark-section {
  display: flex;
  justify-content: center;
  margin: var(--space-2) 0;
}

.btn-secondary {
  padding: var(--space-3) var(--space-6);
  font-size: var(--text-md);
  font-weight: var(--font-semibold);
  background: white;
  color: var(--color-primary);
  border: 1px solid var(--color-primary);
  border-radius: var(--radius-md);
  cursor: pointer;
  touch-action: manipulation;
}
.btn-secondary:hover {
  background: var(--color-info-bg);
}

.lm-modal-mask {
  position: fixed;
  inset: 0;
  background: var(--color-overlay);
  z-index: 9998;
  display: flex;
  align-items: center;
  justify-content: center;
  padding: var(--space-5);
}

.lm-modal-box {
  position: relative;
  background: white;
  border-radius: var(--radius-lg);
  padding: var(--space-6) var(--space-5) var(--space-5);
  max-width: 1000px;
  width: 100%;
  max-height: 90vh;
  overflow: auto;
}

.lm-modal-close {
  position: absolute;
  top: 8px;
  right: 10px;
  width: 28px;
  height: 28px;
  border: none;
  background: var(--color-bg-neutral);
  border-radius: var(--radius-circle);
  cursor: pointer;
  font-weight: var(--font-bold);
  color: var(--color-text-soft);
}
.lm-modal-close:hover {
  background: var(--color-border);
  color: var(--color-danger-dark);
}

/* 地圖類型列 */
.map-type-row {
  display: flex;
  align-items: center;
  justify-content: center;
  flex-wrap: wrap;
  gap: 8px;
  margin-bottom: var(--space-3);
  font-size: var(--text-base);
}

.type-label {
  color: var(--color-text-muted);
  font-weight: var(--font-semibold);
}

.type-badge {
  padding: 2px var(--space-3);
  border-radius: var(--radius-pill);
  font-weight: var(--font-bold);
  font-size: var(--text-md);
}

.badge-color {
  background: var(--color-info-bg);
  color: var(--color-primary);
}

.badge-line {
  background: var(--color-badge-line-bg);
  color: var(--color-badge-line-text);
}

.type-ratio {
  color: var(--color-text-faded);
}

.type-overridden {
  color: var(--color-override-text);
  font-size: var(--text-base);
}

.type-toggle {
  display: flex;
  gap: 4px;
}

.btn-type {
  padding: 2px var(--space-3);
  font-size: var(--text-sm);
  font-weight: var(--font-semibold);
  background: white;
  border: 1px solid var(--color-text-hint);
  border-radius: var(--radius-sm);
  cursor: pointer;
  color: var(--color-text-soft);
  transition:
    background 0.15s,
    border-color 0.15s;
  touch-action: manipulation;
}

.btn-type:hover {
  background: var(--color-bg-hover);
}

.btn-type.active {
  background: var(--color-primary);
  color: white;
  border-color: var(--color-primary);
}

.btn-reset-type {
  border-color: var(--color-override-text);
  color: var(--color-override-text);
}

.btn-reset-type:hover {
  background: var(--color-override-bg);
}
</style>
