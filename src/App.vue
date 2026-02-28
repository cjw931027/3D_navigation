<script setup lang="ts">
// 把 RouterLink (任意門) 也拿進來用
import { RouterView, RouterLink } from 'vue-router'
</script>

<template>
  <div class="app-container">
    <!-- 這是我們的全域導覽列，它永遠不會消失 -->
    <nav class="navbar">
      <!-- RouterLink 就是 Vue 版本的 <a> 標籤，但它不會重新整理網頁！ -->
      <RouterLink to="/" class="nav-btn">回首頁</RouterLink>
      <RouterLink to="/upload" class="nav-btn">去Upweload頁</RouterLink>
    </nav>

    <!-- 下面這台電視機，會根據你點的連結，切換播 Home 或 Upload 的內容 -->
    <div class="content">
      <RouterView />
    </div>
  </div>
</template>

<style>
/* 簡單的導覽列樣式 */
body {
  margin: 0;
  font-family: sans-serif;
  background-color: #f0f2f5;
}

.navbar {
  background-color: #333;
  padding: 15px;
  text-align: center;
}

.nav-btn {
  color: white;
  text-decoration: none;
  margin: 0 10px;
  padding: 8px 15px;
  background-color: #555;
  border-radius: 5px;
  font-weight: bold;
}

.nav-btn:hover {
  background-color: #4CAF50;
}

/* 當前所在頁面的按鈕會變色 */
.router-link-active {
  background-color: #4CAF50;
}

.content {
  padding: 20px;
}
</style>