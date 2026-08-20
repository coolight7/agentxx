#include "test_share_store.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/share_store.h"
#include "neograph/json.h"
#include <iostream>
#include <string>

namespace agentxx {
namespace test {

int g_ss_passed = 0;
int g_ss_failed = 0;

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

    auto insertAndGet = [&](const neograph::json& insertArgs) -> asio::awaitable<std::string> {
        auto result = co_await tool.execute_async(insertArgs);
        auto id     = neograph::json::parse(result).value<size_t>("id", 0);
        auto get    = co_await tool.execute_async(neograph::json{
               {"session_id", insertArgs.value("session_id", std::string{"t1"})},
               {"opt", "get"},
               {"id", id},
        });
        co_return get;
    };

    // #2: 行切片必须保留换行符, 且结尾不多余追加换行
    {
        auto get = co_await insertAndGet(neograph::json{
            {"session_id",  "t1"       },
            {"opt",         "insert"   },
            {"text",        "a\nb\nc\n"},
            {"line_offset", 0          },
            {"line_limit",  2          },
        });
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 中间偏移切片
    {
        auto get = co_await insertAndGet(neograph::json{
            {"session_id",  "t1"          },
            {"opt",         "insert"      },
            {"text",        "a\nb\nc\nd\n"},
            {"line_offset", 1             },
            {"line_limit",  2             },
        });
        XX_TEST_EXPECT_EQ(get, "b\nc\n");
    }

    // 无结尾换行的输入切片 (EOF 边界: 不应多余追加换行)
    {
        auto get = co_await insertAndGet(neograph::json{
            {"session_id",  "t1"     },
            {"opt",         "insert" },
            {"text",        "a\nb\nc"},
            {"line_offset", 0        },
            {"line_limit",  2        },
        });
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 不切片 (无 offset/limit): 原样存储
    {
        auto get = co_await insertAndGet(neograph::json{
            {"session_id", "t1"    },
            {"opt",        "insert"},
            {"text",       "x\ny\n"},
        });
        XX_TEST_EXPECT_EQ(get, "x\ny\n");
    }

    // set / delete 生命周期
    {
        auto ins = co_await tool.execute_async(neograph::json{
            {"session_id", "t1"    },
            {"opt",        "insert"},
            {"text",       "hello" },
        });
        auto id  = neograph::json::parse(ins).value<size_t>("id", 0);

        auto get1 = co_await tool.execute_async(neograph::json{
            {"session_id", "t1" },
            {"opt",        "get"},
            {"id",         id   }
        });
        XX_TEST_EXPECT_EQ(get1, "hello");

        co_await tool.execute_async(neograph::json{
            {"session_id", "t1"   },
            {"opt",        "set"  },
            {"id",         id     },
            {"text",       "world"},
        });
        auto get2 = co_await tool.execute_async(neograph::json{
            {"session_id", "t1" },
            {"opt",        "get"},
            {"id",         id   }
        });
        XX_TEST_EXPECT_EQ(get2, "world");

        co_await tool.execute_async(neograph::json{
            {"session_id", "t1"    },
            {"opt",        "delete"},
            {"id",         id      },
        });
        auto get3 = co_await tool.execute_async(neograph::json{
            {"session_id", "t1" },
            {"opt",        "get"},
            {"id",         id   }
        });
        XX_TEST_EXPECT_TRUE(get3.find("\"error\"") != std::string::npos);
    }

    // 缺少 session_id / opt 的错误处理
    {
        auto r1 = co_await tool.execute_async(neograph::json{
            {"opt", "get"},
            {"id",  1    }
        });
        XX_TEST_EXPECT_TRUE(r1.find("\"error\"") != std::string::npos);
        auto r2 = co_await tool.execute_async(neograph::json{
            {"session_id", "t1"}
        });
        XX_TEST_EXPECT_TRUE(r2.find("\"error\"") != std::string::npos);
    }

    co_return TestResult{g_ss_passed, g_ss_failed};
}

} // namespace test
} // namespace agentxx
