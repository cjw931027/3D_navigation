#include <emscripten/bind.h>
#include <emscripten/val.h>
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

// 邊緣平滑 opening 的 kernel 大小。預設 0=關閉（會永久吃掉窄通道，副作用大於好處）。
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

// sampleDominantColors 的單色版（quantShift=3 量化、topK=1 取最主要色）。
RGB sampleDominantColor(int cx, int cy, int width, int height, int radius) {
    auto colors = sampleDominantColors(cx, cy, width, height, radius, 3, 1);
    return colors[0];
}

// 膨脹：kSize 視窗內只要有一格可走就把整窗設為可走，用於補斷點。
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


// 侵蝕：kSize 視窗內全為可走才保留，用於削毛邊 / 牆壁加厚。
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
// 深色像素 8 連通域依四規則判牆：(A) maxSpan≥spanThreshold 長牆；(B) 大包圍盒+短邊夠寬+低填充率
// 空心框；(C) 大包圍盒+高邊緣覆蓋率 框線被文字污染補救；(D) 細長直線碎片。
// fillRatio=像素/包圍盒面積（文字密、牆框稀疏）。各門檻為具名常數，可微調。
static constexpr int   WALL_MIN_BBOX_AREA       = 1500; // 包圍盒面積下限：低於此值多半是單一文字
static constexpr int   WALL_MIN_BBOX_MIN_SPAN   = 25;   // 包圍盒短邊下限：太細長的物件不視為框
static constexpr float WALL_MAX_FILL_RATIO      = 0.25f;// 純框線的填充率上限
static constexpr int   PERIM_BAND_THICK         = 2;    // 視為「邊緣」的厚度（像素）
static constexpr float WALL_MIN_PERIM_COVERAGE  = 0.60f;// 邊緣覆蓋率下限：框線即使被文字「污染」也應接近 1.0
static constexpr float WALL_RULE_C_MAX_FILL     = 0.55f;// 規則 C 額外限制：避免 B/口/田 等粗體字（邊緣覆蓋率也高）誤判
static constexpr int   WALL_DARK_CHROMA_MAX     = 30;   // 彩度上限：超過視為彩色（紅箭頭等），不視為深色
// 規則 D：直線碎片（短邊極細 + 長邊夠長），補抓被切成多段的框線。
static constexpr int   LINE_MAX_MIN_SPAN        = 3;    // 直線碎片：短邊最多幾像素
static constexpr int   LINE_MIN_MAX_SPAN        = 20;   // 直線碎片：長邊最少幾像素

// === 字塊護欄：方塊狀 + 中等大小 + 筆劃密的深色連通域視為文字，略過所有牆規則 ===
// 解粗體大字（如三樓「病床電梯廳/訪客電梯廳」）被誤判成牆。
static constexpr int   GLYPH_MIN_SIDE           = 12;    // 字有厚度；牆線細(minSpan 小)→不算字
static constexpr int   GLYPH_MAX_SPAN           = 80;    // 字頂多數十px；真牆長邊遠大於此→不算字
static constexpr float GLYPH_MIN_FILL           = 0.30f; // 字筆劃密(≥0.3)；空心框/牆線稀疏(<0.25)→不算字

// === 淺灰「線狀」框線補抓 ===
// 有些圖最外框畫得很淡（如 hospital 外框 ~213）超過 DARK_THRESHOLD 而漏判。對淺灰候選層
// (brightness ∈ [DARK_THRESHOLD, FAINT_DARK_THRESHOLD)) 只併入「細長線狀」連通域，避免大色塊誤收。
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

    // 淺灰線狀框線補抓：淺灰候選層獨立連通，只把細長線狀者併入 isDark（大色塊不收）。
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

        // 邊緣覆蓋率：bbox 四邊邊緣帶內每行/列是否有像素的比例（框線即使連到文字，四邊仍滿）。
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

        // 字塊護欄：方塊狀 + 中等大小 + 筆劃密 → 視為文字，不進任何牆規則。
        bool isGlyph = minSpan >= GLYPH_MIN_SIDE
                    && maxSpan <  GLYPH_MAX_SPAN
                    && fillRatio >= GLYPH_MIN_FILL;

        int ruleHit = 0;
        bool isWall = false;
        if (isGlyph) {
            // 文字：略過所有牆規則
        } else if (maxSpan >= spanThreshold) {
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

// closing（帶屏障 dilate→erode）填補走廊內小洞（小字/圖示），dilate 不覆蓋真牆。
void closeSmallHolesWithBarrier(std::vector<uint8_t>& mask,
                                 const std::vector<uint8_t>& wallMask,
                                 int width, int height, int kSize) {
    if (kSize <= 1) return;
    dilateWithBarrier(mask, wallMask, width, height, kSize);
    erode(mask, width, height, kSize);
}

// opening（erode→帶屏障 dilate）平滑邊緣鋸齒；dilate 不越過真牆。kSize 寧小勿大，否則吃窄門。
void openWithBarrier(std::vector<uint8_t>& mask,
                     const std::vector<uint8_t>& wallMask,
                     int width, int height, int kSize) {
    if (kSize <= 1) return;
    erode(mask, width, height, kSize);
    dilateWithBarrier(mask, wallMask, width, height, kSize);
}

// 邊界包圍門檻：洞翻為可走前，要求外圈鄰居「可走」占比 ≥ 此值（只補被可走包圍的洞，不補獨立非路面島）。
static constexpr float FILL_HOLE_MIN_PASSABLE_BORDER = 0.70f;

// 把走道內的小障礙洞翻為可走。三條件同時成立才翻：(a) 面積 < minArea；
// (b) 不接觸真牆（保證不穿牆）；(c) 邊界可走占比 ≥ FILL_HOLE_MIN_PASSABLE_BORDER。
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
        // 邊界統計：外圈鄰居(不屬於本連通域的相鄰格)中，可走 vs 邊界總數。
        int borderTotal = 0;
        int borderPassable = 0;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            if (!wallMask.empty() && wallMask[cur] == 1) touchesWall = true;
            int cx = cur % width, cy = cur / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (mask[ni] == 1) {
                    // 鄰居是可走 → 外邊界，且是「可走邊界」(分子 + 分母)。
                    borderTotal++;
                    borderPassable++;
                } else if (!visited[ni]) {
                    // 鄰居也是不可走且未訪 → 併入同一連通域(內部，不計邊界)。
                    visited[ni] = true;
                    queue.push_back(ni);
                }
                // 已訪的真牆鄰居計入邊界分母（非可走邊界），壓低貼牆塊的可走占比。
                else if (!wallMask.empty() && wallMask[ni] == 1) {
                    borderTotal++;
                }
            }
        }

        if (touchesWall || (int)component.size() >= minArea) continue;
        // 邊界可走占比門檻(borderTotal==0 表示整塊貼著影像邊界或完全內含，視為不補)。
        if (borderTotal == 0) continue;
        float passableBorderRatio = (float)borderPassable / (float)borderTotal;
        if (passableBorderRatio < FILL_HOLE_MIN_PASSABLE_BORDER) continue;

        for (int idx : component) mask[idx] = 1;
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

// 把可走限制在矩形內（rect 外 + inset 邊一律清為不可走）。
static void clipMaskToRect(std::vector<uint8_t>& mask, int width, int height,
                           int minX, int minY, int maxX, int maxY, int inset) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (x < minX + inset || x > maxX - inset || y < minY + inset || y > maxY - inset)
                mask[y * width + x] = 0;
}

// 把灰色圖框沿矩形 4 邊畫成 thick 寬屏障（mask=0+wallMask=1），補掉偵測缺口、擋住漏出。
// 須在連通分量限制之前呼叫，框外才會與種子斷開被丟棄。
static void carveFrameWall(std::vector<uint8_t>& mask, std::vector<uint8_t>& wallMask,
                           int width, int height,
                           int minX, int minY, int maxX, int maxY, int thick) {
    auto setBarrier = [&](int x, int y) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        mask[y * width + x] = 0;
        if (!wallMask.empty()) wallMask[y * width + x] = 1;
    };
    for (int t = 0; t < thick; t++) {
        for (int x = minX; x <= maxX; x++) { setBarrier(x, minY + t); setBarrier(x, maxY - t); }
        for (int y = minY; y <= maxY; y++) { setBarrier(minX + t, y); setBarrier(maxX - t, y); }
    }
}

// 過度捕捉對策 A（clipMode=1）：裁到所有牆的包圍盒 + margin（建物密集的戶外圖，框外白底被裁掉）。
static void clipToWallBBox(std::vector<uint8_t>& mask, const std::vector<uint8_t>& wallMask,
                           int width, int height, float marginFrac) {
    if (wallMask.empty()) return;
    int minX = width, minY = height, maxX = -1, maxY = -1;
    int total = width * height;
    for (int i = 0; i < total; i++) {
        if (!wallMask[i]) continue;
        int x = i % width, y = i / width;
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
    }
    if (maxX < 0) return;
    int mx = (int)std::lround(width * marginFrac), my = (int)std::lround(height * marginFrac);
    minX = std::max(0, minX - mx); minY = std::max(0, minY - my);
    maxX = std::min(width - 1, maxX + mx); maxY = std::min(height - 1, maxY + my);
    clipMaskToRect(mask, width, height, minX, minY, maxX, maxY, 0);
}

// 過度捕捉對策 B（clipMode=2）：偵測灰色圖框矩形（低彩度灰長連續線的最外緣），供 carveFrameWall 封牆。
// 用於白路=白圖外、靠一圈灰外框分隔的院區圖（hospital）。
static bool detectFrameBBox(const uint8_t* buf, int width, int height, float minRunFrac,
                            int& oMinX, int& oMinY, int& oMaxX, int& oMaxY) {
    auto isGray = [&](int i) {
        int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
        int br = (r + g + b) / 3, ch = std::max({r,g,b}) - std::min({r,g,b});
        return br >= 150 && br <= 228 && ch < 28;
    };
    int needV = (int)(minRunFrac * height), needH = (int)(minRunFrac * width);
    int minX = -1, maxX = -1, minY = -1, maxY = -1;
    for (int x = 0; x < width; x++) {
        int run = 0, best = 0;
        for (int y = 0; y < height; y++) { if (isGray(y*width+x)) { run++; if (run>best) best=run; } else run=0; }
        if (best >= needV) { if (minX < 0) minX = x; maxX = x; }
    }
    for (int y = 0; y < height; y++) {
        int run = 0, best = 0;
        for (int x = 0; x < width; x++) { if (isGray(y*width+x)) { run++; if (run>best) best=run; } else run=0; }
        if (best >= needH) { if (minY < 0) minY = y; maxY = y; }
    }
    if (minX < 0 || minY < 0 || maxX <= minX || maxY <= minY) return false;
    oMinX = minX; oMinY = minY; oMaxX = maxX; oMaxY = maxY;
    return true;
}

// 多種子可通行辨識主流程。seedXs/seedYs 為 JS 陣列，各取主色 union 成 pathColors（多色底圖用）。
// clipMode：0=不裁、1=裁到牆包圍盒、2=灰色圖框封牆（戶外圖排除圖外白底）。
void intelligentFloodFill(int width, int height,
                           emscripten::val seedXs,
                           emscripten::val seedYs,
                           int pathColorTolerance,
                           int closingKernelSize,
                           int wallThicken,
                           int sampleRadius,
                           int denoiseMinArea,
                           int spanThreshold,
                           int smoothClosingSize,
                           int smoothMinWallArea,
                           int clipMode) {
    if (mapBuffer == nullptr) return;

    // 讀多種子座標（過濾出界者）。
    unsigned n = seedXs["length"].as<unsigned>();
    std::vector<int> sxs, sys;
    for (unsigned i = 0; i < n; i++) {
        int sx = seedXs[i].as<int>();
        int sy = seedYs[i].as<int>();
        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
        sxs.push_back(sx);
        sys.push_back(sy);
    }
    if (sxs.empty()) return;

    // 建立牆壁屏障遮罩。darkThreshold=170 涵蓋淺灰框線，低彩度限制仍排除彩色箭頭。
    const uint8_t DARK_THRESHOLD = 170;
    std::vector<uint8_t> wallMask = buildWallMask(width, height, DARK_THRESHOLD, spanThreshold);

    // 每個種子各取主色，union 成多路色（buildPassableMaskRGB 對任一路色 OR 比對）。
    std::vector<RGB> pathColors;
    for (size_t i = 0; i < sxs.size(); i++) {
        auto cs = sampleDominantColors(sxs[i], sys[i], width, height, sampleRadius, 3, 1);
        for (auto& c : cs) pathColors.push_back(c);
    }

    int tolSq = pathColorTolerance * pathColorTolerance;
    std::vector<uint8_t> mask = buildPassableMaskRGB(width, height, pathColors, tolSq,
                                 closingKernelSize, wallThicken, wallMask);

    // 邊緣平滑（opening）。預設關閉（gEdgeSmoothKernelSize=0 → no-op）。詳見其宣告處說明。
    openWithBarrier(mask, wallMask, width, height, gEdgeSmoothKernelSize);

    if (denoiseMinArea > 0) {
        removeSmallMaskComponents(mask, width, height, denoiseMinArea);
    }

    // clipMode==2：先把灰色圖框「封成封閉牆」，必須在連通分量限制之前 →
    // 框外白底會與種子斷開、在下面的 connected-component 被丟棄（解 hospital 漏到框外）。
    int fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
    bool hasFrame = false;
    if (clipMode == 2) {
        hasFrame = detectFrameBBox(mapBuffer, width, height, 0.4f, fx0, fy0, fx1, fy1);
        if (hasFrame)
            carveFrameWall(mask, wallMask, width, height, fx0, fy0, fx1, fy1, 4);
        else
            clipToWallBBox(mask, wallMask, width, height, 0.04f); // 無框則退回牆包圍盒
    }

    // connected-component 限制：從「所有種子」union BFS 染色（避免多色走廊只留其中一段）。
    int searchR = 12 + wallThicken * 3;
    std::vector<uint8_t> seedMask(mask.size(), 0);
    std::vector<int> q;
    const int dx[] = {0,0,-1,1};
    const int dy[] = {-1,1,0,0};
    for (size_t i = 0; i < sxs.size(); i++) {
        int sx = sxs[i], sy = sys[i];
        if (!findNearestPassable(sx, sy, width, height, mask, searchR)) continue;
        int si = sy * width + sx;
        if (mask[si] == 1 && seedMask[si] == 0) { seedMask[si] = 1; q.push_back(si); }
    }
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
    mask = seedMask;

    // 平滑階段：補小洞 → 清孤立小牆塊。所有「牆 → 可走」翻轉都受 wallMask 屏障保護。
    if (smoothClosingSize > 1) {
        closeSmallHolesWithBarrier(mask, wallMask, width, height, smoothClosingSize);
    }
    if (smoothMinWallArea > 0) {
        removeSmallWallComponentsWithBarrier(mask, wallMask, width, height, smoothMinWallArea);
    }

    // 過度捕捉對策後處理：clipMode==1 裁到牆包圍盒；clipMode==2 灰框已封牆，這裡保險裁掉框外殘留。
    if (clipMode == 1) {
        clipToWallBBox(mask, wallMask, width, height, 0.04f);
    } else if (clipMode == 2 && hasFrame) {
        clipMaskToRect(mask, width, height, fx0, fy0, fx1, fy1, 0);
    }

    // 染色：所有最終可走像素塗青藍（多種子可能含多個連通塊，全部上色）。
    int total = width * height;
    for (int i = 0; i < total; i++) {
        if (mask[i]) {
            mapBuffer[i*4]   = 0;
            mapBuffer[i*4+1] = 200;
            mapBuffer[i*4+2] = 255;
        }
    }

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
