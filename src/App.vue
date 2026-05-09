<script setup lang="ts">
import { computed, onMounted, type Component } from 'vue'
import { RouterView, useRoute, useRouter } from 'vue-router'
import { Box, Check, Lock, Route as RouteIcon, Upload } from 'lucide-vue-next'
import { useMapStore } from '@/stores/mapStore'

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

const isUploadComplete = computed(
  () => mapStore.imageRawData != null && mapStore.startPoint != null && mapStore.endPoint != null,
)

const isPathComplete = computed(
  () => mapStore.passableMask != null && mapStore.pathNodes.length > 0,
)

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

function getStepState(step: StepItem) {
  if (step.isComplete) return 'complete'
  if (isActiveStep(step)) return 'active'
  if (step.isAvailable) return 'ready'
  return 'locked'
}

function handleStepClick(step: StepItem) {
  if (!step.isAvailable || isActiveStep(step)) return
  router.push(step.path)
}

onMounted(() => {
  mapStore.initEngine()
})
</script>

<template>
  <div class="app-container">
    <nav class="navbar" aria-label="流程導覽">
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

    <main class="content">
      <RouterView />
    </main>
  </div>
</template>

<style>
body {
  margin: 0;
  font-family:
    -apple-system, BlinkMacSystemFont, 'Segoe UI', 'PingFang TC', 'Microsoft JhengHei',
    'Helvetica Neue', Arial, sans-serif;
  background-color: var(--color-bg-soft);
}

.app-container {
  min-height: 100vh;
}

.navbar {
  background-color: var(--color-white);
  border-bottom: 1px solid var(--color-border);
  padding: var(--space-5) var(--space-4);
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
  background-color: var(--color-white);
  color: var(--color-text-muted);
  transition:
    background-color 0.2s ease,
    border-color 0.2s ease,
    box-shadow 0.2s ease,
    color 0.2s ease;
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
</style>
