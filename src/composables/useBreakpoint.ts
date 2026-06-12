import { onUnmounted, ref, type Ref } from 'vue'

// BP: 900px = 桌面側欄殼層 / 手機單欄 的主斷點(與 App.vue 的 media query 約定一致)。
const DESKTOP_QUERY = '(min-width: 900px)'

/** 是否為桌面寬度(≥900px),隨視窗即時更新。用於 Teleport 的 disabled 切換。 */
export function useIsDesktop(): Ref<boolean> {
  const mql = window.matchMedia(DESKTOP_QUERY)
  const isDesktop = ref(mql.matches)
  const onChange = (e: MediaQueryListEvent) => {
    isDesktop.value = e.matches
  }
  mql.addEventListener('change', onChange)
  onUnmounted(() => mql.removeEventListener('change', onChange))
  return isDesktop
}
