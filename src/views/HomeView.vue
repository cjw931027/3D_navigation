<script setup lang="ts">
import { computed, ref, toRaw } from 'vue'
import { ImageOff, Upload } from 'lucide-vue-next'
import { useMapStore } from '@/stores/mapStore'

const mapStore = useMapStore()
const processTime = ref<number | null>(null)
const isRunning = ref(false)
const previewCanvas = ref<HTMLCanvasElement | null>(null)

const activeTooltip = ref<string | null>(null)
const pathStatus = ref<string | null>(null)

// 輕量 toast：取代原生 alert()。顯示一段時間後自動淡出。
const toastMsg = ref<string | null>(null)
let toastTimer: ReturnType<typeof setTimeout> | null = null
function showToast(msg: string) {
  toastMsg.value = msg
  if (toastTimer) clearTimeout(toastTimer)
  toastTimer = setTimeout(() => (toastMsg.value = null), 2600)
}

// 只有遮罩與路徑節點都存在時，3D 場景才有足夠資料可以建模。
const canGoToScene = computed(() => mapStore.passableMask != null && mapStore.pathNodes.length > 0)

// 進階參數直接對應 core.cpp 的 flood fill 前處理參數，文字提示說明何時調整。
const tooltips: Record<string, { problem: string; fix: string }> = {
  pathColorTolerance: {
    problem: '識別到的區域太小、路徑顏色不均勻',
    fix: '往右（數值調高）；若識別到太多不相關區域則往左',
  },
  closingKernelSize: {
    problem: '路上有文字或圖示造成路徑中斷',
    fix: '往右（數值調高）；若路徑邊緣模糊則往左',
  },
  wallThicken: {
    problem: '路徑從牆壁細縫漏出、蔓延到不該去的區域',
    fix: '往右（數值調高）；若路徑被截斷、範圍偏小則往左',
  },
  sampleRadius: {
    problem: '種子點採到錯誤顏色（點到邊緣或雜色位置）',
    fix: '往右（數值調高）；若路面顏色不均勻採色不準則往左',
  },
}

const paramLabels: Record<string, string> = {
  pathColorTolerance: '路色容差',
  closingKernelSize: '斷點填補',
  wallThicken: '牆壁加厚',
  sampleRadius: '採色半徑',
}

const paramConfig: Record<string, { min: number; max: number; step: number }> = {
  pathColorTolerance: { min: 5, max: 80, step: 1 },
  closingKernelSize: { min: 1, max: 9, step: 2 },
  wallThicken: { min: 0, max: 5, step: 1 },
  sampleRadius: { min: 3, max: 18, step: 1 },
}

// 將 A* 輸出的 pathNodes 疊回預覽圖。
// 路線用鮮紅 #FF3B30，與可走區的青藍 (0,200,255) 強對比；外加白色外框，在深淺背景上都醒目。
// 畫在可走區「上方」（正常 source-over，不用 destination-over）→ 不被半透明青藍蓋住而暗淡。
// 線寬隨 canvas 解析度自適應：canvas 內部解析度 = 圖原始尺寸（可能很大），但 CSS 縮放顯示，
// 若用固定 px，大圖在畫面上會變極細。改用「長邊的比例」+ 下限，確保各種尺寸畫面上都一致夠粗。
function drawPath(ctx: CanvasRenderingContext2D) {
  const nodes = mapStore.pathNodes
  if (nodes.length < 2) return

  const base = Math.max(ctx.canvas.width, ctx.canvas.height)
  const lineW = Math.max(3, base * 0.006) // 紅線寬：約長邊 0.6%，最少 3px
  const outlineW = lineW + Math.max(2, base * 0.004) // 白外框比紅線再寬一圈

  const trace = () => {
    ctx.beginPath()
    ctx.moveTo(nodes[0]!.x, nodes[0]!.y)
    for (let i = 1; i < nodes.length; i++) ctx.lineTo(nodes[i]!.x, nodes[i]!.y)
  }

  ctx.save()
  ctx.lineJoin = 'round'
  ctx.lineCap = 'round'

  // 白色外框（先畫，較寬）
  trace()
  ctx.strokeStyle = 'rgba(255,255,255,0.95)'
  ctx.lineWidth = outlineW
  ctx.stroke()

  // 鮮紅主線（後畫，較窄，疊在外框上）
  trace()
  ctx.strokeStyle = '#FF3B30'
  ctx.lineWidth = lineW
  ctx.stroke()

  ctx.restore()
}

// 起點與終點只畫在畫面層，不回寫到 imageRawData，避免下一次辨識把標記當成路色。
function drawDot(
  ctx: CanvasRenderingContext2D,
  point: { x: number; y: number },
  color: string,
  label: string,
) {
  ctx.beginPath()
  ctx.arc(point.x, point.y, 9, 0, Math.PI * 2)
  ctx.fillStyle = 'white'
  ctx.fill()
  ctx.beginPath()
  ctx.arc(point.x, point.y, 6, 0, Math.PI * 2)
  ctx.fillStyle = color
  ctx.fill()
  ctx.fillStyle = color
  ctx.font = 'bold 13px sans-serif'
  ctx.fillText(label, point.x + 11, point.y - 5)
}

// 預覽圖每次重算後重畫路徑與起訖點，確保顯示結果和目前 store 狀態一致。
function drawOverlay(ctx: CanvasRenderingContext2D) {
  if (mapStore.pathNodes.length >= 2) drawPath(ctx)
  if (mapStore.startPoint) drawDot(ctx, mapStore.startPoint, '#4CAF50', '起點')
  if (mapStore.endPoint) drawDot(ctx, mapStore.endPoint, '#F44336', '終點')
}

// WASM 回傳的是 RGBA buffer，這裡轉成 canvas 可顯示的 ImageData。
function renderToCanvas(
  buffer: Uint8ClampedArray,
  width: number,
  height: number,
): CanvasRenderingContext2D | null {
  const canvas = previewCanvas.value
  if (!canvas) return null
  canvas.width = width
  canvas.height = height
  const ctx = canvas.getContext('2d')
  if (!ctx) return null
  ctx.putImageData(new ImageData(buffer as any, width, height), 0, 0)
  return ctx
}

// 呼叫 core.cpp 的 intelligentFloodFill，產生可通行遮罩後立即執行 A*。
const runFloodFill = () => {
  if (!mapStore.wasmModule) return showToast('引擎尚未就緒')
  const rawData = toRaw(mapStore.imageRawData)
  if (!rawData || mapStore.mapWidth === 0) return showToast('尚未載入地圖')
  if (!mapStore.seedPoints.length) return showToast('缺少種子點，請回上傳頁重新標記')

  const width = mapStore.mapWidth
  const height = mapStore.mapHeight
  const p = mapStore.floodFillParams
  const up = mapStore.upscaleFactor

  const W2 = width * up
  const H2 = height * up
  const size2 = W2 * H2 * 4

  isRunning.value = true
  pathStatus.value = null

  try {
    const t0 = performance.now()

    // 最近鄰上採樣，放大後的 buffer 送 WASM。
    let sendData: Uint8ClampedArray
    if (up === 1) {
      sendData = rawData
    } else {
      sendData = new Uint8ClampedArray(size2)
      for (let y = 0; y < H2; y++) {
        const sy = (y / up) | 0
        for (let x = 0; x < W2; x++) {
          const sx = (x / up) | 0
          const si = (sy * width + sx) * 4
          const di = (y * W2 + x) * 4
          sendData[di] = rawData[si]!
          sendData[di + 1] = rawData[si + 1]!
          sendData[di + 2] = rawData[si + 2]!
          sendData[di + 3] = rawData[si + 3]!
        }
      }
    }

    // mapBuffer 是 WASM 端共用輸入記憶體，JS 需先寫入 RGBA 後再呼叫 C++。
    const pointer = mapStore.wasmModule.allocateMemory(size2) as number
    mapStore.wasmModule.HEAPU8.set(sendData, pointer)

    const denoiseArea = mapStore.denoiseMinArea * up * up
    // smooth* 參數隨上採樣比例縮放：closing kernel 維持奇數（保 dilate/erode 對稱），
    // 牆塊面積閾值以 up² 倍率擴張（與 denoiseArea 同一個邏輯）。
    const smoothClose = (() => {
      const v = Math.round(p.smoothClosingSize * up)
      return v <= 1 ? 0 : v % 2 === 0 ? v + 1 : v
    })()
    const smoothMinWall = p.smoothMinWallArea * up * up
    // 多種子：傳 seedXs / seedYs 陣列 + clipMode 給新版 WASM 介面。
    const seedXs = mapStore.seedPoints.map((s) => Math.round(s.x * up))
    const seedYs = mapStore.seedPoints.map((s) => Math.round(s.y * up))
    mapStore.wasmModule.intelligentFloodFill(
      W2,
      H2,
      seedXs,
      seedYs,
      p.pathColorTolerance,
      p.closingKernelSize,
      p.wallThicken,
      p.sampleRadius,
      denoiseArea,
      p.spanThreshold,
      smoothClose,
      smoothMinWall,
      mapStore.clipMode,
    )

    // 放大尺寸結果取出後，最近鄰下採樣回原尺寸供顯示。
    const resultUp = new Uint8ClampedArray(size2)
    resultUp.set(
      new Uint8ClampedArray(mapStore.wasmModule.HEAPU8.buffer as ArrayBuffer, pointer, size2),
    )

    let displayBuffer: Uint8ClampedArray
    if (up === 1) {
      displayBuffer = resultUp
    } else {
      displayBuffer = new Uint8ClampedArray(width * height * 4)
      for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++) {
          const sx = x * up
          const sy = y * up
          const si = (sy * W2 + sx) * 4
          const di = (y * width + x) * 4
          displayBuffer[di] = resultUp[si]!
          displayBuffer[di + 1] = resultUp[si + 1]!
          displayBuffer[di + 2] = resultUp[si + 2]!
          displayBuffer[di + 3] = resultUp[si + 3]!
        }
      }
    }
    mapStore.floodFillResultData = displayBuffer

    // passableMask 由 core.cpp 保存在全域 buffer，複製一份到 Pinia 供 3D 場景使用。
    const maskPtr = mapStore.wasmModule.getPassableMaskBuffer() as number
    const maskLen = mapStore.wasmModule.getPassableMaskSize() as number
    const maskW = mapStore.wasmModule.getPassableMaskWidth() as number
    const maskH = mapStore.wasmModule.getPassableMaskHeight() as number
    const maskCopy = new Uint8Array(
      mapStore.wasmModule.HEAPU8.buffer as ArrayBuffer,
      maskPtr,
      maskLen,
    ).slice()
    mapStore.setPassableMask(maskCopy, maskW, maskH)

    mapStore.wasmModule.freeMemory()
    processTime.value = Math.round(performance.now() - t0)

    // g_passableMask 位於 WASM 全域，freeMemory 後仍可供 A* 使用。
    let nodeCount = 0
    if (mapStore.startPoint && mapStore.endPoint) {
      nodeCount = mapStore.runAStar()
    }

    const ctx = renderToCanvas(displayBuffer, width, height)
    if (ctx) drawOverlay(ctx)

    if (nodeCount > 0) {
      pathStatus.value = `路徑長度 ${nodeCount} 段`
    } else if (mapStore.startPoint && mapStore.endPoint) {
      pathStatus.value = '找不到路徑，請確認起訖點位於可通行區域'
    }
  } catch (err) {
    console.error('runFloodFill 失敗:', err)
    pathStatus.value = '運算發生錯誤，請查看 console'
  } finally {
    isRunning.value = false
  }
}

// 只重新跑 A*，保留上一次 flood fill 的遮罩，適合調整起訖點後快速重算。
const runAStarOnly = () => {
  if (!mapStore.wasmModule || !mapStore.isEngineReady) return
  if (!mapStore.startPoint || !mapStore.endPoint) return showToast('請先設定起點與終點')
  if (!mapStore.floodFillResultData) return showToast('請先執行一次路徑識別，再使用重算路徑')

  isRunning.value = true
  pathStatus.value = null

  try {
    const nodeCount = mapStore.runAStar()

    const ctx = renderToCanvas(mapStore.floodFillResultData, mapStore.mapWidth, mapStore.mapHeight)
    if (ctx) drawOverlay(ctx)

    if (nodeCount > 0) {
      pathStatus.value = `路徑長度 ${nodeCount} 段`
    } else {
      pathStatus.value = '找不到路徑，請確認起訖點位於可通行區域'
    }
  } catch (err) {
    console.error('runAStarOnly 失敗:', err)
    pathStatus.value = '運算發生錯誤，請查看 console'
  } finally {
    isRunning.value = false
  }
}
</script>

<template>
  <main class="home-container">
    <h1>路徑識別</h1>

    <div class="empty-state" v-if="mapStore.mapWidth === 0">
      <div class="empty-icon">
        <ImageOff :size="48" :stroke-width="1.5" />
      </div>
      <h2 class="empty-title">還沒有載入地圖</h2>
      <p class="empty-desc">
        請先上傳室內平面圖並標記起點與終點，<br />
        系統才能識別可通行路徑。
      </p>
      <RouterLink to="/upload" class="empty-action">
        <Upload :size="16" />
        前往上傳地圖
      </RouterLink>
    </div>

    <template v-else>
      <div class="panel">
        <!-- 路色預覽（多種子）-->
        <div class="color-row">
          <template v-if="mapStore.pathColors.length">
            <div class="color-chip" v-for="(c, i) in mapStore.pathColors" :key="i">
              <span class="swatch" :style="{ background: `rgb(${c.r},${c.g},${c.b})` }"></span>
              rgb({{ c.r }}, {{ c.g }}, {{ c.b }})
            </div>
          </template>
          <div class="color-chip muted" v-else>路色未採樣</div>
        </div>

        <!-- 圖外白底處理（戶外圖用）：白路與圖外背景同色時，把圖外裁掉 -->
        <div class="clip-row">
          <span class="clip-label">圖外白底處理</span>
          <select
            class="clip-select"
            :value="mapStore.clipMode"
            @change="mapStore.setClipMode(Number(($event.target as HTMLSelectElement).value) as 0 | 1 | 2)"
          >
            <option :value="0">無（室內多色底圖）</option>
            <option :value="1">牆包圍盒（建物密集的戶外圖）</option>
            <option :value="2">灰色外框（院區圖有外框線）</option>
          </select>
        </div>

        <!-- 靈敏度 -->
        <div class="section-label" style="margin-top: 20px">
          辨識靈敏度
          <span class="sens-badge">{{ mapStore.sensitivity }}</span>
        </div>

        <div class="sensitivity-wrap">
          <div class="sensitivity-row">
            <span class="sens-end-label">精細</span>
            <div class="slider-with-ticks">
              <input
                type="range"
                :value="mapStore.sensitivity"
                @input="mapStore.setSensitivity(Number(($event.target as HTMLInputElement).value))"
                min="1"
                max="10"
                step="1"
                class="sensitivity-slider"
              />
              <div class="tick-row">
                <span
                  v-for="n in 10"
                  :key="n"
                  class="tick"
                  :class="{ active: mapStore.sensitivity === n }"
                  >{{ n }}</span
                >
              </div>
            </div>
            <span class="sens-end-label">寬鬆</span>
          </div>
          <div class="sensitivity-guide">
            <span>識別範圍太小 → 往右</span>
            <span>識別到多餘區域 → 往左</span>
          </div>
        </div>

        <!-- 進階參數（折疊） -->
        <div class="advanced-toggle" @click="mapStore.showAdvanced = !mapStore.showAdvanced">
          進階參數
          <span class="toggle-arrow">{{ mapStore.showAdvanced ? '▲' : '▼' }}</span>
        </div>

        <div class="advanced-panel" v-if="mapStore.showAdvanced">
          <div
            v-for="key in Object.keys(paramLabels) as (keyof typeof paramLabels)[]"
            :key="key"
            class="param-row"
          >
            <div class="param-label">
              <span class="param-name">
                {{ paramLabels[key] }}
                <span
                  class="tooltip-trigger"
                  @mouseenter="activeTooltip = key"
                  @mouseleave="activeTooltip = null"
                  @touchstart.prevent="activeTooltip = activeTooltip === key ? null : key"
                  >?</span
                >
                <div class="tooltip-box" v-if="activeTooltip === key">
                  <p><strong>什麼情況調整：</strong>{{ tooltips[key]!.problem }}</p>
                  <p><strong>如何調整：</strong>{{ tooltips[key]!.fix }}</p>
                </div>
              </span>
              <strong>{{
                mapStore.floodFillParams[key as keyof typeof mapStore.floodFillParams]
              }}</strong>
            </div>
            <input
              type="range"
              :value="mapStore.floodFillParams[key as keyof typeof mapStore.floodFillParams]"
              @input="
                mapStore.setFloodFillParams({
                  [key]: Number(($event.target as HTMLInputElement).value),
                })
              "
              :min="paramConfig[key]!.min"
              :max="paramConfig[key]!.max"
              :step="paramConfig[key]!.step"
            />
          </div>

          <!-- 遮罩過濾 -->
          <div class="preprocess-section">
            <div class="preprocess-label">遮罩過濾</div>
            <div class="param-row">
              <div class="param-label">
                <span class="param-name">破碎區塊門檻（像素數）</span>
                <strong>{{ mapStore.denoiseMinArea }}</strong>
              </div>
              <input
                type="range"
                :value="mapStore.denoiseMinArea"
                @input="mapStore.denoiseMinArea = Number(($event.target as HTMLInputElement).value)"
                min="0"
                max="400"
                step="10"
              />
              <span class="toggle-hint"
                >BFS 後自動清除遮罩中面積過小的孤立連通域，設為 0 可關閉</span
              >
            </div>

            <div class="param-row">
              <div class="param-label">
                <span class="param-name">平滑：補洞核大小</span>
                <strong>{{ mapStore.floodFillParams.smoothClosingSize }}</strong>
              </div>
              <input
                type="range"
                :value="mapStore.floodFillParams.smoothClosingSize"
                @input="
                  mapStore.setFloodFillParams({
                    smoothClosingSize: Number(($event.target as HTMLInputElement).value),
                  })
                "
                min="1"
                max="9"
                step="2"
              />
              <span class="toggle-hint"
                >走廊內小字 / 圖示形成的小洞會被填補；偵測到的真牆（buildWallMask）絕不會被覆蓋。設為 1 可關閉</span
              >
            </div>

            <div class="param-row">
              <div class="param-label">
                <span class="param-name">平滑：孤立小牆面積</span>
                <strong>{{ mapStore.floodFillParams.smoothMinWallArea }}</strong>
              </div>
              <input
                type="range"
                :value="mapStore.floodFillParams.smoothMinWallArea"
                @input="
                  mapStore.setFloodFillParams({
                    smoothMinWallArea: Number(($event.target as HTMLInputElement).value),
                  })
                "
                min="0"
                max="1200"
                step="25"
              />
              <span class="toggle-hint"
                >走廊內面積小於此值、且「被可走區包圍、不接觸真牆」的障礙(樓梯 / 閘門 / 動線 / 文字圖示啃出的缺口)會被翻為可走；真牆與獨立非路面塊一律保留。設為 0 可關閉</span
              >
            </div>
          </div>
        </div>

        <!-- 按鈕列 -->
        <div class="btn-row">
          <button
            class="btn-run"
            @click="runFloodFill"
            :disabled="!mapStore.isEngineReady || isRunning"
          >
            {{ isRunning ? '運算中...' : '執行路徑識別' }}
          </button>
          <button
            class="btn-astar"
            @click="runAStarOnly"
            :disabled="
              !mapStore.isEngineReady ||
              isRunning ||
              !mapStore.startPoint ||
              !mapStore.endPoint ||
              !mapStore.floodFillResultData
            "
            title="保留目前識別結果，重新計算起訖點之間的最短路徑"
          >
            重算路徑
          </button>
        </div>

        <!-- 結果資訊列 -->
        <div class="result-row" v-if="processTime !== null || pathStatus !== null">
          <span class="result-time" v-if="processTime !== null">
            耗時 <strong>{{ processTime }}</strong> 毫秒
          </span>
          <span
            class="result-path"
            :class="{ error: pathStatus?.startsWith('找不到') || pathStatus?.startsWith('運算') }"
            v-if="pathStatus"
          >
            {{ pathStatus }}
          </span>
        </div>
      </div>

      <div class="canvas-container">
        <canvas ref="previewCanvas"></canvas>
      </div>

      <div class="flow-actions">
        <RouterLink to="/upload" class="flow-btn flow-btn--secondary">上一步：上傳地圖</RouterLink>
        <RouterLink v-if="canGoToScene" to="/scene" class="flow-btn flow-btn--primary">
          下一步：3D 場景
        </RouterLink>
        <button v-else class="flow-btn flow-btn--primary" type="button" disabled>
          下一步：3D 場景
        </button>
      </div>
    </template>

    <!-- 運算中遮罩:取代「卡住感」,顯示 spinner + 文案。覆蓋整頁、含模糊。 -->
    <Transition name="fade">
      <div v-if="isRunning" class="loading-overlay">
        <div class="loading-card">
          <span class="spinner" aria-hidden="true" />
          <span class="loading-text">路徑識別運算中…</span>
        </div>
      </div>
    </Transition>

    <!-- 輕量 toast(取代原生 alert)。放在 main 內讓本元件維持單一根節點
         (外層 <Transition> 過場要求單根),Teleport 仍會把內容送到 body。 -->
    <Teleport to="body">
      <Transition name="toast">
        <div v-if="toastMsg" class="app-toast" role="status">{{ toastMsg }}</div>
      </Transition>
    </Teleport>
  </main>
</template>

<style scoped>
.home-container {
  position: relative;
  padding: var(--space-8);
  max-width: 900px;
  margin: 0 auto;
  text-align: center;
}

/* 運算中遮罩 */
.loading-overlay {
  position: fixed;
  inset: 0;
  z-index: 50;
  display: flex;
  align-items: center;
  justify-content: center;
  background: color-mix(in srgb, var(--color-bg-page) 60%, transparent);
  backdrop-filter: blur(4px);
  -webkit-backdrop-filter: blur(4px);
}
.loading-card {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: var(--space-4);
  padding: var(--space-6) var(--space-8);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-xl);
  box-shadow: var(--shadow-lg);
}
.spinner {
  width: 36px;
  height: 36px;
  border: 3px solid var(--color-bg-neutral);
  border-top-color: var(--color-primary);
  border-radius: var(--radius-circle);
  animation: spin 0.8s linear infinite;
}
@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}
.loading-text {
  font-size: var(--text-md);
  font-weight: var(--font-semibold);
  color: var(--color-text-secondary);
}

/* 全站 toast(此元件 teleport 到 body,故樣式不受 scoped 限制 → 用 :global) */
:global(.app-toast) {
  position: fixed;
  left: 50%;
  bottom: 32px;
  transform: translateX(-50%);
  z-index: 9999;
  max-width: min(90vw, 420px);
  padding: 12px 20px;
  background: var(--color-text);
  color: var(--color-bg-card);
  border-radius: var(--radius-pill);
  box-shadow: var(--shadow-lg);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  text-align: center;
}
:global(.toast-enter-active),
:global(.toast-leave-active) {
  transition:
    opacity var(--dur-base) var(--ease-out),
    transform var(--dur-base) var(--ease-out);
}
:global(.toast-enter-from),
:global(.toast-leave-to) {
  opacity: 0;
  transform: translateX(-50%) translateY(12px);
}
.fade-enter-active,
.fade-leave-active {
  transition: opacity var(--dur-base) var(--ease-out);
}
.fade-enter-from,
.fade-leave-to {
  opacity: 0;
}

h1 {
  font-size: var(--text-h1);
  font-weight: var(--font-bold);
  color: var(--color-text);
  margin-bottom: var(--space-5);
}

.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  max-width: 400px;
  margin: 0 auto;
  padding: var(--space-10) var(--space-5);
}

.empty-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 80px;
  height: 80px;
  border-radius: var(--radius-circle);
  background: var(--color-bg-soft);
  color: var(--color-text-muted);
}

.empty-title {
  margin: var(--space-4) 0 var(--space-2);
  color: var(--color-text);
  font-size: 18px;
  font-weight: var(--font-semibold);
}

.empty-desc {
  margin: 0 0 var(--space-5);
  color: var(--color-text-muted);
  font-size: var(--text-base);
  line-height: 1.6;
  text-align: center;
}

.empty-action {
  display: inline-flex;
  align-items: center;
  gap: var(--space-2);
  min-height: 44px;
  padding: var(--space-2) var(--space-5);
  border-radius: var(--radius-md);
  background: var(--gradient-primary);
  color: var(--color-white);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  text-decoration: none;
  box-shadow: var(--shadow-sm);
  transition:
    filter var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out),
    box-shadow var(--dur-fast) var(--ease-out);
}

.empty-action:hover {
  filter: brightness(1.05);
  transform: translateY(-1px);
  box-shadow: var(--shadow-md);
}
.empty-action:active {
  transform: translateY(0) scale(0.98);
}

.panel {
  margin: 0 auto var(--space-5);
  padding: var(--space-6);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-lg);
  max-width: 760px;
  background: var(--color-white);
  box-shadow: var(--shadow-sm);
  text-align: left;
}

.color-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
  margin-bottom: var(--space-4);
}

/* 圖外白底處理選擇器 */
.clip-row {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  margin-bottom: var(--space-5);
}
.clip-label {
  font-size: var(--text-base);
  color: var(--color-text-secondary);
  font-weight: var(--font-semibold);
}
.clip-select {
  flex: 1;
  min-height: 38px;
  padding: var(--space-1) var(--space-3);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  background: var(--color-white);
  color: var(--color-text-secondary);
  font-size: var(--text-base);
  cursor: pointer;
}

.color-chip {
  display: inline-flex;
  align-items: center;
  gap: var(--space-2);
  font-size: var(--text-base);
  color: var(--color-text-secondary);
  background: var(--color-white);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-pill);
  padding: var(--space-2) var(--space-4);
}
.color-chip.muted {
  color: var(--color-text-placeholder);
}

.swatch {
  display: inline-block;
  width: 14px;
  height: 14px;
  border-radius: var(--radius-sm);
  border: 1px solid var(--color-text-placeholder);
  flex-shrink: 0;
}

.section-label {
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  color: var(--color-text-secondary);
  margin-bottom: var(--space-3);
  display: flex;
  align-items: center;
  gap: var(--space-3);
}

.sens-badge {
  background: var(--color-primary);
  color: var(--color-white);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-4);
  font-size: var(--text-base);
  min-width: 34px;
  text-align: center;
}

.sensitivity-wrap {
  margin-bottom: var(--space-4);
}

.sensitivity-row {
  display: flex;
  align-items: center;
  gap: var(--space-3);
}

.sens-end-label {
  font-size: var(--text-sm);
  color: var(--color-text-muted);
  white-space: nowrap;
  width: 36px;
  text-align: center;
}

.slider-with-ticks {
  flex: 1;
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.sensitivity-slider {
  width: 100%;
  height: 6px;
  margin: var(--space-2) 0;
  cursor: pointer;
  accent-color: var(--color-primary);
}

.sensitivity-slider::-webkit-slider-runnable-track,
.param-row input[type='range']::-webkit-slider-runnable-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}

.sensitivity-slider::-webkit-slider-thumb,
.param-row input[type='range']::-webkit-slider-thumb {
  width: 18px;
  height: 18px;
  margin-top: -6px;
  border: 3px solid var(--color-white);
  border-radius: var(--radius-circle);
  background: var(--color-primary);
  box-shadow: var(--shadow-thumb);
}

.sensitivity-slider::-moz-range-track,
.param-row input[type='range']::-moz-range-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}

.sensitivity-slider::-moz-range-thumb,
.param-row input[type='range']::-moz-range-thumb {
  width: 14px;
  height: 14px;
  border: 3px solid var(--color-white);
  border-radius: var(--radius-circle);
  background: var(--color-primary);
  box-shadow: var(--shadow-thumb);
}

.tick-row {
  display: flex;
  justify-content: space-between;
  padding: 0 var(--space-1);
}

.tick {
  font-size: var(--text-xs);
  color: var(--color-text-tick);
  width: 18px;
  text-align: center;
  line-height: 1;
  transition:
    color 0.15s,
    font-weight 0.15s;
}

.tick.active {
  color: var(--color-primary);
  font-weight: var(--font-bold);
}

.sensitivity-guide {
  display: flex;
  justify-content: space-between;
  font-size: var(--text-sm);
  color: var(--color-text-muted);
  margin-top: var(--space-2);
  padding: 0 48px;
}

.advanced-toggle {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: var(--text-sm);
  font-weight: var(--font-bold);
  color: var(--color-text-secondary);
  cursor: pointer;
  padding: var(--space-4) 0 var(--space-3);
  border-top: 1px solid var(--color-border-light);
  margin-top: var(--space-5);
  user-select: none;
  touch-action: manipulation;
}
.toggle-arrow {
  font-size: var(--text-base);
  color: var(--color-text-muted);
}

.advanced-panel {
  padding: var(--space-4);
  border: 1px solid var(--color-border-light);
  border-radius: var(--radius-md);
  background: var(--color-bg-card);
  margin-bottom: var(--space-4);
}

.param-row {
  margin-bottom: var(--space-4);
}

.param-label {
  display: flex;
  justify-content: space-between;
  align-items: center;
  font-size: var(--text-base);
  color: var(--color-text-secondary);
  margin-bottom: var(--space-1);
}

.param-name {
  position: relative;
  display: inline-flex;
  align-items: center;
  gap: 5px;
}

.param-label strong {
  background: var(--color-primary);
  color: var(--color-white);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-3);
  font-size: var(--text-base);
  min-width: 28px;
  text-align: center;
}

.param-row input[type='range'] {
  width: 100%;
  height: 6px;
  cursor: pointer;
  accent-color: var(--color-primary);
}

.tooltip-trigger {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  border-radius: var(--radius-circle);
  background: var(--color-bg-chip);
  color: var(--color-text-disabled);
  font-size: var(--text-xs);
  font-weight: var(--font-bold);
  cursor: help;
  flex-shrink: 0;
  touch-action: manipulation;
}

.tooltip-box {
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  z-index: 100;
  background: var(--color-text);
  color: var(--color-bg-card);
  font-size: var(--text-sm);
  font-weight: var(--font-normal);
  padding: var(--space-3);
  border-radius: var(--radius-md);
  width: 260px;
  line-height: 1.55;
  box-shadow: var(--shadow-md);
  pointer-events: none;
}
.tooltip-box p {
  margin: 0 0 var(--space-1);
}
.tooltip-box p:last-child {
  margin: 0;
}

.btn-row {
  display: flex;
  gap: var(--space-3);
  margin-top: var(--space-5);
}

.btn-run {
  flex: 1;
  min-height: 48px;
  padding: var(--space-3) var(--space-5);
  font-size: var(--text-md);
  font-weight: var(--font-semibold);
  background: var(--gradient-primary);
  color: var(--color-white);
  border: none;
  border-radius: var(--radius-md);
  cursor: pointer;
  box-shadow: var(--shadow-sm);
  transition:
    filter var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out),
    box-shadow var(--dur-fast) var(--ease-out);
  touch-action: manipulation;
}
.btn-run:hover:not(:disabled) {
  filter: brightness(1.05);
  transform: translateY(-1px);
  box-shadow: var(--shadow-md);
}
.btn-run:active:not(:disabled) {
  transform: translateY(0) scale(0.98);
}
.btn-run:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.btn-astar {
  min-height: 48px;
  padding: var(--space-3) var(--space-5);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  background: var(--color-bg-card);
  color: var(--color-primary);
  border: 1px solid var(--color-primary);
  border-radius: var(--radius-md);
  cursor: pointer;
  transition:
    background 0.2s,
    color 0.2s,
    border-color 0.2s;
  touch-action: manipulation;
  white-space: nowrap;
}
.btn-astar:hover:not(:disabled) {
  background: var(--color-info-bg);
}
.btn-astar:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.result-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-top: var(--space-4);
  padding: var(--space-3) var(--space-4);
  border: 1px solid var(--color-border-light);
  border-radius: var(--radius-md);
  background: var(--color-bg-soft);
  font-size: var(--text-base);
  flex-wrap: wrap;
}

.result-time {
  color: var(--color-info-text);
}

.result-path {
  color: var(--color-primary);
  font-weight: var(--font-medium);
}

.result-path.error {
  color: var(--color-danger-dark);
}

.canvas-container {
  margin-top: var(--space-4);
}

canvas {
  max-width: 100%;
  border: 1px solid var(--color-border);
  border-radius: var(--radius-lg);
  background: var(--color-white);
  box-shadow: var(--shadow-sm);
}

.flow-actions {
  display: flex;
  justify-content: center;
  gap: var(--space-3);
  flex-wrap: wrap;
  margin-top: var(--space-5);
}

.flow-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 42px;
  padding: var(--space-2) var(--space-5);
  border: 1px solid transparent;
  border-radius: var(--radius-md);
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  text-decoration: none;
  cursor: pointer;
  transition:
    background-color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out),
    color var(--dur-fast) var(--ease-out),
    box-shadow var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out),
    opacity var(--dur-fast) var(--ease-out);
}

.flow-btn--primary {
  background: var(--gradient-primary);
  color: var(--color-white);
  box-shadow: var(--shadow-sm);
}

.flow-btn--primary:hover:not(:disabled) {
  filter: brightness(1.05);
  transform: translateY(-1px);
  box-shadow: var(--shadow-md);
}
.flow-btn--primary:active:not(:disabled) {
  transform: translateY(0) scale(0.98);
}

.flow-btn--secondary {
  background: var(--color-bg-card);
  border-color: var(--color-border);
  color: var(--color-text-secondary);
}

.flow-btn--secondary:hover {
  background: var(--color-bg-hover);
  transform: translateY(-1px);
}

.flow-btn:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}

@media (max-width: 640px) {
  .home-container {
    padding: var(--space-4) var(--space-2);
  }

  h1 {
    margin-bottom: var(--space-4);
  }

  .empty-state {
    padding: var(--space-8) var(--space-3);
  }

  .panel {
    padding: var(--space-3);
    border-radius: var(--radius-md);
  }

  .color-row {
    margin-bottom: var(--space-4);
  }

  .color-chip {
    align-items: flex-start;
    width: 100%;
    border-radius: var(--radius-md);
    line-height: 1.5;
  }

  .section-label {
    align-items: flex-start;
    flex-direction: column;
    gap: var(--space-2);
    font-size: var(--text-sm);
  }

  .tick {
    width: 14px;
  }

  .advanced-toggle {
    padding: var(--space-3) 0;
  }

  .sens-badge {
    align-self: flex-start;
  }

  .sensitivity-row {
    align-items: stretch;
    flex-direction: column;
  }

  .sens-end-label {
    width: auto;
    text-align: left;
  }

  .sensitivity-guide {
    flex-direction: column;
    gap: var(--space-1);
    padding: 0;
  }

  .btn-row {
    flex-direction: column;
    gap: var(--space-2);
  }

  .btn-run,
  .btn-astar {
    width: 100%;
    min-height: 46px;
  }

  .advanced-panel {
    padding: var(--space-3);
  }

  .param-label {
    align-items: flex-start;
    flex-direction: column;
    gap: var(--space-2);
  }

  .tooltip-box {
    left: 50%;
    width: min(260px, calc(100vw - 48px));
    transform: translateX(-50%);
  }

  .result-row {
    align-items: flex-start;
    flex-direction: column;
  }

  .canvas-container {
    overflow-x: auto;
    padding-bottom: var(--space-1);
  }

  canvas {
    border-radius: var(--radius-md);
  }

  .flow-actions {
    position: sticky;
    bottom: var(--space-3);
    z-index: 5;
    display: grid;
    grid-template-columns: 1fr;
    margin-top: var(--space-4);
  }

  .flow-btn {
    width: 100%;
    min-height: 46px;
    box-sizing: border-box;
  }
}

/* 前處理區 */
.preprocess-section {
  border-top: 1px solid var(--color-border-light);
  padding-top: var(--space-3);
  margin-top: var(--space-1);
}

.preprocess-label {
  font-size: var(--text-sm);
  font-weight: var(--font-bold);
  color: var(--color-text-placeholder);
  letter-spacing: 0.06em;
  text-transform: uppercase;
  margin-bottom: var(--space-3);
}

.toggle-row {
  display: flex;
  flex-direction: column;
  gap: 3px;
  margin-bottom: var(--space-3);
}

.toggle-label {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  color: var(--color-text-secondary);
  user-select: none;
}

/* 隱藏原生 checkbox */
.toggle-label input[type='checkbox'] {
  position: absolute;
  opacity: 0;
  width: 0;
  height: 0;
}

.toggle-track {
  position: relative;
  display: inline-block;
  width: 34px;
  height: 20px;
  background: var(--color-text-tick);
  border-radius: var(--radius-pill);
  transition: background 0.2s;
  flex-shrink: 0;
}

.toggle-label input:checked + .toggle-track {
  background: var(--color-primary);
}

.toggle-thumb {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 16px;
  height: 16px;
  background: white;
  border-radius: var(--radius-circle);
  transition: transform 0.2s;
  box-shadow: var(--shadow-thumb);
}

.toggle-label input:checked + .toggle-track .toggle-thumb {
  transform: translateX(14px);
}

.toggle-hint {
  font-size: var(--text-xs);
  color: var(--color-text-faded);
  line-height: 1.4;
  padding-left: 42px;
}

@media (max-width: 640px) {
  .toggle-hint {
    padding-left: 0;
  }
}
</style>
