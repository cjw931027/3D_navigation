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
#include <cstdio>

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
// 圖框級厚度版補抓：封閉矩形框（hospital 外框）整圈連通後 bbox 不是線狀（短邊=整框高），
// 但「逐像素厚度」處處是細線。只用於圖框級大元件——中小型淺灰房間框線維持上面的 bbox 規則：
// 它們常被 closing 跨過當門口（b_1 左翼靠此連通），全收成牆會把門封死。
static constexpr int   FAINT_MAX_THICKNESS      = 4;    // 線狀：像素到最近非淺灰像素的 Chebyshev 距離上限
static constexpr int   FAINT_MIN_RUN            = 40;   // 長直線：H/V 連續 run 長度門檻
static constexpr float FAINT_MIN_RUN_COVERAGE   = 0.5f; // 元件中位於長直線上的像素占比下限
static constexpr float FAINT_FRAME_MIN_SPAN_FRAC = 0.2f; // 圖框級：maxSpan ≥ 影像長邊 × 此比例
// （hospital 下框被切成 223~242px 的 L 形梳狀件 → 0.2×1000=200 可收；
//   b_1 室內隔間框最大 181px、0.2×1199=240 → 仍排除，門不會被封死）

// === 混合牆元件的字芯雕除 ===
// JPEG 反鋸齒鏈（亮度 130~170）會把字黏進牆網（三樓「亖、區」黏到走廊線 → 規則 A 整網判牆）。
// 把命中牆規則的元件用更深核心門檻重新分段，符合字塊特徵的子元件連同周圍膠水像素剔出 wallMask。
static constexpr int   CORE_DARK_THRESHOLD      = 130;  // 字芯/牆芯核心深色門檻（鏈像素多在 130~170）
static constexpr int   CARVE_FRINGE_DIST        = 2;    // 中間調像素距「保留牆芯」≤ 此距離才保留
static constexpr int   GLYPH_GROUP_GAP          = 8;    // 筆畫聚類：子元件 bbox 間距 ≤ 此值視為同一字
static constexpr float GLYPH_GROUP_MIN_FILL     = 0.15f;// 群整體填充率下限（亖類橫槓稀疏，比單元件 0.3 寬）
static constexpr int   GLYPH_GROUP_MIN_PIECE    = 8;    // 參與聚類的子元件最小像素數（AA 碎屑會鏈式串接拉長群）
// 彩色墨水字：深色但高彩度（如三樓「等候區」紫字 chroma 31~40）不進 isDark 層，需獨立分析。
// 房間內彩色標題被標成字後是室內孤島，種子 BFS 自然丟棄，不會誤通。
static constexpr int   COLOR_INK_MAX_BRI        = 150;  // 彩色墨水亮度上限（淺粉彩填色 bri 169+ 不會中）

// === 洞分類的兩個守衛（誤翻防護）===
// 環狀守衛：洞若「內部包大片可走」（如 B1 紫色店面塊內的白標籤字把塊掏成薄環）= 房間/
// 店面輪廓而非文字，不翻。粉彩守衛：亮的粉彩填色（chroma 26~80 且 bri≥150）= 店面色塊，
// 不翻；深色彩字（等候區紫字 bri~110）不受此限。
static constexpr int   RING_MIN_INTERIOR        = 50;
static constexpr float RING_MIN_INTERIOR_FRAC   = 0.2f;
static constexpr int   PASTEL_CHROMA_MIN        = 26;
static constexpr int   PASTEL_CHROMA_MAX        = 80;
static constexpr int   PASTEL_MIN_BRI           = 150;

// 前向宣告（定義在洞分類區塊）。
std::vector<int> computeObstacleThickness(const std::vector<uint8_t>& mask, int width, int height);

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
// 混合牆元件的字芯雕除：命中牆規則的元件內，用 CORE_DARK_THRESHOLD 重新分段；
// 符合字塊特徵（GLYPH_*）的核心子元件 = 被反鋸齒鏈黏進牆網的字 → 連同周圍「不貼牆芯」
// 的中間調膠水像素一起剔出 wallMask。單獨橫槓（亖）minSpan 過小不過字塊測試 → 短小
// 子元件依 bbox 鄰近聚成群（碎屑 <GLYPH_GROUP_MIN_PIECE 不參與，防鏈式串接拉長群），
// 群整體符合字塊特徵 → 整群雕。
// glyphMaskOut（可選）：雕掉的像素記錄為「字」，主流程直接設可走（字下面是地板），
// 不依賴洞分類的包圍條件（種子取色偏移時字洞可能黏進背景大連通塊而翻不了）。
// 也用於非牆元件（只標記 glyphMask，wallMask 雕除為 no-op）。
static void carveGlyphCoresFromWallComp(std::vector<uint8_t>& wallMask, const uint8_t* buf,
                                        const std::vector<int>& comp, int width,
                                        int minX, int minY, int maxX, int maxY,
                                        std::vector<uint8_t>* glyphMaskOut = nullptr) {
    int bw = maxX - minX + 1, bh = maxY - minY + 1;
    int localTotal = bw * bh;
    auto localOf = [&](int idx) { return (idx / width - minY) * bw + (idx % width) - minX; };
    std::vector<uint8_t> member(localTotal, 0), core(localTotal, 0);
    for (int idx : comp) {
        int li = localOf(idx);
        member[li] = 1;
        if ((buf[idx*4] + buf[idx*4+1] + buf[idx*4+2]) / 3 < CORE_DARK_THRESHOLD) core[li] = 1;
    }
    // 核心子元件標記（8連通）+ 字塊測試
    struct Sub { int x0, y0, x1, y1, cnt; bool isGlyph, isLong; };
    std::vector<int> label(localTotal, -1);
    std::vector<int> q(localTotal);
    std::vector<Sub> subs;
    for (int s = 0; s < localTotal; s++) {
        if (!core[s] || label[s] != -1) continue;
        int id = (int)subs.size();
        int head = 0, tail = 0;
        q[tail++] = s; label[s] = id;
        int gx0 = s % bw, gx1 = gx0, gy0 = s / bw, gy1 = gy0, cnt = 0;
        while (head < tail) {
            int c = q[head++]; cnt++;
            int cx = c % bw, cy = c / bw;
            if (cx < gx0) gx0 = cx; if (cx > gx1) gx1 = cx;
            if (cy < gy0) gy0 = cy; if (cy > gy1) gy1 = cy;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (nx < 0 || nx >= bw || ny < 0 || ny >= bh) continue;
                    int ni = ny * bw + nx;
                    if (core[ni] && label[ni] == -1) { label[ni] = id; q[tail++] = ni; }
                }
        }
        int gw = gx1 - gx0 + 1, gh = gy1 - gy0 + 1;
        int gMin = std::min(gw, gh), gMax = std::max(gw, gh) - 1;
        Sub sub;
        sub.x0 = gx0; sub.y0 = gy0; sub.x1 = gx1; sub.y1 = gy1; sub.cnt = cnt;
        sub.isGlyph = gMin >= GLYPH_MIN_SIDE && gMax < GLYPH_MAX_SPAN
                   && (float)cnt / (float)(gw * gh) >= GLYPH_MIN_FILL;
        sub.isLong = gMax >= GLYPH_MAX_SPAN;
        subs.push_back(sub);
    }
    std::vector<bool> carveLabel(subs.size());
    for (size_t i = 0; i < subs.size(); i++) carveLabel[i] = subs[i].isGlyph;
    // 筆畫聚類（union-find，bbox 間距 ≤ GLYPH_GROUP_GAP 合群；碎屑不參與）
    {
        std::vector<int> shortIds;
        for (size_t i = 0; i < subs.size(); i++)
            if (!subs[i].isLong && !subs[i].isGlyph && subs[i].cnt >= GLYPH_GROUP_MIN_PIECE)
                shortIds.push_back((int)i);
        std::vector<int> parent((int)subs.size());
        for (size_t i = 0; i < parent.size(); i++) parent[i] = (int)i;
        std::function<int(int)> find = [&](int i) {
            while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
            return i;
        };
        auto near = [&](int a, int b) {
            return subs[a].x0 <= subs[b].x1 + GLYPH_GROUP_GAP && subs[b].x0 <= subs[a].x1 + GLYPH_GROUP_GAP
                && subs[a].y0 <= subs[b].y1 + GLYPH_GROUP_GAP && subs[b].y0 <= subs[a].y1 + GLYPH_GROUP_GAP;
        };
        for (size_t i = 0; i < shortIds.size(); i++)
            for (size_t j = i + 1; j < shortIds.size(); j++)
                if (near(shortIds[i], shortIds[j])) parent[find(shortIds[i])] = find(shortIds[j]);
        std::unordered_map<int, std::vector<int>> groups;
        for (int i : shortIds) groups[find(i)].push_back(i);
        for (auto& kv : groups) {
            int x0 = bw, y0 = bh, x1 = -1, y1 = -1, cnt = 0;
            for (int i : kv.second) {
                x0 = std::min(x0, subs[i].x0); y0 = std::min(y0, subs[i].y0);
                x1 = std::max(x1, subs[i].x1); y1 = std::max(y1, subs[i].y1);
                cnt += subs[i].cnt;
            }
            int gw = x1 - x0 + 1, gh = y1 - y0 + 1;
            int gMin = std::min(gw, gh), gMax = std::max(gw, gh) - 1;
            if (!(gMin >= GLYPH_MIN_SIDE && gMax < GLYPH_MAX_SPAN
                  && (float)cnt / (float)(gw * gh) >= GLYPH_GROUP_MIN_FILL)) continue;
            for (int i : kv.second) carveLabel[i] = true;
        }
    }
    bool any = false;
    for (size_t i = 0; i < carveLabel.size(); i++) if (carveLabel[i]) { any = true; break; }
    if (!any) return;
    std::vector<uint8_t> keptCore(localTotal, 0), carve(localTotal, 0);
    for (int i = 0; i < localTotal; i++) {
        if (!core[i]) continue;
        if (carveLabel[label[i]]) carve[i] = 1;
        else keptCore[i] = 1;
    }
    // 中間調膠水/邊暈：距保留牆芯 ≤ CARVE_FRINGE_DIST 才留（牆的反鋸齒邊），其餘一併雕除
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            int i = y * bw + x;
            if (!member[i] || core[i]) continue;
            bool near2 = false;
            for (int dy = -CARVE_FRINGE_DIST; dy <= CARVE_FRINGE_DIST && !near2; dy++)
                for (int dx = -CARVE_FRINGE_DIST; dx <= CARVE_FRINGE_DIST && !near2; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < bw && ny >= 0 && ny < bh && keptCore[ny * bw + nx]) near2 = true;
                }
            if (!near2) carve[i] = 1;
        }
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++)
            if (carve[y * bw + x]) {
                int gi = (y + minY) * width + (x + minX);
                wallMask[gi] = 0;
                if (glyphMaskOut) (*glyphMaskOut)[gi] = 1;
            }
}

// statsOut 為可選，用於除錯與測試輸出。glyphMaskOut 為可選：輸出「字像素」遮罩
// （字塊護欄元件 + 混合牆元件雕除字芯 + 彩色墨水字），主流程直接設為可走。
std::vector<uint8_t> buildWallMaskFromBuffer(const uint8_t* buf,
                                              int width, int height,
                                              uint8_t darkThreshold,
                                              int spanThreshold,
                                              std::vector<WallComponentStat>* statsOut = nullptr,
                                              std::vector<uint8_t>* glyphMaskOut = nullptr) {
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

    // 淺灰線狀框線補抓：淺灰候選層獨立連通，兩條規則（任一成立併入 isDark）：
    //   1. 舊 bbox 線狀（細長線，中小型牆線碎片）。
    //   2. 圖框級厚度版：封閉矩形框整圈連通後 bbox 不是線狀（短邊=整框高），但逐像素
    //      厚度處處 ≤ FAINT_MAX_THICKNESS；限大元件 + 長直線覆蓋率（排除淺灰文字/暈圈）。
    {
        std::vector<bool> isFaint(total, false);
        std::vector<uint8_t> notFaint(total, 1);
        for (int i = 0; i < total; i++) {
            int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
            int brightness = (r + g + b) / 3;
            int chroma = std::max({r, g, b}) - std::min({r, g, b});
            if (brightness >= (int)darkThreshold && brightness < (int)FAINT_DARK_THRESHOLD
                && chroma < WALL_DARK_CHROMA_MAX) {
                isFaint[i] = true;
                notFaint[i] = 0;
            }
        }
        // 逐像素厚度：到最近非淺灰像素的 Chebyshev 距離
        std::vector<int> dtF = computeObstacleThickness(notFaint, width, height);
        int frameMinSpan = (int)(FAINT_FRAME_MIN_SPAN_FRAC * (float)std::max(width, height));
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
            int head = 0, maxDT = 0;
            while (head < (int)fq.size()) {
                int cur = fq[head++];
                if (dtF[cur] > maxDT) maxDT = dtF[cur];
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
            // 規則 1：細長線狀（中小型牆線碎片）
            if (faintMinSpan <= FAINT_LINE_MAX_MIN_SPAN && faintMaxSpan >= FAINT_LINE_MIN_MAX_SPAN) {
                for (int idx : fq) isDark[idx] = true;
                continue;
            }
            // 規則 2：圖框級厚度版
            if (faintMaxSpan < frameMinSpan || maxDT > FAINT_MAX_THICKNESS) continue;
            // 長直線覆蓋率：元件內位於 H 或 V 連續 run ≥ FAINT_MIN_RUN 的像素占比
            std::vector<uint8_t> occ((size_t)bboxW * bboxH, 0);
            std::vector<uint8_t> onRun((size_t)bboxW * bboxH, 0);
            for (int idx : fq) occ[(idx / width - minY) * bboxW + (idx % width) - minX] = 1;
            for (int y = 0; y < bboxH; y++) {
                int run = 0;
                for (int x = 0; x <= bboxW; x++) {
                    if (x < bboxW && occ[y * bboxW + x]) run++;
                    else {
                        if (run >= FAINT_MIN_RUN)
                            for (int k = x - run; k < x; k++) onRun[y * bboxW + k] = 1;
                        run = 0;
                    }
                }
            }
            for (int x = 0; x < bboxW; x++) {
                int run = 0;
                for (int y = 0; y <= bboxH; y++) {
                    if (y < bboxH && occ[y * bboxW + x]) run++;
                    else {
                        if (run >= FAINT_MIN_RUN)
                            for (int k = y - run; k < y; k++) onRun[k * bboxW + x] = 1;
                        run = 0;
                    }
                }
            }
            int covered = 0;
            for (int idx : fq)
                if (onRun[(idx / width - minY) * bboxW + (idx % width) - minX]) covered++;
            if ((float)covered / (float)fq.size() >= FAINT_MIN_RUN_COVERAGE) {
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
            // 混合元件字芯雕除：把被反鋸齒鏈黏進牆網的字剔出（真牆核心不動）
            carveGlyphCoresFromWallComp(wallMask, buf, queue, width, minX, minY, maxX, maxY, glyphMaskOut);
        } else if (isGlyph && glyphMaskOut) {
            // 字塊護欄命中的獨立字元件：直接記錄為字（主流程設可走），不依賴洞分類
            for (int idx : queue) (*glyphMaskOut)[idx] = 1;
        } else if (glyphMaskOut) {
            // 非牆非字元件（如「區+亖+碎屑」黏成 >80px 的暗色塊）：同樣跑字芯聚類，
            // 只標記 glyphMask（wallMask 本來就 0，雕除是 no-op）
            carveGlyphCoresFromWallComp(wallMask, buf, queue, width, minX, minY, maxX, maxY, glyphMaskOut);
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

    // 彩色墨水字分析：深色高彩度像素（chroma ≥ 30 進不了 isDark 層，如三樓「等候區」紫字）
    // 獨立連通 → 字塊測試（單元件 + 聚類）→ glyphMask。
    if (glyphMaskOut) {
        std::vector<uint8_t> isInk(total, 0);
        for (int i = 0; i < total; i++) {
            int r = buf[i*4], g = buf[i*4+1], b = buf[i*4+2];
            int brightness = (r + g + b) / 3;
            int chroma = std::max({r, g, b}) - std::min({r, g, b});
            if (brightness < COLOR_INK_MAX_BRI && chroma >= WALL_DARK_CHROMA_MAX) isInk[i] = 1;
        }
        struct InkComp { int x0, y0, x1, y1, cnt; bool isGlyph, isLong; std::vector<int> pix; };
        std::vector<InkComp> inkComps;
        std::vector<bool> iv(total, false);
        std::vector<int> iq;
        iq.reserve(512);
        for (int start = 0; start < total; start++) {
            if (!isInk[start] || iv[start]) continue;
            iq.clear();
            iq.push_back(start);
            iv[start] = true;
            int minX = start % width, maxX = minX;
            int minY = start / width, maxY = minY;
            int head = 0;
            while (head < (int)iq.size()) {
                int cur = iq[head++];
                int cx = cur % width, cy = cur / width;
                if (cx < minX) minX = cx;
                if (cx > maxX) maxX = cx;
                if (cy < minY) minY = cy;
                if (cy > maxY) maxY = cy;
                for (int d = 0; d < 8; d++) {
                    int nx = cx + dx8[d], ny = cy + dy8[d];
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                    int ni = ny * width + nx;
                    if (!iv[ni] && isInk[ni]) { iv[ni] = true; iq.push_back(ni); }
                }
            }
            int bw2 = maxX - minX + 1, bh2 = maxY - minY + 1;
            int gMin = std::min(bw2, bh2), gMax = std::max(bw2, bh2) - 1;
            InkComp c;
            c.x0 = minX; c.y0 = minY; c.x1 = maxX; c.y1 = maxY;
            c.cnt = (int)iq.size();
            c.isGlyph = gMin >= GLYPH_MIN_SIDE && gMax < GLYPH_MAX_SPAN
                     && (float)c.cnt / (float)(bw2 * bh2) >= GLYPH_MIN_FILL;
            c.isLong = gMax >= GLYPH_MAX_SPAN;
            c.pix = iq;
            inkComps.push_back(std::move(c));
        }
        for (auto& c : inkComps)
            if (c.isGlyph) for (int idx : c.pix) (*glyphMaskOut)[idx] = 1;
        // 聚類：短小非字墨水元件合群再測（與字芯聚類同邏輯；碎屑不參與）
        std::vector<int> shortIds;
        for (size_t i = 0; i < inkComps.size(); i++)
            if (!inkComps[i].isLong && !inkComps[i].isGlyph && inkComps[i].cnt >= GLYPH_GROUP_MIN_PIECE)
                shortIds.push_back((int)i);
        std::vector<int> parent((int)inkComps.size());
        for (size_t i = 0; i < parent.size(); i++) parent[i] = (int)i;
        std::function<int(int)> find = [&](int i) {
            while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
            return i;
        };
        auto nearC = [&](int a, int b) {
            return inkComps[a].x0 <= inkComps[b].x1 + GLYPH_GROUP_GAP && inkComps[b].x0 <= inkComps[a].x1 + GLYPH_GROUP_GAP
                && inkComps[a].y0 <= inkComps[b].y1 + GLYPH_GROUP_GAP && inkComps[b].y0 <= inkComps[a].y1 + GLYPH_GROUP_GAP;
        };
        for (size_t i = 0; i < shortIds.size(); i++)
            for (size_t j = i + 1; j < shortIds.size(); j++)
                if (nearC(shortIds[i], shortIds[j])) parent[find(shortIds[i])] = find(shortIds[j]);
        std::unordered_map<int, std::vector<int>> groups;
        for (int i : shortIds) groups[find(i)].push_back(i);
        for (auto& kv : groups) {
            int x0 = width, y0 = height, x1 = -1, y1 = -1, cnt = 0;
            for (int i : kv.second) {
                x0 = std::min(x0, inkComps[i].x0); y0 = std::min(y0, inkComps[i].y0);
                x1 = std::max(x1, inkComps[i].x1); y1 = std::max(y1, inkComps[i].y1);
                cnt += inkComps[i].cnt;
            }
            int gw = x1 - x0 + 1, gh = y1 - y0 + 1;
            int gMin = std::min(gw, gh), gMax = std::max(gw, gh) - 1;
            if (!(gMin >= GLYPH_MIN_SIDE && gMax < GLYPH_MAX_SPAN
                  && (float)cnt / (float)(gw * gh) >= GLYPH_GROUP_MIN_FILL)) continue;
            for (int i : kv.second)
                for (int idx : inkComps[i].pix) (*glyphMaskOut)[idx] = 1;
        }
    }

    return wallMask;
}

// 舊介面：emscripten 主流程使用，內部轉呼叫 buildWallMaskFromBuffer。
std::vector<uint8_t> buildWallMask(int width, int height,
                                    uint8_t darkThreshold, int spanThreshold,
                                    std::vector<uint8_t>* glyphMaskOut = nullptr) {
    return buildWallMaskFromBuffer(mapBuffer, width, height, darkThreshold, spanThreshold,
                                   nullptr, glyphMaskOut);
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

// === 洞分類：筆畫厚度（最大內切半徑）===
// 「大字」與「實心小障礙」在面積軸上正好顛倒（字大該翻、方塊小該留），單一面積門檻無解。
// 改用厚度：文字/圖示再大筆畫都細（maxDT 小）；櫃位/方塊再小內部都厚（maxDT 大）。
// 門檻可由 JS setter 調整（harness 調參免重編）。
static int   gStrokeMaxRadius  = 6;     // 細筆畫的最大內切半徑（Chebyshev px）
static int   gNoiseMaxArea     = 120;   // 此面積以下視為雜訊點，不論厚薄照翻
static float gThinMaxAreaFrac  = 0.02f; // 細筆畫翻可走的防呆面積上限（占全圖比例）
static bool  gDumpHoleStats    = false; // 診斷：printf 逐連通塊統計（harness 抓 stdout）
// 字塊分支：JPEG 會把粗體字糊成接近實心的塊（厚度 8~15，與櫃位厚度重疊），但文字為了
// 可讀必然「比地板暗很多」；設施色塊對比低（如博物館櫃位 203 vs 地板 254，僅差 ~50）。
// 實測分布（harness/holestats.mjs）：三樓字塊亮度 116~169 / 地板 250；博物館櫃位 203 / 254。
static constexpr int TEXT_MAX_RADIUS        = 16;   // 糊化字塊的厚度上限（櫃位/手扶梯多 >16）
static constexpr int TEXT_DARKER_THAN_FLOOR = 60;   // 平均亮度需比地板暗至少這麼多
static constexpr int TEXT_MAX_AREA          = 4000; // 單一字塊面積上限（約 65x60）
// 字塊貼著標籤框時，部分邊界是牆（如三樓「病床」貼框邊界可走僅 0.64），對「明確是墨水」
// 的字塊分支放寬包圍門檻；細筆畫分支維持 FILL_HOLE_MIN_PASSABLE_BORDER（防淺色縫隙網被誤翻）。
static constexpr float TEXT_MIN_PASSABLE_BORDER = 0.5f;

void setHoleClassifierParams(int strokeMaxRadius, int noiseMaxArea) {
    gStrokeMaxRadius = strokeMaxRadius;
    gNoiseMaxArea    = noiseMaxArea;
}
void setDumpHoleStats(bool enable) { gDumpHoleStats = enable; }

// 對每個不可走像素計算「到最近可走像素」的 Chebyshev 距離（兩趟 8 鄰域 chamfer，O(N)）。
// 可走像素距離 0。連通塊內的最大值 = 該塊最粗處的內切半徑。
std::vector<int> computeObstacleThickness(const std::vector<uint8_t>& mask,
                                          int width, int height) {
    int total = width * height;
    const int INF = width + height + 2;
    std::vector<int> dist(total, INF);
    for (int i = 0; i < total; i++)
        if (mask[i] == 1) dist[i] = 0;

    // 前向掃描：左、上、左上、右上
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int i = y * width + x;
            if (dist[i] == 0) continue;
            int d = dist[i];
            if (x > 0)              d = std::min(d, dist[i - 1] + 1);
            if (y > 0) {
                                    d = std::min(d, dist[i - width] + 1);
                if (x > 0)          d = std::min(d, dist[i - width - 1] + 1);
                if (x < width - 1)  d = std::min(d, dist[i - width + 1] + 1);
            }
            dist[i] = d;
        }
    }
    // 反向掃描：右、下、右下、左下
    for (int y = height - 1; y >= 0; y--) {
        for (int x = width - 1; x >= 0; x--) {
            int i = y * width + x;
            if (dist[i] == 0) continue;
            int d = dist[i];
            if (x < width - 1)      d = std::min(d, dist[i + 1] + 1);
            if (y < height - 1) {
                                    d = std::min(d, dist[i + width] + 1);
                if (x < width - 1)  d = std::min(d, dist[i + width + 1] + 1);
                if (x > 0)          d = std::min(d, dist[i + width - 1] + 1);
            }
            dist[i] = d;
        }
    }
    return dist;
}

// 連通塊平均亮度（從 mapBuffer 取原圖色）。供「字塊比地板暗很多」判定。
static int componentMeanBrightness(const std::vector<int>& component) {
    if (component.empty() || mapBuffer == nullptr) return 255;
    long long sum = 0;
    for (int idx : component)
        sum += (mapBuffer[idx*4] + mapBuffer[idx*4+1] + mapBuffer[idx*4+2]) / 3;
    return (int)(sum / (long long)component.size());
}

// 連通塊平均彩度。供粉彩守衛判定。
static int componentMeanChroma(const std::vector<int>& component) {
    if (component.empty() || mapBuffer == nullptr) return 0;
    long long sum = 0;
    for (int idx : component) {
        int r = mapBuffer[idx*4], g = mapBuffer[idx*4+1], b = mapBuffer[idx*4+2];
        sum += std::max({r, g, b}) - std::min({r, g, b});
    }
    return (int)(sum / (long long)component.size());
}

// 環內可走面積：塊 bbox(+1 pad) 內從外圈泛洪「非本塊」格，沒被流到且可走的格 =
// 被本塊完全包住的地板（如店面塊內的白色標籤字）。文字頂多包住小字腔，輪廓環會包住大片。
// glyphMask 像素不算（字芯已被直接填可走，算進去會把文字 AA 暈圈誤判成環）。
static int interiorPassableArea(const std::vector<uint8_t>& mask, int width,
                                const std::vector<int>& comp,
                                int minX, int minY, int maxX, int maxY,
                                const std::vector<uint8_t>& glyphMask) {
    int bw = maxX - minX + 3, bh = maxY - minY + 3; // pad 1
    int localTotal = bw * bh;
    std::vector<uint8_t> inComp(localTotal, 0);
    for (int idx : comp)
        inComp[(idx / width - minY + 1) * bw + (idx % width) - minX + 1] = 1;
    std::vector<uint8_t> seen(localTotal, 0);
    std::vector<int> q;
    q.reserve(bw * 2 + bh * 2);
    for (int x = 0; x < bw; x++) { q.push_back(x); q.push_back((bh - 1) * bw + x); }
    for (int y = 0; y < bh; y++) { q.push_back(y * bw); q.push_back(y * bw + bw - 1); }
    for (int i : q) seen[i] = 1;
    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};
    for (int head = 0; head < (int)q.size(); head++) {
        int c = q[head];
        int cx = c % bw, cy = c / bw;
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx4[d], ny = cy + dy4[d];
            if (nx < 0 || nx >= bw || ny < 0 || ny >= bh) continue;
            int ni = ny * bw + nx;
            if (!seen[ni] && !inComp[ni]) { seen[ni] = 1; q.push_back(ni); }
        }
    }
    int interior = 0;
    for (int y = 1; y < bh - 1; y++)
        for (int x = 1; x < bw - 1; x++) {
            int i = y * bw + x;
            if (seen[i] || inComp[i]) continue;
            int gi = (y - 1 + minY) * width + (x - 1 + minX);
            if (mask[gi] == 1 && !(!glyphMask.empty() && glyphMask[gi])) interior++;
        }
    return interior;
}

// 實心障礙島遮罩：非牆不可走連通塊中「厚、非字塊、且被可走包圍的島」（櫃位/設施/色塊店面）。
// 用途：closing 後把這些島逐像素蓋回障礙 → 邊角不被 closing 圓化。
// 必須限定「被可走包圍的島」：背景大區塊（如北車白底）伸進走道的細突刺本來就靠 closing
// 吞掉，整塊蓋回會把走道切斷。（也不能把實心障礙當 dilate 屏障：barrier 旁的 erode 會反咬走道。）
std::vector<uint8_t> buildSolidObstacleMask(const std::vector<uint8_t>& mask,
                                            const std::vector<uint8_t>& wallMask,
                                            int width, int height, int floorBri) {
    int total = width * height;
    std::vector<uint8_t> out(total, 0);
    std::vector<int> dt = computeObstacleThickness(mask, width, height);
    std::vector<bool> visited(total, false);
    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};
    for (int start = 0; start < total; start++) {
        if (visited[start] || mask[start] == 1) continue;
        if (!wallMask.empty() && wallMask[start] == 1) continue;
        std::vector<int> component;
        std::vector<int> queue;
        queue.push_back(start);
        visited[start] = true;
        bool touchesBorder = false;
        int borderTotal = 0, borderPassable = 0, maxDT = 0;
        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            if (dt[cur] > maxDT) maxDT = dt[cur];
            int cx = cur % width, cy = cur / width;
            if (cx == 0 || cx == width - 1 || cy == 0 || cy == height - 1) touchesBorder = true;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!wallMask.empty() && wallMask[ni] == 1) {
                    borderTotal++;
                } else if (mask[ni] == 1) {
                    borderTotal++;
                    borderPassable++;
                } else if (!visited[ni]) {
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }
        int meanBri = componentMeanBrightness(component);
        bool textLike = maxDT <= TEXT_MAX_RADIUS && meanBri <= floorBri - TEXT_DARKER_THAN_FLOOR;
        bool enclosed = !touchesBorder && borderTotal > 0 &&
            (float)borderPassable / (float)borderTotal >= FILL_HOLE_MIN_PASSABLE_BORDER;
        if (enclosed && maxDT > gStrokeMaxRadius && (int)component.size() >= gNoiseMaxArea && !textLike)
            for (int idx : component) out[idx] = 1;
    }
    return out;
}

// 細筆畫/字塊洞翻可走。兩分支（任一成立即翻，且都要求被可走包圍、不碰影像邊界）：
//   1. 細筆畫：maxDT ≤ gStrokeMaxRadius（線條/箭頭/清晰文字）——「面積不設下限」，大字也翻。
//      包圍門檻 FILL_HOLE_MIN_PASSABLE_BORDER。
//   2. 糊化字塊：maxDT ≤ TEXT_MAX_RADIUS 且面積 ≤ TEXT_MAX_AREA 且平均亮度比地板暗
//      TEXT_DARKER_THAN_FLOOR 以上（JPEG 糊化的粗體字；實心淺色櫃位對比低，不會中）。
//      包圍門檻放寬為 TEXT_MIN_PASSABLE_BORDER（字常貼標籤框，部分邊界是牆）。
// 牆像素當分隔邊界：連通塊只含「非可走且非牆」像素。字黏到框線會被框線切開、自成小塊
// （可翻）；框內色塊的邊界全是牆 → 可走占比不足 → 不翻。翻的像素永不含牆（不穿牆底線）。
// 跑在 connected-component 種子 BFS 之前：字若橫跨窄走道，先補掉才不會切斷連通。
void flipThinEnclosedComponents(std::vector<uint8_t>& mask,
                                const std::vector<uint8_t>& wallMask,
                                int width, int height, int floorBri,
                                const std::vector<uint8_t>& glyphMask = {}) {
    int total = width * height;
    int maxArea = (int)(total * gThinMaxAreaFrac);
    std::vector<int> dt = computeObstacleThickness(mask, width, height);
    std::vector<bool> visited(total, false);

    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};

    for (int start = 0; start < total; start++) {
        if (visited[start] || mask[start] == 1) continue;
        if (!wallMask.empty() && wallMask[start] == 1) continue;

        std::vector<int> component;
        std::vector<int> queue;
        queue.push_back(start);
        visited[start] = true;
        bool touchesBorder = false;
        int borderTotal = 0, borderPassable = 0;
        int maxDT = 0;
        int minXc = start % width, maxXc = minXc;
        int minYc = start / width, maxYc = minYc;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            if (dt[cur] > maxDT) maxDT = dt[cur];
            int cx = cur % width, cy = cur / width;
            if (cx < minXc) minXc = cx;
            if (cx > maxXc) maxXc = cx;
            if (cy < minYc) minYc = cy;
            if (cy > maxYc) maxYc = cy;
            if (cx == 0 || cx == width - 1 || cy == 0 || cy == height - 1) touchesBorder = true;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!wallMask.empty() && wallMask[ni] == 1) {
                    borderTotal++;
                } else if (mask[ni] == 1) {
                    borderTotal++;
                    borderPassable++;
                } else if (!visited[ni]) {
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }

        float border = borderTotal > 0
            ? (float)borderPassable / (float)borderTotal : -1.0f;
        bool thin = maxDT <= gStrokeMaxRadius && (int)component.size() <= maxArea;
        int meanBri = componentMeanBrightness(component);
        int meanChroma = componentMeanChroma(component);
        // 粉彩守衛：亮的粉彩填色 = 房間/店面色塊（非字非標記），不翻
        bool pastel = meanChroma >= PASTEL_CHROMA_MIN && meanChroma < PASTEL_CHROMA_MAX &&
            meanBri >= PASTEL_MIN_BRI;
        bool textBlob = maxDT <= TEXT_MAX_RADIUS && (int)component.size() <= TEXT_MAX_AREA &&
            meanBri <= floorBri - TEXT_DARKER_THAN_FLOOR;
        bool flip = !touchesBorder && !pastel &&
            ((thin && border >= FILL_HOLE_MIN_PASSABLE_BORDER) ||
             (textBlob && border >= TEXT_MIN_PASSABLE_BORDER));
        // 環狀守衛：內部包大片可走 = 房間/店面輪廓（B1 紫色塊含白標籤字變薄環），不翻
        if (flip && interiorPassableArea(mask, width, component, minXc, minYc, maxXc, maxYc, glyphMask) >=
            std::max(RING_MIN_INTERIOR, (int)(RING_MIN_INTERIOR_FRAC * (float)component.size())))
            flip = false;

        if (gDumpHoleStats)
            printf("[holestats] phase=thin area=%d maxDT=%d bri=%d chr=%d border=%.2f touchEdge=%d flip=%d\n",
                   (int)component.size(), maxDT, meanBri, meanChroma, border,
                   touchesBorder ? 1 : 0, flip ? 1 : 0);

        if (flip)
            for (int idx : component) mask[idx] = 1;
    }
}

// 把走道內的小障礙洞翻為可走。三條件同時成立才翻：(a) 面積 < minArea；
// (b) 邊界可走占比 ≥ FILL_HOLE_MIN_PASSABLE_BORDER；(c) 非實心厚塊。
// 牆像素當分隔邊界（與 flipThin 相同）：連通塊只含「非可走且非牆」像素，翻的像素永不含牆。
// 厚度護欄：實心厚塊（maxDT > gStrokeMaxRadius 且面積 ≥ gNoiseMaxArea）是真障礙
// （櫃位/設施），即使面積 < minArea 也不翻；雜訊級小點不論厚薄照翻；
// 字塊豁免：比地板暗很多的糊化字塊不算實心障礙（與 flipThin 分支 2 一致）。
void removeSmallWallComponentsWithBarrier(std::vector<uint8_t>& mask,
                                          const std::vector<uint8_t>& wallMask,
                                          int width, int height, int minArea,
                                          int floorBri = 255,
                                          const std::vector<uint8_t>& glyphMask = {}) {
    if (minArea <= 0) return;
    int total = width * height;
    std::vector<int> dt = computeObstacleThickness(mask, width, height);
    std::vector<bool> visited(total, false);

    const int dx4[] = {0, 0, -1, 1};
    const int dy4[] = {-1, 1,  0, 0};

    for (int start = 0; start < total; start++) {
        if (visited[start] || mask[start] == 1) continue;
        if (!wallMask.empty() && wallMask[start] == 1) continue;

        std::vector<int> component;
        std::vector<int> queue;
        queue.push_back(start);
        visited[start] = true;
        // 邊界統計：外圈鄰居(不屬於本連通域的相鄰格)中，可走 vs 邊界總數（含牆邊界）。
        int borderTotal = 0;
        int borderPassable = 0;
        int maxDT = 0;
        int minXc = start % width, maxXc = minXc;
        int minYc = start / width, maxYc = minYc;

        for (int head = 0; head < (int)queue.size(); head++) {
            int cur = queue[head];
            component.push_back(cur);
            if (dt[cur] > maxDT) maxDT = dt[cur];
            int cx = cur % width, cy = cur / width;
            if (cx < minXc) minXc = cx;
            if (cx > maxXc) maxXc = cx;
            if (cy < minYc) minYc = cy;
            if (cy > maxYc) maxYc = cy;
            for (int d = 0; d < 4; d++) {
                int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                int ni = ny * width + nx;
                if (!wallMask.empty() && wallMask[ni] == 1) {
                    // 真牆鄰居計入邊界分母（非可走邊界），壓低貼牆塊的可走占比。
                    borderTotal++;
                } else if (mask[ni] == 1) {
                    // 鄰居是可走 → 外邊界，且是「可走邊界」(分子 + 分母)。
                    borderTotal++;
                    borderPassable++;
                } else if (!visited[ni]) {
                    // 鄰居也是不可走非牆且未訪 → 併入同一連通域(內部，不計邊界)。
                    visited[ni] = true;
                    queue.push_back(ni);
                }
            }
        }

        bool small = (int)component.size() < minArea;
        // 邊界可走占比門檻(borderTotal==0 表示整塊貼著影像邊界或完全內含，視為不補)。
        float passableBorderRatio = borderTotal > 0
            ? (float)borderPassable / (float)borderTotal : -1.0f;
        bool enclosed = passableBorderRatio >= FILL_HOLE_MIN_PASSABLE_BORDER;
        // 厚度護欄：實心厚塊是真障礙（櫃位/方塊），不翻；雜訊級小點與字塊照翻。
        int meanBri = componentMeanBrightness(component);
        int meanChroma = componentMeanChroma(component);
        bool textLike = maxDT <= TEXT_MAX_RADIUS && meanBri <= floorBri - TEXT_DARKER_THAN_FLOOR;
        bool solidObstacle = maxDT > gStrokeMaxRadius && (int)component.size() >= gNoiseMaxArea && !textLike;
        // 粉彩守衛：亮的粉彩填色 = 店面色塊，不翻
        bool pastel = meanChroma >= PASTEL_CHROMA_MIN && meanChroma < PASTEL_CHROMA_MAX &&
            meanBri >= PASTEL_MIN_BRI;
        bool flip = small && enclosed && !solidObstacle && !pastel;
        // 環狀守衛（同 flipThin）：內部包大片可走 = 輪廓環，不翻
        if (flip && interiorPassableArea(mask, width, component, minXc, minYc, maxXc, maxYc, glyphMask) >=
            std::max(RING_MIN_INTERIOR, (int)(RING_MIN_INTERIOR_FRAC * (float)component.size())))
            flip = false;

        if (gDumpHoleStats)
            printf("[holestats] phase=fill area=%d maxDT=%d bri=%d chr=%d border=%.2f flip=%d\n",
                   (int)component.size(), maxDT, meanBri, meanChroma, passableBorderRatio,
                   flip ? 1 : 0);

        if (!flip) continue;

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
    // glyphMask：buildWallMask 認定為「字」的像素（字塊護欄元件 + 混合牆元件雕除字芯 + 彩色墨水字）。
    const uint8_t DARK_THRESHOLD = 170;
    std::vector<uint8_t> glyphMask((size_t)width * height, 0);
    std::vector<uint8_t> wallMask = buildWallMask(width, height, DARK_THRESHOLD, spanThreshold, &glyphMask);

    // 每個種子各取主色，union 成多路色（buildPassableMaskRGB 對任一路色 OR 比對）。
    std::vector<RGB> pathColors;
    for (size_t i = 0; i < sxs.size(); i++) {
        auto cs = sampleDominantColors(sxs[i], sys[i], width, height, sampleRadius, 3, 1);
        for (auto& c : cs) pathColors.push_back(c);
    }

    int tolSq = pathColorTolerance * pathColorTolerance;
    // 地板亮度（取所有路色中最亮者），供「字塊比地板暗很多」與實心障礙判定。
    int floorBri = 0;
    for (auto& c : pathColors) floorBri = std::max(floorBri, (c.r + c.g + c.b) / 3);

    // 先取純色比對 mask（不 closing），記下實心障礙島；closing 照舊（只擋真牆），
    // 結束後把島逐像素蓋回障礙 → 櫃位/設施邊角不被 closing 圓化，走道不受影響。
    std::vector<uint8_t> mask = buildPassableMaskRGB(width, height, pathColors, tolSq,
                                 0, 0, wallMask);
    // 字像素直接設可走（字下面是地板）——在 buildWallMask 已確定是字，不依賴洞分類的
    // 包圍條件（種子取色偏移時，字洞可能與背景大連通塊相黏而翻不了）。
    for (size_t i = 0; i < glyphMask.size(); i++)
        if (glyphMask[i] && wallMask[i] == 0) mask[i] = 1;
    std::vector<uint8_t> solidMask = buildSolidObstacleMask(mask, wallMask, width, height, floorBri);
    auto stampSolid = [&](std::vector<uint8_t>& m) {
        for (size_t i = 0; i < m.size(); i++) if (solidMask[i]) m[i] = 0;
    };
    if (closingKernelSize > 1) {
        dilateWithBarrier(mask, wallMask, width, height, closingKernelSize);
        erode(mask, width, height, closingKernelSize);
        stampSolid(mask);
    }
    if (wallThicken > 0) {
        erode(mask, width, height, wallThicken * 2 + 1);
    }

    // 邊緣平滑（opening）。預設關閉（gEdgeSmoothKernelSize=0 → no-op）。詳見其宣告處說明。
    openWithBarrier(mask, wallMask, width, height, gEdgeSmoothKernelSize);

    if (denoiseMinArea > 0) {
        removeSmallMaskComponents(mask, width, height, denoiseMinArea);
    }

    // 細筆畫/字塊洞（大字/圖示/箭頭）翻可走。在種子 BFS 之前跑：字橫跨窄走道也不會切斷連通。
    flipThinEnclosedComponents(mask, wallMask, width, height, floorBri, glyphMask);

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

    // 平滑階段：補小洞 → 清孤立小牆塊。所有「牆 → 可走」翻轉都受 wallMask 屏障保護；
    // 實心障礙島在 closing 後蓋回，邊角不被圓化。
    if (smoothClosingSize > 1) {
        closeSmallHolesWithBarrier(mask, wallMask, width, height, smoothClosingSize);
        stampSolid(mask);
    }
    if (smoothMinWallArea > 0) {
        removeSmallWallComponentsWithBarrier(mask, wallMask, width, height, smoothMinWallArea, floorBri, glyphMask);
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
    function("setHoleClassifierParams", &setHoleClassifierParams);
    function("setDumpHoleStats",        &setDumpHoleStats);
}
