<script setup lang="ts">
import { computed, onMounted, ref, type Component } from 'vue'
import { RouterView, useRoute, useRouter } from 'vue-router'
import { Box, Check, Lock, Moon, Route as RouteIcon, Sun, Upload } from 'lucide-vue-next'
import { useMapStore } from '@/stores/mapStore'

// 主題：'light' | 'dark' | null(跟隨系統)。手動切換寫入 <html data-theme> 與 localStorage。
type ThemePref = 'light' | 'dark'
const THEME_KEY = 'theme-pref'
const themePref = ref<ThemePref | null>(null)

function systemPrefersDark(): boolean {
  return window.matchMedia?.('(prefers-color-scheme: dark)').matches ?? false
}
// 目前實際呈現的是不是深色(用於切換鈕圖示)。
const isDark = computed(() =>
  themePref.value ? themePref.value === 'dark' : systemPrefersDark(),
)
function applyTheme(pref: ThemePref | null) {
  const el = document.documentElement
  if (pref) el.setAttribute('data-theme', pref)
  else el.removeAttribute('data-theme') // 移除 → 交回系統 prefers-color-scheme
}
function toggleTheme() {
  // 以「目前實際呈現」為基準切到反向,並固定成手動偏好。
  themePref.value = isDark.value ? 'light' : 'dark'
  applyTheme(themePref.value)
  localStorage.setItem(THEME_KEY, themePref.value)
}

// 頂部流程條的資料來源，狀態由 mapStore 內的實際地圖與路徑結果推導。
interface StepItem {
  id: number
  title: string
  path: string
  icon: Component
  isAvailable: boolean
  isComplete: boolean
}

const route = useRoute()
const router = useRouter()
const mapStore = useMapStore()

// 上傳完成代表已經有原始影像，且使用者已標出起點與終點。
const isUploadComplete = computed(
  () => mapStore.imageRawData != null && mapStore.startPoint != null && mapStore.endPoint != null,
)

// 路徑識別完成代表 WASM 已產生可通行遮罩，且 A* 有輸出路徑節點。
const isPathComplete = computed(
  () => mapStore.passableMask != null && mapStore.pathNodes.length > 0,
)

// 三個步驟以資料驅動畫面，之後若路由或門檻調整只需要改這份設定。
const steps = computed<StepItem[]>(() => [
  {
    id: 1,
    title: '上傳地圖',
    path: '/upload',
    icon: Upload,
    isAvailable: true,
    isComplete: isUploadComplete.value,
  },
  {
    id: 2,
    title: '路徑識別',
    path: '/path',
    icon: RouteIcon,
    isAvailable: isUploadComplete.value,
    isComplete: isPathComplete.value,
  },
  {
    id: 3,
    title: '3D 場景',
    path: '/scene',
    icon: Box,
    isAvailable: isPathComplete.value,
    isComplete: false,
  },
])

function isActiveStep(step: StepItem) {
  return route.path === step.path
}

// 狀態優先序會影響圖示與顏色；完成 > 當前頁 > 可用 > 鎖定。
function getStepState(step: StepItem) {
  if (step.isComplete) return 'complete'
  if (isActiveStep(step)) return 'active'
  if (step.isAvailable) return 'ready'
  return 'locked'
}

// 鎖定步驟只呈現狀態，不執行路由切換。
function handleStepClick(step: StepItem) {
  if (!step.isAvailable || isActiveStep(step)) return
  router.push(step.path)
}

onMounted(() => {
  // 還原主題偏好(無則跟隨系統)。
  const saved = localStorage.getItem(THEME_KEY)
  themePref.value = saved === 'light' || saved === 'dark' ? saved : null
  applyTheme(themePref.value)

  // WASM 引擎只在根元件初始化一次，後續頁面共用同一個 mapStore 狀態。
  mapStore.initEngine()
})
</script>

<template>
  <div class="app-container">
    <!-- 根導覽列只顯示流程狀態，實際能否進入下一步由 mapStore 的資料完整度決定。 -->
    <nav class="navbar" aria-label="流程導覽">
      <button
        class="theme-toggle"
        type="button"
        :aria-label="isDark ? '切換為淺色模式' : '切換為深色模式'"
        :title="isDark ? '淺色模式' : '深色模式'"
        @click="toggleTheme"
      >
        <Sun v-if="isDark" :size="18" :stroke-width="2.25" />
        <Moon v-else :size="18" :stroke-width="2.25" />
      </button>
      <div class="stepper">
        <template v-for="(step, index) in steps" :key="step.id">
          <button
            class="step-button"
            :class="`step-button--${getStepState(step)}`"
            type="button"
            :disabled="!step.isAvailable && !isActiveStep(step)"
            :aria-current="isActiveStep(step) ? 'step' : undefined"
            @click="handleStepClick(step)"
          >
            <span class="step-icon" aria-hidden="true">
              <Check v-if="step.isComplete" :size="22" :stroke-width="2.5" />
              <Lock
                v-else-if="!step.isAvailable && !isActiveStep(step)"
                :size="20"
                :stroke-width="2.25"
              />
              <component :is="step.icon" v-else :size="22" :stroke-width="2.25" />
            </span>
            <span class="step-title">{{ step.title }}</span>
          </button>

          <span
            v-if="index < steps.length - 1"
            class="step-line"
            :class="{ 'step-line--complete': step.isComplete && steps[index + 1]?.isComplete }"
            aria-hidden="true"
          />
        </template>
      </div>
    </nav>

    <!-- 子頁面共用同一個 Pinia store，因此切換頁面不會遺失地圖、遮罩與路徑。 -->
    <main class="content">
      <!-- 頁面過場只套在非 3D 頁。3D 場景頁(/scene）完全不經過 <Transition>:
           Three.js canvas 必須在掛載當下量到正確容器尺寸,而 out-in 的延遲掛載 +
           進場 transform 會讓 WebGL 初始化失敗、整頁消失。改用 :key 確保切回前兩頁
           時 Transition 仍會觸發進出場。 -->
      <RouterView v-slot="{ Component, route: r }">
        <template v-if="r.path === '/scene'">
          <component :is="Component" />
        </template>
        <Transition v-else name="page" mode="out-in">
          <component :is="Component" :key="r.path" />
        </Transition>
      </RouterView>
    </main>
  </div>
</template>

<style>
body {
  margin: 0;
  font-family:
    -apple-system, BlinkMacSystemFont, 'Segoe UI', 'PingFang TC', 'Microsoft JhengHei',
    'Helvetica Neue', Arial, sans-serif;
  background-color: var(--color-bg-page);
  color: var(--color-text);
  overflow-x: hidden;
  transition:
    background-color var(--dur-base) var(--ease-out),
    color var(--dur-base) var(--ease-out);
}

.app-container {
  min-height: 100vh;
  overflow-x: hidden;
}

/* 毛玻璃導覽列(半透明 + 背景模糊),延伸 3D 控制項的浮層語言。 */
.navbar {
  position: sticky;
  top: 0;
  z-index: 20;
  display: flex;
  align-items: center;
  background-color: color-mix(in srgb, var(--color-bg-card) 80%, transparent);
  backdrop-filter: saturate(180%) blur(12px);
  -webkit-backdrop-filter: saturate(180%) blur(12px);
  border-bottom: 1px solid var(--color-border);
  padding: var(--space-5) var(--space-4);
}

/* 深淺模式切換鈕,放右側、不擠壓置中 stepper。 */
.theme-toggle {
  position: absolute;
  right: var(--space-4);
  top: 50%;
  transform: translateY(-50%);
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  border: 1px solid var(--color-border);
  border-radius: var(--radius-circle);
  background: var(--color-bg-card);
  color: var(--color-text-soft);
  cursor: pointer;
  box-shadow: var(--shadow-xs);
  transition:
    color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out),
    background-color var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out);
}
.theme-toggle:hover {
  color: var(--color-primary);
  border-color: var(--color-primary);
  transform: translateY(-50%) scale(1.08);
}
.theme-toggle:active {
  transform: translateY(-50%) scale(0.94);
}

.stepper {
  display: flex;
  align-items: center;
  justify-content: center;
  max-width: 600px;
  margin: 0 auto;
  width: 100%;
}

.step-button {
  display: flex;
  flex: 0 0 auto;
  flex-direction: column;
  align-items: center;
  gap: var(--space-2);
  min-width: 88px;
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--color-text-secondary);
  font: inherit;
  font-size: var(--text-base);
  font-weight: var(--font-semibold);
  line-height: 1.2;
  text-align: center;
  cursor: pointer;
}

.step-button:disabled {
  cursor: not-allowed;
}

.step-icon {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  border: 2px solid var(--color-border);
  border-radius: var(--radius-circle);
  background-color: var(--color-bg-card);
  color: var(--color-text-muted);
  transition:
    background-color var(--dur-base) var(--ease-out),
    border-color var(--dur-base) var(--ease-out),
    box-shadow var(--dur-base) var(--ease-out),
    color var(--dur-base) var(--ease-out),
    transform var(--dur-fast) var(--ease-out);
}

/* 可點步驟 hover 時圖示微浮起。 */
.step-button:not(:disabled):hover .step-icon {
  transform: translateY(-2px) scale(1.05);
  box-shadow: var(--shadow-sm);
}
.step-button:not(:disabled):active .step-icon {
  transform: translateY(0) scale(0.97);
}

.step-title {
  color: currentColor;
  white-space: nowrap;
}

.step-button--complete {
  color: var(--color-success-text);
}

.step-button--complete .step-icon {
  border-color: var(--color-success);
  background-color: var(--color-success);
  color: var(--color-white);
}

.step-button--active {
  color: var(--color-primary);
}

.step-button--active .step-icon {
  border-color: var(--color-primary);
  background-color: var(--color-primary);
  color: var(--color-white);
  box-shadow: 0 0 0 4px var(--color-primary-ring);
}

.step-button--ready {
  color: var(--color-text-secondary);
}

.step-button--locked {
  color: var(--color-text-disabled);
}

.step-button--locked .step-icon {
  border-color: var(--color-border-light);
  color: var(--color-text-disabled);
}

.step-line {
  flex: 1 1 72px;
  height: 2px;
  margin: 0 var(--space-3);
  background-color: var(--color-border);
}

.step-line--complete {
  background-color: var(--color-success);
}

.content {
  padding: var(--space-4);
}

/* 頁面切換進場/離場(fade + 上滑)。 */
.page-enter-active {
  transition:
    opacity var(--dur-base) var(--ease-out),
    transform var(--dur-base) var(--ease-out);
}
.page-leave-active {
  transition:
    opacity var(--dur-fast) var(--ease-out),
    transform var(--dur-fast) var(--ease-out);
}
.page-enter-from {
  opacity: 0;
  transform: translateY(10px);
}
.page-leave-to {
  opacity: 0;
  transform: translateY(-6px);
}

@media (max-width: 720px) {
  .navbar {
    position: sticky;
    top: 0;
    z-index: 20;
    padding: var(--space-4) var(--space-3);
  }

  .stepper {
    max-width: 100%;
  }

  .step-button {
    min-width: 68px;
    gap: var(--space-1);
    font-size: var(--text-xs);
  }

  .step-icon {
    width: 34px;
    height: 34px;
  }

  .step-line {
    flex-basis: 28px;
    min-width: 20px;
    margin: 0 var(--space-1);
  }

  .content {
    padding: var(--space-3);
  }
}

@media (max-width: 380px) {
  .step-button {
    min-width: 60px;
  }

  .step-title {
    font-size: 11px;
  }

  .step-line {
    flex-basis: 16px;
    min-width: 12px;
  }
}
</style>
