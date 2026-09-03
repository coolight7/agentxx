#include "agentxx/plugin/plugin_graph_node.h"

#include "agentxx/plugin/op_driver.h"
#include "agentxx/plugin/plugin_manager.h"
#include "agentxx/util/log.h"
#include "fmt/format.h"
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>

namespace agentxx {
namespace plugin {

PluginGraphNode::PluginGraphNode(
    std::string_view                name,
    std::string_view                configJson,
    std::shared_ptr<PluginInstance> instance,
    AgentxxPluginGraphNodeTypeSpec  spec
) :
    name_(name),
    configJson_(configJson),
    type_(spec.type.data ? std::string{spec.type.data, spec.type.size} : std::string{}),
    instance_(std::move(instance)),
    spec_(spec) {}

PluginGraphNode::~PluginGraphNode() = default;

std::string PluginGraphNode::get_name() const {
    return name_;
}

asio::awaitable<neograph::graph::NodeOutput> PluginGraphNode::run(neograph::graph::NodeInput in) {
    auto inst = instance_;
    if (!inst) {
        throw std::runtime_error(fmt::format("graph node `{}`: plugin instance released", name_));
    }
    if (!inst->enabled) {
        throw std::runtime_error(fmt::format("graph node `{}`: plugin disabled", name_));
    }
    if (!spec_.run_start) {
        throw std::runtime_error(fmt::format("graph node `{}`: null run_start callback", name_));
    }

    // 序列化当前 GraphState (插件只读; 修改须经返回的 writes)
    const std::string stateJson  = in.state.serialize().dump();
    const std::string configJson = configJson_;

    auto       ex         = co_await asio::this_coro::executor;
    auto       spec       = spec_;
    auto       instKeep   = inst;
    const auto nodeName   = name_;
    const auto threadId   = in.ctx.thread_id;

    plugin::OpDrive drive;
    drive.start = [spec, instKeep, nodeName, configJson, stateJson, threadId](
                      const AgentxxPluginOperatorNotify* notify,
                      AgentxxPluginString*               err
                  ) -> void* {
        return spec.run_start(
            spec.user_data,
            agentxx_plugin_sv(nodeName.data(), nodeName.size()),
            agentxx_plugin_sv(configJson.data(), configJson.size()),
            agentxx_plugin_sv(stateJson.data(), stateJson.size()),
            agentxx_plugin_sv(threadId.data(), threadId.size()),
            notify,
            err
        );
    };
    drive.cancel = [spec, instKeep](void* op) {
        if (spec.run_cancel) {
            spec.run_cancel(spec.user_data, op);
        }
    };
    auto awaitArgs = plugin::PluginOpAwaitArgs{
        .inst        = std::move(inst),
        .label       = fmt::format("graph node `{}`", name_),
        .ex          = ex,
        .cancelToken = in.ctx.cancel_token,
        .drive       = std::move(drive),
    };
    std::string payload = co_await plugin::awaitPluginOp(std::move(awaitArgs));

    // 解析插件返回的节点输出 JSON:
    // {"writes": [{"channel","value","mode"}], "command": {...}|null, "sends": [...]}
    neograph::graph::NodeOutput out;
    try {
        auto j = neograph::json::parse(payload);
        if (!j.is_object()) {
            throw std::runtime_error("node output is not a JSON object");
        }
        if (j.contains("writes") && j["writes"].is_array()) {
            for (const auto& w : j["writes"]) {
                if (!w.is_object() || !w.contains("channel") || !w["channel"].is_string()
                    || !w.contains("value")) {
                    continue;
                }
                neograph::graph::ChannelWrite cw;
                cw.channel = w["channel"].get<std::string>();
                cw.value   = w["value"];
                if (w.contains("mode") && w["mode"].is_string()
                    && w["mode"].get<std::string>() == "overwrite") {
                    cw.mode = neograph::graph::ChannelWrite::Mode::Overwrite;
                }
                out.writes.push_back(std::move(cw));
            }
        }
        if (j.contains("command") && j["command"].is_object() && !j["command"].is_null()) {
            const auto& cmd = j["command"];
            if (cmd.contains("goto_node") && cmd["goto_node"].is_string()) {
                neograph::graph::Command command;
                command.goto_node = cmd["goto_node"].get<std::string>();
                if (cmd.contains("updates") && cmd["updates"].is_array()) {
                    for (const auto& u : cmd["updates"]) {
                        if (!u.is_object() || !u.contains("channel") || !u["channel"].is_string()
                            || !u.contains("value")) {
                            continue;
                        }
                        neograph::graph::ChannelWrite cw;
                        cw.channel = u["channel"].get<std::string>();
                        cw.value   = u["value"];
                        if (u.contains("mode") && u["mode"].is_string()
                            && u["mode"].get<std::string>() == "overwrite") {
                            cw.mode = neograph::graph::ChannelWrite::Mode::Overwrite;
                        }
                        command.updates.push_back(std::move(cw));
                    }
                }
                out.command = std::move(command);
            }
        }
        if (j.contains("sends") && j["sends"].is_array()) {
            for (const auto& s : j["sends"]) {
                if (!s.is_object() || !s.contains("target_node") || !s["target_node"].is_string()) {
                    continue;
                }
                neograph::graph::Send send;
                send.target_node = s["target_node"].get<std::string>();
                if (s.contains("input") && s["input"].is_object()) {
                    send.input = s["input"];
                }
                out.sends.push_back(std::move(send));
            }
        }
    } catch (const std::exception& e) {
        XX_LOGE(
            "Graph node `{}` (type {}) invalid output JSON: {}; payload: {}",
            name_,
            type_,
            e.what(),
            payload.size() > 512 ? payload.substr(0, 512) + "..." : payload
        );
        throw;
    }

    co_return out;
}

} // namespace plugin
} // namespace agentxx
