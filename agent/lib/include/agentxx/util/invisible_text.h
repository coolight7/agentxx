#pragma once

#include "cstdint"
#include "string_view"

/// 不可见文本判断 (空白 / 零宽 / 格式控制符)
///
/// 用途: 判断一段文本"看起来是不是空的"。
/// 典型场景: 模型偶尔把 U+200B 之类的零宽占位符当成正文输出, 独占一条消息 ——
/// 只看 `empty()` 会把它当正常内容提交, 展示历史里就多出一条看不见内容的空消息
/// (消息列表每条都带一行尾部空行, 于是看起来像消息间距突然变大)。
namespace agentxx::util {

/// 判断单个 Unicode 码点是否不可见
///
/// 覆盖范围: ASCII/C1 控制符与空格、各宽度空格 (U+2000..U+200A)、零宽与双向控制符
/// (U+200B..U+200F)、行/段分隔符 (U+2028/U+2029)、窄空格与中空格、单词连接符与
/// 不可见运算符 (U+2060..U+2064)、双向隔离符 (U+2066..U+206F)、BOM、
/// 盲文空白 (U+2800)、全角空格 (U+3000)、韩文填充符、注释符 (U+FFF9..U+FFFB)
///
/// - `cp` Unicode 码点
/// - `return` 该码点不产生可见字形时为 true
inline bool isInvisibleCodePoint(std::uint32_t cp) {
    if (cp <= 0x20) {
        return true; // ASCII 控制符与空格
    }
    if (cp >= 0x7F && cp <= 0x9F) {
        return true; // DEL 与 C1 控制符
    }
    if (cp == 0x00A0 || cp == 0x00AD || cp == 0x061C || cp == 0x115F || cp == 0x1160
        || cp == 0x17B4 || cp == 0x17B5 || cp == 0x180E || cp == 0x2800 || cp == 0x3000
        || cp == 0x3164 || cp == 0xFEFF || cp == 0xFFA0) {
        return true;
    }
    if (cp >= 0x2000 && cp <= 0x206F) {
        return true; // 空格/零宽/双向控制符/不可见运算符
    }
    if (cp >= 0xFFF9 && cp <= 0xFFFB) {
        return true; // 注释符
    }
    return false;
}

/// 文本是否"视觉为空": 逐码点检查, 全部不可见时为空
///
/// - 空字符串返回 true
/// - 非法 UTF-8 字节按"有内容"处理 (宁可多显示一条, 也不要吞掉真实内容)
///
/// - `text` UTF-8 文本
/// - `return` 不含任何可见字符时为 true
inline bool isVisuallyBlank(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto    byte = static_cast<unsigned char>(text[i]);
        std::uint32_t cp   = 0;
        size_t        len  = 1;
        if (byte < 0x80) {
            cp = byte;
        } else if ((byte & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp  = byte & 0x1FU;
            len = 2;
        } else if ((byte & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp  = byte & 0x0FU;
            len = 3;
        } else if ((byte & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp  = byte & 0x07U;
            len = 4;
        } else {
            return false; // 非法字节: 当作有可见内容
        }
        for (size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3FU);
        }
        if (!isInvisibleCodePoint(cp)) {
            return false;
        }
        i += len;
    }
    return true;
}

} // namespace agentxx::util
