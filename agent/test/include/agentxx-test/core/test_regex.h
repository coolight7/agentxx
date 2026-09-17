#pragma once

#include "utilxx/regex.h"
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "agentxx-test/test_framework.h"

namespace agentxx {
namespace test {

// 原 agentxx::util 已拆分: 基础件在 utilxx_base, 重依赖工具在 utilxx
using namespace utilxx;

TestResult testRegex();

} // namespace test
} // namespace agentxx
