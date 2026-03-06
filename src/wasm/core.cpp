#include <emscripten/bind.h>
#include <iostream>
#include <cstdlib> 

using namespace emscripten;

// 這是一個全域指標，用來記錄我們跟系統借了哪一塊記憶體來放圖片
uint8_t* mapBuffer = nullptr;

// 1. 開闢記憶體 (讓 JS 呼叫)
// JS 告訴我們圖片有多大，我們就去借多大的空間，然後把「地址(指標)」還給 JS
int allocateMemory(int size) {
    if (mapBuffer != nullptr) {
        free(mapBuffer); // 如果之前借過，先還回去，避免記憶體爆掉
    }
    // malloc: 向系統要求分配 size 大小的記憶體
    mapBuffer = (uint8_t*)malloc(size);
    
    // 把記憶體地址 (Pointer) 轉成整數回傳給 JS
    return (int)mapBuffer;
}

// 2. C++ 高速影像處理測試：把整張圖片變成「反色 (負片)」
void invertColors(int size) {
    if (mapBuffer == nullptr) return; // 安全檢查

    // 圖片陣列的結構是 [R, G, B, A, R, G, B, A...]，每 4 個數字是一個像素
    for (int i = 0; i < size; i += 4) {
        mapBuffer[i]     = 255 - mapBuffer[i];     // R (紅色反轉)
        mapBuffer[i + 1] = 255 - mapBuffer[i + 1]; // G (綠色反轉)
        mapBuffer[i + 2] = 255 - mapBuffer[i + 2]; // B (藍色反轉)
        // mapBuffer[i + 3] 是透明度 (A)，我們不更動它
    }
}

// 3. 釋放記憶體 
void freeMemory() {
    if (mapBuffer != nullptr) {
        free(mapBuffer);
        mapBuffer = nullptr;
    }
}

EMSCRIPTEN_BINDINGS(my_module) {
    function("allocateMemory", &allocateMemory);
    function("invertColors", &invertColors);
    function("freeMemory", &freeMemory);
}