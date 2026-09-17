/// 文件系统路径段安全化工具: 把任意文本转为可用作文件名/目录名的路径段
///
/// - 用于"由用户输入/项目路径构造磁盘目录名"的场景 (会话数据目录、代码索引目录等)
/// - 只依赖 agentxx/util/hash.h, 不引入日志等重头文件 (插件侧可直接包含)
/// - 相关: 会话目录名构造见 SessionStore::sanitizeSessionId;
///   CodeGraph 索引目录名构造见 codegraph_manager.cpp 的 sanitizeSegment
#pragma once

#include "utilxx_base/hash.h"
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace utilxx_base {

/// 文件系统路径段清洗: 把文件名/目录名中不合法或易出问题的字符替换为 '_'
/// - 替换对象: Windows 保留字符 `< > : " / \ | ? *` 与 ASCII 控制符 (0x00-0x1F);
///   这些字符在 Windows 上无法用于文件名, 在 Linux 上虽合法但会让路径无法跨平台
/// - 只做逐字符替换 (结果长度与输入相同), 不做截断/保留名规避:
///   超长段用 [truncateFsSegment] / [truncateFsSegmentWithHash],
///   Windows 保留名用 [isWindowsReservedName] 判定后自行加前缀
///
/// - `args`:
///     - [seg] 单个路径段 (不含路径分隔符)
///
/// - `return` 清洗后的路径段
[[nodiscard]] inline std::string sanitizeFsSegment(std::string_view seg) {
    std::string out;
    out.reserve(seg.size());
    for (char c : seg) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' || c == '"'
            || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// 路径段截断 (纯截断, 不追加任何尾缀)
///
/// - `args`:
///     - [seg] 待截断的路径段
///     - [maxLen] 允许的最大长度
///
/// - `return` 长度不超过 [maxLen] 的路径段
[[nodiscard]] inline std::string truncateFsSegment(std::string_view seg, size_t maxLen) {
    return seg.size() <= maxLen ? std::string{seg} : std::string{seg.substr(0, maxLen)};
}

/// 路径段截断 + 8 位 hex 哈希尾缀 (形如 `前缀_1a2b3c4d`, 防不同长段截断后碰撞)
/// - 长度不超过 [maxLen] 时原样返回 (不追加尾缀)
/// - [maxLen] 过小 (<= 9, 放不下尾缀) 时退化为纯截断 (见 [truncateFsSegment])
/// - 哈希取 [hashSource] (建议传未清洗/未截断的原始文本, 不同来源得到不同尾缀);
///   未指定时用 [seg] 自身
///
/// - `args`:
///     - [seg] 待截断的路径段
///     - [maxLen] 允许的最大长度 (含尾缀)
///     - [hashSource] 参与哈希的原始文本
///
/// - `return` 长度不超过 [maxLen] 的路径段
[[nodiscard]] inline std::string truncateFsSegmentWithHash(
    std::string_view seg,
    size_t           maxLen,
    std::string_view hashSource = {}
) {
    if (seg.size() <= maxLen) {
        return std::string{seg};
    }
    if (maxLen <= 9) {
        return truncateFsSegment(seg, maxLen);
    }
    const std::string_view src = hashSource.empty() ? seg : hashSource;
    const uint32_t         h   = static_cast<uint32_t>(hash::fnv1a64(src) & 0xffffffffu);

    std::string out{seg.substr(0, maxLen - 9)};
    out.push_back('_');
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back("0123456789abcdef"[(h >> shift) & 0xfu]);
    }
    return out;
}

/// Windows 保留设备名判定 (忽略扩展名与大小写)
/// - 保留名: CON / PRN / AUX / NUL / COM1-9 / LPT1-9 (含 `con.txt` 这类带扩展名写法)
/// - 这些名字在 Windows 上不能作为文件名/目录名 (用作目录名会导致创建失败),
///   需要加前缀规避 (见各调用方)
/// - 仅做字符串判定, 任意平台均可调用 (便于跨平台单元测试)
///
/// - `args`:
///     - [seg] 单个路径段
///
/// - `return` true 表示该段是 Windows 保留设备名
[[nodiscard]] inline bool isWindowsReservedName(std::string_view seg) {
    std::string name{seg};
    const auto  dot = name.find('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    for (auto& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    static constexpr const char* kReserved[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const auto* r : kReserved) {
        if (name == r) {
            return true;
        }
    }
    return false;
}

} // namespace utilxx_base
