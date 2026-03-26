# 3D 室內導航系統 (3D Indoor Navigation WebApp)

    免硬體建置之輕量化網頁室內導航系統

本專案旨在解決大型室內公共場域（如車站、商場）的迷航問題。跳脫傳統仰賴 Beacon、Bluetooth 或 Wi-Fi 指紋等高昂硬體定位設施的限制，本系統僅需一張場地的 2D 平面圖，結合人機協作與高效能演算法，即可於網頁端即時生成 3D 第一人稱導航體驗。
##  核心特色 (Features)

- **免安裝、零硬體依賴**：基於 Web App 架構，使用者打開瀏覽器即可使用，臨時迷路即搜即用。

- **智慧圖資處理**：使用者點擊「種子點」，系統透過 C++ 實作之洪水填充演算法 (Flood Fill) 自動識別可通行區域。

- **即時路徑規劃**：內建 A* 最短路徑演算法，精準計算起訖點最佳導航路線。

- **高效能運算橋接**：透過 WebAssembly (Wasm) 將高負載影像處理交由 C++ 執行，突破 JS 效能瓶頸。

- **沉浸式 3D 導航**：使用 Three.js 將平面網格動態轉換為 3D 虛擬走廊，並提供直觀的地面路徑引導線。


## 技術棧 (Tech Stack)

- **前端框架**：Vue 3 (Composition API) + Vite

- **狀態管理**：Pinia

- **開發語言**：TypeScript, HTML, CSS

- **核心演算法引擎**：WebAssembly (C++ 編譯)

- **3D 渲染引擎**：Three.js


### 安裝專案依賴套件
```sh
pnpm install
```

### 啟動本地端開發伺服器

```sh
pnpm dev
```

### 編譯與打包

```sh
pnpm build
```

### Lint with [ESLint](https://eslint.org/)

```sh
pnpm lint
```
### 編譯 C++ 成WebAssembly
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
  啟用 Embind 功能。這讓編譯器能夠讀懂 C++ 程式碼最下方的 `EMSCRIPTEN_BINDINGS` 區塊，自動將 C++ 的函數（如 `allocateMemory`、`floodFill`）綁定並轉換為 JavaScript 可以直接呼叫的 API。
## 專案核心目錄結構
- `src/views/` - 系統主要頁面 (如：Home 首頁、Upload 圖資處理、Nav 3D導航)

- `src/components/` - 可重複使用的 UI 元件

- `src/stores/` - Pinia 全域狀態庫 (負責存放跨頁面共用的地圖像素、起訖點座標)

- `src/router/` - Vue Router 網址路由設定

- `public/` - 靜態資源 (如：測試用的台北車站平面圖、3D 材質貼圖)