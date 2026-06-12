<script setup lang="ts">
import { computed, nextTick, onMounted, ref, toRaw } from 'vue'
import { ImageOff, SlidersHorizontal, Upload } from 'lucide-vue-next'
import { useMapStore } from '@/stores/mapStore'
import { useIsDesktop } from '@/composables/useBreakpoint'
import ParamSlider from '@/components/ParamSlider.vue'
import SlideOverDrawer from '@/components/SlideOverDrawer.vue'

const mapStore = useMapStore()
const processTime = ref<number | null>(null)
const isRunning = ref(false)
const previewCanvas = ref<HTMLCanvasElement | null>(null)

const pathStatus = ref<string | null>(null)

// 桌面時控制面板 Teleport 到 App.vue 側欄的 #shell-panel;手機 inline 渲染。
const isDesktop = useIsDesktop()

// 輕量 toast:取代原生 alert()。顯示一段時間後自動淡出。
const toastMsg = ref<string | null>(null)
let toastTimer: ReturnType<typeof setTimeout> | null = null
function showToast(msg: string) {
  toastMsg.value = msg
  if (toastTimer) clearTimeout(toastTimer)
  toastTimer = setTimeout(() => (toastMsg.value = null), 2600)
}

// 只有遮罩與路徑節點都存在時,3D 場景才有足夠資料可以建模。
const canGoToScene = computed(() => mapStore.passableMask != null && mapStore.pathNodes.length > 0)

// 進階參數直接對應 core.cpp 的 flood fill 前處理參數,文字提示說明何時調整。
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

// 把 A* 路徑疊回預覽圖:鮮紅主線 + 白外框,線寬隨長邊比例自適應(大圖才不會變極細)。
function drawPath(ctx: CanvasRenderingContext2D) {
  const nodes = mapStore.pathNodes
  if (nodes.length < 2) return

  const base = Math.max(ctx.canvas.width, ctx.canvas.height)
  const lineW = Math.max(3, base * 0.006) // 紅線寬:約長邊 0.6%,最少 3px
  const outlineW = lineW + Math.max(2, base * 0.004) // 白外框比紅線再寬一圈

  const trace = () => {
    ctx.beginPath()
    ctx.moveTo(nodes[0]!.x, nodes[0]!.y)
    for (let i = 1; i < nodes.length; i++) ctx.lineTo(nodes[i]!.x, nodes[i]!.y)
  }

  ctx.save()
  ctx.lineJoin = 'round'
  ctx.lineCap = 'round'

  // 白色外框(先畫,較寬)
  trace()
  ctx.strokeStyle = 'rgba(255,255,255,0.95)'
  ctx.lineWidth = outlineW
  ctx.stroke()

  // 鮮紅主線(後畫,較窄,疊在外框上)
  trace()
  ctx.strokeStyle = '#FF3B30'
  ctx.lineWidth = lineW
  ctx.stroke()

  ctx.restore()
}

// 起點與終點只畫在畫面層,不回寫到 imageRawData,避免下一次辨識把標記當成路色。
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

// 預覽圖每次重算後重畫路徑與起訖點,確保顯示結果和目前 store 狀態一致。
function drawOverlay(ctx: CanvasRenderingContext2D) {
  if (mapStore.pathNodes.length >= 2) drawPath(ctx)
  if (mapStore.startPoint) drawDot(ctx, mapStore.startPoint, '#4CAF50', '起點')
  if (mapStore.endPoint) drawDot(ctx, mapStore.endPoint, '#F44336', '終點')
}

// WASM 回傳的是 RGBA buffer,這裡轉成 canvas 可顯示的 ImageData。
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
  // DOM lib 要求 ArrayBuffer-backed;此處 buffer 一律由 new Uint8ClampedArray() 建立,斷言安全。
  ctx.putImageData(new ImageData(buffer as Uint8ClampedArray<ArrayBuffer>, width, height), 0, 0)
  return ctx
}

// 呼叫 core.cpp 的 intelligentFloodFill,產生可通行遮罩後立即執行 A*。
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

    // 最近鄰上採樣,放大後的 buffer 送 WASM。
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

    // mapBuffer 是 WASM 端共用輸入記憶體,JS 需先寫入 RGBA 後再呼叫 C++。
    const pointer = mapStore.wasmModule.allocateMemory(size2) as number
    mapStore.wasmModule.HEAPU8.set(sendData, pointer)

    const denoiseArea = mapStore.denoiseMinArea * up * up
    // smooth* 參數隨上採樣比例縮放(closing kernel 維持奇數、牆塊面積以 up² 擴張)。
    const smoothClose = (() => {
      const v = Math.round(p.smoothClosingSize * up)
      return v <= 1 ? 0 : v % 2 === 0 ? v + 1 : v
    })()
    const smoothMinWall = p.smoothMinWallArea * up * up
    // 多種子:傳 seedXs / seedYs 陣列 + clipMode 給新版 WASM 介面。
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

    // 放大尺寸結果取出後,最近鄰下採樣回原尺寸供顯示。
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

    // passableMask 由 core.cpp 保存在全域 buffer,複製一份到 Pinia 供 3D 場景使用。
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

    // g_passableMask 位於 WASM 全域,freeMemory 後仍可供 A* 使用。
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

// 只重新跑 A*,保留上一次 flood fill 的遮罩,適合調整起訖點後快速重算。
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

// 進入頁面時還原預覽:有上次識別結果就畫結果+路徑,否則先畫原圖+起訖點,
// 避免初次進入或從 3D 場景返回時預覽卡一片空白。
onMounted(async () => {
  if (mapStore.mapWidth === 0) return // empty-state 分支,無 canvas
  await nextTick()

  if (mapStore.floodFillResultData) {
    const ctx = renderToCanvas(mapStore.floodFillResultData, mapStore.mapWidth, mapStore.mapHeight)
    if (ctx) drawOverlay(ctx)
    return
  }

  const rawData = toRaw(mapStore.imageRawData)
  if (!rawData) return
  const ctx = renderToCanvas(new Uint8ClampedArray(rawData), mapStore.mapWidth, mapStore.mapHeight)
  if (ctx) drawOverlay(ctx)
})
</script>

<template>
  <main class="home-page">
    <div class="empty-state" v-if="mapStore.mapWidth === 0">
      <div class="empty-icon">
        <ImageOff :size="48" :stroke-width="1.5" />
      </div>
      <h2 class="empty-title">還沒有載入地圖</h2>
      <p class="empty-desc">
        請先上傳室內平面圖並標記起點與終點，<br />
        系統才能識別可通行路徑。
      </p>
      <RouterLink to="/upload" class="btn btn--primary empty-action">
        <Upload :size="16" />
        前往上傳地圖
      </RouterLink>
    </div>

    <template v-else>
      <!-- 預覽區:佔據主要畫面 -->
      <section class="preview-pane">
        <header class="pane-head">
          <h1>路徑識別</h1>
          <p class="pane-sub">執行識別後，確認可行走區域（藍）與規劃路徑（紅）。</p>
        </header>
        <div class="canvas-card">
          <canvas ref="previewCanvas"></canvas>
        </div>
      </section>

      <!-- 控制面板:桌面投送到側欄,手機 inline 在預覽下方 -->
      <Teleport to="#shell-panel" :disabled="!isDesktop" defer>
        <div class="controls">
          <!-- 執行(主鈕)+ 重算(同排省空間) -->
          <div class="run-block">
            <button
              class="btn btn--primary btn--pill run-main"
              @click="runFloodFill"
              :disabled="!mapStore.isEngineReady || isRunning"
            >
              {{ isRunning ? '運算中…' : '執行路徑識別' }}
            </button>
            <button
              class="btn btn--ghost btn--pill run-again"
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

          <!-- 結果資訊 -->
          <div class="result-card" v-if="processTime !== null || pathStatus !== null">
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

          <!-- 進階設定抽屜入口 -->
          <button class="btn btn--ghost advanced-open" @click="mapStore.showAdvanced = true">
            <SlidersHorizontal :size="16" :stroke-width="2.25" />
            進階設定
          </button>

          <!-- 流程導覽 -->
          <div class="flow-actions">
            <RouterLink to="/upload" class="btn btn--ghost">上一步：上傳地圖</RouterLink>
            <RouterLink v-if="canGoToScene" to="/scene" class="btn btn--primary btn--pill">
              下一步：3D 場景
            </RouterLink>
            <button v-else class="btn btn--primary btn--pill" type="button" disabled>
              下一步：3D 場景
            </button>
          </div>
        </div>
      </Teleport>

      <!-- 進階設定抽屜:桌面右側滑出 / 手機底部 sheet -->
      <SlideOverDrawer v-model="mapStore.showAdvanced" title="進階設定">
        <!-- 靈敏度:一般不需調整,收進抽屜 -->
        <div class="drawer-section">
          <div class="drawer-section-label sens-label">
            辨識靈敏度
            <span class="sens-badge">{{ mapStore.sensitivity }}</span>
          </div>
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
          <div class="sens-ends">
            <span>精細</span>
            <span>寬鬆</span>
          </div>
          <div class="sensitivity-guide">
            <span>識別範圍太小 → 往右</span>
            <span>識別到多餘區域 → 往左</span>
          </div>
        </div>

        <div class="drawer-section">
          <div class="drawer-section-label">路色採樣</div>
          <div class="color-row">
            <template v-if="mapStore.pathColors.length">
              <div class="color-chip" v-for="(c, i) in mapStore.pathColors" :key="i">
                <span class="swatch" :style="{ background: `rgb(${c.r},${c.g},${c.b})` }"></span>
                rgb({{ c.r }}, {{ c.g }}, {{ c.b }})
              </div>
            </template>
            <div class="color-chip muted" v-else>路色未採樣</div>
          </div>
        </div>

        <!-- 圖外白底處理(戶外圖用):白路與圖外背景同色時,把圖外裁掉 -->
        <div class="drawer-section">
          <div class="drawer-section-label">圖外白底處理</div>
          <select
            class="clip-select"
            :value="mapStore.clipMode"
            @change="
              mapStore.setClipMode(Number(($event.target as HTMLSelectElement).value) as 0 | 1 | 2)
            "
          >
            <option :value="0">無（室內多色底圖）</option>
            <option :value="1">牆包圍盒（建物密集的戶外圖）</option>
            <option :value="2">灰色外框（院區圖有外框線）</option>
          </select>
        </div>

        <div class="drawer-section">
          <div class="drawer-section-label">進階參數</div>
          <ParamSlider
            v-for="key in Object.keys(paramLabels) as (keyof typeof paramLabels)[]"
            :key="key"
            :label="paramLabels[key]!"
            :model-value="mapStore.floodFillParams[key as keyof typeof mapStore.floodFillParams]"
            :min="paramConfig[key]!.min"
            :max="paramConfig[key]!.max"
            :step="paramConfig[key]!.step"
            :tooltip="tooltips[key]"
            @update:model-value="(v) => mapStore.setFloodFillParams({ [key]: v })"
          />
        </div>

        <div class="drawer-section">
          <div class="drawer-section-label">遮罩過濾</div>
          <ParamSlider
            label="破碎區塊門檻（像素數）"
            :model-value="mapStore.denoiseMinArea"
            :min="0"
            :max="400"
            :step="10"
            hint="BFS 後自動清除遮罩中面積過小的孤立連通域，設為 0 可關閉"
            @update:model-value="(v) => (mapStore.denoiseMinArea = v)"
          />
          <ParamSlider
            label="平滑：補洞核大小"
            :model-value="mapStore.floodFillParams.smoothClosingSize"
            :min="1"
            :max="9"
            :step="2"
            hint="走廊內小字 / 圖示形成的小洞會被填補；偵測到的真牆絕不會被覆蓋。設為 1 可關閉"
            @update:model-value="(v) => mapStore.setFloodFillParams({ smoothClosingSize: v })"
          />
          <ParamSlider
            label="平滑：孤立小牆面積"
            :model-value="mapStore.floodFillParams.smoothMinWallArea"
            :min="0"
            :max="1200"
            :step="25"
            hint="走廊內面積小於此值、且被可走區包圍、不接觸真牆的障礙（樓梯/閘門/文字圖示啃出的缺口）會被翻為可走；真牆與獨立非路面塊一律保留。設為 0 可關閉"
            @update:model-value="(v) => mapStore.setFloodFillParams({ smoothMinWallArea: v })"
          />
        </div>
      </SlideOverDrawer>
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
.home-page {
  position: relative;
  max-width: 1100px;
  margin: 0 auto;
}

/* ── 預覽區 ─────────────────────────────────────────────── */
.pane-head {
  display: flex;
  align-items: baseline;
  gap: var(--space-3);
  flex-wrap: wrap;
  margin-bottom: var(--space-4);
}

h1 {
  margin: 0;
  font-size: var(--text-h2);
  font-weight: var(--font-bold);
  color: var(--color-text);
}

.pane-sub {
  margin: 0;
  font-size: var(--text-sm);
  color: var(--color-text-muted);
}

.canvas-card {
  padding: var(--space-4);
  background: var(--color-bg-card);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-xl);
}

canvas {
  display: block;
  max-width: 100%;
  margin: 0 auto;
  border-radius: var(--radius-md);
}

/* ── 控制面板(桌面 teleport 到側欄 / 手機 inline)────────── */
.controls {
  display: flex;
  flex-direction: column;
  gap: var(--space-4);
  text-align: left;
}

/* 靈敏度區(位於進階設定抽屜內) */
.sens-label {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-3);
}

.sens-badge {
  background: var(--color-primary);
  color: var(--color-white);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-4);
  font-size: var(--text-base);
  min-width: 24px;
  text-align: center;
  font-variant-numeric: tabular-nums;
}

.slider-with-ticks {
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

.sensitivity-slider::-webkit-slider-runnable-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}

.sensitivity-slider::-webkit-slider-thumb {
  width: 18px;
  height: 18px;
  margin-top: -6px;
  border: 3px solid var(--color-white);
  border-radius: var(--radius-circle);
  background: var(--color-primary);
  box-shadow: var(--shadow-thumb);
}

.sensitivity-slider::-moz-range-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}

.sensitivity-slider::-moz-range-thumb {
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

.sens-ends {
  display: flex;
  justify-content: space-between;
  font-size: var(--text-xs);
  color: var(--color-text-muted);
  margin-top: var(--space-1);
}

.sensitivity-guide {
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-size: var(--text-xs);
  color: var(--color-text-faded);
  margin-top: var(--space-2);
}

/* 執行 + 重算同一排:主鈕撐滿剩餘寬度,重算縮到內容寬 */
.run-block {
  display: flex;
  gap: var(--space-2);
}

.run-block .btn--pill {
  width: auto;
  min-width: 0;
}

.run-main {
  flex: 1 1 auto;
}

.run-again {
  flex: 0 1 auto;
  white-space: nowrap;
}

.result-card {
  display: flex;
  flex-direction: column;
  gap: var(--space-1);
  padding: var(--space-3) var(--space-4);
  border: 1px solid var(--color-border-light);
  border-radius: var(--radius-md);
  background: var(--color-bg-soft);
  font-size: var(--text-sm);
}

.result-time {
  color: var(--color-text-muted);
}

.result-path {
  color: var(--color-success-text);
  font-weight: var(--font-medium);
}

.result-path.error {
  color: var(--color-danger);
}

.advanced-open {
  width: 100%;
}

.flow-actions {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
}

/* 桌面:控制面板撐滿側欄剩餘高度,流程按鈕固定在底部。 */
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

/* 手機:流程按鈕 sticky 在底部(mockup 的大藥丸 CTA),橫排減少佔位。 */
@media (max-width: 899.98px) {
  /* 大標題與頂部步驟清單的「路徑識別/目前步驟」重複,手機隱藏省空間 */
  h1 {
    display: none;
  }

  /* inline 渲染時與上方預覽卡保持間距 */
  .controls {
    margin-top: var(--space-4);
  }

  /* 貼底實底工具列:半透明底+模糊,捲動時內容從後方通過不透字 */
  .flow-actions {
    position: sticky;
    bottom: 0;
    z-index: 5;
    margin-top: var(--space-2);
    margin-inline: calc(-1 * var(--space-4));
    padding: var(--space-2) var(--space-4) var(--space-3);
    flex-direction: row;
    background: color-mix(in srgb, var(--color-bg-page) 92%, transparent);
    backdrop-filter: blur(8px);
    -webkit-backdrop-filter: blur(8px);
  }

  .flow-actions .btn--ghost {
    flex: 0 0 auto;
    min-height: var(--cta-h);
    border-radius: var(--radius-pill);
  }

  .flow-actions .btn--pill {
    flex: 1 1 auto;
    width: auto;
  }
}

/* ── 抽屜內容 ───────────────────────────────────────────── */
.drawer-section {
  margin-bottom: var(--space-6);
}
.drawer-section:last-child {
  margin-bottom: 0;
}

.drawer-section-label {
  font-size: var(--text-xs);
  font-weight: var(--font-bold);
  letter-spacing: 0.08em;
  color: var(--color-text-muted);
  margin-bottom: var(--space-3);
}

.color-row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
}

.color-chip {
  display: inline-flex;
  align-items: center;
  gap: var(--space-2);
  font-size: var(--text-sm);
  color: var(--color-text-secondary);
  background: var(--color-bg-soft);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-3);
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

.clip-select {
  width: 100%;
  box-sizing: border-box;
  min-height: 42px;
  padding: var(--space-2) var(--space-3);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-md);
  background: var(--color-bg-soft);
  color: var(--color-text-secondary);
  font-size: var(--text-base);
  cursor: pointer;
}

/* ── 空狀態 ─────────────────────────────────────────────── */
.empty-state {
  display: flex;
  flex-direction: column;
  align-items: center;
  max-width: 400px;
  margin: 0 auto;
  padding: var(--space-10) var(--space-5);
  text-align: center;
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
}

.empty-action {
  min-height: 44px;
  padding: var(--space-2) var(--space-5);
}

/* ── 運算中遮罩 ─────────────────────────────────────────── */
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
</style>
