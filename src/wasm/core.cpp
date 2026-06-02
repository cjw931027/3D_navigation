#include <emscripten/bind.h>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <functional>
#include <cstdint>

using namespace emscripten;

uint8_t* mapBuffer = nullptr;

// intelligentFloodFill 建好的遮罩保留給後續 runAStar 使用。
static std::vector<uint8_t>  g_passableMask;
static int                   g_maskWidth  = 0;
static int                   g_maskHeight = 0;

// 路徑以 x0,y0,x1,y1,... 交錯存放，JS 以 Int32Array 讀取。
static std::vector<int32_t>  g_pathBuffer;

// 邊緣平滑 opening（erode→帶屏障 dilate）的 kernel 大小。預設 0 = 關閉。
//
// 為何關閉：此 opening 原為「3D 牆面斜邊鋸齒」而加（見 645bfce），但當時 3D 還用 greedy
// meshing 直接吃原始 mask，需要先把 mask 磨平。現在 3D 改由 SceneView 的輪廓抽取 +
// Douglas-Peucker 簡化（脫離網格）處理鋸齒，這層 mask 平滑對 3D 已多餘，只剩美化 2D 預覽。
//
// 而它有嚴重副作用：erode 會把「1~2px 寬的窄通道」整條吃掉，後續帶屏障 dilate 又因兩側是
// 牆（wallMask 屏障）而回填不過去 → 窄通道（窄門、柵欄開口）被永久侵蝕、A* 繞路、3D 變牆。
// 代價（吃窄通道）遠大於好處（只剩美化 2D 預覽），故預設關閉。
// 保留函式與此變數：將來若 2D 預覽鋸齒需處理，改回 >1（奇數）即可重新啟用。
static int gEdgeSmoothKernelSize = 0;

int allocateMemory(int size) {
    if (mapBuffer != nullptr) free(mapBuffer);
    mapBuffer = (uint8_t*)malloc(size);
    return (int)mapBuffer;
}

void freeMemory() {
    if (mapBuffer != nullptr) {
        free(mapBuffer);
        mapBuffer = nullptr;
    }
}

// 計算兩個RGB顏色的差異程度，歐幾里得距離平方，省略開根號
inline int colorDistSq(uint8_t r1, uint8_t g1, uint8_t b1,
                        uint8_t r2, uint8_t g2, uint8_t b2) {
    int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr*dr + dg*dg + db*db;
}

struct RGB { uint8_t r, g, b; };

struct HSL { float h, s, l; };

// 依照點選的種子點搜索周圍radius內最常出現的顏色，當作真正的種子點，以防點錯(採色半徑)
std::vector<RGB> sampleDominantColors(int cx, int cy, int width, int height,
                                       int radius, int quantShift, int topK) {
    std::unordered_map<uint32_t, int> freq;
    int totalSamples = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int i = (ny * width + nx) * 4;
            uint8_t r = (mapBuffer[i]   >> quantShift) << quantShift;
            uint8_t g = (mapBuffer[i+1] >> quantShift) << quantShift;
            uint8_t b = (mapBuffer[i+2] >> quantShift) << quantShift;
            uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            freq[key]++;
            totalSamples++;
        }
    }

    int minCount = std::max(1, totalSamples / 10);
    std::vector<std::pair<int, uint32_t>> sorted;
    sorted.reserve(freq.size());
    for (auto& kv : freq) {
        if (kv.second >= minCount)
            sorted.push_back({ kv.second, kv.first });
    }
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
        return a.first > b.first;
    });

    std::vector<RGB> result;
    int k = std::min(topK, (int)sorted.size());
    for (int i = 0; i < k; i++) {
        uint32_t c = sorted[i].second;
        result.push_back({
            (uint8_t)((c >> 16) & 0xFF),
            (uint8_t)((c >>  8) & 0xFF),
            (uint8_t)( c        & 0xFF)
        });
    }
    if (result.empty()) {
        uint32_t best = 0; int bestCount = 0;
        for (auto& kv : freq)
            if (kv.second > bestCount) { bestCount = kv.second; best = kv.first; }
        result.push_back({
            (uint8_t)((best >> 16) & 0xFF),
            (uint8_t)((best >>  8) & 0xFF),
            (uint8_t)( best        & 0xFF)
        });
    }
    return result;
}

// 將最後兩個變數固定，quantShift是把相近的顏色歸類成同一類，設定為3代表使用位元運算(較快)，會同時同除8再乘8
// topK是指選擇最前面幾個點當作種子點，設定為1就是第一多的當種子點
RGB sampleDominantColor(int cx, int cy, int width, int height, int radius) {
    auto colors = sampleDominantColors(cx, cy, width, height, radius, 3, 1);
    return colors[0];
}

// 斷點填補功能，對每個kSize的正方形，如果正方形中有1代表可以走，那就將所有正方形的值設為可以走
// 因為是要偵測牆壁周圍有沒有可行走區域，所以會比較多牆壁，使用if先判斷可以減少時間
void dilate(std::vector<uint8_t>& mask, int width, int height, int kSize) {
    if (kSize <= 1) return;
    std::vector<uint8_t> result = mask;
    int half = kSize / 2;
    for (int y = half; y < height - half; y++)
        for (int x = half; x < width - half; x++)
            if (mask[y*width + x] == 1)
                for (int ky = -half; ky <= half; ky++)
                    for (int kx = -half; kx <= half; kx++)
                        result[(y+ky)*width + (x+kx)] = 1;
    mask = result;
}


// 將斷點填補的點填回去，以及牆壁加厚
// 因為是在牆壁附近判斷kSize周圍有沒有牆壁，很容易就有牆壁，所以判斷如果有牆壁就停止迴圈，也是會減少時間
void erode(std::vector<uint8_t>& mask, int width, int height, int kSize) {
    if (kSize <= 1) return;
    std::vector<uint8_t> result = mask;
    int half = kSize / 2;
    for (int y = half; y < height - half; y++) {
        for (int x = half; x < width - half; x++) {
            bool allOne = true;
            for (int ky = -half; ky <= half && allOne; ky++)
                for (int kx = -half; kx <= half && allOne; kx++)
                    if (mask[(y+ky)*width + (x+kx)] == 0) allOne = false;
            result[y*width + x] = allOne ? 1 : 0;
        }
    }
    mask = result;
}

// 帶屏障的膨脹：與 dilate 相同，但膨脹時絕不覆蓋 wallMask 中標記為牆的像素。
// 空的 wallMask 代表功能停用，等同原本的 dilate。
void dilateWithBarrier(std::vector<uint8_t>& mask, const std::vector<uint8_t>& wallMask,
                        int width, int height, int kSize) {
    if (kSize <= 1) return;
    if (wallMask.empty()) { dilate(mask, width, height, kSize); return; }
    std::vector<uint8_t> result = mask;
    int half = kSize / 2;
    for (int y = half; y < height - half; y++)
        for (int x = half; x < width - half; x++)
            if (mask[y*width + x] == 1)
                for (int ky = -half; ky <= half; ky++)
                    for (int kx = -half; kx <= half; kx++) {
                        int ni = (y+ky)*width + (x+kx);
                        if (wallMask[ni] == 0) // 只有非牆像素才允許膨脹
                            result[ni] = 1;
                    }
    mask = result;
}

// === buildWallMask 可調門檻 ===
// 演算法：對深色像素的 8 連通域，計算 maxSpan、包圍盒面積、填充率，
// 任一條件滿足即視為牆：
//   (A) maxSpan >= 外部傳入 spanThreshold        ── 長條牆（直接判定）
//   (B) bboxArea >= WALL_MIN_BBOX_AREA AND
//       minSpan >= WALL_MIN_BBOX_MIN_SPAN AND
//       fillRatio <= WALL_MAX_FILL_RATIO          ── 空心框（小房間框線）
//   (C) bboxArea >= WALL_MIN_BBOX_AREA AND
//       perimeterCoverage >= WALL_MIN_PERIM_COVERAGE
//                                                 ── 框線+內部文字相連時，
//                                                    用「邊緣覆蓋率」補救判別
// fillRatio = 連通域像素數 / 包圍盒面積。文字筆劃密度高 (>0.35)，
// 牆框只佔包圍盒邊緣，密度低 (<0.25)。
// perimeterCoverage = 連通域中位於包圍盒邊緣 PERIM_BAND_THICK 厚度內、
// 且該行/列實際有像素的比例。空心框沿著四邊有像素，覆蓋率高 (>0.6)；
// 即使框內文字把整體 fillRatio 拉高，框線本身仍會把四邊填滿。
//
// 各門檻設成具名常數，方便依現場資料微調：
static constexpr int   WALL_MIN_BBOX_AREA       = 1500; // 包圍盒面積下限：低於此值多半是單一文字
static constexpr int   WALL_MIN_BBOX_MIN_SPAN   = 25;   // 包圍盒短邊下限：太細長的物件不視為框
static constexpr float WALL_MAX_FILL_RATIO      = 0.25f;// 純框線的填充率上限
static constexpr int   PERIM_BAND_THICK         = 2;    // 視為「邊緣」的厚度（像素）
static constexpr float WALL_MIN_PERIM_COVERAGE  = 0.60f;// 邊緣覆蓋率下限：框線即使被文字「污染」也應接近 1.0
static constexpr float WALL_RULE_C_MAX_FILL     = 0.55f;// 規則 C 額外限制：避免 B/口/田 等粗體字（邊緣覆蓋率也高）誤判
static constexpr int   WALL_DARK_CHROMA_MAX     = 30;   // 彩度上限：超過視為彩色（紅箭頭等），不視為深色
// 規則 D：直線型「牆碎片」── 小房間的框線常被切成多段獨立直線
// 偵測：短邊極細（≤ LINE_MAX_MIN_SPAN）且長邊夠長（≥ LINE_MIN_MAX_SPAN）
// 文字筆劃即便細也不會出現「長度遠大於寬度」的長條形 bbox
static constexpr int   LINE_MAX_MIN_SPAN        = 3;    // 直線碎片：短邊最多幾像素
static constexpr int   LINE_MIN_MAX_SPAN        = 20;   // 直線碎片：長邊最少幾像素

// === 淺灰「線狀」框線補抓 ===
// 有些平面圖的最外圈邊界線畫得很淡（例如 hospital.jpg 外框 brightness≈213），
// 超過 DARK_THRESHOLD(170) → 連 isDark 都進不了 → 四規則完全輪不到它判，
// 結果整圈外框沒被當牆，A* 會把路徑導向院區外那一圈（實際無路可走）。
//
// 不能只調高 DARK_THRESHOLD：那會讓大片淡灰底色（如某些圖的房間填色）整片被誤收成牆。
// 關鍵差異不在亮度而在「形狀」── 外框是細長連續的「線」，淡灰填色是「面」。
//
// 解法：對「淺灰候選層」(brightness ∈ [DARK_THRESHOLD, FAINT_DARK_THRESHOLD)、仍低彩度)
// 獨立做 8 連通域，只有「細長線狀」(minSpan ≤ FAINT_LINE_MAX_MIN_SPAN AND
// maxSpan ≥ FAINT_LINE_MIN_MAX_SPAN) 的連通域才併入 isDark；大色塊（面）被擋掉。
// 併入後一切照舊跑主連通域 + A/B/C/D 規則（淡灰外框多以規則 A/D 命中）。
static constexpr uint8_t FAINT_DARK_THRESHOLD   = 225;  // 淺灰層亮度上限（介於核心牆 170 與白底 ~254 之間）
static constexpr int   FAINT_LINE_MAX_MIN_SPAN  = 4;    // 淺灰線狀：短邊最多幾像素（夠細才算線）
static constexpr int   FAINT_LINE_MIN_MAX_SPAN  = 40;   // 淺灰線狀：長邊最少幾像素（夠長才算框線，非雜點）

// 統計資訊，給 test harness 使用（emscripten 主流程不需要）
struct WallComponentStat {
    int minX, minY, maxX, maxY;
    int pixels;
    int maxSpan;
    int bboxArea;
    float fillRatio;
    float perimCoverage;
    bool isWall;
    int  ruleHit; // 1=長牆 A, 2=空心框 B, 3=邊緣覆蓋 C, 0=非牆
};

// 純函式版：以任意 RGBA buffer 為輸入；spanThreshold<=0 回傳空向量。
// statsOut 為可選，用於除錯與測試輸出。
std::vector<uint8_t> buildWallMaskFromBuffer(const uint8_t* buf,
                                              int width, int height,
                                              uint8_t darkThreshold,
                                              int spanThreshold,
                                              std::vector<WallComponentStat>* statsOut = nullptr) {
    std::vector<uint8_t> wallMask;
    if (spanThreshold <= 0) return wallMask;

    int total = width * height;
    wallMask.assign(total, 0);

    // 標記深色像素：必須同時是「暗」且「低彩度」，避免紅色箭頭等彩色像素被誤抓
    std::vector<bool> isDark(total, false);
    for (int i = 0; i < total; i++) {
        int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
        int brightness = (r + g + b) / 3;
        int chroma = std::max({r, g, b}) - std::min({r, g, b});
        if (brightness < (int)darkThreshold && chroma < WALL_DARK_CHROMA_MAX) isDark[i] = true;
    }

    const int dx8[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    const int dy8[] = {-1, -1, -1,  0, 0,  1, 1, 1};

    std::vector<int> queue;
    queue.reserve(512);

    // === 淺灰線狀框線補抓（見上方常數註解）===
    // 對「淺灰候選層」獨立連通，只把細長線狀者併入 isDark（大色塊不收）。
    // 淺灰層定義：darkThreshold ≤ brightness < FAINT_DARK_THRESHOLD，且仍低彩度。
    {
        std::vector<bool> isFaint(total, false);
        for (int i = 0; i < total; i++) {
            int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
            int brightness = (r + g + b) / 3;
            int chroma = std::max({r, g, b}) - std::min({r, g, b});
            if (brightness >= (int)darkThreshold && brightness < (int)FAINT_DARK_THRESHOLD
                && chroma < WALL_DARK_CHROMA_MAX) {
                isFaint[i] = true;
            }
        }
        std::vector<bool> faintVisited(total, false);
        std::vector<int> fq;
        fq.reserve(512);
        for (int start = 0; start < total; start++) {
            if (!isFaint[start] || faintVisited[start]) continue;
            fq.clear();
            fq.push_back(start);
            faintVisited[start] = true;
            int minX = start % width, maxX = minX;
            int minY = start / width, maxY = minY;
            int head = 0;
            while (head < (int)fq.size()) {
                int cur = fq[head++];
                int cx = cur % width, cy = cur / width;
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;
                for (int d = 0; d < 8; d++) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                    int ni = ny * width + nx;
                    if (!faintVisited[ni] && isFaint[ni]) {
                        faintVisited[ni] = true;
                        fq.push_back(ni);
                    }
                }
            }
            int bboxW = maxX - minX + 1;
            int bboxH = maxY - minY + 1;
            int faintMaxSpan = std::max(bboxW, bboxH) - 1;
            int faintMinSpan = std::min(bboxW, bboxH);
            // 只有「細長線狀」的淺灰連通域才採納為牆候選（併入 isDark）；色塊（面）丟棄。
            if (faintMinSpan <= FAINT_LINE_MAX_MIN_SPAN && faintMaxSpan >= FAINT_LINE_MIN_MAX_SPAN) {
                for (int idx : fq) isDark[idx] = true;
            }
        }
    }

    std::vector<bool> visited(total, false);

    for (int start = 0; start < total; start++) {
        if (!isDark[start] || visited[start]) continue;

        queue.clear();
        queue.push_back(start);
        visited[start] = true;

        int minX = start % width, maxX = minX;
        int minY = start / width, maxY = minY;
        int head = 0;

        while (head < (int)queue.size()) {
            int cur = queue[head++];
            int cx = cur % width, cy = cur / width;
            if (cx < minX) minX = cx;
            if (cx > maxX) maxX = cx;
            if (cy < minY) minY = cy;
            if (cy > maxY) maxY = cy;

            for (int d = 0; d < 8; d++) {
                int nx = cx + dx8[d], ny = cy + dy8[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!visited[ni] && isDark[ni]) {
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }

        int bboxW = maxX - minX + 1;
        int bboxH = maxY - minY + 1;
        int maxSpan = std::max(bboxW, bboxH) - 1; // 與舊版定義保持一致（差值，不含 +1）
        int minSpan = std::min(bboxW, bboxH);
        int bboxArea = bboxW * bboxH;
        int pixels = (int)queue.size();
        float fillRatio = (float)pixels / (float)std::max(1, bboxArea);

        // 計算「邊緣覆蓋率」：bbox 四邊各取 PERIM_BAND_THICK 厚度，
        // 看每一行/列在邊緣帶內是否至少有 1 個連通域像素。
        // 框線即便連到內部文字，四邊邊緣行/列仍會被框線佔滿。
        float perimCoverage = 0.0f;
        if (bboxArea >= WALL_MIN_BBOX_AREA) {
            // 在 bbox 內建一張小型 occupancy map
            std::vector<uint8_t> occ(bboxArea, 0);
            for (int idx : queue) {
                int x = idx % width, y = idx / width;
                occ[(y - minY) * bboxW + (x - minX)] = 1;
            }
            auto rowHasInBand = [&](int row) {
                // row 為 bbox 內 y 座標：檢查 row 該列在左/右邊緣 PERIM_BAND_THICK 內是否有像素
                for (int x = 0; x < std::min(PERIM_BAND_THICK, bboxW); x++)
                    if (occ[row * bboxW + x]) return true;
                for (int x = std::max(0, bboxW - PERIM_BAND_THICK); x < bboxW; x++)
                    if (occ[row * bboxW + x]) return true;
                return false;
            };
            auto colHasInBand = [&](int col) {
                for (int y = 0; y < std::min(PERIM_BAND_THICK, bboxH); y++)
                    if (occ[y * bboxW + col]) return true;
                for (int y = std::max(0, bboxH - PERIM_BAND_THICK); y < bboxH; y++)
                    if (occ[y * bboxW + col]) return true;
                return false;
            };
            int hit = 0, sample = 0;
            // 取上/下兩條 row band：對每個 column 檢查上下邊緣帶內是否有像素
            for (int x = 0; x < bboxW; x++) { if (colHasInBand(x)) hit++; sample++; }
            // 取左/右兩條 col band：對每個 row 檢查左右邊緣帶內是否有像素
            for (int y = 0; y < bboxH; y++) { if (rowHasInBand(y)) hit++; sample++; }
            perimCoverage = sample > 0 ? (float)hit / (float)sample : 0.0f;
        }

        int ruleHit = 0;
        bool isWall = false;
        if (maxSpan >= spanThreshold) {
            isWall = true; ruleHit = 1;                      // (A) 長牆
        } else if (bboxArea >= WALL_MIN_BBOX_AREA
                && minSpan   >= WALL_MIN_BBOX_MIN_SPAN
                && fillRatio <= WALL_MAX_FILL_RATIO) {
            isWall = true; ruleHit = 2;                      // (B) 空心框
        } else if (bboxArea >= WALL_MIN_BBOX_AREA
                && perimCoverage >= WALL_MIN_PERIM_COVERAGE
                && fillRatio <= WALL_RULE_C_MAX_FILL) {
            isWall = true; ruleHit = 3;                      // (C) 框線+文字相連
        } else if (minSpan   <= LINE_MAX_MIN_SPAN
                && maxSpan   >= LINE_MIN_MAX_SPAN) {
            isWall = true; ruleHit = 4;                      // (D) 直線碎片
        }

        if (isWall) {
            for (int idx : queue) wallMask[idx] = 1;
        }

        if (statsOut) {
            WallComponentStat s;
            s.minX = minX; s.minY = minY; s.maxX = maxX; s.maxY = maxY;
            s.pixels = pixels; s.maxSpan = maxSpan;
            s.bboxArea = bboxArea; s.fillRatio = fillRatio;
            s.perimCoverage = perimCoverage;
            s.isWall = isWall; s.ruleHit = ruleHit;
            statsOut->push_back(s);
        }
    }

    return wallMask;
}

// 舊介面：emscripten 主流程使用，內部轉呼叫 buildWallMaskFromBuffer。
std::vector<uint8_t> buildWallMask(int width, int height,
                                    uint8_t darkThreshold, int spanThreshold) {
    return buildWallMaskFromBuffer(mapBuffer, width, height, darkThreshold, spanThreshold);
}

// 洪水填充演算法
int bfsFill(int width, int height, int seedX, int seedY,
            const std::vector<uint8_t>& passableMask,
            bool doColor) {
    if (passableMask[seedY * width + seedX] == 0) return 0;

    std::vector<bool> visited(width * height, false);
    std::vector<int> qX, qY;
    qX.reserve(width * height / 4);
    qY.reserve(width * height / 4);
    qX.push_back(seedX);
    qY.push_back(seedY);
    visited[seedY * width + seedX] = true;

    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    int head = 0, count = 0;

    while (head < (int)qX.size()) {
        int cx = qX[head], cy = qY[head++];
        count++;
        if (doColor) {
            int pi = (cy * width + cx) * 4;
            mapBuffer[pi]   = 0;
            mapBuffer[pi+1] = 200;
            mapBuffer[pi+2] = 255;
        }
        for (int i = 0; i < 4; i++) {
            int nx = cx + dx[i], ny = cy + dy[i];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int ni = ny * width + nx;
            if (!visited[ni] && passableMask[ni] == 1) {
                visited[ni] = true;
                qX.push_back(nx);
                qY.push_back(ny);
            }
        }
    }
    return count;
}

// 防止使用者起訖點點到牆壁
bool findNearestPassable(int& sx, int& sy, int width, int height,
                         const std::vector<uint8_t>& mask, int searchR = 12) {
    if (mask[sy * width + sx] == 1) return true;
    for (int r = 1; r <= searchR; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                int nx = sx + dx, ny = sy + dy;
                if (nx >= 0 && nx < width && ny >= 0 && ny < height)
                    if (mask[ny * width + nx] == 1) { sx = nx; sy = ny; return true; }
            }
    return false;
}

std::vector<uint8_t> buildPassableMaskRGB(int width, int height,
                                           const std::vector<RGB>& pathColors,
                                           int pathTolSq,
                                           int closingKernelSize, int wallThicken,
                                           const std::vector<uint8_t>& wallMask = {}) {
    int total = width * height;
    std::vector<uint8_t> mask(total, 0);

    for (int i = 0; i < total; i++) {
        uint8_t pr = mapBuffer[i*4], pg = mapBuffer[i*4+1], pb = mapBuffer[i*4+2];
        for (auto& c : pathColors) {
            if (colorDistSq(pr, pg, pb, c.r, c.g, c.b) <= pathTolSq) {
                mask[i] = 1;
                break;
            }
        }
    }

    if (closingKernelSize > 1) {
        dilateWithBarrier(mask, wallMask, width, height, closingKernelSize);
        erode(mask, width, height, closingKernelSize);
    }
    if (wallThicken > 0) {
        erode(mask, width, height, wallThicken * 2 + 1);
    }
    return mask;
}

// 對 passableMask 做 closing（dilate→erode）填補走廊內被誤判為牆的小洞（小字、圖示等）。
// dilate 階段一律不可覆蓋 wallMask 中的真牆；erode 階段純粹收縮可走區、不會吃到牆，無需 barrier。
void closeSmallHolesWithBarrier(std::vector<uint8_t>& mask,
                                 const std::vector<uint8_t>& wallMask,
                                 int width, int height, int kSize) {
    if (kSize <= 1) return;
    dilateWithBarrier(mask, wallMask, width, height, kSize);
    erode(mask, width, height, kSize);
}

// 對 passableMask 做 opening（erode→dilate）平滑邊緣鋸齒。
// erode 階段把 1~2 px 的邊緣突起磨掉；dilate 階段把整體形狀還原回原本範圍。
// dilate 必須走 dilateWithBarrier，不可越過 wallMask（真牆絕對不能被覆蓋）。
// kSize 寧小勿大：太大會吃掉窄門與細牆。
void openWithBarrier(std::vector<uint8_t>& mask,
                     const std::vector<uint8_t>& wallMask,
                     int width, int height, int kSize) {
    if (kSize <= 1) return;
    erode(mask, width, height, kSize);
    dilateWithBarrier(mask, wallMask, width, height, kSize);
}

// 清除「牆」這邊面積小於 minArea 的孤立連通域：走廊裡偶有的小黑點 / 邊緣鋸齒突起
// 會被視為可走。但若該連通域有任一格屬於 wallMask，整塊都保留（屬於真牆延伸）。
void removeSmallWallComponentsWithBarrier(std::vector<uint8_t>& mask,
                                          const std::vector<uint8_t>& wallMask,
                                          int width, int height, int minArea) {
    if (minArea <= 0) return;
    int total = width * height;
    std::vector<bool> visited(total, false);

    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};

    for (int start = 0; start < total; start++) {
        if (visited[start] || mask[start] == 1) continue;

        std::vector<int> component;
        std::vector<int> queue;
        queue.push_back(start);
        visited[start] = true;
        bool touchesWall = false;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            if (!wallMask.empty() && wallMask[cur] == 1) touchesWall = true;
            int cx = cur % width, cy = cur / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!visited[ni] && mask[ni] == 0) {
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }

        if (!touchesWall && (int)component.size() < minArea) {
            for (int idx : component) mask[idx] = 1;
        }
    }
}

// 清除遮罩中面積小於 minArea 的孤立連通域，避免雜訊碎塊干擾 A*。
void removeSmallMaskComponents(std::vector<uint8_t>& mask,
                                int width, int height, int minArea) {
    int total = width * height;
    std::vector<bool> visited(total, false);

    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};

    for (int start = 0; start < total; start++) {
        if (visited[start] || mask[start] == 0) continue;

        std::vector<int> component;
        std::vector<int> queue;
        queue.reserve(minArea * 2);
        queue.push_back(start);
        visited[start] = true;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            int cx = cur % width, cy = cur / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!visited[ni] && mask[ni] == 1) {
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }

        if ((int)component.size() < minArea) {
            for (int idx : component) mask[idx] = 0;
        }
    }
}

//   對深色像素做 8 連通域分析，包圍盒最大跨度 < spanThreshold 的視為文字排除，
//   其餘（真牆壁）列入屏障，dilate 時不得越過。設為 0 等同舊行為（無屏障）。
void intelligentFloodFill(int width, int height,
                           int seedX, int seedY,
                           int pathColorTolerance,
                           int closingKernelSize,
                           int wallThicken,
                           int sampleRadius,
                           int denoiseMinArea,
                           int spanThreshold,
                           int smoothClosingSize,
                           int smoothMinWallArea) {
    if (mapBuffer == nullptr) return;
    if (seedX < 0 || seedX >= width || seedY < 0 || seedY >= height) return;

    // 以深色像素的 8 連通域跨度區分「文字」與「牆壁」，建立絕對屏障遮罩。
    // darkThreshold = 170：放寬為「深色 + 中灰」皆視為候選；
    // 許多平面圖的房間框線是淺灰（亮度 130~165），128 抓不到，
    // 必須放寬到 170 才能涵蓋。低彩度限制 (chroma<30) 仍排除彩色填充與紅箭頭。
    const uint8_t DARK_THRESHOLD = 170;
    std::vector<uint8_t> wallMask = buildWallMask(width, height, DARK_THRESHOLD, spanThreshold);

    std::vector<uint8_t> mask;

    auto pathColors = sampleDominantColors(seedX, seedY, width, height,
                                                sampleRadius, 3, 1);
        int tolSq = pathColorTolerance * pathColorTolerance;
        mask = buildPassableMaskRGB(width, height, pathColors, tolSq,
                                     closingKernelSize, wallThicken, wallMask);

    // 邊緣平滑（opening: erode→帶屏障 dilate）。預設關閉（gEdgeSmoothKernelSize=0 → no-op）。
    // 3D 鋸齒已改由 SceneView 輪廓簡化處理，此層對 3D 多餘；且 erode 會吃掉窄通道、屏障 dilate
    // 又救不回，導致窄門/柵欄開口不可走。詳見 gEdgeSmoothKernelSize 宣告處的說明。
    openWithBarrier(mask, wallMask, width, height, gEdgeSmoothKernelSize);

    if (denoiseMinArea > 0) {
        removeSmallMaskComponents(mask, width, height, denoiseMinArea);
    }

    int searchR = 12 + wallThicken * 3;
    int sx = seedX, sy = seedY;
    if (!findNearestPassable(sx, sy, width, height, mask, searchR)) return;

    // 先做 connected-component 限制:只保留種子點的連通分量
    std::vector<uint8_t> seedMask(mask.size(), 0);
    if (mask[sy * width + sx] == 1) {
        std::vector<int> q;
        q.push_back(sy * width + sx);
        seedMask[sy * width + sx] = 1;
        const int dx[] = {0,0,-1,1};
        const int dy[] = {-1,1,0,0};
        for (int h = 0; h < (int)q.size(); h++) {
            int idx = q[h];
            int cx = idx % width, cy = idx / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx[d], ny = cy + dy[d];
                if (nx<0||nx>=width||ny<0||ny>=height) continue;
                int ni = ny*width + nx;
                if (seedMask[ni] == 0 && mask[ni] == 1) {
                    seedMask[ni] = 1;
                    q.push_back(ni);
                }
            }
        }
    }
    mask = seedMask;

    // 平滑階段：清理走廊內小黑點 / 邊緣鋸齒。所有「牆 → 可走」翻轉都受 wallMask 屏障保護，
    // 確保 buildWallMask 認定的真牆壁不會被吃掉。順序：先 closing 補小洞 → 再清孤立小牆塊。
    if (smoothClosingSize > 1) {
        closeSmallHolesWithBarrier(mask, wallMask, width, height, smoothClosingSize);
    }
    if (smoothMinWallArea > 0) {
        removeSmallWallComponentsWithBarrier(mask, wallMask, width, height, smoothMinWallArea);
    }

    bfsFill(width, height, sx, sy, mask, true); // 渲染出可行走區域
    g_passableMask = mask;
    g_maskWidth    = width;
    g_maskHeight   = height;
}

int runAStar(int startX, int startY, int endX, int endY) {
    g_pathBuffer.clear();

    if (g_passableMask.empty() || g_maskWidth <= 0 || g_maskHeight <= 0) return 0;

    int W = g_maskWidth, H = g_maskHeight;

    int sx = startX, sy = startY;
    int ex = endX,   ey = endY;
    if (!findNearestPassable(sx, sy, W, H, g_passableMask, 20)) return 0;
    if (!findNearestPassable(ex, ey, W, H, g_passableMask, 20)) return 0;

    if (sx == ex && sy == ey) {
        g_pathBuffer = { sx, sy };
        return 1;
    }

    struct Node {
        float f, g;
        int   idx;
        bool operator>(const Node& o) const { return f > o.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> openSet;

    std::vector<float> gCost(W * H, 1e30f);
    std::vector<int>   parent(W * H, -1);
    std::vector<bool>  closed(W * H, false);

    const int   DDX[8]  = {  0,  0, -1,  1, -1, -1,  1,  1 };
    const int   DDY[8]  = { -1,  1,  0,  0, -1,  1, -1,  1 };
    const float COST[8] = { 1.f, 1.f, 1.f, 1.f,
                             1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f };

    auto heuristic = [&](int x, int y) -> float {
        float dx = (float)std::abs(x - ex);
        float dy = (float)std::abs(y - ey);
        return std::max(dx, dy) + (1.41421356f - 1.0f) * std::min(dx, dy);
    };

    int startIdx = sy * W + sx;
    int endIdx   = ey * W + ex;
    gCost[startIdx] = 0.0f;
    openSet.push({ heuristic(sx, sy), 0.0f, startIdx });

    bool found = false;

    while (!openSet.empty()) {
        Node cur = openSet.top(); openSet.pop();
        int ci = cur.idx;
        if (closed[ci]) continue;
        closed[ci] = true;
        if (ci == endIdx) { found = true; break; }

        int cx = ci % W, cy = ci / W;

        for (int d = 0; d < 8; d++) {
            int nx = cx + DDX[d], ny = cy + DDY[d];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            int ni = ny * W + nx;
            if (closed[ni] || g_passableMask[ni] == 0) continue;

            // 斜角不得穿越兩側牆角。
            if (d >= 4) {
                int sideAx = cx + DDX[d], sideAy = cy;
                int sideBx = cx,          sideBy = cy + DDY[d];
                if (g_passableMask[sideAy * W + sideAx] == 0 ||
                    g_passableMask[sideBy * W + sideBx] == 0) continue;
            }

            float ng = cur.g + COST[d];
            if (ng < gCost[ni]) {
                gCost[ni]  = ng;
                parent[ni] = ci;
                openSet.push({ ng + heuristic(nx, ny), ng, ni });
            }
        }
    }

    if (!found) return 0;

    std::vector<int32_t> reversed;
    for (int i = endIdx; i != -1; i = parent[i]) {
        reversed.push_back(i % W);
        reversed.push_back(i / W);
    }

    int pairCount = (int)(reversed.size() / 2);
    g_pathBuffer.resize(reversed.size());
    for (int i = 0; i < pairCount; i++) {
        g_pathBuffer[i * 2]     = reversed[(pairCount - 1 - i) * 2];      // x
        g_pathBuffer[i * 2 + 1] = reversed[(pairCount - 1 - i) * 2 + 1];  // y
    }

    return pairCount;
}

int getPathBuffer() {
    return (int)(intptr_t)(g_pathBuffer.data());
}

int getPathLength() {
    return (int)(g_pathBuffer.size() / 2);
}

int getPassableMaskBuffer()  { return (int)(intptr_t)(g_passableMask.data()); }
int getPassableMaskSize()    { return (int)g_passableMask.size(); }
int getPassableMaskWidth()   { return g_maskWidth;  }
int getPassableMaskHeight()  { return g_maskHeight; }

EMSCRIPTEN_BINDINGS(my_module) {
    function("allocateMemory",        &allocateMemory);
    function("freeMemory",            &freeMemory);
    function("intelligentFloodFill",  &intelligentFloodFill);
    function("runAStar",              &runAStar);
    function("getPathBuffer",         &getPathBuffer);
    function("getPathLength",         &getPathLength);
    function("getPassableMaskBuffer", &getPassableMaskBuffer);
    function("getPassableMaskSize",   &getPassableMaskSize);
    function("getPassableMaskWidth",  &getPassableMaskWidth);
    function("getPassableMaskHeight", &getPassableMaskHeight);
}
