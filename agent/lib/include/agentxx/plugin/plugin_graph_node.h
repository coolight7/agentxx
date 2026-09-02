#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "neograph/graph/node.h"
#include <memory>
#include <string>

namespace agentxx {
namespace plugin {

class PluginInstance;

/// 插件自定义节点 (宿主侧 GraphNode 子类, 委托插件 C 回调执行)
///
/// 设计: 遵循插件系统"统一异步操作模型" (两件套 start/cancel + 锚定协程):
/// - 引擎调用 run(NodeInput) → 序列化 GraphState → 调插件 run_start 回调
/// - 插件完成时经 notify->done 上报节点输出 JSON (writes/command/sends)
/// - 宿主解析 JSON 构造 NodeOutput; 取消经 run_cancel 联动
///
/// 节点规范 (neograph 约定): 节点实例被引擎共享于并发 run, 必须无状态或自
/// 同步 —— 本类只保存构造期快照 (name/config/type/spec), 每次 run 从 state
/// 派生态, 符合 stateless 约束。
class PluginGraphNode : public neograph::graph::GraphNode {
public:

    PluginGraphNode(
        std::string_view                           name,
        std::string_view                           configJson,
        std::shared_ptr<PluginInstance>            instance,
        AgentxxPluginGraphNodeTypeSpec             spec
    );

    ~PluginGraphNode() override;

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput in) override;

    std::string get_name() const override;

private:

    std::string                       name_;
    std::string                       configJson_;
    std::string                       type_;
    std::shared_ptr<PluginInstance>   instance_;
    AgentxxPluginGraphNodeTypeSpec    spec_;
};

} // namespace plugin
} // namespace agentxx
