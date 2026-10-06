#pragma once

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

/// Wire 协议字段清单生成与新鲜度校验 (计划 PRO-4 / TST-2)
///
/// 每个线消息类型一个示例实例 → 序列化 → 生成 `agent/schema/wire-schema.json`
/// 与 `docs/zh-cn/design/wire-protocol-fields.md`, 并与仓库内的生成物比对;
/// 设 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 时写回生成物 (之后人工 review diff)。
TestResult testWireSchema();

} // namespace test
} // namespace agentxx
