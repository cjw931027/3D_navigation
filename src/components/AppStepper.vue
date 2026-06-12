<script setup lang="ts">
import { Check, Lock } from 'lucide-vue-next'

// 全域三步驟的資料形狀;狀態由 mapStore 的實際資料完整度推導(見 App.vue)。
export interface StepItem {
  id: number
  title: string
  path: string
  isAvailable: boolean
  isComplete: boolean
}

const props = defineProps<{
  steps: StepItem[]
  currentPath: string
}>()

const emit = defineEmits<{ navigate: [path: string] }>()

function isActive(step: StepItem) {
  return props.currentPath === step.path
}

// 狀態優先序會影響圓圈與顏色;完成 > 當前頁 > 可用 > 鎖定。
function getStepState(step: StepItem) {
  if (step.isComplete) return 'complete'
  if (isActive(step)) return 'active'
  if (step.isAvailable) return 'ready'
  return 'locked'
}

// 鎖定步驟只呈現狀態,不執行路由切換。
function handleClick(step: StepItem) {
  if (!step.isAvailable || isActive(step)) return
  emit('navigate', step.path)
}
</script>

<template>
  <ol class="stepper">
    <li
      v-for="(step, index) in steps"
      :key="step.id"
      class="step"
      :class="[
        `step--${getStepState(step)}`,
        { 'step--linked': index > 0 && step.isComplete && steps[index - 1]?.isComplete },
      ]"
    >
      <button
        class="step-button"
        type="button"
        :disabled="!step.isAvailable && !isActive(step)"
        :aria-current="isActive(step) ? 'step' : undefined"
        @click="handleClick(step)"
      >
        <span class="step-circle" aria-hidden="true">
          <Check v-if="step.isComplete" :size="18" :stroke-width="2.5" />
          <Lock v-else-if="!step.isAvailable && !isActive(step)" :size="14" :stroke-width="2.25" />
          <template v-else>{{ step.id }}</template>
        </span>
        <span class="step-text">
          <span class="step-title">{{ step.title }}</span>
          <span v-if="isActive(step)" class="step-caption">目前步驟</span>
        </span>
      </button>
    </li>
  </ol>
</template>

<style scoped>
.stepper {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
}

.step {
  position: relative;
}

/* 圓圈之間的直向連接線(兩端步驟都完成時轉綠)。 */
.step + .step {
  margin-top: var(--space-4);
}
.step + .step::before {
  content: '';
  position: absolute;
  left: 16px;
  top: calc(-1 * var(--space-4));
  width: 2px;
  height: var(--space-4);
  background: var(--color-border);
}
.step--linked::before {
  background: var(--color-success);
}

.step-button {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  width: 100%;
  padding: 0;
  border: 0;
  background: transparent;
  color: inherit;
  font: inherit;
  text-align: left;
  cursor: pointer;
}
.step-button:disabled {
  cursor: not-allowed;
}

.step-circle {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 34px;
  height: 34px;
  flex: 0 0 34px;
  box-sizing: border-box;
  border: 2px solid var(--color-border);
  border-radius: var(--radius-circle);
  background: var(--color-bg-neutral);
  color: var(--color-text-soft);
  font-size: var(--text-md);
  font-weight: var(--font-bold);
  transition:
    background-color var(--dur-base) var(--ease-out),
    border-color var(--dur-base) var(--ease-out),
    box-shadow var(--dur-base) var(--ease-out),
    color var(--dur-base) var(--ease-out),
    transform var(--dur-fast) var(--ease-out);
}

.step-button:not(:disabled):hover .step-circle {
  transform: scale(1.06);
  border-color: var(--color-primary);
}
.step-button:not(:disabled):active .step-circle {
  transform: scale(0.96);
}

.step-text {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.step-title {
  font-size: var(--text-md);
  font-weight: var(--font-semibold);
  color: var(--color-text-secondary);
  white-space: nowrap;
}

.step-caption {
  font-size: var(--text-xs);
  font-weight: var(--font-medium);
  color: var(--color-primary);
}

/* 當前步驟:藍底白字 + 光圈,標題提亮。 */
.step--active .step-circle {
  border-color: var(--color-primary);
  background: var(--color-primary);
  color: var(--color-white);
  box-shadow: 0 0 0 4px var(--color-primary-ring);
}
.step--active .step-title {
  color: var(--color-text);
  font-weight: var(--font-bold);
}

/* 已完成:綠底打勾。 */
.step--complete .step-circle {
  border-color: var(--color-success);
  background: var(--color-success);
  color: var(--color-white);
}

/* 鎖定:灰框灰字。 */
.step--locked .step-circle {
  border-color: var(--color-border-light);
  background: transparent;
  color: var(--color-text-disabled);
}
.step--locked .step-title {
  color: var(--color-text-disabled);
}
</style>
