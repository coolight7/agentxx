#include "agentxx-test/core/test_share_store.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/share_store.h"
#include "utilxx_base/json.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ss_passed = 0;
int g_ss_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ss_passed
#define XX_TEST_FAILED g_ss_failed

namespace agentxx {
namespace test {

static std::shared_ptr<agentxx::agent::AgentContext> makeShareStoreCtx() {
    auto ctx                     = std::make_shared<agentxx::agent::AgentContext>();
    ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
    ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
    return ctx;
}

asio::awaitable<TestResult> run_share_store_tests() {
    g_ss_passed = 0;
    g_ss_failed = 0;

    auto ctx  = makeShareStoreCtx();
    auto tool = agentxx::tools::SessionShareStoreTool{ctx};

    // 会话 `t1` 已分配 id 的最大值 (各用例 insert 后累积, 用于非法 id 校验)
    size_t maxId = 0;

    auto insertAndGet = [&](const utilxx_base::Json& insertArgs) -> asio::awaitable<std::string> {
        auto result = co_await tool.execute_async(insertArgs);
        auto id     = utilxx_base::Json::parse(result).value<size_t>("id", 0);
        auto get    = co_await tool.execute_async(utilxx_base::Json{
               {"sessionId", insertArgs.value("sessionId", std::string{"t1"})},
               {"opt", "get"},
               {"id", id},
        });
        co_return get;
    };

    // #2: 行切片必须保留换行符, 且结尾不多余追加换行
    {
        auto get = co_await insertAndGet(utilxx_base::Json{
            {"sessionId",   "t1"       },
            {"opt",         "insert"   },
            {"text",        "a\nb\nc\n"},
            {"line_offset", 0          },
            {"line_limit",  2          },
        });
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 中间偏移切片
    {
        auto get = co_await insertAndGet(utilxx_base::Json{
            {"sessionId",   "t1"          },
            {"opt",         "insert"      },
            {"text",        "a\nb\nc\nd\n"},
            {"line_offset", 1             },
            {"line_limit",  2             },
        });
        XX_TEST_EXPECT_EQ(get, "b\nc\n");
    }

    // 无结尾换行的输入切片 (EOF 边界: 不应多余追加换行)
    {
        auto get = co_await insertAndGet(utilxx_base::Json{
            {"sessionId",   "t1"     },
            {"opt",         "insert" },
            {"text",        "a\nb\nc"},
            {"line_offset", 0        },
            {"line_limit",  2        },
        });
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 不切片 (无 offset/limit): 原样存储
    {
        auto get = co_await insertAndGet(utilxx_base::Json{
            {"sessionId", "t1"    },
            {"opt",       "insert"},
            {"text",      "x\ny\n"},
        });
        XX_TEST_EXPECT_EQ(get, "x\ny\n");
    }

    // set 生命周期: insert → get → set → get
    {
        auto ins = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"    },
            {"opt",       "insert"},
            {"text",      "hello" },
        });
        auto id  = utilxx_base::Json::parse(ins).value<size_t>("id", 0);

        auto get1 = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   }
        });
        XX_TEST_EXPECT_EQ(get1, "hello");

        co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"   },
            {"opt",       "set"  },
            {"id",        id     },
            {"text",      "world"},
        });
        auto get2 = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   }
        });
        XX_TEST_EXPECT_EQ(get2, "world");

        // 该会话已分配的 id 最大值 (后续用于非法 id 校验)
        maxId = id;
    }

    // 非法 id: 大于自增 id (从未分配过) 的 id 按参数错误抛出
    {
        // 未用过的会话: 自增 id 为 0, 任何 id 都非法
        bool threwEmpty = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t-new"},
                {"opt",       "get"  },
                {"id",        1      }
            });
        } catch (const std::invalid_argument& e) {
            threwEmpty = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("out of range") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwEmpty);

        // get: 已分配 id 之后的下一个 id 未分配 → 抛异常
        bool threwGet = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"      },
                {"opt",       "get"     },
                {"id",        maxId + 1 }
            });
        } catch (const std::invalid_argument& e) {
            threwGet = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("out of range") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwGet);

        // set: 同样拒绝未分配的 id (不能凭空写入新 id)
        bool threwSet = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"      },
                {"opt",       "set"     },
                {"id",        maxId + 1 },
                {"text",      "new"     },
            });
        } catch (const std::invalid_argument& e) {
            threwSet = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("out of range") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwSet);
    }

    // `delete` 已移除: 按非法 opt 抛出
    {
        bool threwDelete = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"    },
                {"opt",       "delete"},
                {"id",        maxId   },
            });
        } catch (const std::invalid_argument& e) {
            threwDelete = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("`opt` is invalid") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwDelete);
    }

    // 参数检查失败一律抛异常 (不再返回编码后的错误 JSON):
    // 缺少 sessionId / 缺少 opt / 空 id / 非法 opt
    {
        bool threwNoSession = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"opt", "get"},
                {"id",  1    }
            });
        } catch (const std::invalid_argument& e) {
            threwNoSession = true;
            XX_TEST_EXPECT_TRUE(std::string_view{e.what()}.find("sessionId") != std::string::npos);
        }
        XX_TEST_EXPECT_TRUE(threwNoSession);

        bool threwNoOpt = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"}
            });
        } catch (const std::invalid_argument& e) {
            threwNoOpt = true;
            XX_TEST_EXPECT_TRUE(std::string_view{e.what()}.find("`opt` is empty") != std::string::npos);
        }
        XX_TEST_EXPECT_TRUE(threwNoOpt);

        bool threwNoId = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1" },
                {"opt",       "get"}
            });
        } catch (const std::invalid_argument& e) {
            threwNoId = true;
            XX_TEST_EXPECT_TRUE(std::string_view{e.what()}.find("`id` is empty") != std::string::npos);
        }
        XX_TEST_EXPECT_TRUE(threwNoId);

        bool threwBadOpt = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"    },
                {"opt",       "nope"  },
                {"id",        1       }
            });
        } catch (const std::invalid_argument& e) {
            threwBadOpt = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("`opt` is invalid") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwBadOpt);
    }

    co_return TestResult{g_ss_passed, g_ss_failed};
}

} // namespace test
} // namespace agentxx
