<script setup lang="ts">
  import { useMapStore } from '@/stores/mapStore'
  const mapStore = useMapStore()
</script>

<template>
  <main>
      <div class="info-box">
        <!-- 這裡直接去 mapStore 拿出 mapWidth 跟 mapHeight 顯示在畫面上 -->
        <p>現在地圖寬度是： <strong>{{ mapStore.mapWidth }}</strong></p>
        <p>現在地圖高度是： <strong>{{ mapStore.mapHeight }}</strong></p>
    </div>
  </main>
</template>

<style>
.info-box {
  margin: 20px auto;
  padding: 20px;
  border: 2px dashed #ccc;
  border-radius: 8px;
  max-width: 300px;
  background-color: #f9f9f9;
  color: #333;
}
</style>