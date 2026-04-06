#include <emscripten/bind.h>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <functional>

using namespace emscripten;

uint8_t* mapBuffer = nullptr;

//  A* 用：可通行遮罩全域快取
//  intelligentFloodFill 執行後，會將遮罩存入此緩衝區，
//  供後續 runAStar 直接使用，不需重新建立遮罩。
static std::vector<uint8_t>  g_passableMask;
static int                   g_maskWidth  = 0;
static int                   g_maskHeight = 0;

// A* 路徑結果緩衝區（交替存 x,y：x0,y0,x1,y1,...）
// JS 透過 getPathBuffer() 取得指標後用 Int32Array 直接讀取。
static std::vector<int32_t>  g_pathBuffer;

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

inline int colorDistSq(uint8_t r1, uint8_t g1, uint8_t b1,
                        uint8_t r2, uint8_t g2, uint8_t b2) {
    int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr*dr + dg*dg + db*db;
}

struct RGB { uint8_t r, g, b; };

//  HSL 色彩空間
struct HSL { float h, s, l; };

HSL rgb2hsl(uint8_t r8, uint8_t g8, uint8_t b8) {
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    float l = (mx + mn) * 0.5f;
    float d = mx - mn;
    float h = 0, s = 0;
    if (d > 1e-6f) {
        s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else              h = (r - g) / d + 4.0f;
        h *= 60.0f;
    }
    return { h, s, l };
}

float hslDist(HSL a, HSL b) {
    float dL = a.l - b.l;
    float dS = a.s - b.s;
    float satAvg = (a.s + b.s) * 0.5f;
    if (satAvg < 0.12f) {
        return std::sqrt(dL * dL * 4.0f + dS * dS);
    }
    float dH = std::abs(a.h - b.h);
    if (dH > 180.0f) dH = 360.0f - dH;
    dH /= 360.0f;
    return std::sqrt(dH * dH * 2.0f + dS * dS + dL * dL * 2.0f);
}

//  採色：取周圍 Top-K 眾數顏色
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

RGB sampleDominantColor(int cx, int cy, int width, int height, int radius) {
    auto colors = sampleDominantColors(cx, cy, width, height, radius, 3, 1);
    return colors[0];
}

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
                                           int closingKernelSize, int wallThicken) {
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
        dilate(mask, width, height, closingKernelSize);
        erode(mask, width, height, closingKernelSize);
    }
    if (wallThicken > 0) {
        erode(mask, width, height, wallThicken * 2 + 1);
    }
    return mask;
}

std::vector<uint8_t> buildPassableMaskHSL(int width, int height,
                                           const std::vector<RGB>& pathColors,
                                           float hslTolerance,
                                           int closingKernelSize, int wallThicken) {
    int total = width * height;
    std::vector<uint8_t> mask(total, 0);

    std::vector<HSL> protoHSL;
    protoHSL.reserve(pathColors.size());
    for (auto& c : pathColors) protoHSL.push_back(rgb2hsl(c.r, c.g, c.b));

    for (int i = 0; i < total; i++) {
        HSL px = rgb2hsl(mapBuffer[i*4], mapBuffer[i*4+1], mapBuffer[i*4+2]);
        for (auto& ph : protoHSL) {
            if (hslDist(px, ph) <= hslTolerance) {
                mask[i] = 1;
                break;
            }
        }
    }

    if (closingKernelSize > 1) {
        dilate(mask, width, height, closingKernelSize);
        erode(mask, width, height, closingKernelSize);
    }
    if (wallThicken > 0) {
        erode(mask, width, height, wallThicken * 2 + 1);
    }
    return mask;
}

// ============================================================
//  前處理 1：光影均一化
//
//  將圖片轉換到 LAB 色彩空間，用局部平均亮度做歸一化，
//  消除因光源不均（室內陰影、室外逆光）造成的亮度差異。
//  blockSize：局部區塊大小（建議 64），越大消除範圍越廣。
// ============================================================
void normalizeLight(int width, int height, int blockSize) {
    int total = width * height;

    // 計算每個像素的感知亮度（0.299R + 0.587G + 0.114B）
    std::vector<float> lum(total);
    for (int i = 0; i < total; i++) {
        float r = mapBuffer[i*4]   / 255.0f;
        float g = mapBuffer[i*4+1] / 255.0f;
        float b = mapBuffer[i*4+2] / 255.0f;
        lum[i] = 0.299f * r + 0.587f * g + 0.114f * b;
    }

    // 計算每個像素的局部平均亮度（box blur 近似）
    std::vector<float> localAvg(total, 0.0f);
    int half = blockSize / 2;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float sum = 0.0f;
            int cnt = 0;
            int y0 = std::max(0, y - half), y1 = std::min(height - 1, y + half);
            int x0 = std::max(0, x - half), x1 = std::min(width  - 1, x + half);
            for (int yy = y0; yy <= y1; yy += 4)
                for (int xx = x0; xx <= x1; xx += 4) {
                    sum += lum[yy * width + xx];
                    cnt++;
                }
            localAvg[y * width + x] = (cnt > 0) ? sum / cnt : 0.5f;
        }
    }

    // 用局部亮度歸一化：pixel_out = pixel_in / localAvg * globalTarget
    float globalTarget = 0.72f;
    for (int i = 0; i < total; i++) {
        float avg = localAvg[i];
        if (avg < 0.05f) avg = 0.05f;
        float scale = globalTarget / avg;
        // 只調整亮度，保持色相不變：對 RGB 同比例縮放
        float nr = std::min(1.0f, mapBuffer[i*4]   / 255.0f * scale);
        float ng = std::min(1.0f, mapBuffer[i*4+1] / 255.0f * scale);
        float nb = std::min(1.0f, mapBuffer[i*4+2] / 255.0f * scale);
        mapBuffer[i*4]   = (uint8_t)(nr * 255);
        mapBuffer[i*4+1] = (uint8_t)(ng * 255);
        mapBuffer[i*4+2] = (uint8_t)(nb * 255);
    }
}

// ============================================================
//  前處理 2：小圖案噪點清除
//
//  對圖片做連通區域分析（4-連通 BFS），找出面積小於
//  minArea 的孤立色塊（文字、箭頭、小圖示），把它們的
//  顏色替換成周圍像素的眾數顏色，使其融入背景。
//  minArea：低於此像素數的連通區域才會被清除（建議 80）。
//  tolSq：判斷「相同色塊」的 RGB 距離平方容差（建議 900=30^2）。
// ============================================================
void removeNoise(int width, int height, int minArea, int tolSq) {
    int total = width * height;
    std::vector<int> label(total, -1);
    int nextLabel = 0;

    std::vector<std::vector<int>> regions;

    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};

    // BFS 分割：把顏色相近的相鄰像素歸入同一連通區域
    for (int start = 0; start < total; start++) {
        if (label[start] != -1) continue;
        int sx = start % width, sy = start / width;
        uint8_t sr = mapBuffer[start*4], sg = mapBuffer[start*4+1], sb = mapBuffer[start*4+2];

        std::vector<int> region;
        std::vector<int> queue;
        queue.push_back(start);
        label[start] = nextLabel;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            region.push_back(cur);
            int cx = cur % width, cy = cur / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (label[ni] != -1) continue;
                uint8_t nr = mapBuffer[ni*4], ng = mapBuffer[ni*4+1], nb = mapBuffer[ni*4+2];
                if (colorDistSq(sr, sg, sb, nr, ng, nb) <= tolSq) {
                    label[ni] = nextLabel;
                    queue.push_back(ni);
                }
            }
        }
        regions.push_back(region);
        nextLabel++;
    }

    // 對面積小於 minArea 的區域，把顏色替換成鄰近大區域的主色
    for (auto& region : regions) {
        if ((int)region.size() >= minArea) continue;

        // 統計邊界外一圈像素的眾數顏色
        std::unordered_map<uint32_t, int> freq;
        for (int idx : region) {
            int cx = idx % width, cy = idx / width;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                // 只採不屬於本區域的鄰居
                if (label[ni] == label[idx]) continue;
                uint8_t r = (mapBuffer[ni*4]   >> 3) << 3;
                uint8_t g = (mapBuffer[ni*4+1] >> 3) << 3;
                uint8_t b = (mapBuffer[ni*4+2] >> 3) << 3;
                uint32_t key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                freq[key]++;
            }
        }

        if (freq.empty()) continue;
        uint32_t best = 0; int bestCnt = 0;
        for (auto& kv : freq)
            if (kv.second > bestCnt) { bestCnt = kv.second; best = kv.first; }

        uint8_t fr = (best >> 16) & 0xFF;
        uint8_t fg = (best >>  8) & 0xFF;
        uint8_t fb =  best        & 0xFF;
        for (int idx : region) {
            mapBuffer[idx*4]   = fr;
            mapBuffer[idx*4+1] = fg;
            mapBuffer[idx*4+2] = fb;
        }
    }
}

//  intelligentFloodFill
//  新增參數：
//    normalizelighting (int) — 1 = 執行光影均一化，0 = 跳過
//    denoiseMinArea    (int) — > 0 = 清除小於此面積的噪點色塊，0 = 跳過
void intelligentFloodFill(int width, int height,
                           int seedX, int seedY,
                           int pathColorTolerance,
                           int closingKernelSize,
                           int wallThicken,
                           int sampleRadius,
                           int mode,
                           int normalizelighting,
                           int denoiseMinArea) {
    if (mapBuffer == nullptr) return;
    if (seedX < 0 || seedX >= width || seedY < 0 || seedY >= height) return;

    // 前處理 1：光影均一化（可選）
    if (normalizelighting) {
        normalizeLight(width, height, 64);
    }

    // 前處理 2：小噪點清除（可選，minArea > 0 時啟用）
    if (denoiseMinArea > 0) {
        removeNoise(width, height, denoiseMinArea, 900);
    }

    std::vector<uint8_t> mask;

    if (mode == 1) {
        int quantShift = 2;
        int topK = 2;
        auto pathColors = sampleDominantColors(seedX, seedY, width, height,
                                                sampleRadius, quantShift, topK);
        float hslTol;
        if (pathColorTolerance <= 40) {
            float t = (float)(pathColorTolerance - 10) / 30.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            hslTol = 0.04f + t * 0.06f;
        } else {
            float t = (float)(pathColorTolerance - 40) / 30.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            hslTol = 0.10f + t * 0.10f;
        }
        mask = buildPassableMaskHSL(width, height, pathColors, hslTol,
                                     closingKernelSize, wallThicken);
    } else {
        auto pathColors = sampleDominantColors(seedX, seedY, width, height,
                                                sampleRadius, 3, 1);
        int tolSq = pathColorTolerance * pathColorTolerance;
        mask = buildPassableMaskRGB(width, height, pathColors, tolSq,
                                     closingKernelSize, wallThicken);
    }

    int searchR = 12 + wallThicken * 3;
    int sx = seedX, sy = seedY;
    if (!findNearestPassable(sx, sy, width, height, mask, searchR)) return;

    bfsFill(width, height, sx, sy, mask, true);


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

    // 資料結構 
    struct Node {
        float f, g;
        int   idx;
        bool operator>(const Node& o) const { return f > o.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> openSet;

    std::vector<float> gCost(W * H, 1e30f);
    std::vector<int>   parent(W * H, -1);
    std::vector<bool>  closed(W * H, false);

    // 8 方向定義
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

            // 斜角防穿牆角：確認兩側直線鄰格都可通行
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

    // 回溯並輸出路徑（起點 → 終點順序） 
    std::vector<int32_t> reversed;
    for (int i = endIdx; i != -1; i = parent[i]) {
        reversed.push_back(i % W);  // x
        reversed.push_back(i / W);  // y
    }

    int pairCount = (int)(reversed.size() / 2);
    g_pathBuffer.resize(reversed.size());
    for (int i = 0; i < pairCount; i++) {
        g_pathBuffer[i * 2]     = reversed[(pairCount - 1 - i) * 2];      // x
        g_pathBuffer[i * 2 + 1] = reversed[(pairCount - 1 - i) * 2 + 1];  // y
    }

    return pairCount;
}

// JS 存取路徑結果
int getPathBuffer() {
    return (int)(intptr_t)(g_pathBuffer.data());
}

int getPathLength() {
    return (int)(g_pathBuffer.size() / 2);
}

// JS 存取可通行遮罩（供 JS 側直線化使用）
int getPassableMaskBuffer()  { return (int)(intptr_t)(g_passableMask.data()); }
int getPassableMaskSize()    { return (int)g_passableMask.size(); }
int getPassableMaskWidth()   { return g_maskWidth;  }
int getPassableMaskHeight()  { return g_maskHeight; }

void floodFill(int width, int height, int seedX, int seedY, int tolerance) {
    if (mapBuffer == nullptr) return;
    if (seedX < 0 || seedX >= width || seedY < 0 || seedY >= height) return;

    int si = (seedY * width + seedX) * 4;
    uint8_t tR = mapBuffer[si], tG = mapBuffer[si+1], tB = mapBuffer[si+2];
    int tolSq = tolerance * tolerance;

    std::vector<bool> visited(width * height, false);
    std::vector<int> qX, qY;
    qX.reserve(width * height / 4);
    qY.reserve(width * height / 4);
    qX.push_back(seedX); qY.push_back(seedY);
    visited[seedY * width + seedX] = true;

    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    int head = 0;

    while (head < (int)qX.size()) {
        int cx = qX[head], cy = qY[head++];
        int pi = (cy * width + cx) * 4;
        mapBuffer[pi]   = 0;
        mapBuffer[pi+1] = 200;
        mapBuffer[pi+2] = 255;
        for (int i = 0; i < 4; i++) {
            int nx = cx + dx[i], ny = cy + dy[i];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
            int ni = ny * width + nx;
            if (!visited[ni]) {
                int npi = ni * 4;
                if (colorDistSq(mapBuffer[npi], mapBuffer[npi+1], mapBuffer[npi+2],
                                tR, tG, tB) <= tolSq) {
                    visited[ni] = true;
                    qX.push_back(nx); qY.push_back(ny);
                }
            }
        }
    }
}

EMSCRIPTEN_BINDINGS(my_module) {
    function("allocateMemory",        &allocateMemory);
    function("freeMemory",            &freeMemory);
    function("floodFill",             &floodFill);
    function("intelligentFloodFill",  &intelligentFloodFill);
    function("normalizeLight",        &normalizeLight);
    function("removeNoise",           &removeNoise);
    function("runAStar",              &runAStar);
    function("getPathBuffer",         &getPathBuffer);
    function("getPathLength",         &getPathLength);
    function("getPassableMaskBuffer", &getPassableMaskBuffer);
    function("getPassableMaskSize",   &getPassableMaskSize);
    function("getPassableMaskWidth",  &getPassableMaskWidth);
    function("getPassableMaskHeight", &getPassableMaskHeight);
}