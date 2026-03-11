#include <emscripten/bind.h>
#include <iostream>
#include <cstdlib> 

using namespace emscripten;

// 全域指標，記錄我們跟系統借了哪一塊記憶體來放圖片
uint8_t* mapBuffer = nullptr;

// 開記憶體 (讓 JS 呼叫)
// JS 告訴我們圖片有多大，我們就去借多大的空間，然後把「地址(指標)」還給 JS
int allocateMemory(int size) {
    if (mapBuffer != nullptr) {
        free(mapBuffer);
    }
    // malloc: 向系統要求分配 size 大小的記憶體
    mapBuffer = (uint8_t*)malloc(size);
    // 把記憶體地址轉成整數回傳給 JS
    return (int)mapBuffer;
}

// 釋放記憶體 
void freeMemory() {
    if (mapBuffer != nullptr) {
        free(mapBuffer);
        mapBuffer = nullptr;
    }
}

// 計算兩個顏色的差異 (使用歐幾里得距離的平方，避免開根號以追求極致效能)
bool isColorSimilar(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2, int tolerance) {
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;
    // 兩點距離的平方 <= 容差的平方
    return (dr * dr + dg * dg + db * db) <= (tolerance * tolerance);
}

void floodFill(int width, int height, int startX, int startY, int tolerance) {
    if (mapBuffer == nullptr) return;
    
    // 檢查起點是否超出邊界
    if (startX < 0 || startX >= width || startY < 0 || startY >= height) return;

    // 取得「種子點(起點)」的顏色，作為我們要尋找的目標顏色
    int startIndex = (startY * width + startX) * 4;
    uint8_t targetR = mapBuffer[startIndex];
    uint8_t targetG = mapBuffer[startIndex + 1];
    uint8_t targetB = mapBuffer[startIndex + 2];

    // 建立一個紀錄「走過沒有」的陣列 (大小 = 圖片像素總數)
    std::vector<bool> visited(width * height, false);
    
    // 建立 Queue 來儲存準備要檢查的座標點 (為了效能，使用兩個一維陣列取代傳統 Queue)
    std::vector<int> queueX;
    std::vector<int> queueY;
    // 預先分配記憶體，避免執行到一半卡頓
    queueX.reserve(width * height / 4);
    queueY.reserve(width * height / 4);

    // 把起點放入 Queue
    queueX.push_back(startX);
    queueY.push_back(startY);
    visited[startY * width + startX] = true;

    int head = 0; // Queue 的讀取進度指標

    // 定義往 上、下、左、右 四個方向走的座標偏移量
    int dx[] = {0, 0, -1, 1};
    int dy[] = {-1, 1, 0, 0};

    // 開始擴散！直到 Queue 空了為止
    while (head < queueX.size()) {
        int cx = queueX[head];
        int cy = queueY[head];
        head++; // 讀取下一個

        // 🎯 找到路了！把這個像素塗成「發光的青藍色」(代表可通行區域)
        int pixelIndex = (cy * width + cx) * 4;
        mapBuffer[pixelIndex] = 0;       // R (0)
        mapBuffer[pixelIndex + 1] = 200; // G (200)
        mapBuffer[pixelIndex + 2] = 255; // B (255)
        // mapBuffer[pixelIndex + 3] 是透明度 A，保持原本的樣子

        // 往四個方向探測
        for (int i = 0; i < 4; i++) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];

            // 確保沒有超出圖片範圍
            if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                int nIndex = ny * width + nx;
                
                // 如果還沒走過
                if (!visited[nIndex]) {
                    int nPixelIndex = nIndex * 4;
                    uint8_t r = mapBuffer[nPixelIndex];
                    uint8_t g = mapBuffer[nPixelIndex + 1];
                    uint8_t b = mapBuffer[nPixelIndex + 2];

                    // 如果顏色跟種子點夠像，就把它加入 Queue 繼續擴散
                    if (isColorSimilar(r, g, b, targetR, targetG, targetB, tolerance)) {
                        visited[nIndex] = true; // 標記為走過
                        queueX.push_back(nx);
                        queueY.push_back(ny);
                    }
                }
            }
        }
    }
}



EMSCRIPTEN_BINDINGS(my_module) {
    function("allocateMemory", &allocateMemory);
    function("freeMemory", &freeMemory);
    function("floodFill", &floodFill);
}