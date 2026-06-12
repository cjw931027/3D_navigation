<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch } from 'vue'
import { X } from 'lucide-vue-next'

// 進階設定抽屜:桌面(≥900px)右側滑出、手機底部 bottom sheet。
// overlay 蓋在內容上而非推擠版面,避免 canvas 因容器改變而重排。
const props = defineProps<{
  modelValue: boolean
  title: string
}>()

const emit = defineEmits<{ 'update:modelValue': [value: boolean] }>()

const panelRef = ref<HTMLElement | null>(null)
let lastFocused: HTMLElement | null = null

function close() {
  emit('update:modelValue', false)
}

function onKeydown(e: KeyboardEvent) {
  if (e.key === 'Escape') close()
}

watch(
  () => props.modelValue,
  (open) => {
    if (open) {
      lastFocused = document.activeElement as HTMLElement | null
      window.addEventListener('keydown', onKeydown)
      document.body.style.overflow = 'hidden'
      nextTick(() => panelRef.value?.focus())
    } else {
      window.removeEventListener('keydown', onKeydown)
      document.body.style.overflow = ''
      lastFocused?.focus()
      lastFocused = null
    }
  },
)

onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown)
  document.body.style.overflow = ''
})
</script>

<template>
  <Teleport to="body">
    <Transition name="drawer">
      <div v-if="modelValue" class="drawer-root">
        <div class="drawer-scrim" @click="close" />
        <section
          ref="panelRef"
          class="drawer-panel"
          role="dialog"
          aria-modal="true"
          :aria-label="title"
          tabindex="-1"
        >
          <span class="drawer-grab" aria-hidden="true" />
          <header class="drawer-head">
            <h2 class="drawer-title">{{ title }}</h2>
            <button class="drawer-close" type="button" aria-label="關閉" @click="close">
              <X :size="18" :stroke-width="2.25" />
            </button>
          </header>
          <div class="drawer-body">
            <slot />
          </div>
        </section>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.drawer-root {
  position: fixed;
  inset: 0;
  z-index: 200;
}

.drawer-scrim {
  position: absolute;
  inset: 0;
  background: var(--color-overlay);
}

.drawer-panel {
  position: absolute;
  display: flex;
  flex-direction: column;
  background: var(--color-bg-card);
  outline: none;
}

.drawer-grab {
  display: none;
}

.drawer-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-3);
  padding: var(--space-4) var(--space-5) var(--space-3);
  border-bottom: 1px solid var(--color-border-light);
  flex-shrink: 0;
}

.drawer-title {
  margin: 0;
  font-size: var(--text-lg);
  font-weight: var(--font-bold);
  color: var(--color-text);
}

.drawer-close {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  border: 1px solid var(--color-border);
  border-radius: var(--radius-circle);
  background: transparent;
  color: var(--color-text-soft);
  cursor: pointer;
  transition:
    color var(--dur-fast) var(--ease-out),
    border-color var(--dur-fast) var(--ease-out);
}
.drawer-close:hover {
  color: var(--color-primary);
  border-color: var(--color-primary);
}

.drawer-body {
  flex: 1 1 auto;
  min-height: 0;
  overflow-y: auto;
  padding: var(--space-5);
  overscroll-behavior: contain;
}

/* 手機:底部抽屜(BP: 900px) */
@media (max-width: 899.98px) {
  .drawer-panel {
    left: 0;
    right: 0;
    bottom: 0;
    max-height: 82dvh;
    border-top: 1px solid var(--color-border);
    border-radius: var(--radius-xl) var(--radius-xl) 0 0;
    padding-bottom: env(safe-area-inset-bottom, 0px);
  }

  .drawer-grab {
    display: block;
    width: 40px;
    height: 4px;
    margin: var(--space-2) auto 0;
    border-radius: var(--radius-pill);
    background: var(--color-border);
    flex-shrink: 0;
  }

  .drawer-enter-from .drawer-panel,
  .drawer-leave-to .drawer-panel {
    transform: translateY(100%);
  }
}

/* 桌面:右側滑出 */
@media (min-width: 900px) {
  .drawer-panel {
    top: 0;
    right: 0;
    bottom: 0;
    width: min(var(--drawer-w), 92vw);
    border-left: 1px solid var(--color-border);
    box-shadow: var(--shadow-lg);
  }

  .drawer-enter-from .drawer-panel,
  .drawer-leave-to .drawer-panel {
    transform: translateX(100%);
  }
}

.drawer-enter-active,
.drawer-leave-active {
  transition: opacity 0.28s var(--ease-out);
}
.drawer-enter-active .drawer-panel,
.drawer-leave-active .drawer-panel {
  transition: transform 0.28s var(--ease-out);
}
.drawer-enter-from,
.drawer-leave-to {
  opacity: 1;
}
.drawer-enter-from .drawer-scrim,
.drawer-leave-to .drawer-scrim {
  opacity: 0;
}
.drawer-enter-active .drawer-scrim,
.drawer-leave-active .drawer-scrim {
  transition: opacity 0.28s var(--ease-out);
}
</style>
