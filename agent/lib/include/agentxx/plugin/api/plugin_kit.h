/// 插件开发 SDK (C++ header-only) —— agentxx 侧领域部分
///
/// 分层 (插件框架内核已拆分为 cxx_pluginxx 独立工程):
/// - **通用部分**位于 `pluginxx/kit/kit.h`: 跨边界字符串工具 (PluginStringView /
///   PluginString)、通用接口表查询与聚合、实例级 Logger、Task/锚定协程原语、
///   CancelRegistry / OpCtl / ArgReader、后台任务 spawn、通用导出宏 —— 本头在开头
///   包含它, 并把其中的名字逐条 using 引入 `agentxx::plugin`, 插件源码书写
///   `agentxx::plugin::Xxx` 与拆分前完全一致;
/// - **本头只提供 agent 领域 helper**: 接口表聚合 (AgentIfaces / ClientIfaces)、
///   工具模式构建 (ToolSchemaBuilder)、工具注册 (tool / fast_tool / blocking_tool /
///   polled_tool)、钩子 (hook)、图节点 (graph node)、工具权限声明、阻塞便捷调用
///   (call_tool_blocking)、client 侧特化渲染与实例上下文 (ClientPluginBase)。
#pragma once
#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/ui/build.h"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "asio/post.hpp"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include "pluginxx/kit/kit.h"
#include "utilxx_base/container_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/json_view.h"
#include <type_traits>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

/// asio 命名空间别名 (与 pluginxx/kit/kit.h 中同名别名指向同一目标, 重复声明合法)
namespace asio = ::boost::asio;

namespace agentxx {
namespace plugin {

/* ==================== 通用 SDK 名的引入 (pluginxx/kit/kit.h) ====================
 *
 * 插件源码一直书写 `agentxx::plugin::Xxx`, 为保持**插件源码零改动**, 这里把内核通用
 * 部分的名字逐条引入本命名空间:
 * - 类型本体只有 `pluginxx` 一份 (using 声明不产生新类型, 无 ODR 风险);
 * - `agentxx::plugin::Task<T>` 与 `pluginxx::Task<T>` 是同一模板;
 * - 新增内核通用名字时按需在此追加一行, 引入面始终显式可读。
 *
 * 注意: 这不是"转发头"——没有 `agentxx/plugin/api/plugin_kit_core.h` 之类的路径兼容层,
 * 通用 SDK 只能经 `pluginxx/kit/kit.h` 包含。
 */

using pluginxx::ArgReader;
using pluginxx::CancelledException;
using pluginxx::CancelRegistry;
using pluginxx::capability;
using pluginxx::ctxGuardLogger;
using pluginxx::invoke_cap;
using pluginxx::invoke_capability_blocking;
using pluginxx::Json;
using pluginxx::jsonEscape;
using pluginxx::JsonView;
using pluginxx::logCreateFailure;
using pluginxx::Logger;
using pluginxx::offload;
using pluginxx::OpCtl;
using pluginxx::PluginBaseT;
using pluginxx::PluginIfaceCore;
using pluginxx::pluginLog;
using pluginxx::pluginStrdup;
using pluginxx::PluginString;
using pluginxx::PluginStringView;
using pluginxx::queryInterface;
using pluginxx::RootRequest;
using pluginxx::sleep;
using pluginxx::spawn;
using pluginxx::Task;
using pluginxx::validateInterface;
using pluginxx::yield;

/* ==================== 接口表聚合 ==================== */

/// agent 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct AgentIfaces {
    const AgentxxPluginToolsIface*      tools        = nullptr; ///< "agentxx.agent.tools"
    const AgentxxPluginPermissionIface* permission   = nullptr; ///< "agentxx.agent.permission"
    const AgentxxPluginHooksIface*      hooks        = nullptr; ///< "agentxx.agent.hooks"
    const PluginxxEventsIface*          events       = nullptr; ///< "pluginxx.events"
    const PluginxxCapabilitiesIface*    capabilities = nullptr; ///< "pluginxx.capabilities"
    const PluginxxSchedulerIface*       scheduler    = nullptr; ///< "pluginxx.scheduler"
    const AgentxxPluginSessionIface*    session      = nullptr; ///< "agentxx.agent.session"
    const PluginxxPluginsIface*         plugins      = nullptr; ///< "pluginxx.plugins"
    const PluginxxConfigIface*          config       = nullptr; ///< "pluginxx.config"
    const AgentxxPluginPromptIface*     prompt       = nullptr; ///< "agentxx.agent.prompt"
    const PluginxxJsonIface*            json         = nullptr; ///< "pluginxx.json"
    const PluginxxLogIface*             log          = nullptr; ///< "pluginxx.log"
    const AgentxxPluginResourcesIface*  resources    = nullptr; ///< "agentxx.agent.resources"
    const AgentxxPluginModelIface*      model        = nullptr; ///< "agentxx.agent.model"
    const PluginxxCancelIface*          cancel       = nullptr; ///< "pluginxx.cancel"
    const AgentxxPluginGraphIface*      graph        = nullptr; ///< "agentxx.agent.graph"
    const PluginxxTasksIface*           tasks        = nullptr; ///< "pluginxx.tasks"
    /// "pluginxx.coroutine_runtime": 协程驱动 (host driver/wake 协议)。
    const PluginxxCoroutineRuntimeIface* coroutineRuntime = nullptr;

    /// 从宿主查询全部已知 agent 侧接口表 (host 为空时返回全 NULL 聚合)
    static AgentIfaces query(const PluginxxHost* host) {
        AgentIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.tools = queryInterface<AgentxxPluginToolsIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_TOOLS);
        f.permission = queryInterface<AgentxxPluginPermissionIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_PERMISSION
        );
        f.hooks  = queryInterface<AgentxxPluginHooksIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_HOOKS);
        f.events = queryInterface<PluginxxEventsIface>(host, PLUGINXX_IFACE_EVENTS);
        f.capabilities
            = queryInterface<PluginxxCapabilitiesIface>(host, PLUGINXX_IFACE_CAPABILITIES);
        f.scheduler = queryInterface<PluginxxSchedulerIface>(host, PLUGINXX_IFACE_SCHEDULER);
        f.session
            = queryInterface<AgentxxPluginSessionIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_SESSION);
        f.plugins = queryInterface<PluginxxPluginsIface>(host, PLUGINXX_IFACE_PLUGINS);
        f.config  = queryInterface<PluginxxConfigIface>(host, PLUGINXX_IFACE_CONFIG);
        f.prompt
            = queryInterface<AgentxxPluginPromptIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_PROMPT);
        f.json      = queryInterface<PluginxxJsonIface>(host, PLUGINXX_IFACE_JSON);
        f.log       = queryInterface<PluginxxLogIface>(host, PLUGINXX_IFACE_LOG);
        f.resources = queryInterface<AgentxxPluginResourcesIface>(
            host,
            AGENTXX_PLUGIN_IFACE_AGENT_RESOURCES
        );
        f.model  = queryInterface<AgentxxPluginModelIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_MODEL);
        f.cancel = queryInterface<PluginxxCancelIface>(host, PLUGINXX_IFACE_CANCEL);
        f.graph  = queryInterface<AgentxxPluginGraphIface>(host, AGENTXX_PLUGIN_IFACE_AGENT_GRAPH);
        f.tasks  = queryInterface<PluginxxTasksIface>(host, PLUGINXX_IFACE_TASKS);
        f.coroutineRuntime
            = queryInterface<PluginxxCoroutineRuntimeIface>(host, PLUGINXX_IFACE_COROUTINE_RUNTIME);
        return f;
    }
};

/// client 侧接口表聚合 (一次查询; 成员为 NULL 表示宿主未实现该接口)
struct ClientIfaces {
    const AgentxxClientUiIface*      ui      = nullptr; ///< "agentxx.client.ui"
    const AgentxxClientEventsIface*  events  = nullptr; ///< "agentxx.client.events"
    const AgentxxClientSessionIface* session = nullptr; ///< "agentxx.client.session"
    const AgentxxClientWireIface*    wire    = nullptr; ///< "agentxx.client.wire"
    const AgentxxClientSelfIface*    self    = nullptr; ///< "agentxx.client.self"
    const AgentxxClientJsonIface*    json    = nullptr; ///< "agentxx.client.json"
    const AgentxxClientLogIface*     log     = nullptr; ///< "agentxx.client.log"

    /// 从宿主查询全部已知 client 侧接口表 (host 为空时返回全 NULL 聚合)
    static ClientIfaces query(const PluginxxHost* host) {
        ClientIfaces f;
        if (!host || !host->vtable || !host->vtable->query_interface) {
            return f;
        }
        f.ui      = queryInterface<AgentxxClientUiIface>(host, AGENTXX_IFACE_CLIENT_UI);
        f.events  = queryInterface<AgentxxClientEventsIface>(host, AGENTXX_IFACE_CLIENT_EVENTS);
        f.session = queryInterface<AgentxxClientSessionIface>(host, AGENTXX_IFACE_CLIENT_SESSION);
        f.wire    = queryInterface<AgentxxClientWireIface>(host, AGENTXX_IFACE_CLIENT_WIRE);
        f.self    = queryInterface<AgentxxClientSelfIface>(host, AGENTXX_IFACE_CLIENT_SELF);
        f.json    = queryInterface<AgentxxClientJsonIface>(host, AGENTXX_IFACE_CLIENT_JSON);
        f.log     = queryInterface<AgentxxClientLogIface>(host, AGENTXX_IFACE_CLIENT_LOG);
        return f;
    }
};

/* ==================== 提示词描述解析结构 ==================== */

struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

inline std::string
    toolPromptArgDesc(const ToolPromptText& p, std::string_view key, std::string_view fallback) {
    auto it = p.args.find(key);
    if (it != p.args.end() && !it->second.empty()) {
        return it->second;
    }
    return std::string{fallback};
}

/* ==================== 声明式模式构建器 ToolSchemaBuilder ==================== */

class ToolSchemaBuilder {
public:

    explicit ToolSchemaBuilder(ToolPromptText prompt = {}) :
        prompt_(std::move(prompt)) {}

    ToolSchemaBuilder& string(
        std::string_view           name,
        std::string_view           desc,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& integer(
        std::string_view       name,
        std::string_view       desc,
        bool                   required = false,
        std::optional<int64_t> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "integer";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& number(
        std::string_view      name,
        std::string_view      desc,
        bool                  required = false,
        std::optional<double> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "number";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& boolean(
        std::string_view    name,
        std::string_view    desc,
        bool                required = false,
        std::optional<bool> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "boolean";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder&
        stringArray(std::string_view name, std::string_view desc, bool required = false) {
        return array(name, desc, "string", required);
    }

    ToolSchemaBuilder& array(
        std::string_view name,
        std::string_view desc,
        std::string_view itemType = "string",
        bool             required = false
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "array";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["items"]       = utilxx_base::Json{
                  {"type", std::string(itemType)}
        };
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    ToolSchemaBuilder& enumString(
        std::string_view           name,
        std::string_view           desc,
        std::vector<std::string>   options,
        bool                       required = false,
        std::optional<std::string> defVal   = std::nullopt
    ) {
        utilxx_base::Json prop;
        prop["type"]        = "string";
        prop["description"] = toolPromptArgDesc(prompt_, name, desc);
        prop["enum"]        = options;
        if (defVal.has_value()) {
            prop["default"] = *defVal;
        }
        properties_[std::string(name)] = std::move(prop);
        if (required) {
            required_.push_back(std::string(name));
        }
        return *this;
    }

    std::string build() const {
        utilxx_base::Json schema;
        schema["type"]       = "object";
        schema["properties"] = properties_;
        if (!required_.empty()) {
            schema["required"] = required_;
        } else {
            schema["required"] = utilxx_base::Json::array();
        }
        return schema.dump();
    }

private:

    ToolPromptText           prompt_;
    utilxx_base::Json        properties_ = utilxx_base::Json::object();
    std::vector<std::string> required_;
};

/* ==================== 插件实例上下文基类 (agentxx 领域扩展) ==================== */

/// agentxx 插件实例上下文基类
///
/// 通用能力 (host / iface / log / config / workDir / language / strdup / jsonEscape /
/// cancelRegistry / 协程驱动桥 / 后台协作任务) 全部继承自 [pluginxx::PluginBaseT],
/// 以 agent 侧接口表聚合 [AgentIfaces] 作为其模板实参;
/// 本类只补齐 agent 领域 helper: 工具模式构建 (schema)、工具提示词描述 (toolPrompt)、
/// 会话共享存储 (addShareStore), 并在 [onHostReady] 中挂钩领域事件。
class PluginBase : public pluginxx::PluginBaseT<AgentIfaces> {
public:

    /// 声明式工具模式构建器 (基于工具提示词描述)
    ToolSchemaBuilder schema(std::string_view toolName) const {
        return ToolSchemaBuilder(toolPrompt(toolName));
    }

    /// 工具提示词描述 (经宿主 agentxx.agent.config 接口表查询; 缺失时返回空描述)
    ToolPromptText toolPrompt(std::string_view tool) const {
        ToolPromptText res;
        if (!host || !iface.config || !iface.config->get_tool_prompt) {
            return res;
        }
        auto           toolSv = PluginStringView::from(tool.data(), tool.size());
        PluginxxString s{nullptr, 0};
        iface.config->get_tool_prompt(host, &toolSv, &s);
        if (!s.data) {
            return res;
        }
        std::string jsonStr(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        try {
            auto j = utilxx_base::Json::parse(jsonStr);
            if (j.contains("depict") && j["depict"].is_string()) {
                res.depict = j["depict"].get<std::string>();
            }
            if (j.contains("args") && j["args"].is_object()) {
                for (const auto& [k, v] : j["args"].items()) {
                    if (v.is_string()) {
                        utilxx_base::insertOrAssignHeterogeneous(res.args, k, v.get<std::string>());
                    }
                }
            }
        } catch (...) {
        }
        return res;
    }

    /// 写入会话共享存储 (经宿主 agentxx.agent.session 接口表)
    int64_t addShareStore(PluginxxStringView tid, std::string_view content) const {
        if (!host || !iface.session || !iface.session->add_share_store) {
            return -1;
        }
        auto contentSv = PluginStringView::from(content.data(), content.size());
        return iface.session->add_share_store(host, &tid, &contentSv);
    }

    int64_t addShareStore(std::string_view tid, std::string_view content) const {
        return addShareStore(PluginStringView::from(tid.data(), tid.size()), content);
    }

protected:

    /// 领域挂钩: 基类 [init] 末尾调用 —— 挂钩会话轮次开始事件, 自动为当前会话重置
    /// 实例级取消登记 (宿主下发 cancel 时已通过 execute_cancel 写入 CancelRegistry)
    void onHostReady() override {
        if (!iface.events || !iface.events->subscribe) {
            return;
        }
        auto topicSv   = PluginStringView::fromCstr("plugin.agentxx.round_start");
        roundStartSub_ = iface.events->subscribe(
            host,
            &topicSv,
            [](const PluginxxStringView* ev, void* ud) {
                auto* self = static_cast<PluginBase*>(ud);
                if (!self || !ev || !ev->data) {
                    return;
                }
                try {
                    auto j = utilxx_base::Json::parse(
                        std::string_view{ev->data, static_cast<size_t>(ev->size)}
                    );
                    std::string sid = j.value("sessionId", "");
                    if (!sid.empty()) {
                        self->cancelRegistry.clearCancelled(sid);
                    }
                } catch (...) {
                }
            },
            this
        );
    }

private:

    /// 会话轮次开始事件的订阅句柄 (实例销毁时由宿主随实例一并清理)
    PluginxxSubscription* roundStartSub_ = nullptr;
};

/* ==================== 内核 detail 名的引入 (pluginxx/kit/kit.h) ====================
 *
 * 领域 helper (polled_tool / graph node / call_tool awaiter) 需要内核协程设施的
 * detail 类型与函数; 同样以 using 声明引入, 类型本体仍在 pluginxx::detail 一份。
 */

namespace detail {

using pluginxx::detail::advanceRootOnce;
using pluginxx::detail::autoStopSpawns;
using pluginxx::detail::AwaiterState;
using pluginxx::detail::BridgeRoot;
using pluginxx::detail::callLifecycleEntry;
using pluginxx::detail::CompletionGuard;
using pluginxx::detail::destroyBridgeFrame;
using pluginxx::detail::finishIfDone;
using pluginxx::detail::invokeCap;
using pluginxx::detail::InvokeCapAwaiter;
using pluginxx::detail::InvokeCapState;
using pluginxx::detail::jsonGet;
using pluginxx::detail::OffloadAwaiter;
using pluginxx::detail::PolledRoot;
using pluginxx::detail::PollOneBridge;
using pluginxx::detail::PromiseBase;
using pluginxx::detail::resumePluginCoroutine;
using pluginxx::detail::RootRequest;
using pluginxx::detail::SleepAwaiter;
using pluginxx::detail::spawnTaskImpl;
using pluginxx::detail::startBridgedRoot;
using pluginxx::detail::YieldAwaiter;

struct CallToolState {
    const PluginxxHost*            host  = nullptr;
    const AgentxxPluginToolsIface* tools = nullptr;
    /// 协程驱动桥 (可空): 完成回调经它投递 continuation 并唤醒 driver。
    PollOneBridge*            bridge = nullptr;
    std::string               name;
    std::string               argsJson;
    std::string               threadId;
    PluginxxOperatorHandle*   opHandle = nullptr;
    int32_t                   status   = PLUGINXX_OPERATOR_OK;
    std::string               payload;
    std::string               startError;
    std::atomic<AwaiterState> state{AwaiterState::INIT};
    void*                     coroAddr = nullptr;
};

struct CallToolAwaiter {
    std::shared_ptr<CallToolState> st;

    CallToolAwaiter(
        const PluginxxHost*            in_host,
        const AgentxxPluginToolsIface* in_tools,
        std::string_view               in_name,
        std::string_view               in_args,
        std::string_view               in_tid,
        PollOneBridge*                 in_bridge = nullptr
    ) :
        st(std::make_shared<CallToolState>()) {
        st->host     = in_host;
        st->tools    = in_tools;
        st->name     = std::string(in_name);
        st->argsJson = std::string(in_args);
        st->threadId = std::string(in_tid);
        st->bridge   = in_bridge;
    }

    bool await_ready() const noexcept {
        return !st || !st->tools || !st->tools->call_tool_async;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        st->coroAddr = h.address();
        st->state.store(AwaiterState::CALLING, std::memory_order_release);

        auto*          holder = new std::shared_ptr<CallToolState>(st);
        PluginxxString err{nullptr, 0};
        auto           nameSv = PluginStringView::from(st->name.data(), st->name.size());
        auto           argsSv = PluginStringView::from(st->argsJson.data(), st->argsJson.size());
        auto           tidSv  = PluginStringView::from(st->threadId.data(), st->threadId.size());

        st->opHandle = st->tools->call_tool_async(
            st->host,
            &nameSv,
            &argsSv,
            &tidSv,
            [](void* ud, int32_t cbSt, const PluginxxStringView* pl) {
                auto* hp = static_cast<std::shared_ptr<CallToolState>*>(ud);
                auto  s  = *hp;
                delete hp;

                s->status = cbSt;
                if (pl && pl->data && pl->size > 0) {
                    s->payload.assign(pl->data, static_cast<size_t>(pl->size));
                }

                auto expected = AwaiterState::CALLING;
                if (s->state.compare_exchange_strong(
                        expected,
                        AwaiterState::COMPLETED,
                        std::memory_order_acq_rel
                    )) {
                    return;
                }

                auto handle = std::coroutine_handle<Promise>::from_address(s->coroAddr);
                handle.promise().clear_outstanding();

                // 桥接路径不在宿主回调栈内恢复插件协程 (见 [resumePluginCoroutine])。
                resumePluginCoroutine(s->bridge, handle);
            },
            holder,
            &err
        );

        if (!st->opHandle) {
            delete holder;
            if (err.data) {
                st->startError.assign(err.data, static_cast<size_t>(err.size));
                PluginString::free(st->host, &err);
            }
            return false;
        }

        auto expected = AwaiterState::CALLING;
        if (st->state.compare_exchange_strong(
                expected,
                AwaiterState::SUSPENDED,
                std::memory_order_acq_rel
            )) {
            h.promise().set_outstanding([st = this->st]() {
                if (st->tools && st->tools->op_cancel && st->opHandle) {
                    st->tools->op_cancel(st->opHandle);
                }
            });
            return true;
        }

        return false;
    }

    std::string await_resume() {
        if (!st->startError.empty()) {
            throw std::runtime_error("call_tool start failed: " + st->startError);
        }
        if (st->status == PLUGINXX_OPERATOR_CANCELLED) {
            throw CancelledException(st->payload.empty() ? "call_tool cancelled" : st->payload);
        }
        if (st->status != PLUGINXX_OPERATOR_OK) {
            throw std::runtime_error(st->payload.empty() ? "call_tool failed" : st->payload);
        }
        return std::move(st->payload);
    }
};
} // namespace detail

inline detail::CallToolAwaiter call_tool(
    const PluginBase& ctx,
    std::string_view  name,
    std::string_view  argsJson,
    std::string_view  threadId = {}
) {
    return detail::CallToolAwaiter{
        ctx.host,
        ctx.iface.tools,
        name,
        argsJson,
        threadId,
        &ctx.bridge()
    };
}

/// ==================== (kit::tool / fast_tool / blocking_tool / hook / capability)
/// ====================

template<typename Ctx, typename TaskFn>
inline void tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    TaskFn&&         fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct ToolShim {
        Ctx*                               ctx = nullptr;
        std::decay_t<TaskFn>               fn;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
    };

    auto shim = ctx.storeShim(std::make_unique<ToolShim>(
        ToolShim{&ctx, std::forward<TaskFn>(fn), std::make_shared<std::atomic<bool>>(false)}
    ));

    struct Job {
        ToolShim*                          shim;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        /// 拥有 args/session/tool_call_id：业务协程可能挂起后继续读取这些视图
        /// （F13），因此它们必须比 start 调用活得久。
        detail::RootRequest request;
        /// OpCtl 同样必须由 Job 拥有：业务协程以引用接收它，挂起后仍会读取
        /// （例如 ctl.throw_if_cancelled()），放在 start 栈上会悬垂。
        OpCtl ctl;
    };

    AgentxxPluginToolSpec spec{};
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                         user_data,
                            const PluginxxStringView*     args_json,
                            const PluginxxStringView*     thread_id,
                            const PluginxxStringView*     tool_call_id,
                            const PluginxxOperatorNotify* notify,
                            PluginxxString*               error_out) -> void* {
        auto* shim = static_cast<ToolShim*>(user_data);
        (void)error_out;
        // 先建立拥有型 Request，再把视图交给业务代码：协程挂起期间 args/
        // session/tool_call_id 由 Job 持有，不再指向宿主借用缓冲区。
        auto request = detail::RootRequest::forTool(
            shim && shim->ctx ? shim->ctx->host : nullptr,
            args_json,
            thread_id,
            tool_call_id
        );
        CancelRegistry* cancelReg = nullptr;
        if (shim->ctx) {
            cancelReg = &shim->ctx->cancelRegistry;
        }
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        if (cancelReg && !request.sessionId.empty() && cancelReg->isCancelled(request.sessionId)) {
            cancelFlag->store(true, std::memory_order_release);
        }

        OpCtl ctl{
            cancelFlag,
            shim->ctx ? shim->ctx->host : nullptr,
            shim->ctx ? shim->ctx->iface.cancel : nullptr,
            request.sessionId,
            cancelReg
        };

        auto* job = new Job{shim, cancelFlag, nullptr, std::move(request), std::move(ctl)};

        auto task = shim->fn(*shim->ctx, job->request.args(), job->ctl);
        if (!task.handle_) {
            delete job;
            return nullptr;
        }

        auto h        = task.handle_;
        task.handle_  = nullptr;
        auto& p       = h.promise();
        p.notify_     = notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr};
        p.host_       = shim->ctx->host;
        p.cancelFlag_ = cancelFlag;

        job->coroAddr = h.address();

        // 根的首步由 host driver 推进 (不在 execute_start 里同步跑插件协程,
        // 因此不会有 completion 重入, 也不会占住宿主 IO 线程)。
        detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
            delete job;
        });
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<Job*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->shim && job->shim->ctx && !job->request.sessionId.empty()) {
            job->shim->ctx->cancelRegistry.cancel(job->request.sessionId);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

template<typename Ctx, typename SyncFn>
inline void fast_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    SyncFn&&         fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct FastShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<SyncFn> fn;
    };

    auto shim = ctx.storeShim(std::make_unique<FastShim>(FastShim{&ctx, std::forward<SyncFn>(fn)}));

    AgentxxPluginToolSpec spec{};
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                         user_data,
                            const PluginxxStringView*     args_json,
                            const PluginxxStringView*     thread_id,
                            const PluginxxStringView*     tool_call_id,
                            const PluginxxOperatorNotify* notify,
                            PluginxxString*               error_out) -> void* {
        auto* shim = static_cast<FastShim*>(user_data);
        (void)tool_call_id;
        try {
            std::string_view args(
                args_json && args_json->data ? args_json->data : "{}",
                args_json ? static_cast<size_t>(args_json->size) : 0
            );
            std::string_view tid(
                thread_id && thread_id->data ? thread_id->data : "",
                thread_id ? static_cast<size_t>(thread_id->size) : 0
            );
            std::string res;
            if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view, std::string_view>) {
                res = shim->fn(*shim->ctx, args, tid);
            } else if constexpr (std::is_invocable_v<SyncFn, Ctx&, std::string_view>) {
                res = shim->fn(*shim->ctx, args);
            } else {
                res = shim->fn(args);
            }

            if (notify && notify->done) {
                auto resSv = PluginStringView::from(res.data(), res.size());
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &resSv);
            }
        } catch (const std::exception& e) {
            if (notify && notify->done) {
                std::string what  = e.what();
                auto        errSv = PluginStringView::from(what.data(), what.size());
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
            } else if (error_out) {
                *error_out = PluginString::fromCstr(shim->ctx->host, e.what());
            }
        } catch (...) {
            if (notify && notify->done) {
                auto errSv = PluginStringView::fromCstr("unknown error");
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &errSv);
            } else if (error_out) {
                *error_out = PluginString::fromCstr(shim->ctx->host, "unknown error in fast_tool");
            }
        }
        return nullptr;
    };

    spec.execute_cancel = nullptr;

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

template<typename Ctx, typename BlockFn>
inline void blocking_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    BlockFn&&        fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct BlockShim {
        Ctx*                  ctx = nullptr;
        std::decay_t<BlockFn> fn;
    };

    auto shim
        = ctx.storeShim(std::make_unique<BlockShim>(BlockShim{&ctx, std::forward<BlockFn>(fn)}));

    struct Job {
        BlockShim*              shim = nullptr;
        PluginxxOperatorNotify  notify{};
        std::string             args;
        std::string             tid;
        std::string             tcid;
        std::string             workDir;
        std::string             argsJson;
        std::string             resultPayload;
        std::string             errorPayload;
        bool                    isCancelled   = false;
        PluginxxOperatorHandle* offloadHandle = nullptr;
    };

    AgentxxPluginToolSpec spec{};
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                         user_data,
                            const PluginxxStringView*     args_json,
                            const PluginxxStringView*     thread_id,
                            const PluginxxStringView*     tool_call_id,
                            const PluginxxOperatorNotify* notify,
                            PluginxxString*               error_out) -> void* {
        auto* shim = static_cast<BlockShim*>(user_data);
        (void)error_out;
        std::string tidStr(
            thread_id && thread_id->data ? thread_id->data : "",
            thread_id ? static_cast<size_t>(thread_id->size) : 0
        );
        std::string workDirCache;
        std::string argsJsonCache;
        if (shim && shim->ctx) {
            workDirCache  = shim->ctx->workDir(tidStr);
            argsJsonCache = shim->ctx->argsJson();
            if (!tidStr.empty() && shim->ctx->cancelRegistry.isCancelled(tidStr)) {
                shim->ctx->cancelRegistry.cancel(tidStr);
            }
        }
        auto* job = new Job{
            .shim   = shim,
            .notify = notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr},
            .args   = std::string(
                args_json && args_json->data ? args_json->data : "{}",
                args_json ? static_cast<size_t>(args_json->size) : 0
            ),
            .tid  = std::move(tidStr),
            .tcid = std::string(
                tool_call_id && tool_call_id->data ? tool_call_id->data : "",
                tool_call_id ? static_cast<size_t>(tool_call_id->size) : 0
            ),
            .workDir       = std::move(workDirCache),
            .argsJson      = std::move(argsJsonCache),
            .resultPayload = {},
            .errorPayload  = {},
            .isCancelled   = false
        };

        PluginxxString scheduleError{};
        if (shim && shim->ctx && shim->ctx->iface.scheduler
            && shim->ctx->iface.scheduler->offload) {
            job->offloadHandle = shim->ctx->iface.scheduler->offload(
                shim->ctx->host,
                [](void* ud, const PluginxxCancelToken* token, PluginxxString* err_out) -> void* {
                    (void)err_out;
                    auto* j = static_cast<Job*>(ud);
                    try {
                        if constexpr (std::is_invocable_v<
                                          BlockFn,
                                          Ctx&,
                                          std::string_view,
                                          std::string_view,
                                          std::string_view,
                                          const PluginxxCancelToken*>) {
                            j->resultPayload
                                = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 const PluginxxCancelToken*>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, j->tid, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view,
                                                 std::string_view>) {
                            j->resultPayload
                                = j->shim->fn(*j->shim->ctx, j->args, j->tid, j->workDir);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 const PluginxxCancelToken*>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 std::string_view,
                                                 const PluginxxCancelToken*>) {
                            j->resultPayload = j->shim->fn(j->args, token);
                        } else if constexpr (std::is_invocable_v<
                                                 BlockFn,
                                                 Ctx&,
                                                 std::string_view,
                                                 std::string_view>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args, j->tid);
                        } else if constexpr (std::is_invocable_v<BlockFn, Ctx&, std::string_view>) {
                            j->resultPayload = j->shim->fn(*j->shim->ctx, j->args);
                        } else {
                            j->resultPayload = j->shim->fn(j->args);
                        }
                    } catch (const CancelledException& e) {
                        j->isCancelled  = true;
                        j->errorPayload = e.what();
                    } catch (const std::exception& e) {
                        j->errorPayload = e.what();
                    } catch (...) {
                        j->errorPayload = "unknown blocking tool error";
                    }
                    return nullptr;
                },
                [](void* ud, int32_t status, void* res, const PluginxxStringView* err) {
                    (void)res;
                    auto*              j       = static_cast<Job*>(ud);
                    int32_t            st      = status;
                    PluginxxStringView payload = PluginStringView::from(nullptr, 0);

                    if (!PluginStringView::empty(err)) {
                        st      = PLUGINXX_OPERATOR_FAILED;
                        payload = *err;
                    } else if (!j->errorPayload.empty()) {
                        if (j->isCancelled) {
                            st = PLUGINXX_OPERATOR_CANCELLED;
                        } else {
                            st = PLUGINXX_OPERATOR_FAILED;
                        }
                        payload = PluginStringView::from(
                            j->errorPayload.data(),
                            j->errorPayload.size()
                        );
                    } else if (st == PLUGINXX_OPERATOR_CANCELLED || j->isCancelled) {
                        st = PLUGINXX_OPERATOR_CANCELLED;
                    } else {
                        st      = PLUGINXX_OPERATOR_OK;
                        payload = PluginStringView::from(
                            j->resultPayload.data(),
                            j->resultPayload.size()
                        );
                    }

                    if (j->notify.done) {
                        j->notify.done(j->notify.host_ud, st, &payload);
                    }
                    // OpCore::onEndpointDone 先在当前调用内线性化
                    // completionSubmitted_，因此 notify.done 返回后，宿主
                    // cancel 入口不会再把这个 provider handle 交回插件。
                    // 这使得 Job 可以在这里直接回收，不依赖第二个异步
                    // post，也不会留下自引用控制块泄漏。
                    delete j;
                },
                job,
                &scheduleError
            );
            if (!job->offloadHandle) {
                if (scheduleError.data) {
                    if (error_out) {
                        *error_out    = scheduleError;
                        scheduleError = {};
                    } else {
                        PluginString::free(shim->ctx->host, &scheduleError);
                    }
                } else if (error_out) {
                    *error_out = PluginString::fromCstr(
                        shim->ctx->host,
                        "blocking tool: scheduler offload rejected"
                    );
                }
                delete job;
                return nullptr;
            }
        } else {
            if (error_out && shim && shim->ctx) {
                *error_out = PluginString::fromCstr(
                    shim->ctx->host,
                    "blocking tool: scheduler offload unavailable"
                );
            }
            delete job;
            return nullptr;
        }
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<Job*>(op);
        if (job->offloadHandle && job->shim && job->shim->ctx && job->shim->ctx->iface.scheduler
            && job->shim->ctx->iface.scheduler->op_cancel) {
            job->shim->ctx->iface.scheduler->op_cancel(job->offloadHandle);
        }
        if (job->shim && job->shim->ctx && !job->tid.empty()) {
            job->shim->ctx->cancelRegistry.cancel(job->tid);
        }
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

/* ==================== 受控轮询工具 (polled_tool) ====================
 *
 * 适用: 业务体是 **asio 协程**、等待的是**插件本地 reactor 上的内核就绪事件**
 * (socket 收发 / 子进程管道 / 文件 IO / 本地 steady_timer) 的工具。
 *
 * 为什么需要它: 这类等待没有"宿主可见唤醒源" —— driver 里的 `poll_one` 只会执行
 * 已经就绪的 handler, 不会让私有 reactor 的等待对象到期。因此插件在注册时就
 * **声明**"该工具需要受控轮询驱动", 桥据此在有在途操作时继续申请请求:
 * - 有进展 (本轮 `poll_one` 执行到了 handler) → 立即续下一次请求;
 * - 无进展 → 退避 `PollOneBridge::kPollIntervalMs` (宿主计时器) 后再驱动;
 * - 连续有进展超过 `PollOneBridge::kPollBurstMax` 步 → 强制让出一次, 交给宿主;
 * - **没有在途 polled 操作时不申请请求、不建定时器** (空闲零开销)。
 *
 * 业务签名与 [blocking_tool] 完全一致 (只把返回值换成 `asio::awaitable<std::string>`),
 * 因此迁移通常只是换一个注册函数名:
 * ```cpp
 * polled_tool(ctx, name, depict, schema,
 *     [](Ctx& c, std::string_view args, std::string_view tid, std::string_view workDir,
 *        const PluginxxCancelToken* cancel) -> asio::awaitable<std::string> {
 *         ArgReader reader(args);
 *         co_return co_await doAsync(reader.raw(), c.workDir? ...);
 *     });
 * ```
 *
 * 约束 (与实现一起遵守):
 * - **不要**在 polled 协程里做重 CPU/阻塞工作: 它运行在宿主 IO 线程上
 *   (目录遍历 / 全文件扫描 / 向量相似度这类继续用 `blocking_tool`);
 * - **不要**在 polled 协程里 `co_await` kit 的 `Task` 型原语 (`sleep`/`call_tool`/
 *   `invoke_cap`): 它们的 awaiter 依赖 kit 自有 promise 接口, 而这里是
 *   `asio::awaitable`。需要等待时用 asio 原生 `steady_timer` (pump 下可用) 或经
 *   `bridge().local_executor()` 投递后续步骤;
 */

namespace detail {

/// 受控轮询工具的 Job 侧取消查询 (kit 自造的会话级令牌, 不是宿主 token):
/// 只读 Job 的取消标志与实例 CancelRegistry, 生命周期与该 polled 根相同。
/// 模板参数是 `polled_tool` 内的局部 Job 类型 (在其完整之后实例化)。
template<typename Job>
inline int32_t PLUGINXX_CALL polledJobTokenIsRequestedAbi(const PluginxxCancelToken* token) {
    auto* job = (token && token->host_ud) ? static_cast<const Job*>(token->host_ud) : nullptr;
    return (job && job->cancelled()) ? 1 : 0;
}

/// 受控轮询工具的业务体包装 (两条路径共用): 运行业务协程并把终态写回 Job。
///
/// - 异常映射: `CancelledException` → CANCELLED, 其它异常 → FAILED;
/// - 业务体正常返回但期间已请求取消 → CANCELLED (payload 为空, 与
///   `blocking_tool` 的取消语义一致);
/// - 本协程自身不决定"由谁上报/释放 Job": 由 [runPolledPumpJob] 收尾。
template<typename Job>
inline asio::awaitable<void> runPolledToolBody(Job* job) {
    try {
        if (job->cancelled()) {
            throw CancelledException("polled tool cancelled before execution");
        }
        job->payload = co_await job->shim
                           ->fn(*job->shim->ctx, job->args, job->tid, job->workDir, &job->token);
        job->status = PLUGINXX_OPERATOR_OK;
    } catch (const CancelledException& e) {
        job->status  = PLUGINXX_OPERATOR_CANCELLED;
        job->payload = e.what();
    } catch (const std::exception& e) {
        job->status  = PLUGINXX_OPERATOR_FAILED;
        job->payload = e.what();
    } catch (...) {
        job->status  = PLUGINXX_OPERATOR_FAILED;
        job->payload = "unknown polled tool error";
    }
    if (job->status == PLUGINXX_OPERATOR_OK && job->cancelled()) {
        job->status = PLUGINXX_OPERATOR_CANCELLED;
        job->payload.clear();
    }
}

/// 受控轮询工具的业务体包装 (泵路径): 在桥的本地执行器上跑完业务协程, 然后
/// 上报终态并回收 Job (泵路径下 Job 由本协程回收)。
///
/// 停止/关闭路径 (桥 `stop` → [PollOneBridge::failAllPolledRoots]) 可能先一步
/// 认领完成权并回收 Job: 那时 `claimFinish()` 返回 false, 本协程只做退出。
template<typename Job>
inline asio::awaitable<void> runPolledPumpJob(Job* job) {
    co_await runPolledToolBody(job);
    // 先取本地副本: 收尾 (cleanup) 会释放 Job 本身。
    auto          root    = job->root;
    auto*         bridge  = job->bridge;
    const int32_t status  = job->status;
    std::string   payload = job->payload;
    if (!root || !root->claimFinish()) {
        co_return;
    }
    if (bridge) {
        bridge->removePolledRoot(root);
    }
    root->notifyHost(status, payload);
    // cleanup 释放 Job (幂等); 本地 `root` 保证 PolledRoot 自身活到本语句之后。
    root->runCleanup();
}

} // namespace detail

template<typename Ctx, typename PolledFn>
inline void polled_tool(
    Ctx&             ctx,
    std::string_view name,
    std::string_view depict,
    std::string_view schema,
    PolledFn&&       fn,
    int64_t          default_timeout_ms = 0,
    int32_t          flags              = 0
) {
    using PolledFnType = std::decay_t<PolledFn>;
    static_assert(
        std::is_invocable_v<
            PolledFnType,
            Ctx&,
            std::string_view,
            std::string_view,
            std::string_view,
            const PluginxxCancelToken*>,
        "polled_tool 业务体签名必须是 (Ctx&, std::string_view args, std::string_view tid, "
        "std::string_view workDir, const PluginxxCancelToken*) -> asio::awaitable<std::string>"
    );

    auto&       storage     = ctx.storage_;
    std::string finalDepict = ctx.toolPrompt(name).depict;
    if (finalDepict.empty()) {
        finalDepict = depict;
    }
    storage.push_back(std::move(finalDepict));
    storage.push_back(std::string(schema));

    struct PolledShim {
        Ctx*         ctx = nullptr;
        PolledFnType fn;
    };

    auto shim
        = ctx.storeShim(std::make_unique<PolledShim>(PolledShim{&ctx, std::forward<PolledFn>(fn)}));

    /// 一次 polled 根操作的拥有型状态。
    /// 生命周期: 业务协程帧 + [detail::runPolledPumpJob] 的收尾;
    /// 回收由 [detail::PolledRoot] 的 cleanup 兜底 (停止/关闭时会提前接管)。
    struct Job {
        PolledShim*                         shim   = nullptr;
        detail::PollOneBridge*              bridge = nullptr;
        std::shared_ptr<detail::PolledRoot> root;
        PluginxxOperatorNotify              notify{};
        detail::RootRequest                 request;
        std::string                         tid;     ///< 会话标识 (= request.sessionId)
        std::string                         args;    ///< 工具参数 JSON
        std::string                         workDir; ///< 会话工作目录 (IO 线程预取)
        std::shared_ptr<std::atomic<bool>>  cancelFlag;
        /// 传给业务体的取消令牌: kit 自造 (is_requested 读取消标志 + CancelRegistry)。
        PluginxxCancelToken token{nullptr, nullptr};
        /// 终态 (由业务体包装写入; 上报方见上)。
        int32_t     status = PLUGINXX_OPERATOR_OK;
        std::string payload;

        /// 是否已请求取消 (本实例取消标志 + 会话级 CancelRegistry)。
        bool cancelled() const noexcept {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                return true;
            }
            if (shim && shim->ctx && !tid.empty() && shim->ctx->cancelRegistry.isCancelled(tid)) {
                return true;
            }
            return false;
        }
    };

    // 自造取消令牌的查询函数: 按 host_ud 还原 Job (Job 为局部类型, 因此用
    // 上面的模板实例化; 此处 Job 已完整)。
    constexpr auto kTokenIsRequested = &detail::polledJobTokenIsRequestedAbi<Job>;

    AgentxxPluginToolSpec spec{};
    spec.name        = PluginStringView::from(name.data(), name.size());
    spec.description = PluginStringView::from(
        storage[storage.size() - 2].data(),
        storage[storage.size() - 2].size()
    );
    spec.parameters_json    = PluginStringView::from(storage.back().data(), storage.back().size());
    spec.user_data          = shim;
    spec.default_timeout_ms = default_timeout_ms;
    spec.flags              = flags;
    spec._reserved          = 0;

    spec.execute_start = [](void*                         user_data,
                            const PluginxxStringView*     args_json,
                            const PluginxxStringView*     thread_id,
                            const PluginxxStringView*     tool_call_id,
                            const PluginxxOperatorNotify* notify,
                            PluginxxString*               error_out) -> void* {
        auto* s = static_cast<PolledShim*>(user_data);
        if (!s || !s->ctx) {
            if (error_out) {
                *error_out = PluginString::fromCstr(nullptr, "polled tool: context released");
            }
            return nullptr;
        }
        auto& c = *s->ctx;
        // 拥有型 Request: args/session/tool_call_id 在整个 polled 根期间有效
        // (协程会挂起并继续读取它们)。
        auto request    = detail::RootRequest::forTool(c.host, args_json, thread_id, tool_call_id);
        auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
        if (!request.sessionId.empty() && c.cancelRegistry.isCancelled(request.sessionId)) {
            cancelFlag->store(true, std::memory_order_release);
        }

        auto* job       = new Job();
        job->shim       = s;
        job->notify     = notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr};
        job->request    = std::move(request);
        job->tid        = job->request.sessionId;
        job->args       = job->request.argsJson;
        job->workDir    = c.workDir(job->tid);
        job->cancelFlag = cancelFlag;
        job->root       = std::make_shared<detail::PolledRoot>(job->notify);
        job->token      = PluginxxCancelToken{kTokenIsRequested, job};
        {
            // 兜底回收: 停止/关闭路径 (failAllPolledRoots) 会执行它; 正常完成时
            // 由各自的上报方先执行, 这里保持幂等。
            auto* jobPtr = job;
            job->root->setCleanup([jobPtr] {
                delete jobPtr;
            });
        }

        auto* bridge = &c.bridge();
        job->bridge  = bridge;
        bridge->addPolledRoot(job->root);
        try {
            // 业务协程跑在**桥的本地执行器**上: 它的 socket/管道/文件等待因此
            // 注册到桥的 reactor, 由受控轮询推进 (首步恒异步, 不在本调用内执行)。
            asio::co_spawn(bridge->local_executor(), detail::runPolledPumpJob(job), asio::detached);
        } catch (...) {
            // 首步都无法排队: 按**拒绝**处理 (未产生任何宿主可见副作用),
            // 回收 Job 并返回 NULL + error。
            bridge->removePolledRoot(job->root);
            auto keep = job->root;
            job->root->claimFinish();
            job->root->runCleanup();
            if (error_out) {
                *error_out
                    = PluginString::fromCstr(c.host, "polled tool: failed to schedule coroutine");
            }
            return nullptr;
        }
        // co_spawn 已把首步投递到本地执行器, 但 kit 看不到这次投递
        // (不计入 readySteps_), 因此必须显式 wake —— 否则首步不会被驱动。
        bridge->wake();
        return job;
    };

    spec.execute_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<Job*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->shim && job->shim->ctx) {
            if (!job->tid.empty()) {
                job->shim->ctx->cancelRegistry.cancel(job->tid);
            }
            // 取消在途退避定时器, 让插件立刻得到一次驱动 (不必等满一个退避量子),
            // 从而尽快在阶段边界看到取消并收束根。
            if (job->bridge) {
                job->bridge->kickPumpWait();
            }
        }
    };

    if (ctx.iface.tools && ctx.iface.tools->register_tool) {
        ctx.iface.tools->register_tool(ctx.host, &spec);
    }
}

/* ==================== 钩子业务签名分发 (同步/异步共用) ==================== */

namespace detail {

/// 按可调用性选择钩子业务签名：fn(ctx, point, input) / fn(ctx, input) / fn(input)。
/// 返回类型原样转发：同步钩子通常是 void，异步钩子返回 Task<T>（F19 用返回类型
/// 严格区分同步与异步，签名不匹配时在 if constexpr 分支给出明确错误）。
template<typename HookFn, typename Ctx>
inline decltype(auto) invokeHook(HookFn& fn, Ctx& ctx, int32_t pt, std::string_view input) {
    if constexpr (std::is_invocable_v<HookFn, Ctx&, AgentxxPluginHookPoint, std::string_view>) {
        return fn(ctx, static_cast<AgentxxPluginHookPoint>(pt), input);
    } else if constexpr (std::is_invocable_v<HookFn, Ctx&, std::string_view>) {
        return fn(ctx, input);
    } else {
        return fn(input);
    }
}

} // namespace detail

template<typename Ctx, typename HookFn>
inline void hook(Ctx& ctx, AgentxxPluginHookPoint point, HookFn&& fn) {
    struct HookShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<HookFn> fn;
    };

    /// 异步钩子的 provider 句柄：拥有输入 Request，并由 promise.opCleanup_ 回收。
    struct HookJob {
        HookShim*                          shim = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        detail::RootRequest                request;
    };

    auto shim = ctx.storeShim(std::make_unique<HookShim>(HookShim{&ctx, std::forward<HookFn>(fn)}));

    AgentxxPluginHookSpec spec{};
    spec.point     = point;
    spec._reserved = 0;
    spec.user_data = shim;

    spec.hook_start = [](void*                         user_data,
                         int32_t                       pt,
                         const PluginxxStringView*     node_input_json,
                         const PluginxxOperatorNotify* notify,
                         PluginxxString*               error_out) -> void* {
        auto* shim = static_cast<HookShim*>(user_data);
        (void)error_out;
        if (!shim || !shim->ctx) {
            detail::CompletionGuard guard(notify);
            guard.failed("hook context released");
            return nullptr;
        }

        /// 输入纳入拥有型 Request：同步钩子在调用期间有效，异步 Task 由 HookJob
        /// 持有到协程真正结束（F13）。
        auto request = detail::RootRequest::forHook(shim->ctx->host, node_input_json);

        using HookRet = decltype(detail::invokeHook(shim->fn, *shim->ctx, pt, std::string_view{}));
        if constexpr (std::is_void_v<HookRet>) {
            /// 同步 void 钩子：调用返回即完成；异常统一映射为终态。
            detail::CompletionGuard guard(notify);
            try {
                detail::invokeHook(shim->fn, *shim->ctx, pt, request.args());
                guard.ok();
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else {
            /// Task<T> 钩子（通常 Task<void>）：由 promise 在协程结束后收束完成
            /// 通知，返回 Job 作为宿主可取消的 provider 句柄（F19）。
            auto* job = new HookJob{
                shim,
                std::make_shared<std::atomic<bool>>(false),
                nullptr,
                std::move(request)
            };
            auto task = detail::invokeHook(shim->fn, *shim->ctx, pt, job->request.args());
            if (!task.handle_) {
                delete job;
                detail::CompletionGuard guard(notify);
                guard.failed("hook returned an empty task");
                return nullptr;
            }
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.notify_     = notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr};
            p.host_       = job->request.host;
            p.cancelFlag_ = job->cancelFlag;
            job->coroAddr = h.address();

            // 根的首步由 host driver 推进 (不在 start 里同步跑插件协程,
            // 因此没有 completion 重入, 也不占住宿主 IO 线程)。
            detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
                delete job;
            });
            return job;
        }
    };

    spec.hook_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<HookJob*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    if (ctx.iface.hooks && ctx.iface.hooks->register_hook) {
        ctx.iface.hooks->register_hook(ctx.host, &spec);
    }
}

namespace detail {

/// 按可调用性选择图节点业务签名：fn(ctx, request, ctl) / fn(ctx, request)。
/// 返回类型原样转发：快同步节点返回 `std::string`（节点输出 JSON），
/// 锚定协程节点返回 `Task<T>`（同上，result 为节点输出 JSON）。
template<typename NodeFn, typename Ctx>
inline decltype(auto) invokeGraphNode(NodeFn& fn, Ctx& ctx, const RootRequest& req, OpCtl ctl) {
    if constexpr (std::is_invocable_v<NodeFn, Ctx&, const RootRequest&, OpCtl>) {
        return fn(ctx, req, std::move(ctl));
    } else {
        return fn(ctx, req);
    }
}

} // namespace detail

/// 注册插件自定义图节点类型（统一 root adapter）。
///
/// 与 tool/hook/capability 使用同一套生命周期与完成协议：
/// - 输入（node/config/state/thread_id）由 [RootRequest] 拥有，跨挂起点有效（F13）；
/// - 返回 `std::string`：快同步节点，调用返回即完成（异常 → FAILED）；
/// - 返回 `Task<std::string>`：锚定协程节点，宿主获得可取消的 provider 句柄，
///   完成通知在协程真正结束后 exactly-once 发出；
/// - `run_cancel` 置取消标志并取消嵌套 awaiter。
///
/// `return`: 0 = 注册成功；非 0 = 类型名冲突或宿主不支持（调用方应使 start 事务失败）。
template<typename Ctx, typename NodeFn>
inline int32_t
    graph_node(Ctx& ctx, std::string_view type, std::string_view configSchema, NodeFn&& fn) {
    if (!ctx.iface.graph || !ctx.iface.graph->register_node_type) {
        return -1;
    }

    struct NodeShim {
        Ctx*                 ctx = nullptr;
        std::decay_t<NodeFn> fn;
    };

    /// 异步节点的 provider 句柄：拥有输入 Request，由 promise.opCleanup_ 回收。
    struct NodeJob {
        NodeShim*                          shim = nullptr;
        std::shared_ptr<std::atomic<bool>> cancelFlag;
        void*                              coroAddr = nullptr;
        detail::RootRequest                request;
    };

    auto shim = ctx.storeShim(std::make_unique<NodeShim>(NodeShim{&ctx, std::forward<NodeFn>(fn)}));

    AgentxxPluginGraphNodeTypeSpec spec{};
    spec.type               = PluginStringView::from(type.data(), type.size());
    spec.config_schema_json = PluginStringView::from(configSchema.data(), configSchema.size());
    spec.user_data          = shim;

    spec.run_start = [](void*                         user_data,
                        const PluginxxStringView*     node_name,
                        const PluginxxStringView*     config_json,
                        const PluginxxStringView*     state_json,
                        const PluginxxStringView*     thread_id,
                        const PluginxxOperatorNotify* notify,
                        PluginxxString*               error_out) -> void* {
        auto* shim = static_cast<NodeShim*>(user_data);
        (void)error_out;
        if (!shim || !shim->ctx) {
            detail::CompletionGuard guard(notify);
            guard.failed("graph node context released");
            return nullptr;
        }
        // 输入纳入拥有型 Request：快同步节点在调用期间有效，异步节点由 Job
        // 持有到协程真正结束（F13）。
        auto request = detail::RootRequest::forGraphNode(
            shim->ctx->host,
            node_name,
            config_json,
            state_json,
            thread_id
        );
        auto  cancelFlag = std::make_shared<std::atomic<bool>>(false);
        OpCtl ctl{
            cancelFlag,
            shim->ctx->host,
            shim->ctx->iface.cancel,
            request.sessionId,
            &shim->ctx->cancelRegistry
        };

        using NodeRet = decltype(detail::invokeGraphNode(shim->fn, *shim->ctx, request, ctl));
        if constexpr (std::is_void_v<NodeRet> || std::is_convertible_v<NodeRet, std::string_view>) {
            /// 快同步节点：调用返回即完成；异常统一映射为终态。
            detail::CompletionGuard guard(notify);
            try {
                if constexpr (std::is_void_v<NodeRet>) {
                    detail::invokeGraphNode(shim->fn, *shim->ctx, request, std::move(ctl));
                    guard.ok();
                } else {
                    guard.ok(detail::invokeGraphNode(shim->fn, *shim->ctx, request, std::move(ctl))
                    );
                }
            } catch (...) {
                guard.fromCurrentException();
            }
            return nullptr;
        } else {
            /// Task<T> 节点：由 promise 在协程结束后收束完成通知，
            /// 返回 Job 作为宿主可取消的 provider 句柄。
            auto* job = new NodeJob{shim, std::move(cancelFlag), nullptr, std::move(request)};
            auto task = detail::invokeGraphNode(shim->fn, *shim->ctx, job->request, std::move(ctl));
            if (!task.handle_) {
                delete job;
                detail::CompletionGuard guard(notify);
                guard.failed("graph node returned an empty task");
                return nullptr;
            }
            auto h        = task.handle_;
            task.handle_  = nullptr;
            auto& p       = h.promise();
            p.notify_     = notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr};
            p.host_       = job->request.host;
            p.cancelFlag_ = job->cancelFlag;
            job->coroAddr = h.address();

            // 根的首步由 host driver 推进 (不在 start 里同步跑插件协程,
            // 因此没有 completion 重入, 也不占住宿主 IO 线程)。
            detail::startBridgedRoot(shim->ctx->bridge(), p.notify_, h, [job] {
                delete job;
            });
            return job;
        }
    };

    spec.run_cancel = [](void* user_data, void* op) {
        (void)user_data;
        if (!op) {
            return;
        }
        auto* job = static_cast<NodeJob*>(op);
        if (job->cancelFlag) {
            job->cancelFlag->store(true, std::memory_order_release);
        }
        if (job->coroAddr) {
            auto handle
                = std::coroutine_handle<detail::PromiseBase<void>>::from_address(job->coroAddr);
            handle.promise().cancel_outstanding();
        }
    };

    return ctx.iface.graph->register_node_type(ctx.host, &spec);
}
/* ==================== 工具权限声明 ====================
 *
 * 工具权限限制由工具来源方 (插件) 在注册工具后声明: 声明"哪些参数是受约束
 * 目标"以及读/写作用域; 具体判定 (白/黑名单、permission.mode 默认规则、
 * 用户记住的选择、工作区隔离、完全授权) 由宿主权限中间件统一执行, 插件
 * 不参与判定。
 *
 * 典型用法 (文件系统类工具):
 *     blocking_tool(ctx, "my_read", depict, schema, fn);
 *     registerReadPathPermission(ctx, "my_read", "path");
 *
 * 权限声明属于附加能力: 宿主未装配权限中间件时返回非 0, 插件可忽略 (工具
 * 照常可用, 只是不参与权限判定)。
 */

/// 工具权限作用域 (读/写各自一套规则, 互不影响)
enum class PermissionScope : int32_t {
    Read  = AGENTXX_PLUGIN_PERMISSION_SCOPE_READ,
    Write = AGENTXX_PLUGIN_PERMISSION_SCOPE_WRITE,
};

/// 工具权限目标来源
enum class PermissionTarget : int32_t {
    None = AGENTXX_PLUGIN_PERMISSION_TARGET_NONE,
    Path = AGENTXX_PLUGIN_PERMISSION_TARGET_PATH,
    Text = AGENTXX_PLUGIN_PERMISSION_TARGET_TEXT,
};

/// 工具权限声明 (字段含义见 C ABI 的 AgentxxPluginToolPermissionSpec)
struct ToolPermissionSpec {
    std::string_view toolName{};                     ///< 目标工具名 (须已注册)
    PermissionScope  scope{PermissionScope::Read};   ///< 权限作用域
    PermissionTarget target{PermissionTarget::None}; ///< 目标来源
    /// 目标参数名 (args 字段名); 目标值按实际 JSON 类型处理: 字符串为单目标,
    /// 数组 (如 `file_patterns`) 逐项判定
    std::string_view targetArg{};
    std::string_view category{}; ///< 权限分类文本 (空 = 按作用域生成)
};

/// 声明工具权限 (工具注册后调用); 返回 C ABI 状态码 (0 成功)
inline int32_t registerToolPermission(
    const PluginxxHost*                 host,
    const AgentxxPluginPermissionIface* iface,
    const ToolPermissionSpec&           spec
) {
    if (!host || !iface || !iface->register_tool_permission || spec.toolName.empty()) {
        return -1;
    }
    AgentxxPluginToolPermissionSpec abiSpec{};
    abiSpec.tool_name   = PluginStringView::from(spec.toolName);
    abiSpec.scope       = static_cast<int32_t>(spec.scope);
    abiSpec.target_kind = static_cast<int32_t>(spec.target);
    abiSpec.target_arg  = PluginStringView::from(spec.targetArg);
    abiSpec.struct_size = sizeof(AgentxxPluginToolPermissionSpec);
    abiSpec.category    = PluginStringView::from(spec.category);
    return iface->register_tool_permission(host, &abiSpec);
}

/// 声明工具权限 (从插件实例上下文取宿主与接口表)
template<typename Ctx>
inline int32_t registerToolPermission(const Ctx& ctx, const ToolPermissionSpec& spec) {
    return registerToolPermission(ctx.host, ctx.iface.permission, spec);
}

/// 便捷: 声明"路径参数 + 读作用域" (读取类工具, 目标参数值为路径)
/// - 参数值为字符串数组时自动逐项判定 (如 `file_patterns`), 无需额外设置
/// - 模式/前缀类参数 (glob 表达式) 建议配合 checkPathDecisions/filterPathPermissions
///   在工具内对展开出的实际路径逐项复核 (见"路径权限查询"一节)
template<typename Ctx>
inline int32_t registerReadPathPermission(
    const Ctx&       ctx,
    std::string_view toolName,
    std::string_view pathArg
) {
    ToolPermissionSpec spec;
    spec.toolName  = toolName;
    spec.scope     = PermissionScope::Read;
    spec.target    = PermissionTarget::Path;
    spec.targetArg = pathArg;
    return registerToolPermission(ctx, spec);
}

/// 便捷: 声明"路径参数 + 写作用域" (写入/编辑类工具, 目标参数值为路径)
template<typename Ctx>
inline int32_t registerWritePathPermission(
    const Ctx&       ctx,
    std::string_view toolName,
    std::string_view pathArg
) {
    ToolPermissionSpec spec;
    spec.toolName  = toolName;
    spec.scope     = PermissionScope::Write;
    spec.target    = PermissionTarget::Path;
    spec.targetArg = pathArg;
    return registerToolPermission(ctx, spec);
}

/// 撤销工具权限声明 (工具注销/插件禁用卸载时由宿主自动撤销, 一般无需手动调用)
template<typename Ctx>
inline int32_t unregisterToolPermission(const Ctx& ctx, std::string_view toolName) {
    if (!ctx.host || !ctx.iface.permission || !ctx.iface.permission->unregister_tool_permission
        || toolName.empty()) {
        return -1;
    }
    auto nameSv = PluginStringView::from(toolName);
    return ctx.iface.permission->unregister_tool_permission(ctx.host, &nameSv);
}

/* ---- 路径权限查询 (供模式/前缀参数工具逐项过滤) ----
 *
 * 用途: 声明式权限目标只能描述"扫描起点"(决定是否询问用户一次); 对 glob/grep
 * 这类模式参数, 模式展开后可能触及被拒绝的子目录, 因此工具在枚举出实际路径后
 * 还要逐项查询一次, 只处理已明确允许的路径。
 *
 * 判定三态 (见 PathDecision): 与工具调用权限检查同一口径, 只是"需要询问"的场合
 * 返回 Ask 而**不发起任何询问/中断** (本查询纯只读)。
 * 工具侧约定: Deny 丢弃; Ask 表示"未获批准", 同样不应访问 (fail-closed)。
 */

/// 路径权限判定结果 (与 C ABI AGENTXX_PLUGIN_PERMISSION_DECISION_* 对应)
enum class PathDecision : int32_t {
    Deny  = AGENTXX_PLUGIN_PERMISSION_DECISION_DENY,  ///< 已明确拒绝
    Allow = AGENTXX_PLUGIN_PERMISSION_DECISION_ALLOW, ///< 已明确允许
    Ask   = AGENTXX_PLUGIN_PERMISSION_DECISION_ASK,   ///< 未获批准 (本查询不询问)
};

/// 批量查询路径权限 (三态; 不发起询问/不产生中断)
/// - 入参 [paths] 建议为绝对路径 (相对路径按会话工作目录解析)
/// - `return` 与 [paths] 等长的判定数组; 宿主不支持或调用失败时返回**空数组**
///   (调用方应跳过过滤按原行为处理, 而不是把路径当成拒绝)
inline std::vector<PathDecision> checkPathDecisions(
    const PluginxxHost*                 host,
    const AgentxxPluginPermissionIface* iface,
    PermissionScope                     scope,
    std::string_view                    sessionId,
    const std::vector<std::string>&     paths
) {
    // 单次批量上限与宿主一致 (见 plugin_manager_vtable.cpp); 超出由调用方分批
    constexpr size_t kMaxBatch = 16384;
    if (!host || !iface || !iface->check_paths || paths.empty() || paths.size() > kMaxBatch) {
        return {};
    }
    std::vector<PluginxxStringView> views;
    views.reserve(paths.size());
    for (const auto& path : paths) {
        views.push_back(PluginStringView::from(path));
    }
    AgentxxPluginPermissionPathQuery query{};
    query.struct_size = sizeof(AgentxxPluginPermissionPathQuery);
    query.scope       = static_cast<int32_t>(scope);
    query.path_count  = static_cast<int32_t>(views.size());
    query.session_id  = PluginStringView::from(sessionId);
    query.paths       = views.data();

    std::vector<int32_t> decisions(views.size(), AGENTXX_PLUGIN_PERMISSION_DECISION_ASK);
    if (iface->check_paths(host, &query, decisions.data()) != 0) {
        return {};
    }
    std::vector<PathDecision> out;
    out.reserve(decisions.size());
    for (auto decision : decisions) {
        out.push_back(static_cast<PathDecision>(decision));
    }
    return out;
}

/// 同上 (从插件实例上下文取宿主与接口表)
template<typename Ctx>
inline std::vector<PathDecision> checkPathDecisions(
    const Ctx&                      ctx,
    PermissionScope                 scope,
    std::string_view                sessionId,
    const std::vector<std::string>& paths
) {
    return checkPathDecisions(ctx.host, ctx.iface.permission, scope, sessionId, paths);
}

/// 单路径权限查询 (便捷; 内部走批量接口)
/// - 查询不可用或失败时返回 [PathDecision::Ask] (调用方按未获批准处理)
template<typename Ctx>
inline PathDecision checkPathDecision(
    const Ctx&       ctx,
    std::string_view path,
    PermissionScope  scope,
    std::string_view sessionId = {}
) {
    std::vector<std::string> paths{std::string{path}};
    auto                     decisions = checkPathDecisions(ctx, scope, sessionId, paths);
    if (decisions.size() != 1) {
        return PathDecision::Ask;
    }
    return decisions.front();
}

/// 批量过滤路径 (工具内使用): 返回与 [paths] 等长的标记数组 (1 = 已明确允许)
/// - Deny 与 Ask 都标记为不可访问 (fail-closed: 未获批准的范围不进入结果)
/// - 宿主不支持查询时返回**空数组**, 调用方应跳过过滤 (保持原行为)
/// - 内部按 [batchSize] 分批调用宿主 (默认 512 项, 避免长占宿主 io 线程),
///   并对相同路径去重后查询
inline std::vector<uint8_t> filterPathPermissions(
    const PluginxxHost*                 host,
    const AgentxxPluginPermissionIface* iface,
    PermissionScope                     scope,
    std::string_view                    sessionId,
    const std::vector<std::string>&     paths,
    size_t                              batchSize = 512
) {
    if (!host || !iface || !iface->check_paths || paths.empty()) {
        return {};
    }
    if (batchSize == 0) {
        batchSize = 512;
    }
    std::vector<uint8_t>                     allowed(paths.size(), 0);
    std::unordered_map<std::string, uint8_t> queried;
    std::vector<std::string>                 pending;
    std::vector<size_t>                      pendingIndex;

    auto flush = [&]() -> bool {
        if (pending.empty()) {
            return true;
        }
        auto decisions = checkPathDecisions(host, iface, scope, sessionId, pending);
        if (decisions.size() != pending.size()) {
            return false; // 查询失败: 交由调用方跳过过滤
        }
        for (size_t i = 0; i < decisions.size(); ++i) {
            const uint8_t ok         = decisions[i] == PathDecision::Allow ? 1 : 0;
            allowed[pendingIndex[i]] = ok;
            queried.insert_or_assign(pending[i], ok);
        }
        pending.clear();
        pendingIndex.clear();
        return true;
    };

    for (size_t i = 0; i < paths.size(); ++i) {
        auto it = queried.find(paths[i]);
        if (it != queried.end()) {
            allowed[i] = it->second;
            continue;
        }
        pending.push_back(paths[i]);
        pendingIndex.push_back(i);
        if (pending.size() >= batchSize && !flush()) {
            return {};
        }
    }
    if (!flush()) {
        return {};
    }
    return allowed;
}

/// 同上 (从插件实例上下文取宿主与接口表)
template<typename Ctx>
inline std::vector<uint8_t> filterPathPermissions(
    const Ctx&                      ctx,
    PermissionScope                 scope,
    std::string_view                sessionId,
    const std::vector<std::string>& paths,
    size_t                          batchSize = 512
) {
    return filterPathPermissions(
        ctx.host,
        ctx.iface.permission,
        scope,
        sessionId,
        paths,
        batchSize
    );
}

/* ==================== 阻塞便捷函数 (基于 condvar) ==================== */

inline PluginxxString call_tool_blocking(
    const PluginxxHost*            host,
    const AgentxxPluginToolsIface* tools,
    const PluginxxSchedulerIface*  sched,
    std::string_view               name,
    std::string_view               args_json,
    std::string_view               thread_id,
    PluginxxString*                error_out
) {
    if (!host || !tools || !tools->call_tool_async) {
        if (error_out) {
            *error_out = PluginString::fromCstr(host, "tools iface not available");
        }
        return PluginxxString{nullptr, 0};
    }
    if (sched && sched->is_io_thread && sched->is_io_thread(host)) {
        if (error_out) {
            *error_out = PluginString::fromCstr(
                host,
                "call_tool_blocking cannot be called on io thread; use co_await call_tool instead"
            );
        }
        return PluginxxString{nullptr, 0};
    }

    struct SyncState {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done   = false;
        int32_t                 status = PLUGINXX_OPERATOR_OK;
        std::string             payload;
    } state;

    auto nameSv = PluginStringView::from(name.data(), name.size());
    auto argsSv = PluginStringView::from(args_json.data(), args_json.size());
    auto tidSv  = PluginStringView::from(thread_id.data(), thread_id.size());

    PluginxxOperatorHandle* handle = tools->call_tool_async(
        host,
        &nameSv,
        &argsSv,
        &tidSv,
        [](void* ud, int32_t st, const PluginxxStringView* pl) {
            auto*           s = static_cast<SyncState*>(ud);
            std::lock_guard lk(s->mtx);
            s->done   = true;
            s->status = st;
            if (pl && pl->data && pl->size > 0) {
                s->payload.assign(pl->data, static_cast<size_t>(pl->size));
            }
            s->cv.notify_one();
        },
        &state,
        error_out
    );

    if (!handle) {
        return PluginxxString{nullptr, 0};
    }

    {
        std::unique_lock lk(state.mtx);
        state.cv.wait(lk, [&]() {
            return state.done;
        });
    }

    if (state.status != PLUGINXX_OPERATOR_OK) {
        if (error_out) {
            auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
            *error_out = PluginString::from(host, &paySv);
        }
        return PluginxxString{nullptr, 0};
    }

    auto paySv = PluginStringView::from(state.payload.data(), state.payload.size());
    return PluginString::from(host, &paySv);
}

/* ==================== Client 侧工具特化渲染适配器 ==================== */

struct ToolRenderInput {
    std::string_view toolCallId;
    std::string_view toolName;
    std::string_view argsJson;
    std::string_view resultText;
    bool             isFinished = false;
    bool             isError    = false;
    int              maxWidth   = 0;
};

struct ToolRenderOutput {
    std::string       displayName;
    std::string       summary;
    utilxx_base::Json items = utilxx_base::Json::array();
};

/// 注册基于回调函数的工具特化渲染器 (<key, 渲染func>)
template<typename Fn>
inline int32_t registerToolRenderer(
    const PluginxxHost*                                  host,
    const AgentxxClientUiIface*                          ui,
    std::string_view                                     toolName,
    Fn&&                                                 fn,
    std::vector<std::unique_ptr<void, void (*)(void*)>>& shimStorage
) {
    if (!host || !ui || !ui->register_tool_renderer) {
        return -1;
    }
    using DecayedFn = std::decay_t<Fn>;

    struct RenderShim {
        const PluginxxHost* host = nullptr;
        DecayedFn           fn;
    };

    auto* shim = new RenderShim{host, std::forward<Fn>(fn)};
    shimStorage.emplace_back(shim, [](void* p) {
        delete static_cast<RenderShim*>(p);
    });

    auto renderCb
        = [](void* user_data, const AgentxxToolRenderInput* input, AgentxxToolRenderOutput* output
          ) -> int32_t {
        if (!user_data || !input || !output) {
            return -1;
        }
        auto*           shim = static_cast<RenderShim*>(user_data);
        ToolRenderInput in{
            .toolCallId = PluginStringView::str(input->tool_call_id),
            .toolName   = PluginStringView::str(input->tool_name),
            .argsJson   = PluginStringView::str(input->args_json),
            .resultText = PluginStringView::str(input->result_text),
            .isFinished = input->is_finished != 0,
            .isError    = input->is_error != 0,
            .maxWidth   = input->max_width,
        };
        ToolRenderOutput out;
        try {
            shim->fn(in, out);
        } catch (...) {
            return -1;
        }
        if (!out.displayName.empty()) {
            output->displayName = PluginString::from(shim->host, out.displayName);
        }
        if (!out.summary.empty()) {
            output->summary = PluginString::from(shim->host, out.summary);
        }
        if (!out.items.empty()) {
            output->items_json = PluginString::from(shim->host, out.items.dump());
        }
        return 0;
    };

    AgentxxToolRenderSpec spec{};
    spec.version       = 1;
    spec.tool_name     = PluginStringView::from(toolName.data(), toolName.size());
    spec.render_fn     = renderCb;
    spec.user_data     = shim;
    spec.template_json = PluginStringView::from(nullptr, 0);

    return ui->register_tool_renderer(host, &spec);
}

/// 注册基于预设模版的工具特化渲染器
inline int32_t registerToolTemplate(
    const PluginxxHost*         host,
    const AgentxxClientUiIface* ui,
    std::string_view            toolName,
    std::string_view            displayName,
    std::string_view            summaryKey
) {
    if (!host || !ui || !ui->register_tool_renderer) {
        return -1;
    }
    utilxx_base::Json j;
    j["displayName"]    = std::string(displayName);
    j["summaryKey"]     = std::string(summaryKey);
    std::string jsonStr = j.dump();

    AgentxxToolRenderSpec spec{};
    spec.version       = 1;
    spec.tool_name     = PluginStringView::from(toolName.data(), toolName.size());
    spec.render_fn     = nullptr;
    spec.user_data     = nullptr;
    spec.template_json = PluginStringView::from(jsonStr.data(), jsonStr.size());

    return ui->register_tool_renderer(host, &spec);
}

/* ==================== Client 侧通用交互: ActionController (header-only) ====================
 *
 * 插件侧 Lambda 风格的动作绑定设施 (实例内存 map<action_id, handler>):
 * - JSON 只传 action_id 字符串 (函数指针不可序列化, 见三铁律), 映射留在插件侧内存
 * - 只在 IO 线程 on/dispatch (与事件 handler 同约定), 无需锁
 * - dispatch 为 C 回调 (填入 bind_action_handler), 空指针守卫 + 参数解析失败给 {}
 *   + handler 异常吞掉记日志, 不外泄 C 边界
 */

namespace kit {

class ActionController {
public:

    using Handler = std::function<void(const utilxx_base::Json& args)>;

    /// 注册动作处理器 (IO 线程; 同 actionId 覆盖)
    void on(std::string actionId, Handler h) {
        handlers_[std::move(actionId)] = std::move(h);
    }

    /// 注销动作处理器 (不存在忽略)
    void off(const std::string& actionId) {
        handlers_.erase(actionId);
    }

    /// 生成 button JSON (action_id 自增 act_N; args 缺省 {}; role 缺省 normal)
    /// - onClick 为空时仍生成可点按钮 (固定 id 由调用方另行 on() 绑定, 如 planning 常量)
    utilxx_base::Json makeButton(
        std::string       label,
        Handler           onClick = nullptr,
        std::string       prefix  = "",
        std::string       role    = "normal",
        utilxx_base::Json args    = utilxx_base::Json::object()
    ) {
        const std::string id = "act_" + std::to_string(++counter_);
        if (onClick) {
            handlers_[id] = std::move(onClick);
        }
        utilxx_base::Json btn = utilxx_base::Json::object();
        btn["kind"]           = "button";
        btn["label"]          = std::move(label);
        if (!prefix.empty()) {
            btn["prefix"] = std::move(prefix);
        }
        btn["action_id"] = id;
        btn["args"]      = std::move(args);
        btn["role"]      = std::move(role);
        return btn;
    }

    /// C 回调 (填入 bind_action_handler 的 on_action; ud = 本实例指针)
    static void PLUGINXX_CALL dispatch(const AgentxxUiActionContext* ctx, void* ud) {
        auto* self = static_cast<ActionController*>(ud);
        if (!self || !ctx) {
            return;
        }
        try {
            const std::string actionId(
                ctx->action_id.data ? ctx->action_id.data : "",
                static_cast<size_t>(ctx->action_id.size)
            );
            auto it = self->handlers_.find(actionId);
            if (it == self->handlers_.end() || !it->second) {
                return;
            }
            utilxx_base::Json args = utilxx_base::Json::object();
            if (ctx->action_args.data && ctx->action_args.size > 0) {
                try {
                    auto parsed = utilxx_base::Json::parse(std::string_view{
                        ctx->action_args.data,
                        static_cast<size_t>(ctx->action_args.size)
                    });
                    if (parsed.is_object()) {
                        args = std::move(parsed);
                    }
                } catch (...) {
                    // 解析失败给 {} (约定)
                }
            }
            it->second(args);
        } catch (...) {
            // handler 异常不外泄 C 边界 (宿主另有兜底, 此处静默吞掉)
        }
    }

    bool empty() const noexcept {
        return handlers_.empty();
    }

    size_t size() const noexcept {
        return handlers_.size();
    }

private:

    std::unordered_map<std::string, Handler> handlers_;
    uint64_t                                 counter_ = 0;
};

} // namespace kit

/* ==================== 客户端插件实例上下文基类 ==================== */

class ClientPluginBase {
public:

    const PluginxxHost*   host = nullptr;
    ClientIfaces          iface{};
    Logger                log;
    kit::ActionController actions;

    ClientPluginBase() = default;

    virtual ~ClientPluginBase() {
        if (lifeToken_) {
            lifeToken_->store(false, std::memory_order_release);
        }
    }

    std::shared_ptr<std::atomic<bool>> lifeToken() const {
        return lifeToken_;
    }

    void init(const PluginxxHost* h) {
        host      = h;
        iface     = ClientIfaces::query(h);
        log.host  = h;
        log.logFn = (iface.log && iface.log->log) ? iface.log->log : nullptr;
    }

    std::string clientState() const {
        if (!host || !iface.session || !iface.session->get_client_state) {
            return "{}";
        }
        PluginxxString s{nullptr, 0};
        iface.session->get_client_state(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string argsJson() const {
        if (!host || !iface.self || !iface.self->get_plugin_args) {
            return "{}";
        }
        PluginxxString s{nullptr, 0};
        iface.self->get_plugin_args(host, &s);
        if (!s.data) {
            return "{}";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string configPath() const {
        if (!host || !iface.self || !iface.self->get_plugin_config_path) {
            return "";
        }
        PluginxxString s{nullptr, 0};
        iface.self->get_plugin_config_path(host, &s);
        if (!s.data) {
            return "";
        }
        std::string res(s.data, static_cast<size_t>(s.size));
        PluginString::free(host, &s);
        return res;
    }

    std::string language() const {
        if (!host || !iface.self || !iface.self->get_language) {
            return "en";
        }
        PluginxxString s{nullptr, 0};
        if (iface.self->get_language(host, &s) == 0 && s.data) {
            std::string res(s.data, static_cast<size_t>(s.size));
            PluginString::free(host, &s);
            return res;
        }
        return "en";
    }

    bool setLanguage(std::string_view lang) const {
        if (!host || !iface.self || !iface.self->set_language) {
            return false;
        }
        auto sv = PluginStringView::from(lang.data(), lang.size());
        return iface.self->set_language(host, &sv) == 0;
    }

    void showToast(std::string_view text, int32_t level = 0) const {
        if (!host || !iface.ui || !iface.ui->show_toast) {
            return;
        }
        auto sv = PluginStringView::from(text.data(), text.size());
        iface.ui->show_toast(host, &sv, level);
    }

    int32_t sendUserInput(std::string_view sessionId, std::string_view text) const {
        if (!host || !iface.session || !iface.session->send_user_input) {
            return -1;
        }
        auto sidSv = PluginStringView::from(sessionId.data(), sessionId.size());
        auto txtSv = PluginStringView::from(text.data(), text.size());
        return iface.session->send_user_input(host, &sidSv, &txtSv);
    }

    void requestCancel(std::string_view sessionId) const {
        if (!host || !iface.session || !iface.session->request_cancel) {
            return;
        }
        auto sidSv = PluginStringView::from(sessionId.data(), sessionId.size());
        iface.session->request_cancel(host, &sidSv);
    }

    void registerTemplate(std::string_view tool, std::string_view display, std::string_view key) {
        registerToolTemplate(host, iface.ui, tool, display, key);
    }

    // ==================== 展示扩展便捷方法 ====================
    //
    // 这些方法是展示注册接口的薄封装: 把 `agentxx::ui::Items` 构建器 (见
    // [build.h](/agent/lib/include/agentxx/ui/build.h)) 直接提交给面板/Info 段落/
    // overlay, 避免各处手写 JSON 拼装。宿主不支持对应子能力时返回非 0, 插件应
    // 按返回值降级 (通常是改用更简单的组件或纯文本)。

    /// 更新面板内容 (组件树直接提交)
    int32_t setPanelItems(AgentxxPanel* panel, const agentxx::ui::Items& ui) const {
        if (!host || !iface.ui || !iface.ui->update_panel || panel == nullptr) {
            return -1;
        }
        const std::string json = ui.dump();
        auto              sv   = PluginStringView::from(json.data(), json.size());
        return iface.ui->update_panel(host, panel, &sv);
    }

    /// 更新面板内容 (原始 items JSON, 形态同 `{"items":[...]}` 或裸数组)
    int32_t setPanelJson(AgentxxPanel* panel, std::string_view itemsJson) const {
        if (!host || !iface.ui || !iface.ui->update_panel || panel == nullptr) {
            return -1;
        }
        auto sv = PluginStringView::from(itemsJson.data(), itemsJson.size());
        return iface.ui->update_panel(host, panel, &sv);
    }

    /// 更新 Info 栏段落内容 (组件树直接提交)
    int32_t setInfoSectionItems(AgentxxInfoSection* section, const agentxx::ui::Items& ui) const {
        if (!host || !iface.ui || !iface.ui->update_info_section || section == nullptr) {
            return -1;
        }
        const std::string json = ui.dump();
        auto              sv   = PluginStringView::from(json.data(), json.size());
        return iface.ui->update_info_section(host, section, &sv);
    }

    /// 更新状态栏项文本 (参数为 `{"text":"...","tooltip":"..."}` 形态的 JSON)
    int32_t setStatusJson(AgentxxStatusItem* item, std::string_view json) const {
        if (!host || !iface.ui || !iface.ui->update_status_item || item == nullptr) {
            return -1;
        }
        auto sv = PluginStringView::from(json.data(), json.size());
        return iface.ui->update_status_item(host, item, &sv);
    }

    /// 更新状态栏项文本 (仅文本, 由本方法组装 JSON)
    int32_t setStatusText(AgentxxStatusItem* item, std::string_view text) const {
        utilxx_base::Json json = utilxx_base::Json::object();
        json["text"]           = text;
        return setStatusJson(item, json.dump());
    }

    /// 打开自定义 overlay (组件树作为内容)
    /// - `extraJson` 为扩展 JSON (如 `{"size":"large"}`), 可空
    int32_t showItemsOverlay(
        std::string_view        title,
        const agentxx::ui::Items& ui,
        std::string_view        extraJson = {}
    ) const {
        return showOverlay(AGENTXX_OVERLAY_CUSTOM, title, ui.dump(), extraJson);
    }

    /// 打开通用 overlay (type 见 AgentxxOverlayType; payload 语义随类型)
    int32_t showOverlay(
        int              type,
        std::string_view title,
        std::string_view payload,
        std::string_view extraJson = {}
    ) const {
        if (!host || !iface.ui || !iface.ui->open_overlay) {
            return -1;
        }
        AgentxxOverlaySpec spec{};
        spec.version    = 1;
        spec.type       = type;
        spec.title      = PluginStringView::from(title.data(), title.size());
        spec.payload    = PluginStringView::from(payload.data(), payload.size());
        spec.extra_json = PluginStringView::from(extraJson.data(), extraJson.size());
        return iface.ui->open_overlay(host, &spec);
    }

    // ==================== 能力协商查询 ====================

    /// 宿主是否声明了某个能力名 (查 `get_client_state().interfaces`)
    /// - 能力名常量见 `agentxx/plugin/plugin_interfaces.h` 的 plugin_interfaces 命名空间
    ///   (如 "agentxx.client.components" / "agentxx.client.form")
    /// - 老宿主未提供该能力时返回 false, 插件据此降级 (不报错、不静默丢内容)
    bool hostSupports(std::string_view capability) const {
        if (capability.empty()) {
            return false;
        }
        const auto state = clientStateJson();
        const auto it    = state.find("interfaces");
        if (it == state.end() || !it->is_array()) {
            return false;
        }
        for (const auto& entry : *it) {
            if (entry.is_string() && entry.get_string_view() == capability) {
                return true;
            }
        }
        return false;
    }

    /// 当前 client 状态解析结果 (解析失败返回空对象)
    utilxx_base::Json clientStateJson() const {
        const std::string text = clientState();
        if (text.empty() || text == "{}") {
            return utilxx_base::Json::object();
        }
        try {
            return utilxx_base::Json::parse(text);
        } catch (...) {
            return utilxx_base::Json::object();
        }
    }

    template<typename Fn>
    void registerRenderer(std::string_view tool, Fn&& fn) {
        registerToolRenderer(host, iface.ui, tool, std::forward<Fn>(fn), shims_);
    }

    template<typename T>
    T* storeShim(std::unique_ptr<T> shim) {
        T* raw = shim.get();
        shims_.emplace_back(shim.release(), [](void* ptr) {
            delete static_cast<T*>(ptr);
        });
        return raw;
    }

private:

    std::shared_ptr<std::atomic<bool>> lifeToken_ = std::make_shared<std::atomic<bool>>(true);
    std::vector<std::unique_ptr<void, void (*)(void*)>> shims_;
};

/// client 侧 create 阶段异常上报 (client 日志接口表为独立类型)
inline void logClientCreateFailure(
    const PluginxxHost* host,
    std::string_view    plugin,
    std::string_view    msg
) noexcept {
    if (!host || !host->vtable || !host->vtable->query_interface) {
        return;
    }
    auto  iid = PluginStringView::fromCstr(AGENTXX_IFACE_CLIENT_LOG);
    auto* iface
        = static_cast<const AgentxxClientLogIface*>(host->vtable->query_interface(host, &iid));
    if (!iface || !iface->log) {
        return;
    }
    std::string text = fmt::format("[{}] create failed: {}", plugin, msg);
    auto        sv   = PluginStringView::from(text.data(), text.size());
    iface->log(host, 4, &sv);
}

/// Agent 侧插件入口导出 (生成 `agentxx_plugin_agent_{get_info,create,start,stop,destroy}`)。
///
/// 内核只提供"带符号前缀"的通用导出宏 (见 `pluginxx/kit/kit.h` 的
/// [PLUGINXX_EXPORT_PLUGIN]); 入口符号名属于本宿主命名空间, 因此在这里包一层。
/// 运行时由 PluginManager::entrySymbols() 交出同一批符号名。
#define AGENTXX_PLUGIN_AGENT_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn) \
    PLUGINXX_EXPORT_PLUGIN(agentxx_plugin_agent_, CtxType, Name, Ver, Desc, StartFn, StopFn)

/// Agent 侧只导出 start/stop 两个入口 (供手写 create/destroy 的插件使用)。
///
/// 与 [AGENTXX_PLUGIN_AGENT_EXPORT] 的生命周期部分同语义, 但由插件自己实现
/// `agentxx_plugin_agent_create` / `agentxx_plugin_agent_destroy`。
#define AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn) \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(agentxx_plugin_agent_, CtxType, StartFn, StopFn)

/// Client 侧只导出 start/stop 两个入口 (与
/// [AGENTXX_PLUGIN_AGENT_LIFECYCLE_EXPORT] 对称)。
#define AGENTXX_PLUGIN_CLIENT_LIFECYCLE_EXPORT(CtxType, StartFn, StopFn) \
    extern "C" PLUGINXX_EXPORT void* agentxx_plugin_client_start(        \
        void*                         plugin_ctx,                        \
        const PluginxxOperatorNotify* notify,                            \
        PluginxxString*               err                                \
    ) {                                                                  \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                   \
        return agentxx::plugin::detail::callLifecycleEntry(              \
            ctx ? ctx->host : nullptr,                                   \
            plugin_ctx,                                                  \
            notify,                                                      \
            err,                                                         \
            "client plugin start",                                       \
            [&]() -> void* {                                             \
                return (StartFn)(*ctx, notify, err);                     \
            }                                                            \
        );                                                               \
    }                                                                    \
    extern "C" PLUGINXX_EXPORT void* agentxx_plugin_client_stop(         \
        void*                         plugin_ctx,                        \
        const PluginxxOperatorNotify* notify,                            \
        PluginxxString*               err                                \
    ) {                                                                  \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                   \
        return agentxx::plugin::detail::callLifecycleEntry(              \
            ctx ? ctx->host : nullptr,                                   \
            plugin_ctx,                                                  \
            notify,                                                      \
            err,                                                         \
            "client plugin stop",                                        \
            [&]() -> void* {                                             \
                return (StopFn)(*ctx, notify, err);                      \
            }                                                            \
        );                                                               \
    }

/// Client 侧插件入口导出 (与 [AGENTXX_PLUGIN_AGENT_EXPORT] 对称)。
///
/// `StartFn` / `StopFn` 形如
/// `void*(CtxType&, const PluginxxOperatorNotify*, PluginxxString* error_out)`;
/// start 里做 UI 项/命令/订阅注册, stop 里撤销插件自管资源 (线程/定时器/订阅)。
/// 纯 UI 插件 (无 agent 侧入口) 只导出 get_info + 下列四个入口即可。
#define AGENTXX_PLUGIN_CLIENT_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn)                   \
    extern "C" PLUGINXX_EXPORT const AgentxxClientPluginInfo* agentxx_plugin_client_get_info(void \
    ) {                                                                                           \
        static const AgentxxClientPluginInfo info{                                                \
            AGENTXX_CLIENT_PLUGIN_API_VERSION,                                                    \
            0,                                                                                    \
            agentxx::plugin::PluginStringView::fromCstr(Name),                                    \
            agentxx::plugin::PluginStringView::fromCstr(Ver),                                     \
            agentxx::plugin::PluginStringView::fromCstr(Desc),                                    \
        };                                                                                        \
        return &info;                                                                             \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT int32_t                                                            \
        agentxx_plugin_client_create(const PluginxxHost* host, void** plugin_ctx) {               \
        if (!host || !plugin_ctx) {                                                               \
            return -1;                                                                            \
        }                                                                                         \
        try {                                                                                     \
            auto ctx = std::make_unique<CtxType>();                                               \
            ctx->init(host);                                                                      \
            *plugin_ctx = ctx.release();                                                          \
            return 0;                                                                             \
        } catch (const std::exception& e) {                                                       \
            agentxx::plugin::logClientCreateFailure(host, Name, e.what());                        \
        } catch (...) {                                                                           \
            agentxx::plugin::logClientCreateFailure(host, Name, "unknown exception");             \
        }                                                                                         \
        return -1;                                                                                \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void* agentxx_plugin_client_start(                                 \
        void*                         plugin_ctx,                                                 \
        const PluginxxOperatorNotify* notify,                                                     \
        PluginxxString*               err                                                         \
    ) {                                                                                           \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        return agentxx::plugin::detail::callLifecycleEntry(                                       \
            ctx ? ctx->host : nullptr,                                                            \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            "client plugin start",                                                                \
            [&]() -> void* {                                                                      \
                return (StartFn)(*ctx, notify, err);                                              \
            }                                                                                     \
        );                                                                                        \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void* agentxx_plugin_client_stop(                                  \
        void*                         plugin_ctx,                                                 \
        const PluginxxOperatorNotify* notify,                                                     \
        PluginxxString*               err                                                         \
    ) {                                                                                           \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        return agentxx::plugin::detail::callLifecycleEntry(                                       \
            ctx ? ctx->host : nullptr,                                                            \
            plugin_ctx,                                                                           \
            notify,                                                                               \
            err,                                                                                  \
            "client plugin stop",                                                                 \
            [&]() -> void* {                                                                      \
                return (StopFn)(*ctx, notify, err);                                               \
            }                                                                                     \
        );                                                                                        \
    }                                                                                             \
    extern "C" PLUGINXX_EXPORT void agentxx_plugin_client_destroy(void* plugin_ctx) {             \
        auto* ctx = static_cast<CtxType*>(plugin_ctx);                                            \
        if (ctx) {                                                                                \
            delete ctx;                                                                           \
        }                                                                                         \
    }
} // namespace plugin
} // namespace agentxx
