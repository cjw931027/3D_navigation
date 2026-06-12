<script setup lang="ts">
import { ref } from 'vue'

// 參數滑桿:標籤 + 可選 ? 提示 + 數值徽章 + range + 可選說明文字。
defineProps<{
  label: string
  modelValue: number
  min: number
  max: number
  step: number
  tooltip?: { problem: string; fix: string }
  hint?: string
}>()

const emit = defineEmits<{ 'update:modelValue': [value: number] }>()

const showTip = ref(false)

function onInput(e: Event) {
  emit('update:modelValue', Number((e.target as HTMLInputElement).value))
}
</script>

<template>
  <div class="param-row">
    <div class="param-label">
      <span class="param-name">
        {{ label }}
        <span
          v-if="tooltip"
          class="tooltip-trigger"
          @mouseenter="showTip = true"
          @mouseleave="showTip = false"
          @touchstart.prevent="showTip = !showTip"
          >?</span
        >
        <div class="tooltip-box" v-if="tooltip && showTip">
          <p><strong>什麼情況調整:</strong>{{ tooltip.problem }}</p>
          <p><strong>如何調整:</strong>{{ tooltip.fix }}</p>
        </div>
      </span>
      <strong class="param-value">{{ modelValue }}</strong>
    </div>
    <input type="range" :value="modelValue" :min="min" :max="max" :step="step" @input="onInput" />
    <span v-if="hint" class="param-hint">{{ hint }}</span>
  </div>
</template>

<style scoped>
.param-row {
  display: flex;
  flex-direction: column;
  gap: var(--space-1);
  margin-bottom: var(--space-4);
}

.param-label {
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: var(--space-2);
  font-size: var(--text-base);
  color: var(--color-text-secondary);
}

.param-name {
  position: relative;
  display: inline-flex;
  align-items: center;
  gap: 5px;
}

.param-value {
  background: var(--color-bg-chip);
  color: var(--color-text);
  border-radius: var(--radius-pill);
  padding: var(--space-1) var(--space-3);
  font-size: var(--text-sm);
  min-width: 28px;
  text-align: center;
  font-variant-numeric: tabular-nums;
}

.tooltip-trigger {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 16px;
  height: 16px;
  border-radius: var(--radius-circle);
  background: var(--color-bg-chip);
  color: var(--color-text-soft);
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
  width: min(260px, calc(100vw - 48px));
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

input[type='range'] {
  width: 100%;
  height: 6px;
  cursor: pointer;
  accent-color: var(--color-primary);
}
input[type='range']::-webkit-slider-runnable-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}
input[type='range']::-webkit-slider-thumb {
  width: 18px;
  height: 18px;
  margin-top: -6px;
  border: 3px solid var(--color-white);
  border-radius: var(--radius-circle);
  background: var(--color-primary);
  box-shadow: var(--shadow-thumb);
}
input[type='range']::-moz-range-track {
  height: 6px;
  border-radius: var(--radius-pill);
  background: var(--color-border);
}
input[type='range']::-moz-range-thumb {
  width: 14px;
  height: 14px;
  border: 3px solid var(--color-white);
  border-radius: var(--radius-circle);
  background: var(--color-primary);
  box-shadow: var(--shadow-thumb);
}

.param-hint {
  font-size: var(--text-xs);
  color: var(--color-text-hint);
  line-height: 1.5;
}
</style>
