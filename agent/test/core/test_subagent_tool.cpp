// SubAgentManagerTool (`agentxx_subagent` 工具) 单元测试
//
// 设计要点 (见 agent/lib/include/agentxx/tools/subagent.h 与 subagent_shared.h):
// - 注册表仅承载静态元数据 (SubAgentTaskBase); 实际执行由 AgentHost 派生
//   独立 agent 完成 (中断委派), 本模块聚焦工具层行为:
//   1) 可用性: 工具名/定义 schema (enum 列表随注册表更新, required 字段,
//      tasks 数组结构)
//   2) 参数校验错误兼容: 空 subagent / 空 message+messages / 未知 subagent
//      名 / AgentContext 失效 → 均返回 {"error": ...} 而非崩溃或抛异常
//   3) 中断流程: 合法请求首次调用抛 NodeInterrupt 并存储中断参数
//      (interruptArgs: name="subagent", tasks 数组字段完整映射, resultId)
//   4) resume 结果提取: 预置 interruptResult 后按 makeSubagentResumeKey 规则
//      提取; 单任务返回纯文本 / 多任务返回 json 数组 / 缺失 key 的任务跳过 /
//      resultId 为空按任务序号兜底 (summarization 直接调用路径)
//   5) parseSubagentBatchFromInterrupt: 统一批量语义解析 + 旧单发格式兼容
//   6) 写入侧 buildSubagentResumeValues 与读取侧 execute_async 的 key 规则
//      闭环一致 (含 hasError 任务写 {"error": ...})

#include "test_subagent_tool.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/event/events.h"
#include "agentxx/middlewares/middleware.h"
#include "agentxx/tools/subagent.h"
#include "agentxx/tools/subagent_shared.h"
#include "agentxx/util/exception.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_sat_passed = 0;
int g_sat_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_sat_passed
#define XX_TEST_FAILED g_sat_failed

namespace agentxx {
namespace test {

namespace {

/// 构造带最小依赖的测试环境: AgentConfig (prompt 默认表) + MiddlewareContext
struct SubagentToolEnv {
    std::shared_ptr<agentxx::agent::AgentContext>        ctx       = nullptr;
    std::shared_ptr<agentxx::tools::SubAgentManagerTool> tool      = nullptr;
    std::string                                          sessionId = "sat_test_thread";

    SubagentToolEnv() {
        ctx                          = std::make_shared<agentxx::agent::AgentContext>();
        ctx->agentConfig             = std::make_shared<agentxx::agent::AgentConfig>();
        ctx->middlewareHandleContext = std::make_shared<agentxx::middleware::MiddlewareContext>();
        tool = std::make_shared<agentxx::tools::SubAgentManagerTool>("subagent_manager", ctx);
    }

    /// 注册一个 subagent 任务项
    void registerTask(const std::string& name, const std::string& depict) {
        tool->subAgentList.insert(
            std::make_pair(name, std::make_shared<agentxx::tools::SubAgentNormalTask>(name, depict))
        );
    }

    /// 清空指定会话的中断参数/结果 (各用例隔离)
    void resetInterruptState() {
        auto* mctx = ctx->middlewareHandleContext.get();
        mctx->removeGraphDataItem(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
        );
        mctx->removeGraphDataItem(
            sessionId,
            agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult
        );
    }

    /// 读取已存储的中断参数列表 (首次调用抛 NodeInterrupt 后写入)
    std::vector<agentxx::middleware::InterruptHandleArg>& interruptArgs() {
        return ctx->middlewareHandleContext
            ->getGraphDataItemValue<std::vector<agentxx::middleware::InterruptHandleArg>>(
                sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptArgs
            );
    }
};

} // namespace

asio::awaitable<TestResult> run_subagent_tool_tests() {
    g_sat_passed = 0;
    g_sat_failed = 0;

    // ==================== S1. 可用性: 工具名与定义 schema ====================

    {
        auto env = std::make_shared<SubagentToolEnv>();

        // 工具名固定为 agentxx_subagent (LLM 侧调用名)
        XX_TEST_EXPECT_EQ(env->tool->get_name(), std::string{"agentxx_subagent"});

        // 未注册任何 subagent 时定义即可生成 (enum 为空数组), 不崩溃
        {
            const auto def = env->tool->get_definition();
            XX_TEST_EXPECT_EQ(def.name, std::string{"agentxx_subagent"});
            const auto& params = def.parameters;
            XX_TEST_EXPECT_TRUE(params.is_object());
            XX_TEST_EXPECT_EQ(params.value("type", std::string{}), std::string{"object"});
            // required 字段: LLM 必须提供 subagent 与 message
            const auto& required = params["required"];
            XX_TEST_EXPECT_TRUE(required.is_array() && required.size() == 2);
            bool hasSubagentReq = false, hasMessageReq = false;
            for (const auto& r : required) {
                if (r == "subagent") {
                    hasSubagentReq = true;
                }
                if (r == "message") {
                    hasMessageReq = true;
                }
            }
            XX_TEST_EXPECT_TRUE(hasSubagentReq && hasMessageReq);
            // 顶层属性齐备
            const auto& props = params["properties"];
            for (const char* key :
                 {"tasks",
                  "subagent",
                  "system_prompt",
                  "message",
                  "messages",
                  "session_id",
                  "tools",
                  "enable_summarization"}) {
                XX_TEST_EXPECT_TRUE(props.contains(key));
            }
            // enum 为空数组 (无注册项)
            XX_TEST_EXPECT_TRUE(props["subagent"]["enum"].is_array());
            XX_TEST_EXPECT_EQ(props["subagent"]["enum"].size(), size_t{0});
        }

        // 注册后 enum 同步更新且描述携带注册表内容
        env->registerTask("researcher", "Do research");
        env->registerTask("coder", "Write code");
        {
            const auto  def     = env->tool->get_definition();
            const auto& topEnum = def.parameters["properties"]["subagent"]["enum"];
            XX_TEST_EXPECT_EQ(topEnum.size(), size_t{2});
            bool hasResearcher = false, hasCoder = false;
            for (const auto& e : topEnum) {
                if (e == "researcher") {
                    hasResearcher = true;
                }
                if (e == "coder") {
                    hasCoder = true;
                }
            }
            XX_TEST_EXPECT_TRUE(hasResearcher && hasCoder);
            // 描述中包含注册表的 depict 文本 (LLM 据此选择)
            const auto& desc = def.parameters["properties"]["subagent"]["description"];
            XX_TEST_EXPECT_TRUE(desc.is_string());
            XX_TEST_EXPECT_TRUE(desc.get<std::string>().find("Do research") != std::string::npos);
            XX_TEST_EXPECT_TRUE(desc.get<std::string>().find("Write code") != std::string::npos);

            // tasks 数组元素 schema 同样携带 enum
            const auto& taskItems = def.parameters["properties"]["tasks"]["items"];
            XX_TEST_EXPECT_TRUE(taskItems.is_object());
            const auto& taskEnum = taskItems["properties"]["subagent"]["enum"];
            XX_TEST_EXPECT_EQ(taskEnum.size(), size_t{2});
            // 任务项 required: subagent + message
            const auto& taskRequired = taskItems["required"];
            bool        tHasSubagent = false, tHasMessage = false;
            for (const auto& r : taskRequired) {
                if (r == "subagent") {
                    tHasSubagent = true;
                }
                if (r == "message") {
                    tHasMessage = true;
                }
            }
            XX_TEST_EXPECT_TRUE(tHasSubagent && tHasMessage);
        }

        // SubAgentTaskBase 元数据承载
        {
            const agentxx::tools::SubAgentNormalTask normal("n1", "depict-1");
            XX_TEST_EXPECT_EQ(normal.name, std::string{"n1"});
            XX_TEST_EXPECT_EQ(normal.depict, std::string{"depict-1"});
            // Normal 任务默认系统提示为空串 (由宿主派生时回退默认)
            XX_TEST_EXPECT_EQ(normal.systemPrompt, std::string{""});
        }
    }

    // ==================== S2. 参数校验错误兼容 ====================

    {
        auto env = std::make_shared<SubagentToolEnv>();
        env->registerTask("alpha", "A");

        // --- A. 空 subagent 名 ---
        {
            auto r = co_await env->tool->execute_async(neograph::json{
                {"message", "m"}
            });
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"Arg `subagent` is empty"})"});
        }

        // --- B. 空 message 且无 messages (两者至少其一) ---
        {
            auto r = co_await env->tool->execute_async(neograph::json{
                {"subagent", "alpha"}
            });
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"Arg `message` is empty"})"});
            // message 非法时不得触发中断 (未产生 interruptArgs)
            XX_TEST_EXPECT_TRUE(env->interruptArgs().empty());
        }

        // --- C. 有 messages 无 message → 合法 (进入中断流程) ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",     "alpha"       },
                {"messages",
                 neograph::json::array({neograph::json{
                     {"role", "user"},
                     {"content", "hi"},
                 }})                           },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_msgs"   },
            };
            bool threwInterrupt = false;
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
                threwInterrupt = true;
            }
            XX_TEST_EXPECT_TRUE(threwInterrupt);
        }

        // --- D. 未知 subagent 名: 错误信息列出可用名 ---
        {
            env->resetInterruptState();
            env->registerTask("beta", "B");
            auto r = co_await env->tool->execute_async(neograph::json{
                {"subagent", "nope"},
                {"message",  "m"   },
            });
            XX_TEST_EXPECT_EQ(
                r,
                std::string{R"({"error":"Arg `subagent` is not one of [alpha,beta]"})"}
            );
        }

        // --- E. 批量任务中第二个非法 → 整体拒绝, 不派发任何任务 ---
        {
            env->resetInterruptState();
            auto r = co_await env->tool->execute_async(neograph::json{
                {"tasks",
                 neograph::json::array(
                     {neograph::json{{"subagent", "alpha"}, {"message", "ok"}},
                      neograph::json{{"subagent", "ghost"}, {"message", "bad"}}}
                 )},
            });
            XX_TEST_EXPECT_TRUE(r.find("\"error\"") != std::string::npos);
            XX_TEST_EXPECT_TRUE(r.find("not one of") != std::string::npos);
            // 校验失败不产生中断参数
            XX_TEST_EXPECT_TRUE(env->interruptArgs().empty());
        }

        // --- F. AgentContext 失效 (weak_ptr 过期) → 返回错误而非崩溃 ---
        {
            auto orphanTool = std::make_shared<agentxx::tools::SubAgentManagerTool>(
                "orphan",
                std::weak_ptr<agentxx::agent::AgentContext>{}
            );
            orphanTool->subAgentList.insert(std::make_pair(
                "alpha",
                std::make_shared<agentxx::tools::SubAgentNormalTask>("alpha", "A")
            ));
            auto r = co_await orphanTool->execute_async(neograph::json{
                {"subagent", "alpha"},
                {"message",  "m"    },
            });
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"AgentContext not available"})"});
        }

        // --- G. middlewareHandleContext 缺失 → 同样返回错误 ---
        {
            auto envNoMctx = std::make_shared<SubagentToolEnv>();
            envNoMctx->ctx->middlewareHandleContext.reset();
            envNoMctx->registerTask("alpha", "A");
            auto r = co_await envNoMctx->tool->execute_async(neograph::json{
                {"subagent", "alpha"},
                {"message",  "m"    },
            });
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"AgentContext not available"})"});
        }
    }

    // ==================== S3. 中断流程: 参数存储与 NodeInterrupt ====================

    {
        auto env = std::make_shared<SubagentToolEnv>();
        env->registerTask("worker", "W");

        // --- A. 批量任务: 中断参数完整映射所有字段 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"tasks",
                 neograph::json::array(
                     {neograph::json{
                          {"subagent", "worker"},
                          {"system_prompt", "sp-1"},
                          {"message", "task one"},
                          {"sessionId", "ctx-thread-1"},
                          {"tools", neograph::json::array({"agentxx_share_store"})},
                          {"enable_summarization", false},
                          {"result_id", "rid-1"},
                      },
                      neograph::json{
                          {"subagent", "worker"},
                          {"message", "task two"},
                      }}
                 )                             },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_batch"  },
            };
            bool threwInterrupt = false;
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
                threwInterrupt = true;
            }
            XX_TEST_EXPECT_TRUE(threwInterrupt);

            const auto& stored = env->interruptArgs();
            XX_TEST_EXPECT_EQ(stored.size(), size_t{1});
            const auto& arg = stored[0];
            XX_TEST_EXPECT_EQ(arg.name, std::string{"subagent"});
            XX_TEST_EXPECT_EQ(arg.resultId, std::string{"call_batch"});
            const auto& tasks = arg.arg["tasks"];
            XX_TEST_EXPECT_TRUE(tasks.is_array());
            XX_TEST_EXPECT_EQ(tasks.size(), size_t{2});
            // 任务 1: 全字段透传
            XX_TEST_EXPECT_EQ(tasks[0].value("subagent", std::string{}), std::string{"worker"});
            XX_TEST_EXPECT_EQ(tasks[0].value("system_prompt", std::string{}), std::string{"sp-1"});
            XX_TEST_EXPECT_EQ(tasks[0].value("message", std::string{}), std::string{"task one"});
            XX_TEST_EXPECT_EQ(
                tasks[0].value("sessionId", std::string{}),
                std::string{"ctx-thread-1"}
            );
            XX_TEST_EXPECT_TRUE(tasks[0]["tools"].is_array());
            XX_TEST_EXPECT_EQ(
                tasks[0]["tools"][0].get<std::string>(),
                std::string{"agentxx_share_store"}
            );
            XX_TEST_EXPECT_TRUE(tasks[0]["enable_summarization"].is_boolean());
            XX_TEST_EXPECT_FALSE(tasks[0]["enable_summarization"].get<bool>());
            XX_TEST_EXPECT_EQ(tasks[0].value("result_id", std::string{}), std::string{"rid-1"});
            // 任务 2: 仅必填字段 (可选字段不写入)
            XX_TEST_EXPECT_EQ(tasks[1].value("message", std::string{}), std::string{"task two"});
            XX_TEST_EXPECT_FALSE(tasks[1].contains("sessionId"));
            XX_TEST_EXPECT_FALSE(tasks[1].contains("result_id"));
        }

        // --- B. 单任务模式 (顶层字段): 包装为 1 个 task, resultId 取 tool_call_id ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",      "worker"      },
                {"system_prompt", "solo-sp"     },
                {"message",       "solo task"   },
                {"sessionId",     env->sessionId},
                {"tool_call_id",  "call_solo"   },
            };
            bool threwInterrupt = false;
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
                threwInterrupt = true;
            }
            XX_TEST_EXPECT_TRUE(threwInterrupt);
            const auto& stored = env->interruptArgs();
            XX_TEST_EXPECT_EQ(stored.size(), size_t{1});
            XX_TEST_EXPECT_EQ(stored[0].name, std::string{"subagent"});
            XX_TEST_EXPECT_EQ(stored[0].resultId, std::string{"call_solo"});
            const auto& tasks = stored[0].arg["tasks"];
            XX_TEST_EXPECT_EQ(tasks.size(), size_t{1});
            XX_TEST_EXPECT_EQ(tasks[0].value("message", std::string{}), std::string{"solo task"});
        }
    }

    // ==================== S4. resume 结果提取 ====================

    {
        auto env = std::make_shared<SubagentToolEnv>();
        env->registerTask("worker", "W");

        // --- A. 单任务 + 自定义 resultId → 返回纯文本 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",     "worker"      },
                {"message",      "m"           },
                {"result_id",    "rid-A"       },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_A"      },
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            // 写入侧按 key 规则回填结果
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                neograph::json{
                    {"call_A_rid-A", "single result text"}
            }
            );
            auto r = co_await env->tool->execute_async(args);
            XX_TEST_EXPECT_EQ(r, std::string{"single result text"});
        }

        // --- B. 多任务 → 按 makeSubagentResumeKey 规则提取并返回 json 数组 (按任务顺序) ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"tasks",
                 neograph::json::array(
                     {neograph::json{
                          {"subagent", "worker"},
                          {"message", "t1"},
                          {"result_id", "r1"}
                      },
                      neograph::json{{"subagent", "worker"}, {"message", "t2"}, {"result_id", "r2"}}
                     }
                 )                             },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_B"      },
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                // 故意乱序写入, 验证读取按任务顺序聚合
                neograph::json{
                    {"call_B_r2", "second result"},
                    {"call_B_r1", "first result" },
            }
            );
            auto r      = co_await env->tool->execute_async(args);
            auto parsed = neograph::json::parse(r);
            XX_TEST_EXPECT_TRUE(parsed.is_array());
            XX_TEST_EXPECT_EQ(parsed.size(), size_t{2});
            XX_TEST_EXPECT_EQ(parsed[0].get<std::string>(), std::string{"first result"});
            XX_TEST_EXPECT_EQ(parsed[1].get<std::string>(), std::string{"second result"});
        }

        // --- C. resultId 全空 (summarization 直接调用路径): 按任务序号兜底编号 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",  "worker"      },
                {"messages",
                 neograph::json::array({neograph::json{
                     {"role", "user"},
                     {"content", "compress me"},
                 }})                        },
                {"sessionId", env->sessionId},
                // 注意: 无 tool_call_id (压缩中间件直接调用)
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                neograph::json{
                    {"1", "summary text"}
            }
            );
            auto r = co_await env->tool->execute_async(args);
            XX_TEST_EXPECT_EQ(r, std::string{"summary text"});
        }

        // --- D. 非 string 结果值 (如 {"error":...}): dump 后作为文本返回 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",     "worker"      },
                {"message",      "m"           },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_D"      },
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                neograph::json{
                    {"call_D_1", neograph::json{{"error", "subagent failed"}}},
            }
            );
            auto r = co_await env->tool->execute_async(args);
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"subagent failed"})"});
        }

        // --- E. 部分 key 缺失: 缺失任务跳过, 剩余单结果降级为纯文本 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"tasks",
                 neograph::json::array(
                     {neograph::json{
                          {"subagent", "worker"},
                          {"message", "t1"},
                          {"result_id", "r1"}
                      },
                      neograph::json{{"subagent", "worker"}, {"message", "t2"}, {"result_id", "r2"}}
                     }
                 )                             },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "call_E"      },
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            // 仅写入 r2 的结果 (r1 缺失, 如宿主超时未应答)
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                neograph::json{
                    {"call_E_r2", "only second"}
            }
            );
            auto r = co_await env->tool->execute_async(args);
            // outputs 只剩 1 个 → 返回纯文本
            XX_TEST_EXPECT_EQ(r, std::string{"only second"});
        }

        // --- F. resultId 为空且按序号未命中: 兜底取 map 第一个字符串值 ---
        {
            env->resetInterruptState();
            neograph::json args{
                {"subagent",  "worker"      },
                {"message",   "m"           },
                {"sessionId", env->sessionId},
                // 无 tool_call_id
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                // 序号 key "1" 缺失 (如旧版写入方以其他规则命名), 兜底逻辑生效
                neograph::json{
                    {"legacy_key", "fallback string"}
            }
            );
            auto r = co_await env->tool->execute_async(args);
            XX_TEST_EXPECT_EQ(r, std::string{"fallback string"});
        }
    }

    // ==================== S5. parseSubagentBatchFromInterrupt 解析 ====================

    {
        // --- A. 统一批量语义: 全字段解析 ---
        {
            // 结构化透传消息 (逐层构造, 避免深层内联嵌套初始化)
            neograph::json passthroughMsg;
            passthroughMsg["role"]    = "system";
            passthroughMsg["content"] = "sys";
            auto passthroughMsgs      = neograph::json::array({passthroughMsg});

            neograph::json taskItem{
                {"subagent",             "researcher"                                  },
                {"system_prompt",        "be brief"                                    },
                {"message",              "find foo"                                    },
                {"messages",             std::move(passthroughMsgs)                    },
                {"sessionId",            "same-ctx-thread"                             },
                {"tools",                neograph::json::array({"agentxx_share_store"})},
                {"enable_summarization", false                                         },
                {"result_id",            "parse-r1"                                    },
            };

            agentxx::middleware::InterruptHandleArg handleArg;
            handleArg.name     = "subagent";
            handleArg.resultId = "call_parse";
            handleArg.arg      = neograph::json{
                     {"tasks", neograph::json::array({std::move(taskItem)})},
            };
            auto batch = agentxx::tools::parseSubagentBatchFromInterrupt(
                handleArg,
                "parent-agent",
                "parent-thread",
                nullptr
            );
            XX_TEST_EXPECT_EQ(batch.parentAgentName, std::string{"parent-agent"});
            XX_TEST_EXPECT_EQ(batch.parentSessionId, std::string{"parent-thread"});
            XX_TEST_EXPECT_EQ(batch.tasks.size(), size_t{1});
            const auto& t = batch.tasks[0];
            XX_TEST_EXPECT_EQ(t.subagentName, std::string{"researcher"});
            XX_TEST_EXPECT_EQ(t.systemPrompt, std::string{"be brief"});
            XX_TEST_EXPECT_EQ(t.message, std::string{"find foo"});
            XX_TEST_EXPECT_TRUE(t.messages.has_value());
            if (t.messages.has_value()) {
                XX_TEST_EXPECT_TRUE(t.messages->is_array());
                XX_TEST_EXPECT_EQ(t.messages->size(), size_t{1});
            }
            XX_TEST_EXPECT_EQ(t.sessionId, std::string{"same-ctx-thread"});
            XX_TEST_EXPECT_TRUE(t.tools.has_value());
            if (t.tools.has_value()) {
                XX_TEST_EXPECT_EQ(t.tools->size(), size_t{1});
                XX_TEST_EXPECT_EQ(
                    (*t.tools)[0].get<std::string>(),
                    std::string{"agentxx_share_store"}
                );
            }
            XX_TEST_EXPECT_TRUE(t.enableSummarization.has_value());
            XX_TEST_EXPECT_FALSE(t.enableSummarization.value_or(true));
            XX_TEST_EXPECT_EQ(t.resultId, std::string{"parse-r1"});
        }

        // --- B. 多任务批量解析 ---
        {
            agentxx::middleware::InterruptHandleArg handleArg;
            handleArg.name     = "subagent";
            handleArg.resultId = "call_multi";
            handleArg.arg      = neograph::json{
                     {"tasks",
                      neograph::json::array(
                     {neograph::json{{"subagent", "a"}, {"message", "m1"}, {"result_id", "x1"}},
                           neograph::json{{"subagent", "b"}, {"message", "m2"}, {"result_id", "x2"}},
                           neograph::json{{"subagent", "c"}, {"message", "m3"}, {"result_id", "x3"}}}
                 )}
            };
            auto batch
                = agentxx::tools::parseSubagentBatchFromInterrupt(handleArg, "p", "t", nullptr);
            XX_TEST_EXPECT_EQ(batch.tasks.size(), size_t{3});
            XX_TEST_EXPECT_EQ(batch.tasks[0].subagentName, std::string{"a"});
            XX_TEST_EXPECT_EQ(batch.tasks[2].resultId, std::string{"x3"});
            // 可选字段缺省: 不设置 (nullopt)
            XX_TEST_EXPECT_FALSE(batch.tasks[0].messages.has_value());
            XX_TEST_EXPECT_FALSE(batch.tasks[0].tools.has_value());
            XX_TEST_EXPECT_FALSE(batch.tasks[0].enableSummarization.has_value());
            XX_TEST_EXPECT_EQ(batch.tasks[0].sessionId, std::string{""});
        }

        // --- C. 旧单发格式兼容: 直接含 subagent 字段 → 包装为 1 个 task,
        //         resultId 回退到 interrupt.resultId ---
        {
            agentxx::middleware::InterruptHandleArg handleArg;
            handleArg.name     = "subagent";
            handleArg.resultId = "call_legacy";
            handleArg.arg      = neograph::json{
                     {"subagent",             "old-style"},
                     {"system_prompt",        "old-sp"   },
                     {"message",              "old-msg"  },
                     {"enable_summarization", true       },
            };
            auto batch
                = agentxx::tools::parseSubagentBatchFromInterrupt(handleArg, "p", "t", nullptr);
            XX_TEST_EXPECT_EQ(batch.tasks.size(), size_t{1});
            const auto& t = batch.tasks[0];
            XX_TEST_EXPECT_EQ(t.subagentName, std::string{"old-style"});
            XX_TEST_EXPECT_EQ(t.systemPrompt, std::string{"old-sp"});
            XX_TEST_EXPECT_EQ(t.message, std::string{"old-msg"});
            XX_TEST_EXPECT_TRUE(t.enableSummarization.has_value());
            XX_TEST_EXPECT_TRUE(t.enableSummarization.value_or(false));
            // 旧格式无任务级 result_id → 使用中断级 resultId
            XX_TEST_EXPECT_EQ(t.resultId, std::string{"call_legacy"});
        }

        // --- D. 空参数 (既非批量也非单发): 包装为 1 个全空 task, 不崩溃 ---
        {
            agentxx::middleware::InterruptHandleArg handleArg;
            handleArg.name     = "subagent";
            handleArg.resultId = "";
            handleArg.arg      = neograph::json::object();
            auto batch
                = agentxx::tools::parseSubagentBatchFromInterrupt(handleArg, "p", "t", nullptr);
            XX_TEST_EXPECT_EQ(batch.tasks.size(), size_t{1});
            XX_TEST_EXPECT_EQ(batch.tasks[0].subagentName, std::string{""});
            XX_TEST_EXPECT_EQ(batch.tasks[0].message, std::string{""});
            XX_TEST_EXPECT_EQ(batch.tasks[0].resultId, std::string{""});
        }
    }

    // ==================== S6. 写入-读取 key 规则闭环 (build ↔ extract) ====================

    {
        // 写入侧 (agent_runner 中断循环) 与读取侧 (execute_async) 共用
        // makeSubagentResumeKey; 此处模拟完整 resume 往返验证规则一致

        // --- A. 正常任务: content 以 json string 写入 → 读取还原纯文本 ---
        {
            agentxx::events::RespSubagentBatch resp;
            resp.results.push_back(agentxx::events::RespSubagentBatchItem{
                .resultId = "res-1",
                .content  = "payload-one",
            });
            resp.results.push_back(agentxx::events::RespSubagentBatchItem{
                .resultId = "", // 无 result_id → 按序号兜底 (第 2 个任务)
                .content  = "payload-two",
            });
            neograph::json resumeValues;
            agentxx::tools::buildSubagentResumeValues(resumeValues, resp, "tc_closed_loop");
            XX_TEST_EXPECT_TRUE(resumeValues.contains("tc_closed_loop_res-1"));
            XX_TEST_EXPECT_TRUE(resumeValues.contains("tc_closed_loop_2"));

            // 读取侧: 相同 toolCallId + 任务 resultId 组合
            auto env = std::make_shared<SubagentToolEnv>();
            env->registerTask("w", "w");
            env->resetInterruptState();
            neograph::json args{
                {"tasks",
                 neograph::json::array(
                     {neograph::json{{"subagent", "w"}, {"message", "m1"}, {"result_id", "res-1"}},
                      neograph::json{{"subagent", "w"}, {"message", "m2"}}}
                 )                               },
                {"sessionId",    env->sessionId  },
                {"tool_call_id", "tc_closed_loop"},
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                resumeValues
            );
            auto r      = co_await env->tool->execute_async(args);
            auto parsed = neograph::json::parse(r);
            XX_TEST_EXPECT_TRUE(parsed.is_array());
            XX_TEST_EXPECT_EQ(parsed.size(), size_t{2});
            XX_TEST_EXPECT_EQ(parsed[0].get<std::string>(), std::string{"payload-one"});
            XX_TEST_EXPECT_EQ(parsed[1].get<std::string>(), std::string{"payload-two"});
        }

        // --- B. 失败任务: hasError → 写入 {"error": errorMessage}, 读取端原样透传 ---
        {
            agentxx::events::RespSubagentBatch resp;
            resp.results.push_back(agentxx::events::RespSubagentBatchItem{
                .resultId     = "err-task",
                .content      = "",
                .hasError     = true,
                .errorMessage = "depth budget exhausted",
            });
            neograph::json resumeValues;
            agentxx::tools::buildSubagentResumeValues(resumeValues, resp, "tc_err");
            XX_TEST_EXPECT_TRUE(resumeValues["tc_err_err-task"].is_object());
            XX_TEST_EXPECT_EQ(
                resumeValues["tc_err_err-task"].value("error", std::string{}),
                std::string{"depth budget exhausted"}
            );

            // 读取端把 error 对象 dump 为文本 (交由父 LLM 自行处理错误信息)
            auto env = std::make_shared<SubagentToolEnv>();
            env->registerTask("w", "w");
            env->resetInterruptState();
            neograph::json args{
                {"subagent",     "w"           },
                {"message",      "m"           },
                {"result_id",    "err-task"    },
                {"sessionId",    env->sessionId},
                {"tool_call_id", "tc_err"      },
            };
            try {
                (void)co_await env->tool->execute_async(args);
            } catch (const neograph::graph::NodeInterrupt&) {
            }
            env->ctx->middlewareHandleContext->setGraphDataItemValue<neograph::json>(
                env->sessionId,
                agentxx::middleware::MiddlewareContext::graphDataKey_interruptResult,
                resumeValues
            );
            auto r = co_await env->tool->execute_async(args);
            XX_TEST_EXPECT_EQ(r, std::string{R"({"error":"depth budget exhausted"})"});
        }

        // --- C. toolCallId 为空 (无前缀): key 直接为 resultId / 序号 ---
        {
            XX_TEST_EXPECT_EQ(
                agentxx::tools::makeSubagentResumeKey("", "explicit-id", 3),
                std::string{"explicit-id"}
            );
            XX_TEST_EXPECT_EQ(agentxx::tools::makeSubagentResumeKey("", "", 7), std::string{"7"});
            XX_TEST_EXPECT_EQ(
                agentxx::tools::makeSubagentResumeKey("tc", "", 4),
                std::string{"tc_4"}
            );
            XX_TEST_EXPECT_EQ(
                agentxx::tools::makeSubagentResumeKey("tc", "rid", 9),
                std::string{"tc_rid"}
            );
        }
    }

    co_return TestResult{g_sat_passed, g_sat_failed};
}

} // namespace test
} // namespace agentxx
