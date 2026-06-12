<script setup lang="ts">
import { computed, ref, shallowRef, onMounted, onUnmounted, toRaw, nextTick } from 'vue'
import { useRouter } from 'vue-router'
import { FileImage, RotateCcw, UploadCloud, ZoomOut } from 'lucide-vue-next'
import { useMapStore } from '@/stores/mapStore'
import { useIsDesktop } from '@/composables/useBreakpoint'

const mapStore = useMapStore()
const router = useRouter()

// 桌面時控制面板 Teleport 到 App.vue 側欄的 #shell-panel;手機 inline 渲染。
const isDesktop = useIsDesktop()

const mapCanvas = ref<HTMLCanvasElement | null>(null)
const canvasWrapper = ref<HTMLDivElement | null>(null)
const magnifier = ref<HTMLCanvasElement | null>(null)

const statusMessage = ref<string>('請選擇一張室內平面圖')
const selectionStep = ref<number>(0)
// 自訂上傳卡片使用的檔名與拖曳狀態，不影響實際地圖資料。
const selectedFileName = ref('')
const isDraggingFile = ref(false)

// 多種子：步驟 1 可連續點多個路色種子（多色底圖需點多種走廊色）。
const seedPoints = ref<{ x: number; y: number }[]>([])
const startPoint = ref<{ x: number; y: number } | null>(null)
const endPoint = ref<{ x: number; y: number } | null>(null)

const originalImageData = shallowRef<ImageData | null>(null)

const showMagnifier = ref(false)
const magnifierPos = ref({ x: 0, y: 0 })

// Pinch-to-zoom / pan 狀態
// canvas 顯示用 CSS transform 縮放，原始像素座標仍維持不變，方便送給 WASM。
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
  1: '點擊地圖上的走廊路面採樣路色（可點多個顏色）',
  2: '點擊地圖標記起點',
  3: '點擊地圖標記終點',
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

    seedPoints.value = mapStore.seedPoints.map((s) => ({ ...s }))
    if (mapStore.startPoint) startPoint.value = { ...mapStore.startPoint }
    if (mapStore.endPoint) endPoint.value = { ...mapStore.endPoint }

    if (startPoint.value && endPoint.value) selectionStep.value = 4
    else if (startPoint.value) selectionStep.value = 3
    else selectionStep.value = 1 // 種子步驟可有 0+ 個，統一回到步驟 1（可續點或按下一步）

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

// 將使用者選取或拖放的圖片解碼成 ImageData，並縮放到 WASM 可接受的尺寸。
function processImageFile(file: File) {
  selectedFileName.value = file.name

  resetAll()
  statusMessage.value = '讀取中…'

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
  reader.readAsDataURL(file)
}

const handleFileUpload = (event: Event) => {
  const target = event.target as HTMLInputElement
  if (!target.files?.length) return
  processImageFile(target.files[0]!)
}

// 拖放和檔案選擇走同一個 processImageFile，避免兩套讀圖流程產生狀態差異。
function handleFileDrop(event: DragEvent) {
  isDraggingFile.value = false
  const file = event.dataTransfer?.files?.[0]
  if (!file || !file.type.startsWith('image/')) return
  processImageFile(file)
}

// dragleave 會在滑過子元素時觸發，需確認真的離開 upload card 才關閉高亮。
function handleDragLeave(event: DragEvent) {
  const current = event.currentTarget as HTMLElement
  const next = event.relatedTarget as Node | null
  if (!next || !current.contains(next)) isDraggingFile.value = false
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
  // 標記順序：種子點(可多個) → 起點 → 終點，種子點供 core.cpp 採樣路色。
  if (selectionStep.value === 1) {
    // 多種子：每點一下新增一個，停在步驟 1 等使用者按「下一步」。
    seedPoints.value = [...seedPoints.value, { x, y }]
    mapStore.addSeedPoint({ x, y })
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

// 種子標記完成 → 進入起點標記。
function finishSeeds() {
  if (!seedPoints.value.length) return
  selectionStep.value = 2
}

// 復原最後一個種子。
function undoLastSeed() {
  if (!seedPoints.value.length) return
  mapStore.removeSeedPoint(seedPoints.value.length - 1)
  seedPoints.value = seedPoints.value.slice(0, -1)
  const canvas = mapCanvas.value
  if (canvas) {
    const ctx = canvas.getContext('2d')
    if (ctx) redrawCanvas(ctx)
  }
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

  seedPoints.value.forEach((s, i) =>
    drawDot(s, '#2196F3', seedPoints.value.length > 1 ? `種子${i + 1}` : '種子'),
  )
  if (startPoint.value) drawDot(startPoint.value, '#4CAF50', '起點')
  if (endPoint.value) drawDot(endPoint.value, '#F44336', '終點')
}

// ── 重設 ──────────────────────────────────────────────────

function resetAll() {
  seedPoints.value = []
  startPoint.value = null
  endPoint.value = null
  if (selectionStep.value === 0) return
  selectionStep.value = 1
  mapStore.clearSeedPoints()
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

const goToProcess = () => router.push('/path')

// 單一情境式 CTA:步驟 1 推進到起點標記、全部完成後前往路徑識別,中間步驟顯示指引並反灰。
const ctaLabel = computed(() => {
  if (selectionStep.value === 1)
    return seedPoints.value.length
      ? `下一步：標記起點（已點 ${seedPoints.value.length} 個）`
      : '請先點選走廊路面'
  if (selectionStep.value === 2) return '請在地圖上點選起點'
  if (selectionStep.value === 3) return '請在地圖上點選終點'
  return '下一步：路徑識別'
})

const ctaEnabled = computed(
  () => (selectionStep.value === 1 && seedPoints.value.length > 0) || selectionStep.value === 4,
)

function onCtaClick() {
  if (selectionStep.value === 1) finishSeeds()
  else if (selectionStep.value === 4) goToProcess()
}
</script>

<template>
  <main class="upload-page">
    <!-- 空狀態:大虛線拖放區(mockup 樣式),整區可點擊或拖放上傳 -->
    <label
      v-if="mapStore.mapWidth === 0"
      class="drop-zone"
      :class="{ 'drop-zone--dragging': isDraggingFile }"
      @dragenter.prevent="isDraggingFile = true"
      @dragover.prevent="isDraggingFile = true"
      @dragleave.prevent="handleDragLeave"
      @drop.prevent="handleFileDrop"
    >
      <input
        class="visually-hidden"
        type="file"
        accept="image/png, image/jpeg"
        @change="handleFileUpload"
      />
      <span class="drop-icon" aria-hidden="true">
        <UploadCloud :size="32" :stroke-width="2" />
      </span>
      <strong class="drop-title">上傳平面圖以進行後續操作</strong>
      <span class="drop-sub">點擊框框中的任何位置進行上傳<br />支援 PNG、JPG</span>
    </label>

    <!-- 預覽區:canvas 常駐掛載(v-show),上傳繪製與 touch 監聽都依賴它存在 -->
    <section class="preview-pane" v-show="mapStore.mapWidth > 0">
      <div class="hint-bar" v-if="selectionStep > 0 && selectionStep < 4">
        {{ stepMessages[selectionStep] }}
      </div>
      <div class="hint-bar hint-bar--done" v-else-if="selectionStep === 4">
        標記完成，可前往路徑識別
      </div>

      <div class="canvas-shell">
        <div
          ref="canvasWrapper"
          class="canvas-wrapper"
          :class="{ locked: selectionStep === 4 || selectionStep === 0 }"
        >
          <canvas
            v-show="mapStore.mapWidth > 0"
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

        <!-- 浮動縮放重置:只在放大時出現,貼著地圖不佔面板空間 -->
        <button v-if="scale > 1.05" class="zoom-reset" type="button" @click="resetZoom">
          <ZoomOut :size="14" aria-hidden="true" />
          重置縮放
        </button>
      </div>
    </section>

    <!-- 控制面板:桌面投送到側欄,手機 inline 在預覽下方 -->
    <Teleport to="#shell-panel" :disabled="!isDesktop" defer>
      <div class="controls" v-if="mapStore.mapWidth > 0">
        <!-- 檔案資訊卡 -->
        <div class="file-card">
          <span class="file-icon" aria-hidden="true">
            <FileImage :size="18" :stroke-width="2" />
          </span>
          <div class="file-meta">
            <strong>{{ selectedFileName || '已載入平面圖' }}</strong>
            <span>{{ statusMessage }}</span>
          </div>
          <label class="btn btn--ghost file-rebtn">
            <input
              class="visually-hidden"
              type="file"
              accept="image/png, image/jpeg"
              @change="handleFileUpload"
            />
            重新上傳
          </label>
        </div>

        <!-- 標記子步驟:點擊已完成的列可重新標記該步驟(取代「全部重設 / 重設起訖點」按鈕) -->
        <div class="substeps" aria-label="標記進度">
          <div class="substep" :class="{ active: selectionStep === 1, done: selectionStep > 1 }">
            <button
              class="substep-main"
              type="button"
              :disabled="selectionStep <= 1"
              title="重新標記走廊路面（會清除所有標記）"
              @click="resetAll"
            >
              <span class="substep-dot" aria-hidden="true"></span>
              點選走廊路面（可點多個）
              <RotateCcw
                v-if="selectionStep > 1"
                class="substep-redo"
                :size="13"
                aria-hidden="true"
              />
            </button>
            <button
              v-if="selectionStep === 1 && seedPoints.length"
              class="substep-chip"
              type="button"
              title="復原最後一個種子"
              @click="undoLastSeed"
            >
              <RotateCcw :size="12" aria-hidden="true" />
              復原
            </button>
          </div>
          <div class="substep" :class="{ active: selectionStep === 2, done: selectionStep > 2 }">
            <button
              class="substep-main"
              type="button"
              :disabled="selectionStep <= 2"
              title="重新標記起點與終點"
              @click="resetStartEnd"
            >
              <span class="substep-dot" aria-hidden="true"></span>
              點選起點
              <RotateCcw
                v-if="selectionStep > 2"
                class="substep-redo"
                :size="13"
                aria-hidden="true"
              />
            </button>
          </div>
          <div class="substep" :class="{ active: selectionStep === 3, done: selectionStep > 3 }">
            <button
              class="substep-main"
              type="button"
              :disabled="selectionStep <= 3"
              title="重新標記起點與終點"
              @click="resetStartEnd"
            >
              <span class="substep-dot" aria-hidden="true"></span>
              點選終點
              <RotateCcw
                v-if="selectionStep > 3"
                class="substep-redo"
                :size="13"
                aria-hidden="true"
              />
            </button>
          </div>
        </div>

        <!-- 路色預覽(多種子) -->
        <div class="color-row" v-if="mapStore.pathColors.length">
          <div class="color-chip" v-for="(c, i) in mapStore.pathColors" :key="i">
            <span class="swatch" :style="{ background: `rgb(${c.r},${c.g},${c.b})` }"></span>
            rgb({{ c.r }}, {{ c.g }}, {{ c.b }})
          </div>
        </div>

        <!-- 單一情境式 CTA:依目前子步驟推進流程 -->
        <div class="flow-actions">
          <button
            class="btn btn--primary btn--pill"
            type="button"
            :disabled="!ctaEnabled"
            @click="onCtaClick"
          >
            {{ ctaLabel }}
          </button>
        </div>
      </div>
    </Teleport>

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
  </main>
</template>

<style scoped>
.upload-page {
  max-width: 1100px;
  margin: 0 auto;
}

/* ── 虛線拖放區(空狀態)──────────────────────────────────── */
.drop-zone {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--space-3);
  min-height: min(520px, 64dvh);
  padding: var(--space-8) var(--space-5);
  box-sizing: border-box;
  border: 2px dashed var(--color-border);
  border-radius: var(--radius-xl);
  background: var(--color-bg-soft);
  cursor: pointer;
  text-align: center;
  transition:
    border-color var(--dur-base) var(--ease-out),
    background-color var(--dur-base) var(--ease-out),
    box-shadow var(--dur-base) var(--ease-out);
}

.drop-zone:hover,
.drop-zone:focus-within {
  border-color: var(--color-primary);
  background: var(--color-bg-hover);
}

.drop-zone--dragging {
  border-color: var(--color-primary);
  background: var(--color-bg-hover);
  box-shadow: 0 0 0 4px var(--color-primary-ring);
}

.drop-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 64px;
  height: 64px;
  border-radius: var(--radius-lg);
  background: var(--color-primary);
  color: var(--color-white);
  margin-bottom: var(--space-2);
}

.drop-title {
  font-size: var(--text-md);
  font-weight: var(--font-semibold);
  color: var(--color-text);
}

.drop-sub {
  font-size: var(--text-sm);
  color: var(--color-text-muted);
  line-height: 1.7;
}

/* ── 預覽區 ─────────────────────────────────────────────── */
.hint-bar {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-2) var(--space-4);
  margin-bottom: var(--space-3);
  border-radius: var(--radius-md);
  background: var(--color-info-bg);
  color: var(--color-info-text);
  font-size: var(--text-sm);
  font-weight: var(--font-medium);
  line-height: 1.5;
}

.hint-bar--done {
  background: var(--color-success-bg);
  color: var(--color-success-text);
}

.canvas-shell {
  position: relative;
}

/* 浮動縮放重置:玻璃感小藥丸,貼著地圖右上角 */
.zoom-reset {
  position: absolute;
  top: var(--space-2);
  right: var(--space-2);
  z-index: 2;
  display: inline-flex;
  align-items: center;
  gap: var(--space-1);
  min-height: 32px;
  padding: var(--space-1) var(--space-3);
  border: 1px solid var(--glass-border);
  border-radius: var(--radius-pill);
  background: var(--glass-bg);
  backdrop-filter: blur(var(--blur-glass));
  -webkit-backdrop-filter: blur(var(--blur-glass));
  color: var(--color-text-secondary);
  font: inherit;
  font-size: var(--text-xs);
  font-weight: var(--font-semibold);
  cursor: pointer;
  transition:
    color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out);
}
.zoom-reset:hover {
  color: var(--color-primary);
  border-color: var(--color-primary);
}

/* Canvas wrapper
   使用 flex 讓圖片在容器中置中;不設 max-height 截斷,讓圖片完整顯示後再可捲動 */
.canvas-wrapper {
  position: relative;
  overflow: auto; /* 圖片超出時可捲動,不裁切 */
  min-height: 320px;
  border: 1px solid var(--color-border);
  border-radius: var(--radius-xl);
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
  /* 寬度自適應容器,高度等比縮放,圖片完整顯示 */
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
  background: var(--color-bg-card);
}

/* ── 控制面板(桌面 teleport 到側欄 / 手機 inline)────────── */
.controls {
  display: flex;
  flex-direction: column;
  gap: var(--space-4);
  text-align: left;
}

.file-card {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: var(--space-3);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-lg);
  background: var(--color-bg-card);
}

.file-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  flex: 0 0 36px;
  border-radius: var(--radius-md);
  background: var(--color-info-bg);
  color: var(--color-primary);
}

.file-meta {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.file-meta strong {
  font-size: var(--text-sm);
  color: var(--color-text);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.file-meta span {
  font-size: var(--text-xs);
  color: var(--color-text-muted);
}

.file-rebtn {
  min-height: 32px;
  padding: var(--space-1) var(--space-3);
  font-size: var(--text-xs);
  white-space: nowrap;
}

/* 標記子步驟 checklist */
.substeps {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
  padding: var(--space-3);
  border: 1px solid var(--color-border-light);
  border-radius: var(--radius-lg);
  background: var(--color-bg-soft);
}

.substep {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  font-size: var(--text-sm);
  color: var(--color-text-muted);
}

/* 子步驟列本體:已完成時可點擊重做,hover 提示 */
.substep-main {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  flex: 1;
  min-width: 0;
  padding: var(--space-1) var(--space-2);
  margin: calc(-1 * var(--space-1)) calc(-1 * var(--space-2));
  border: 0;
  border-radius: var(--radius-md);
  background: transparent;
  color: inherit;
  font: inherit;
  text-align: left;
  cursor: default;
}
.substep-main:not(:disabled) {
  cursor: pointer;
}
.substep-main:not(:disabled):hover {
  background: var(--color-bg-hover);
  color: var(--color-text);
}

.substep-redo {
  margin-left: auto;
  flex-shrink: 0;
  color: var(--color-text-faded);
}
.substep-main:not(:disabled):hover .substep-redo {
  color: var(--color-primary);
}

/* 復原最後一個種子的小 chip(只在標記種子時出現) */
.substep-chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  flex-shrink: 0;
  min-height: 26px;
  padding: 2px var(--space-2);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-pill);
  background: transparent;
  color: var(--color-text-soft);
  font: inherit;
  font-size: var(--text-xs);
  cursor: pointer;
  transition:
    color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out);
}
.substep-chip:hover {
  border-color: var(--color-primary);
  color: var(--color-primary);
}

.substep-dot {
  width: 10px;
  height: 10px;
  flex: 0 0 10px;
  box-sizing: border-box;
  border-radius: var(--radius-circle);
  border: 2px solid var(--color-border);
  background: transparent;
  transition:
    border-color var(--dur-base) var(--ease-out),
    background-color var(--dur-base) var(--ease-out);
}

.substep.active {
  color: var(--color-text);
  font-weight: var(--font-semibold);
}
.substep.active .substep-dot {
  border-color: var(--color-primary);
  background: var(--color-primary);
}

.substep.done {
  color: var(--color-success-text);
}
.substep.done .substep-dot {
  border-color: var(--color-success);
  background: var(--color-success);
}

/* 路色 chips */
.color-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
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

.flow-actions {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
}

/* 桌面:控制面板撐滿側欄剩餘高度,CTA 固定在底部。 */
@media (min-width: 900px) {
  .controls {
    flex: 1 0 auto;
    box-sizing: border-box;
    padding-bottom: var(--space-2);
  }

  .flow-actions {
    margin-top: auto;
    padding-top: var(--space-4);
  }
}

/* 手機:CTA sticky 在底部(mockup 的大藥丸按鈕)。 */
@media (max-width: 899.98px) {
  .canvas-wrapper {
    min-height: 260px;
    max-height: 62dvh;
  }

  /* inline 渲染時與上方預覽區保持間距 */
  .controls {
    margin-top: var(--space-4);
  }

  .drop-zone {
    min-height: min(420px, 58dvh);
  }

  /* 貼底實底工具列:半透明底+模糊,捲動時內容從後方通過不透字 */
  .flow-actions {
    position: sticky;
    bottom: 0;
    z-index: 5;
    margin-top: var(--space-2);
    margin-inline: calc(-1 * var(--space-4));
    padding: var(--space-2) var(--space-4) var(--space-3);
    background: color-mix(in srgb, var(--color-bg-page) 92%, transparent);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
  }
}
</style>
