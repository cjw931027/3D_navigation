<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { RouterView, useRoute, useRouter } from 'vue-router'
import { Box, Moon, Sun } from 'lucide-vue-next'
import AppStepper, { type StepItem } from '@/components/AppStepper.vue'
import { useMapStore } from '@/stores/mapStore'

// 主題:明確二態('dark' 預設 / 'light' opt-in),寫入 <html data-theme> 與 localStorage。
type ThemePref = 'light' | 'dark'
const THEME_KEY = 'theme-pref'
const themePref = ref<ThemePref>('dark')

const isDark = computed(() => themePref.value === 'dark')

function applyTheme(pref: ThemePref) {
  document.documentElement.setAttribute('data-theme', pref)
}
function toggleTheme() {
  themePref.value = isDark.value ? 'light' : 'dark'
  applyTheme(themePref.value)
  localStorage.setItem(THEME_KEY, themePref.value)
}

const route = useRoute()
const router = useRouter()
const mapStore = useMapStore()

// 上傳完成代表已經有原始影像,且使用者已標出起點與終點。
const isUploadComplete = computed(
  () => mapStore.imageRawData != null && mapStore.startPoint != null && mapStore.endPoint != null,
)

// 路徑識別完成代表 WASM 已產生可通行遮罩,且 A* 有輸出路徑節點。
const isPathComplete = computed(
  () => mapStore.passableMask != null && mapStore.pathNodes.length > 0,
)

// 三個步驟以資料驅動畫面,之後若路由或門檻調整只需要改這份設定。
const steps = computed<StepItem[]>(() => [
  {
    id: 1,
    title: '上傳地圖',
    path: '/upload',
    isAvailable: true,
    isComplete: isUploadComplete.value,
  },
  {
    id: 2,
    title: '路徑識別',
    path: '/path',
    isAvailable: isUploadComplete.value,
    isComplete: isPathComplete.value,
  },
  {
    id: 3,
    title: '3D 場景',
    path: '/scene',
    isAvailable: isPathComplete.value,
    isComplete: false,
  },
])

const isScene = computed(() => route.path === '/scene')

onMounted(() => {
  // 還原主題偏好(index.html 的 pre-paint script 已先套用屬性,這裡同步 ref)。
  themePref.value = localStorage.getItem(THEME_KEY) === 'light' ? 'light' : 'dark'
  applyTheme(themePref.value)

  // WASM 引擎只在根元件初始化一次,後續頁面共用同一個 mapStore 狀態。
  mapStore.initEngine()
})
</script>

<template>
  <div class="app-shell">
    <!-- 桌面側欄(<900px 隱藏但保留在 DOM,#shell-panel 是各頁控制項的 Teleport 目標)。 -->
    <aside class="sidebar" aria-label="流程導覽">
      <div class="brand">
        <span class="brand-mark" aria-hidden="true">
          <Box :size="20" :stroke-width="2.25" />
        </span>
        <span class="brand-name">3D 室內導航</span>
      </div>

      <div class="sidebar-label">設定步驟</div>
      <AppStepper :steps="steps" :current-path="route.path" @navigate="router.push" />

      <div id="shell-panel" class="shell-panel"></div>

      <button
        class="theme-row"
        type="button"
        :aria-label="isDark ? '切換為淺色模式' : '切換為深色模式'"
        @click="toggleTheme"
      >
        <Sun v-if="isDark" :size="18" :stroke-width="2.25" />
        <Moon v-else :size="18" :stroke-width="2.25" />
        <span>{{ isDark ? '淺色模式' : '深色模式' }}</span>
      </button>
    </aside>

    <div class="main-col">
      <!-- 手機頁頂:mockup 式直式步驟清單(3D 場景頁全螢幕沉浸,不顯示)。 -->
      <header class="mobile-head" v-if="!isScene" aria-label="流程導覽">
        <div class="mobile-head-row">
          <span class="sidebar-label">設定步驟</span>
          <button
            class="btn btn--icon mobile-theme"
            type="button"
            :aria-label="isDark ? '切換為淺色模式' : '切換為深色模式'"
            @click="toggleTheme"
          >
            <Sun v-if="isDark" :size="17" :stroke-width="2.25" />
            <Moon v-else :size="17" :stroke-width="2.25" />
          </button>
        </div>
        <AppStepper :steps="steps" :current-path="route.path" @navigate="router.push" />
      </header>

      <!-- 子頁面共用同一個 Pinia store,因此切換頁面不會遺失地圖、遮罩與路徑。 -->
      <main class="content" :class="{ 'content--scene': isScene }">
        <!-- 頁面過場只套在非 3D 頁。3D 場景頁(/scene)完全不經過 <Transition>:
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

.app-shell {
  display: flex;
  min-height: 100dvh;
}

/* ── 桌面側欄(BP: 900px)──────────────────────────────────── */
.sidebar {
  display: none;
}

.main-col {
  flex: 1 1 auto;
  min-width: 0;
  display: flex;
  flex-direction: column;
}

.brand {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  margin-bottom: var(--space-8);
}

.brand-mark {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 36px;
  height: 36px;
  border-radius: var(--radius-lg);
  background: var(--color-primary);
  color: var(--color-white);
}

.brand-name {
  font-size: var(--text-md);
  font-weight: var(--font-bold);
  letter-spacing: 0.02em;
}

.sidebar-label {
  font-size: var(--text-xs);
  font-weight: var(--font-semibold);
  letter-spacing: 0.12em;
  color: var(--color-text-muted);
  margin-bottom: var(--space-4);
}

/* 各頁控制項的 Teleport 目標:佔據側欄剩餘高度,內部可捲動。 */
.shell-panel {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  margin-top: var(--space-6);
  display: flex;
  flex-direction: column;
  scrollbar-width: thin;
  scrollbar-color: var(--color-border) transparent;
}

.theme-row {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  width: 100%;
  margin-top: var(--space-3);
  padding: 10px var(--space-3);
  border: 1px solid var(--color-border);
  border-radius: var(--radius-lg);
  background: transparent;
  color: var(--color-text-soft);
  font: inherit;
  font-size: var(--text-sm);
  font-weight: var(--font-medium);
  cursor: pointer;
  transition:
    color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out),
    background-color var(--dur-fast) var(--ease-out);
}
.theme-row:hover {
  color: var(--color-primary);
  border-color: var(--color-primary);
  background: var(--color-bg-hover);
}

/* ── 手機頁頂(mockup 式)──────────────────────────────────── */
.mobile-head {
  padding: var(--space-5) var(--space-4) var(--space-4);
  border-bottom: 1px solid var(--color-border);
}

.mobile-head-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding-bottom: var(--space-2);
  border-bottom: 1px solid var(--color-border);
  margin-bottom: var(--space-4);
}

.mobile-theme {
  width: 36px;
  height: 36px;
}

.content {
  flex: 1 1 auto;
  min-width: 0;
  padding: var(--space-4);
}

.content--scene {
  padding: 0;
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

@media (min-width: 900px) {
  .sidebar {
    display: flex;
    flex-direction: column;
    flex: 0 0 var(--sidebar-w);
    box-sizing: border-box;
    position: sticky;
    top: 0;
    height: 100dvh;
    padding: var(--space-6) var(--space-4) var(--space-4);
    background: var(--color-bg-soft);
    border-right: 1px solid var(--color-border);
  }

  .mobile-head {
    display: none;
  }

  .content {
    padding: clamp(16px, 3vw, 32px);
  }

  .content--scene {
    padding: 0;
  }
}
</style>
