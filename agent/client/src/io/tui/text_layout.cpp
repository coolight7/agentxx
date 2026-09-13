#include "agentxx-client/io/tui/text_layout.h"

#include <algorithm>
#include <cstddef>

#include <markdown/text_utils.hpp>

namespace agentxx {
namespace client {

size_t estimateLines(std::string_view s, int width) {
    if (s.empty()) {
        return 1;
    }
    size_t useWidth = (width <= 0) ? 80 : static_cast<size_t>(width);
    size_t lines    = 1;
    size_t col      = 0;
    size_t i        = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            ++lines;
            col = 0;
            ++i;
            continue;
        }
        size_t len  = markdown::utf8_byte_length(s[i]);
        len         = std::min(len, s.size() - i);
        const int w = markdown::codepoint_width(markdown::utf8_codepoint(s.data() + i, len));
        if (w > 0) {
            // 组合字符/零宽字符不占列, 不触发折行
            col += w;
            if (col >= useWidth) {
                ++lines;
                col = 0;
            }
        }
        i += len;
    }
    return lines;
}

std::vector<std::string> wrapTextToLines(std::string_view textContent, int maxWidth) {
    std::vector<std::string> result;
    if (textContent.empty()) {
        return result;
    }
    const size_t targetWidth = static_cast<size_t>(std::max(1, maxWidth));

    size_t start = 0;
    while (start < textContent.size()) {
        const size_t     nextNl = textContent.find('\n', start);
        std::string_view line   = (nextNl == std::string_view::npos)
                                      ? textContent.substr(start)
                                      : textContent.substr(start, nextNl - start);
        start = (nextNl == std::string_view::npos) ? textContent.size() : nextNl + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            result.emplace_back();
            continue;
        }

        size_t chunkStart   = 0;
        size_t currentWidth = 0;
        size_t i            = 0;
        while (i < line.size()) {
            size_t charLen = markdown::utf8_byte_length(line[i]);
            charLen        = std::min(charLen, line.size() - i);
            const int w = markdown::codepoint_width(markdown::utf8_codepoint(line.data() + i, charLen));
            const int charWidth = std::max(0, w);

            if (currentWidth + charWidth > targetWidth && currentWidth > 0) {
                result.push_back(std::string(line.substr(chunkStart, i - chunkStart)));
                chunkStart   = i;
                currentWidth = 0;
            }
            currentWidth += charWidth;
            i += charLen;
        }
        if (chunkStart < line.size()) {
            result.push_back(std::string(line.substr(chunkStart)));
        }
    }
    return result;
}

int collapsedPreviewBudget(int maxWidth, int prefixCols) {
    constexpr int kSlack     = 1;
    constexpr int kMinBudget = 8;
    const int     avail      = maxWidth - prefixCols - kSlack;
    return (avail >= kMinBudget) ? avail : kMinBudget;
}

std::string_view lastNonBlankLine(std::string_view s) {
    size_t end = s.size();
    while (end > 0) {
        const size_t nl    = s.rfind('\n', end - 1);
        const size_t begin = (nl == std::string_view::npos) ? 0 : nl + 1;
        bool         blank = true;
        for (size_t i = begin; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c != ' ' && c != '\t' && c != '\r') {
                blank = false;
                break;
            }
        }
        if (!blank) {
            return s.substr(begin, end - begin);
        }
        // 当前行全为空白 (如 token 恰好以换行结尾): 向前回退一行
        end = (nl == std::string_view::npos) ? 0 : nl;
    }
    return {};
}

} // namespace client
} // namespace agentxx
