#pragma once

/// 设备标识与散列 (MD5 / 设备唯一标识)
///
/// 由原 `agentxx/util/util.h` 拆分的散列部分, 依赖 OpenSSL EVP。

#include <string>
#include <string_view>

namespace utilxx {

/// 计算字符串的 MD5 散列值
/// - 返回 32 位全小写十六进制字符串
///
/// - `args`:
///     - [input] 待计算散列的原始字符串
///
/// - `return` 32 位十六进制 MD5 字符串
[[nodiscard]] std::string md5Hex(std::string_view input);

/// 获取本机设备唯一标识
/// - 基于操作系统机器特征 (Linux machine-id / Windows MachineGuid / Hostname)
///   计算所得的 32 位全小写十六进制 MD5 字符串
/// - 结果按进程缓存 (最多计算一次), 避免反复读取系统信息
///
/// - `return` 32 位设备标识 MD5 字符串
[[nodiscard]] std::string getDeviceId();

} // namespace utilxx
