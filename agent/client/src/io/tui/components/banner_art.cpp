#include "agentxx-client/io/tui/components/banner_art.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace agentxx::client {

using namespace ftxui;

namespace {

/// 艺术字行数 ("AGENT" 与右侧图案同高)
constexpr size_t kArtRows = 6;

/// 左侧 "AGENT" 前缀的列数 (动画不改动这部分)
constexpr size_t kPrefixColumns = 44;

/// 右侧图案框的列数: 大字 "+" 与机器人图案都在这个框内变形
constexpr size_t kPatternColumns = 24;

/// 单个 "+" 字形的列数
constexpr size_t kPlusColumns = 9;

/// 两个 "+" 字形在图案框内的左边界 (对称摆放: 2 + 9 + 2 + 9 + 2 = 24)
constexpr std::array<size_t, 2> kPlusOffsets = {2, 13};

/// "+" 字形竖笔画在字形内的起始列 (笔画宽 2 列: 3、4)
constexpr size_t kPlusStemColumn = 3;

/// "+" 字形横笔画在字形内的起始行 (笔画高 2 行: 2、3)
constexpr size_t kPlusBarRow = 2;

/// 首帧保持帧数: 先让 "++" 停留一下, 再开始变形
constexpr size_t kHoldFrames = 5;

/// 变形帧数 (从 "+" 到机器人图案)
constexpr size_t kMorphFrames = 16;

/// 加号收回阶段占变形进度的比例: 前 45% 加号由外向内收回, 之后机器人图案
/// 由中间两只眼睛向外长出 —— 两阶段首尾相接, 两种图案不会长时间挤在一处
constexpr float kRetractRatio = 0.45F;

/// 原始艺术字 (6 行; 左侧 "AGENT" 前缀 + 右侧机器人图案)
///
/// 每行按 "前 44 列 = AGENT 前缀, 其后 24 列 = 机器人图案" 切分; 行内空格参与
/// 图案对齐 (不要删), 行尾空格无影响 (生成帧时每行都会补齐到 44 + 24 列)
constexpr std::string_view kArtSource = R"___(
 █████╗  ██████╗ ███████╗███╗   ██╗████████╗      ╔══╗     ╔══╗
██╔══██╗██╔════╝ ██╔════╝████╗  ██║╚══██╔══╝   ╔══╬══╬═════╬══╬══╗
███████║██║  ███╗█████╗  ██╔██╗ ██║   ██║    ╔═╬  ║++║     ║++║  ╬═╗
██╔══██║██║   ██║██╔══╝  ██║╚██╗██║   ██║    ╚═╬       \_/       ╬═╝
██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║      ╚═══════   ═══════╝
╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝         ╚══╝     ╚══╝
)___";

/// 大字 "+" 的字形 (6 行 × 9 列; 与 "AGENT" 同一绘制风格:
/// 实心方块 + 圆角制表符 + 底部阴影行)
constexpr std::array<std::string_view, kArtRows> kPlusGlyph = {
    "   ██╗   ",
    "   ██║   ",
    "████████╗",
    "╚══██╔══╝",
    "   ██║   ",
    "   ╚═╝   ",
};

/// 艺术字网格: 每行等宽的单字符序列 (含空格, 一个码点一列)
using ArtGrid = std::vector<std::vector<std::string>>;

/// 按行为单位切分文本 (跳过首尾空行: 艺术字文本首尾各带一个换行)
std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    size_t                        pos = 0;
    while (pos <= text.size()) {
        const size_t     nl  = text.find('\n', pos);
        const size_t     end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() || !lines.empty()) {
            lines.push_back(line);
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

/// 一行文本按码点切分 (艺术字使用的方块/制表符均为单列宽字符)
/// - 非法或被截断的字节按单字节成组, 保证不丢内容
///
/// - `args`:
///     - [line] 单行文本 (不含换行)
///
/// - `return` 每个元素为一个字符的 UTF-8 字节
std::vector<std::string> splitChars(std::string_view line) {
    std::vector<std::string> chars;
    chars.reserve(line.size());
    size_t i = 0;
    while (i < line.size()) {
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        size_t              len  = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            len = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            len = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            len = 4;
        }
        len = std::min(len, line.size() - i);
        chars.emplace_back(line.substr(i, len));
        i += len;
    }
    return chars;
}

/// 取字符序列的 [begin, begin + count) 段, 越界部分补空格
///
/// - `return` 长度恒为 count 的一行
std::vector<std::string> sliceChars(
    const std::vector<std::string>& chars,
    size_t                          begin,
    size_t                          count
) {
    std::vector<std::string> out(count, std::string{" "});
    for (size_t i = 0; i < count; ++i) {
        const size_t src = begin + i;
        if (src < chars.size()) {
            out[i] = chars[src];
        }
    }
    return out;
}

/// 大字 "+" 的图案框 (6 行 × kPatternColumns 列): 字形按 kPlusOffsets 摆放两次
ArtGrid buildPlusGrid() {
    ArtGrid grid(kArtRows, std::vector<std::string>(kPatternColumns, std::string{" "}));
    for (size_t y = 0; y < kArtRows; ++y) {
        const auto glyph = splitChars(kPlusGlyph[y]);
        for (const size_t offset : kPlusOffsets) {
            for (size_t i = 0; i < kPlusColumns && i < glyph.size(); ++i) {
                if (offset + i < kPatternColumns) {
                    grid[y][offset + i] = glyph[i];
                }
            }
        }
    }
    return grid;
}

/// 每个格子到最近一个 "+" 字形中心的距离 (变形顺序的依据)
///
/// 字形中心 = 竖笔画与横笔画交叉处: 前段由加号自己收回, 后段由机器人图案长出,
/// 于是加号先从外端收回、机器人图案先从眼睛 (与加号重合处) 向外展开
///
/// - `return` 与图案框同形的距离表
std::vector<std::vector<float>> buildDistanceField() {
    std::vector<std::pair<float, float>> centers;
    for (const size_t offset : kPlusOffsets) {
        centers.emplace_back(
            static_cast<float>(offset + kPlusStemColumn) + 0.5F,
            static_cast<float>(kPlusBarRow) + 0.5F
        );
    }
    std::vector<std::vector<float>> dist(
        kArtRows,
        std::vector<float>(kPatternColumns, 0.F)
    );
    for (size_t y = 0; y < kArtRows; ++y) {
        for (size_t x = 0; x < kPatternColumns; ++x) {
            float best = -1.F;
            for (const auto& [cx, cy] : centers) {
                const float dx = static_cast<float>(x) - cx;
                const float dy = static_cast<float>(y) - cy;
                const float d  = std::sqrt(dx * dx + dy * dy);
                best           = (best < 0.F) ? d : std::min(best, d);
            }
            dist[y][x] = best;
        }
    }
    return dist;
}

/// 把 "AGENT" 前缀与图案框拼成一帧文本 (6 行, 每行 kPrefixColumns + kPatternColumns 列)
///
/// 所有行补齐到同一宽度: FTXUI 的多行文本按最宽行定块宽, 各帧等宽才能保证
/// 居中的艺术字在动画过程中不左右跳动
std::string composeFrame(const ArtGrid& prefix, const ArtGrid& pattern) {
    std::string frame;
    for (size_t y = 0; y < kArtRows; ++y) {
        if (y > 0) {
            frame += '\n';
        }
        for (size_t x = 0; x < kPrefixColumns; ++x) {
            frame += prefix[y][x];
        }
        for (size_t x = 0; x < kPatternColumns; ++x) {
            frame += pattern[y][x];
        }
    }
    return frame;
}

/// 生成动画帧序列 (首帧大字 "+", 末帧机器人图案)
///
/// 变形规则 (p 为变形进度 0..1, dn 为该格到加号中心的归一化距离):
/// - 加号格: p >= kRetractRatio * (1 - dn) 时清空 —— 由外端向中心收回
/// - 机器人格: p >= kRetractRatio + (1 - kRetractRatio) * dn 时写入 —— 由中心向外长出
///
/// - `return` kHoldFrames + kMorphFrames 帧文本 (每帧 6 行)
std::vector<std::string> buildFrames() {
    const auto lines = splitLines(kArtSource);

    ArtGrid prefix(kArtRows, std::vector<std::string>(kPrefixColumns, std::string{" "}));
    ArtGrid robot(kArtRows, std::vector<std::string>(kPatternColumns, std::string{" "}));
    for (size_t y = 0; y < kArtRows; ++y) {
        const auto chars
            = (y < lines.size()) ? splitChars(lines[y]) : std::vector<std::string>{};
        prefix[y] = sliceChars(chars, 0, kPrefixColumns);
        robot[y]  = sliceChars(chars, kPrefixColumns, kPatternColumns);
    }

    const ArtGrid                 plus = buildPlusGrid();
    const auto                    dist = buildDistanceField();
    float                         dmax = 0.F;
    for (const auto& row : dist) {
        for (const float d : row) {
            dmax = std::max(dmax, d);
        }
    }
    if (dmax <= 0.F) {
        dmax = 1.F;
    }

    std::vector<std::string> frames;
    frames.reserve(kHoldFrames + kMorphFrames);
    const std::string holdFrame = composeFrame(prefix, plus);
    for (size_t i = 0; i < kHoldFrames; ++i) {
        frames.push_back(holdFrame);
    }
    for (size_t i = 0; i < kMorphFrames; ++i) {
        const float p = static_cast<float>(i + 1) / static_cast<float>(kMorphFrames);
        ArtGrid     grid = plus;
        for (size_t y = 0; y < kArtRows; ++y) {
            for (size_t x = 0; x < kPatternColumns; ++x) {
                const float dn = dist[y][x] / dmax;
                if (grid[y][x] != " " && p >= kRetractRatio * (1.F - dn)) {
                    grid[y][x] = " ";
                }
                if (robot[y][x] != " " && p >= kRetractRatio + (1.F - kRetractRatio) * dn) {
                    grid[y][x] = robot[y][x];
                }
            }
        }
        frames.push_back(composeFrame(prefix, grid));
    }
    return frames;
}

} // namespace

const std::vector<std::string>& bannerArtFrames() {
    // 首次调用时生成一次 (常量结果, 进程内复用; 函数级静态初始化线程安全)
    static const std::vector<std::string> frames = buildFrames();
    return frames;
}

const std::string& bannerArtFinalFrame() {
    return bannerArtFrames().back();
}

BannerArtComponent::BannerArtComponent(Config config) : config_(config) {
    // 帧间隔下限保护: 非正值视为每次动画回调推进一帧 (实际仍受 FTXUI 帧率上限约束)
    interval_ = config_.frameInterval > std::chrono::milliseconds(0)
                    ? std::chrono::duration_cast<std::chrono::duration<float>>(
                          config_.frameInterval
                      )
                    : std::chrono::duration<float>(0.F);
}

bool BannerArtComponent::animating() const {
    return !finished_ && TUISettings::instance().isAnimationEnabled(config_.requiredLevel);
}

Element BannerArtComponent::OnRender() {
    const auto& frames = bannerArtFrames();

    if (animating()) {
        if (!started_) {
            // 首次渲染开始播放 (即使此前 RequestAnimationFrame 被丢弃 —— 如屏幕尚未
            // 启动 —— 后续渲染仍会重新发起)
            started_ = true;
            frame_   = 0;
            elapsed_ = {};
        }
        // 播放中: 请求下一帧 (FTXUI 收到请求后才安排下一次动画回调与重绘)
        animation::RequestAnimationFrame();
    } else if (!finished_) {
        // 动画等级不足: 不做动画, 直接展示末帧
        finished_ = true;
        frame_    = frames.size() - 1;
    }

    return text(frames[std::min(frame_, frames.size() - 1)]);
}

void BannerArtComponent::OnAnimation(animation::Params& params) {
    // 播放已结束或动画被禁用: 停止帧循环 (画面保持当前帧)
    if (finished_ || !TUISettings::instance().isAnimationEnabled(config_.requiredLevel)) {
        finished_ = true;
        return;
    }
    if (!started_) {
        return; // banner 尚未渲染 (未开始播放): 不推进, 也不续约
    }

    const size_t last = bannerArtFrames().size() - 1;

    // 按累计时长推进帧; 单次回调间隔可能跨越多帧, 循环消化。
    // 上限保护: 极端滞后的回调最多追平 ~1s 对应的帧数后归零剩余时长,
    // 避免异常时间差导致长时间空转。
    elapsed_ += params.duration();
    if (interval_.count() > 0.F) {
        int guard = static_cast<int>(1.F / interval_.count()) + 1;
        while (elapsed_ >= interval_ && guard-- > 0) {
            elapsed_ -= interval_;
            if (frame_ >= last) {
                finished_ = true; // 已到末帧: 停在机器人图案, 不再续约
                break;
            }
            ++frame_;
        }
        if (guard <= 0) {
            elapsed_ = {};
        }
    } else {
        frame_ = (frame_ >= last) ? last : frame_ + 1;
        if (frame_ >= last) {
            finished_ = true;
        }
    }
    if (finished_) {
        return;
    }

    // 续约下一帧 (FTXUI 收到请求后才安排下一次动画回调 + 重绘)
    animation::RequestAnimationFrame();
}

} // namespace agentxx::client
