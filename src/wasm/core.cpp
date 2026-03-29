#include <emscripten/bind.h>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>

using namespace emscripten;

uint8_t* mapBuffer = nullptr;

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

// ============================================================
//  HSL 色彩空間
// ============================================================
struct HSL { float h, s, l; };  // h: 0~360, s: 0~1, l: 0~1

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

// HSL 距離：低飽和度（灰色系）時忽略 H，只比 L
float hslDist(HSL a, HSL b) {
    float dL = a.l - b.l;
    float dS = a.s - b.s;
    // 飽和度都很低時 → 灰色系，H 不可靠
    float satAvg = (a.s + b.s) * 0.5f;
    if (satAvg < 0.12f) {
        // 權重集中在亮度
        return std::sqrt(dL * dL * 4.0f + dS * dS);
    }
    // 色相環距離
    float dH = std::abs(a.h - b.h);
    if (dH > 180.0f) dH = 360.0f - dH;
    dH /= 360.0f;  // normalize to 0~0.5
    return std::sqrt(dH * dH * 2.0f + dS * dS + dL * dL * 2.0f);
}

// ============================================================
//  採色：取周圍 Top-K 眾數顏色
// ============================================================
// quantShift: 量化位移（3=32階, 2=64階）
//
// [修改 1] minCount 門檻從 5% 提高到 10%，過濾噪點／標記文字等
//          次要顏色，避免它們進入 topK 造成遮罩誤納。
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

    // [修改 1] 門檻：至少佔 10% 的採樣量（原為 5%）
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
    // 至少回傳一個
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

// 向下相容：取單一最大眾數
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

// BFS 擴散，回傳填到的像素數量（供外部判斷結果是否合理）
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

// [修改 3] searchR 改為外部傳入，讓呼叫端可根據 wallThicken 動態調整
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

// ============================================================
//  建立路色遮罩 — RGB 版（室內用）
// ============================================================
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

// ============================================================
//  建立路色遮罩 — HSL 版（室外用）
// ============================================================
std::vector<uint8_t> buildPassableMaskHSL(int width, int height,
                                           const std::vector<RGB>& pathColors,
                                           float hslTolerance,
                                           int closingKernelSize, int wallThicken) {
    int total = width * height;
    std::vector<uint8_t> mask(total, 0);

    // 預轉原型色到 HSL
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
//  主函式：intelligentFloodFill
//
//  mode: 0 = indoor (RGB, single-color, quantShift=3)
//        1 = outdoor (HSL, multi-color, quantShift=2)
//
//  pathColorTolerance:
//    - indoor: RGB 歐式距離閾值 (建議 10~80)
//    - outdoor: 整數 10~70，內部映射為 HSL 距離閾值
// ============================================================
void intelligentFloodFill(int width, int height,
                           int seedX, int seedY,
                           int pathColorTolerance,
                           int closingKernelSize,
                           int wallThicken,
                           int sampleRadius,
                           int mode) {
    if (mapBuffer == nullptr) return;
    if (seedX < 0 || seedX >= width || seedY < 0 || seedY >= height) return;

    std::vector<uint8_t> mask;

    if (mode == 1) {
        // === 室外模式 ===
        int quantShift = 2;  // 64 色階

        // [修改 2] topK 從 4 降為 2，只取最主要的兩個顏色，
        //          避免噪點／標記文字被納入遮罩造成 BFS 漏出。
        int topK = 2;
        auto pathColors = sampleDominantColors(seedX, seedY, width, height,
                                                sampleRadius, quantShift, topK);

        // [修改 4] HSL 容差映射改為根號曲線：
        //   原本：hslTol = pathColorTolerance / 300.0f  （線性，低端太寬）
        //   現在：hslTol = sqrt(t) * 0.22f，t = (tol-10)/(70-10)
        //   效果：靈敏度 1（tol=10）→ 0.0，靈敏度 10（tol=70）→ 0.22
        //         曲線在低靈敏度端較陡，高靈敏度端較平緩，
        //         低飽和度地圖不會在低靈敏度就過度匹配。
        float t = (float)(pathColorTolerance - 10) / 60.0f;  // 0~1
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float hslTol = std::sqrt(t) * 0.22f;

        mask = buildPassableMaskHSL(width, height, pathColors, hslTol,
                                     closingKernelSize, wallThicken);
    } else {
        // === 室內模式 ===
        auto pathColors = sampleDominantColors(seedX, seedY, width, height,
                                                sampleRadius, 3, 1);
        int tolSq = pathColorTolerance * pathColorTolerance;
        mask = buildPassableMaskRGB(width, height, pathColors, tolSq,
                                     closingKernelSize, wallThicken);
    }

    // [修改 3] searchR 動態計算：wallThicken 越大，erode 壓縮越多，
    //          需要更大的搜尋半徑才能從種子點回到可通行區域。
    //          base=12，每增加 1 的 wallThicken 額外加 3。
    int searchR = 12 + wallThicken * 3;

    int sx = seedX, sy = seedY;
    if (!findNearestPassable(sx, sy, width, height, mask, searchR)) return;

    bfsFill(width, height, sx, sy, mask, true);
}

// 舊介面保留（向下相容）
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
    function("allocateMemory",       &allocateMemory);
    function("freeMemory",           &freeMemory);
    function("floodFill",            &floodFill);
    function("intelligentFloodFill", &intelligentFloodFill);
}