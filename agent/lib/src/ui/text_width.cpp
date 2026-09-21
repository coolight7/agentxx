#include "agentxx/ui/text_width.h"

#include <algorithm>
#include <array>

namespace agentxx {
namespace ui {

namespace {

/// 码点区间 (闭区间)
struct Range {
    char32_t first;
    char32_t last;
};

/// 零宽区间 (组合字符/变体选择符/零宽控制符; 不占列)
constexpr std::array<Range, 8> kZeroWidthRanges = {{
    {0x0300, 0x036F}, // 组合附加符号
    {0x0483, 0x0489}, // 西里尔组合符号
    {0x0591, 0x05BD}, // 希伯来附加符号
    {0x200B, 0x200F}, // 零宽空格 / 方向标记
    {0x2028, 0x202E}, // 行/段落分隔符与双向控制符
    {0xFE00, 0xFE0F}, // 变体选择符
    {0xFE20, 0xFE2F}, // 组合半标记
    {0x1AB0, 0x1AFF}, // 组合附加符号扩展
}};

/// 双宽区间 (东亚宽字符/全角字符/emoji; 占 2 列)
constexpr std::array<Range, 22> kWideRanges = {{
    {0x1100, 0x115F}, // 谚文字母
    {0x2E80, 0x303E}, // 中日韩部首与符号 (不含 0x303F)
    {0x3041, 0x33FF}, // 假名/注音/中日韩兼容符号
    {0x3400, 0x4DBF}, // 中日韩统一表意文字扩展 A
    {0x4E00, 0x9FFF}, // 中日韩统一表意文字
    {0xA000, 0xA4CF}, // 彝文
    {0xAC00, 0xD7A3}, // 谚文音节
    {0xF900, 0xFAFF}, // 中日韩兼容表意文字
    {0xFE10, 0xFE19}, // 竖排标点
    {0xFE30, 0xFE6F}, // 中日韩兼容形式
    {0xFF00, 0xFF60}, // 全角 ASCII
    {0xFFE0, 0xFFE6}, // 全角符号 (含 ￥)
    {0x1F300, 0x1F64F}, // 天气/装饰/表情
    {0x1F680, 0x1F6FF}, // 交通与地图符号
    {0x1F7E0, 0x1F7FF}, // 几何图形扩展
    {0x1F900, 0x1F9FF}, // 补充符号与象形文字
    {0x1FA70, 0x1FAFF}, // 符号与象形文字扩展 A
    {0x1F004, 0x1F004}, // 麻将红中
    {0x1F0CF, 0x1F0CF}, // 小丑牌
    {0x20000, 0x2FFFD}, // 中日韩统一表意文字扩展 B~
    {0x30000, 0x3FFFD}, // 中日韩统一表意文字扩展 G~
    {0x16FE0, 0x16FFF}, // 表意符号与标点
}};

bool inRanges(const std::array<Range, 8>& ranges, char32_t cp) {
    for (const auto& r : ranges) {
        if (cp >= r.first && cp <= r.last) {
            return true;
        }
    }
    return false;
}

bool inRanges(const std::array<Range, 22>& ranges, char32_t cp) {
    for (const auto& r : ranges) {
        if (cp >= r.first && cp <= r.last) {
            return true;
        }
    }
    return false;
}

} // namespace

int codePointWidth(char32_t cp) {
    if (cp == 0) {
        return 0;
    }
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
        // 控制字符: 按 1 列处理 (终端通常不显示, 但排版时留出位置更安全)
        return 1;
    }
    if (cp < 0x300) {
        return 1;
    }
    if (inRanges(kZeroWidthRanges, cp)) {
        return 0;
    }
    if (inRanges(kWideRanges, cp)) {
        return 2;
    }
    return 1;
}

char32_t decodeNextCodePoint(std::string_view text, size_t& pos) {
    if (pos >= text.size()) {
        return 0;
    }
    const size_t          start = pos;
    const auto            byte  = static_cast<unsigned char>(text[start]);
    static constexpr char32_t kReplacement = 0xFFFD;
    if (byte < 0x80) {
        pos = start + 1;
        return byte;
    }
    int      extra   = 0;
    char32_t cp      = 0;
    if ((byte & 0xE0) == 0xC0) {
        extra = 1;
        cp    = byte & 0x1F;
    } else if ((byte & 0xF0) == 0xE0) {
        extra = 2;
        cp    = byte & 0x0F;
    } else if ((byte & 0xF8) == 0xF0) {
        extra = 3;
        cp    = byte & 0x07;
    } else {
        pos = start + 1;
        return kReplacement;
    }
    if (start + static_cast<size_t>(extra) >= text.size()) {
        pos = start + 1;
        return kReplacement;
    }
    for (int i = 1; i <= extra; ++i) {
        const auto next = static_cast<unsigned char>(text[start + static_cast<size_t>(i)]);
        if ((next & 0xC0) != 0x80) {
            pos = start + 1;
            return kReplacement;
        }
        cp = (cp << 6) | (next & 0x3F);
    }
    pos = start + static_cast<size_t>(extra) + 1;
    return cp;
}

int displayWidth(std::string_view text) {
    int    width = 0;
    size_t pos   = 0;
    while (pos < text.size()) {
        width += codePointWidth(decodeNextCodePoint(text, pos));
    }
    return width;
}

std::string_view prefixByWidth(std::string_view text, int maxWidth, int& usedWidth) {
    usedWidth = 0;
    if (maxWidth <= 0) {
        return {};
    }
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t   start = pos;
        const char32_t cp    = decodeNextCodePoint(text, pos);
        const int      w     = codePointWidth(cp);
        if (usedWidth + w > maxWidth) {
            pos = start; // 回退到该码点之前 (不能只取宽字符的一半)
            break;
        }
        usedWidth += w;
    }
    return text.substr(0, pos);
}

std::string truncateToWidth(std::string_view text, int maxWidth, std::string_view ellipsis) {
    if (maxWidth <= 0) {
        return {};
    }
    const int total = displayWidth(text);
    if (total <= maxWidth) {
        return std::string{text};
    }
    const int ellipsisWidth = displayWidth(ellipsis);
    if (ellipsisWidth >= maxWidth) {
        int used = 0;
        return std::string{prefixByWidth(text, maxWidth, used)};
    }
    int             used = 0;
    std::string_view head = prefixByWidth(text, maxWidth - ellipsisWidth, used);
    std::string      out{head};
    out += ellipsis;
    return out;
}

std::string padRightToWidth(std::string_view text, int width) {
    const int total = displayWidth(text);
    if (total >= width) {
        return std::string{text};
    }
    std::string out{text};
    out.append(static_cast<size_t>(width - total), ' ');
    return out;
}

} // namespace ui
} // namespace agentxx
