#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// 配置键目录 (计划 CFG-9)
///
/// 三件事一起做:
/// 1. **可执行校验**: 目录里每个键都带"样例 yaml + 观测函数 + 期望值",
///    逐个真实加载一次配置并比对 (默认值与样例值各一次) —— 加载器改了键名/
///    语义而目录没跟上时直接失败;
/// 2. **源码扫描**: 从 `client/src/config_loader.cpp` 扫出所有被解析的 yaml 键名,
///    与目录做集合比对 (加载器新增键却忘了登记时失败);
/// 3. **生成物**: 由目录生成 `agent/schema/config-keys.json` 与
///    `docs/zh-cn/design/config-keys.md`, 逐字节比对做新鲜度门禁
///    (`AGENTXX_UPDATE_CONFIG_KEYS=1` 一键更新)。
TestResult testConfigKeys();

} // namespace test
} // namespace agentxx
