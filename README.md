# 3D 室內導航系統 (3D Indoor Navigation WebApp)

    免硬體建置之輕量化網頁室內導航系統

本專案旨在解決大型室內公共場域（如車站、商場）的迷航問題。跳脫傳統仰賴 Beacon、Bluetooth 或 Wi-Fi 指紋等高昂硬體定位設施的限制，本系統僅需一張場地的 2D 平面圖，結合人機協作與高效能演算法，即可於網頁端即時生成 3D 第一人稱導航體驗。

## 核心特色 (Features)

- **免安裝、零硬體依賴**：基於 Web App 架構，使用者打開瀏覽器即可使用，臨時迷路即搜即用。

- **智慧圖資處理**：使用者點擊「種子點」，系統透過 C++ 實作之洪水填充演算法 (Flood Fill) 自動識別可通行區域。

- **即時路徑規劃**：內建 A* 最短路徑演算法，精準計算起訖點最佳導航路線。

- **高效能運算橋接**：透過 WebAssembly (Wasm) 將高負載影像處理交由 C++ 執行，突破 JS 效能瓶頸。

- **沉浸式 3D 導航**：使用 Three.js 將平面網格動態轉換為 3D 虛擬走廊，並提供直觀的地面路徑引導線。

- **手機優先介面**：深色科技風設計，手機為垂直單欄流程 + 底部大按鈕，桌面（900px 以上）為側欄 + 大預覽雙欄佈局；支援淺色主題切換。

## 使用流程

系統為三步驟精靈式流程。測試請自備室內平面圖，建議使用走廊與牆面色塊分明、對比清楚的圖檔（如車站、商場的官方資訊圖）效果最佳。

1. **上傳地圖**(`/upload`)：上傳平面圖後，依序在地圖上點選 —
   - 走廊路面（種子點，可點多個不同顏色的走廊；手機可雙指縮放、按住顯示放大鏡精準取點）
   - 起點與終點
2. **路徑識別**(`/path`)：按「執行路徑識別」，WASM 引擎以種子點顏色長出可通行區域（藍色）並以 A* 規劃路徑（紅線）。結果不理想時，開「進階設定」抽屜調整辨識靈敏度與各項參數後重跑；只改起訖點可用「重算路徑」快速重算。
3. **3D 場景**(`/scene`)：俯瞰模式檢視全貌，或切換第一人稱沿引導線行走 — 虛擬搖桿移動、滑動轉視角、右上角圓形小地圖顯示即時位置，偏離路徑時可一鍵重新規劃。

## 技術棧 (Tech Stack)

- **前端框架**：Vue 3 (Composition API) + Vite

- **狀態管理**：Pinia

- **開發語言**：TypeScript, HTML, CSS

- **核心演算法引擎**：WebAssembly（C++ 編譯）

- **3D 渲染引擎**：Three.js

## 快速開始

環境需求：Node.js `^20.19.0` 或 `>=22.12.0`、pnpm。（只有要重新編譯 C++ 核心時才需要安裝 [Emscripten](https://emscripten.org/)；倉庫已內附編譯好的 `core.js` / `core.wasm`。）

### 安裝專案依賴套件
```sh
pnpm install
```

### 啟動本地端開發伺服器

```sh
pnpm dev
```

### 編譯與打包（含型別檢查）

```sh
pnpm build
```

### 型別檢查 / Lint / 格式化

```sh
pnpm type-check
pnpm lint
pnpm format
```

### 編譯 C++ 成 WebAssembly
```sh
emcc src/wasm/core.cpp -o src/wasm/core.js -s MODULARIZE=1 -s EXPORT_ES6=1 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_RUNTIME_METHODS="['HEAPU8']" --bind
```
* **`-s MODULARIZE=1`**
  將生成的 JavaScript 程式碼包裝成一個模組（Factory Function），而不是直接污染全域變數 (Global Scope)。這是現代前端開發的標準作法，確保模組可以被安全地 `await` 載入與初始化。

* **`-s EXPORT_ES6=1`**
  告訴編譯器輸出符合 ES6 標準的模組（亦即包含 `export default` 語法）。因為專案使用了 Vue 3 與 Vite 這種現代前端建置工具，必須開啟此選項才能在 Vue 元件中正常使用 `import loadWasm from '@/wasm/core.js'`。

* **`-s ALLOW_MEMORY_GROWTH=1`**
  允許 WebAssembly 的記憶體在執行期間動態增長。當 C++ 呼叫 `malloc` 需要分配的記憶體大於初始預設值（通常是 16MB）時，系統會自動擴充記憶體上限，避免處理超大解析度圖片時發生「Out of Memory (OOM)」崩潰錯誤。

* **`-s EXPORTED_RUNTIME_METHODS="['HEAPU8']"`**
  明確指示 Emscripten 暴露出底層的 `HEAPU8` 屬性。`HEAPU8` 是存取 WebAssembly 記憶體的「視窗」，開啟這個權限後，前端的 JavaScript 才能透過 `wasmModule.HEAPU8.set()` 直接將圖片的像素資料 (Uint8ClampedArray) 寫入 C++ 的記憶體空間中。

* **`--bind`**
  啟用 Embind 功能。這讓編譯器能夠讀懂 C++ 程式碼最下方的 `EMSCRIPTEN_BINDINGS` 區塊，自動將 C++ 的函數（如 `allocateMemory`、`intelligentFloodFill`）綁定並轉換為 JavaScript 可以直接呼叫的 API。

## 專案目錄結構

- `src/views/` - 三大頁面：`UploadView`（上傳與標記）、`HomeView`（路徑識別）、`SceneView`（3D 場景）

- `src/components/` - 共用 UI 元件：`AppStepper`（步驟清單）、`SlideOverDrawer`（進階設定抽屜）、`ParamSlider`（參數滑桿）

- `src/composables/` - 組合式函式：`useBreakpoint`（900px 斷點偵測，驅動桌面/手機版面切換）

- `src/stores/` - Pinia 全域狀態庫（地圖像素、種子點與起訖點、可通行遮罩與路徑節點、WASM 模組實例）

- `src/utils/` - 3D 場景輔助演算法（輪廓抽取與簡化、圓對線段碰撞）

- `src/wasm/` - C++ 核心（`core.cpp`）與 Emscripten 編譯產物（`core.js` / `core.wasm`）

- `src/assets/` - 設計 token（`tokens.css`，深色預設的全站配色/間距/陰影）與共用樣式（`ui.css`）

- `src/router/` - Vue Router 路由設定（`/upload` → `/path` → `/scene`）

- `public/` - 靜態資源（favicon）

以下目錄僅存在於本機開發環境，未納入版控：

- `test_maps/` - 測試用平面圖（多取自官方網站，版權考量不公開，請自備）

- `harness/` - 演算法 JS 原型與回歸驗證腳本（Node 直接執行，不經瀏覽器；修改 C++ 前先在此驗證）

- `change-records/` - 開發演進記錄與報告用文件
