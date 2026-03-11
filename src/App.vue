<script setup lang="ts">
import { RouterView, RouterLink } from 'vue-router'
</script>

<template>
  <div class="app-container">
    <nav class="navbar">
      <RouterLink to="/" class="nav-btn">🏠 首頁</RouterLink>
      <RouterLink to="/upload" class="nav-btn">📤 上傳地圖</RouterLink>
    </nav>

    <div class="content">
      <RouterView />
    </div>
  </div>
</template>

<style>
body {
  margin: 0;
  font-family: sans-serif;
  background-color: #f0f2f5;
}

.navbar {
  background-color: #1a1a2e;
  padding: 15px;
  text-align: center;
  box-shadow: 0 2px 8px rgba(0,0,0,0.3);
}

.nav-btn {
  color: white;
  text-decoration: none;
  margin: 0 10px;
  padding: 8px 18px;
  background-color: #16213e;
  border-radius: 5px;
  font-weight: bold;
  transition: 0.2s;
}

.nav-btn:hover {
  background-color: #0f3460;
}

.router-link-active {
  background-color: #00c8ff;
  color: #1a1a2e;
}

.content {
  padding: 20px;
}
</style>