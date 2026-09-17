#pragma once

#include "agentxx-test/test_framework.h"
#include "utilxx_base/string_util.h"

#include <cassert>

// 原 agentxx::util 已拆分: 基础件在 utilxx_base, 重依赖工具在 utilxx
using namespace utilxx_base;

namespace agentxx {
namespace test {

TestResult testStringUtil();

} // namespace test
} // namespace agentxx
