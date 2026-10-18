#include "agentxx-test/core/test_share_store.h"

#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/share_store.h"
#include "agentxx/util/neograph_json_bridge.h"
#include "agentxx/util/share_store_offload.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

    // 工具 schema: 分页参数统一为 `offset`/`limit`, `opt` 取值为 `get`/`new`/`update`
    // - 参数描述取自主程序提示词表 (AgentPrompt::toolPrompt): key 与 schema 参数名
    //   不一致时描述会变空, 因此这里同时校对两侧一致性
    {
        auto        def    = tool.get_definition();
        auto        params = agentxx::util::fromNeographJson(def.parameters);
        const auto& props  = params["properties"];
        XX_TEST_EXPECT_TRUE(props.contains("offset"));
        XX_TEST_EXPECT_TRUE(props.contains("limit"));
        XX_TEST_EXPECT_FALSE(props.contains("line_offset"));
        XX_TEST_EXPECT_FALSE(props.contains("line_limit"));

        auto optDoc  = props["opt"]["description"].get<std::string>();
        auto offDoc  = props["offset"]["description"].get<std::string>();
        auto limDoc  = props["limit"]["description"].get<std::string>();
        auto textDoc = props["text"]["description"].get<std::string>();
        auto idDoc   = props["id"]["description"].get<std::string>();
        XX_TEST_EXPECT_TRUE(!optDoc.empty());
        XX_TEST_EXPECT_TRUE(!offDoc.empty());
        XX_TEST_EXPECT_TRUE(!limDoc.empty());
        XX_TEST_EXPECT_TRUE(!textDoc.empty());
        XX_TEST_EXPECT_TRUE(!idDoc.empty());
        // 描述里说明 offset/limit 只对 `get` 有效
        XX_TEST_EXPECT_TRUE(offDoc.find("get") != std::string::npos);
        XX_TEST_EXPECT_TRUE(limDoc.find("get") != std::string::npos);

        const auto& opEnum = props["opt"]["enum"];
        XX_TEST_EXPECT_TRUE(opEnum.is_array());
        std::vector<std::string> opts;
        for (const auto& item : opEnum) {
            opts.push_back(item.get<std::string>());
        }
        XX_TEST_EXPECT_EQ(opts.size(), size_t{3});
        XX_TEST_EXPECT_TRUE(std::find(opts.begin(), opts.end(), "get") != opts.end());
        XX_TEST_EXPECT_TRUE(std::find(opts.begin(), opts.end(), "new") != opts.end());
        XX_TEST_EXPECT_TRUE(std::find(opts.begin(), opts.end(), "update") != opts.end());
        // `id` 声明为字符串: 模型传数字时主程序按模式改成字符串, 实现两种都接受
        XX_TEST_EXPECT_EQ(props["id"]["type"].get<std::string>(), std::string{"string"});
    }

    /// 存入文本后取回 (`new` + `get`): 取回参数由调用方给出
    /// (`offset`/`limit` 只对 `get` 有效, 因此切片参数一律传给 `get`)
    auto insertThenGet
        = [&](std::string_view text, const utilxx_base::Json& getExtra = utilxx_base::Json::object()
          ) -> asio::awaitable<std::string> {
        auto ins     = co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"             },
                {"opt",       "new"            },
                {"text",      std::string{text}},
        });
        auto id      = utilxx_base::Json::parse(ins).value<size_t>("id", 0);
        auto getArgs = utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   },
        };
        for (const auto& [k, v] : getExtra.items()) {
            getArgs[k] = v;
        }
        co_return co_await tool.execute_async(getArgs);
    };

    // #2: 取回时的行切片必须保留换行符, 且结尾不多余追加换行
    {
        auto get = co_await insertThenGet(
            "a\nb\nc\n",
            utilxx_base::Json{
                {"offset", 0},
                {"limit",  2},
        }
        );
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 中间偏移切片
    {
        auto get = co_await insertThenGet(
            "a\nb\nc\nd\n",
            utilxx_base::Json{
                {"offset", 1},
                {"limit",  2},
        }
        );
        XX_TEST_EXPECT_EQ(get, "b\nc\n");
    }

    // 无结尾换行的输入切片 (EOF 边界: 不应多余追加换行)
    {
        auto get = co_await insertThenGet(
            "a\nb\nc",
            utilxx_base::Json{
                {"offset", 0},
                {"limit",  2},
        }
        );
        XX_TEST_EXPECT_EQ(get, "a\nb\n");
    }

    // 不切片 (无 offset/limit): 原样取回
    {
        auto get = co_await insertThenGet("x\ny\n");
        XX_TEST_EXPECT_EQ(get, "x\ny\n");
    }

    // offset/limit 只对 `get` 有效: `new`/`update` 传入时按无效参数忽略, 文本原样存储
    {
        auto ins = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"          },
            {"opt",       "new"         },
            {"text",      "a\nb\nc\nd\n"},
            {"offset",    1             },
            {"limit",     2             },
        });
        auto id  = utilxx_base::Json::parse(ins).value<size_t>("id", 0);
        auto all = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   },
        });
        XX_TEST_EXPECT_EQ(all, "a\nb\nc\nd\n");

        co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"          },
            {"opt",       "update"      },
            {"id",        id            },
            {"text",      "w\nx\ny\nz\n"},
            {"offset",    2             },
            {"limit",     1             },
        });
        auto updated = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   },
        });
        XX_TEST_EXPECT_EQ(updated, "w\nx\ny\nz\n");
    }

    // update 生命周期: new → get → update → get
    {
        auto ins = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"   },
            {"opt",       "new"  },
            {"text",      "hello"},
        });
        auto id  = utilxx_base::Json::parse(ins).value<size_t>("id", 0);

        auto get1 = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1" },
            {"opt",       "get"},
            {"id",        id   }
        });
        XX_TEST_EXPECT_EQ(get1, "hello");

        co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"    },
            {"opt",       "update"},
            {"id",        id      },
            {"text",      "world" },
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
                {"sessionId", "t1"     },
                {"opt",       "get"    },
                {"id",        maxId + 1}
            });
        } catch (const std::invalid_argument& e) {
            threwGet = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("out of range") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwGet);

        // update: 同样拒绝未分配的 id (不能凭空写入新 id)
        bool threwSet = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"     },
                {"opt",       "update" },
                {"id",        maxId + 1},
                {"text",      "new"    },
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
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("`opt` is empty") != std::string::npos
            );
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
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("`id` is empty") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwNoId);

        bool threwBadOpt = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1"  },
                {"opt",       "nope"},
                {"id",        1     }
            });
        } catch (const std::invalid_argument& e) {
            threwBadOpt = true;
            XX_TEST_EXPECT_TRUE(
                std::string_view{e.what()}.find("`opt` is invalid") != std::string::npos
            );
        }
        XX_TEST_EXPECT_TRUE(threwBadOpt);
    }

    // 旧参数名 (`line_offset`/`line_limit`) 已统一为 `offset`/`limit`: 传入旧名直接报参数错误,
    // 不能静默忽略 (忽略会让调用方以为取到了指定行区间)
    {
        bool        threwLegacy = false;
        std::string errText;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId",   "t1" },
                {"opt",         "get"},
                {"id",          maxId},
                {"line_offset", 0    },
            });
        } catch (const std::invalid_argument& e) {
            threwLegacy = true;
            errText     = e.what();
        }
        XX_TEST_EXPECT_TRUE(threwLegacy);
        XX_TEST_EXPECT_TRUE(errText.find("renamed") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // 单条文本分页卸载 (agentxx::util::offloadShareStoreIfOversized):
    // agent 队列输入与 toolcall 输出压缩共用, 这里直接按阈值/截断分支核对
    // -----------------------------------------------------------------------
    {
        // 本组用例统一的会话 id (与上面 `t1` 隔离, 便于按 id 取回核对原文)
        const std::string sid = "offload-session";

        // 阈值 0 = 不检查
        {
            auto r = agentxx::util::offloadShareStoreIfOversized(ctx, sid, "hello", 0);
            XX_TEST_EXPECT_TRUE(!r.offloaded);
            XX_TEST_EXPECT_EQ(r.storeId, size_t{0});
            XX_TEST_EXPECT_EQ(r.text, std::string{"hello"});
        }

        // 未达阈值 (字符数): 原样返回
        {
            const std::string text(128, 'a');
            auto              r = agentxx::util::offloadShareStoreIfOversized(ctx, sid, text, 129);
            XX_TEST_EXPECT_TRUE(!r.offloaded);
            XX_TEST_EXPECT_EQ(r.text, text);
        }

        // 会话 id 为空 / agent 上下文为空: 不做卸载, 原样返回
        {
            const std::string text(512, 'a');
            auto              r1 = agentxx::util::offloadShareStoreIfOversized(ctx, "", text, 64);
            XX_TEST_EXPECT_TRUE(!r1.offloaded);
            XX_TEST_EXPECT_EQ(r1.text, text);

            auto r2 = agentxx::util::offloadShareStoreIfOversized(
                std::shared_ptr<agentxx::agent::AgentContext>{},
                sid,
                text,
                64
            );
            XX_TEST_EXPECT_TRUE(!r2.offloaded);
            XX_TEST_EXPECT_EQ(r2.text, text);
        }

        // 多行超限: 按行截断, 完整核对提示格式 (该格式与 toolcall 输出压缩共用)
        {
            // 20 行 "ab\n" (每行 3 字符) = 60 字符; 阈值 10 字符 → 片段到第 10 个
            // 字符 (即第 4 行开头), 片段内 3 个换行, 最后一个换行在下标 8
            std::string longText;
            for (int i = 0; i < 20; ++i) {
                longText += "ab\n";
            }
            auto r
                = agentxx::util::offloadShareStoreIfOversized(ctx, "offload-lines", longText, 10);
            XX_TEST_EXPECT_TRUE(r.offloaded);
            XX_TEST_EXPECT_EQ(r.storeId, size_t{1});
            XX_TEST_EXPECT_EQ(
                r.text,
                fmt::format(
                    "[Content offloaded. Use the `agentxx_share_store` tool to fetch the content by "
                    "ID {}. Total {} lines, show [1, {}], hide [{}, {}].]\n{}\n...",
                    1,
                    20,
                    3,
                    4,
                    20,
                    "ab\nab\nab"
                )
            );
            // 原文可在 store 中完整取回
            auto stored
                = ctx->middlewareHandleContext->getShareStoreItemValue("offload-lines", r.storeId);
            XX_TEST_EXPECT_TRUE(stored.has_value());
            XX_TEST_EXPECT_EQ(stored.value_or(std::string{}), longText);
        }

        // 单行超长: 按行截断几乎不剩内容, 改为按字符截断并提示按 id 取全文
        {
            const std::string longText(1024, 'z');
            auto              r
                = agentxx::util::offloadShareStoreIfOversized(ctx, "offload-single", longText, 128);
            XX_TEST_EXPECT_TRUE(r.offloaded);
            XX_TEST_EXPECT_EQ(
                r.text,
                fmt::format(
                    "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full "
                    "content by ID {}. Total {} lines.]\n{}...",
                    1,
                    1,
                    std::string(128, 'z')
                )
            );
            auto stored
                = ctx->middlewareHandleContext->getShareStoreItemValue("offload-single", r.storeId);
            XX_TEST_EXPECT_TRUE(stored.has_value());
            XX_TEST_EXPECT_EQ(stored.value_or(std::string{}), longText);
        }
    }

    // `id` 按声明形态 (字符串) 传入: 与数字写法等价; 非数字字符串报参数错误
    {
        auto ins    = co_await tool.execute_async(utilxx_base::Json{
               {"sessionId", "t1"                 },
               {"opt",       "new"                },
               {"text",      "string id content\n"},
        });
        auto idNum  = utilxx_base::Json::parse(ins).value<size_t>("id", 0);
        auto getStr = co_await tool.execute_async(utilxx_base::Json{
            {"sessionId", "t1"                 },
            {"opt",       "get"                },
            {"id",        std::to_string(idNum)},
        });
        XX_TEST_EXPECT_EQ(getStr, "string id content\n");

        bool threw = false;
        try {
            (void)co_await tool.execute_async(utilxx_base::Json{
                {"sessionId", "t1" },
                {"opt",       "get"},
                {"id",        "abc"},
            });
        } catch (const std::invalid_argument& e) {
            threw = std::string_view{e.what()}.find("`id`") != std::string_view::npos;
        }
        XX_TEST_EXPECT_TRUE(threw);
    }

    co_return TestResult{g_ss_passed, g_ss_failed};
}

} // namespace test
} // namespace agentxx
