#pragma once

#include "test_framework.h"

namespace agentxx {
namespace test {

/// FFI C API 测试模块 (test_ffi_c_api.cpp)
/// - 纯 C API 调用 (dlopen 形态同构, 直接链接 libagentxx 静态副本)
/// - 覆盖: 版本/内存、create 错误路径 (char** log)、生命周期+对话 (mockLLM)、
///   HIL 权限中断应答、运行中取消、状态错误、日志 drain
/// - 同步模块: 内部自建线程 (FFI 运行时自带 io 线程), 不依赖测试 ioCtx
agentxx::test::TestResult testFfiCApi();

} // namespace test
} // namespace agentxx