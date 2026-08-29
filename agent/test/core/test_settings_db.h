#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// 全局设置 SQLite 存储 (util::SettingsDb) 测试:
/// KV 读写/覆盖、整数与布尔存取、懒创建目录、重启恢复、默认路径
TestResult testSettingsDb();

} // namespace test
} // namespace agentxx
