#pragma once

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_kit.h"
#include "neograph/graph/node.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace agentxx {
namespace plugin {

class PluginInstance;

/// GraphRegistry 没有删除类型的接口时，宿主用 slot 间接管理插件工厂。
/// 注册表中的 factory 只捕获 slot；卸载/重载通过代次切换使旧节点失效。
struct GraphTypeSlot {
    struct Snapshot {
        std::shared_ptr<PluginInstance> instance;
        AgentxxPluginGraphNodeTypeSpec spec{};
        std::string type;
        std::string configSchemaJson;
        uint64_t generation = 0;
        bool active = false;

        Snapshot() = default;

        Snapshot(const Snapshot& other) :
            instance(other.instance),
            spec(other.spec),
            type(other.type),
            configSchemaJson(other.configSchemaJson),
            generation(other.generation),
            active(other.active) {
            rebindSpec();
        }

        Snapshot& operator=(const Snapshot& other) {
            if (this != &other) {
                instance = other.instance;
                spec = other.spec;
                type = other.type;
                configSchemaJson = other.configSchemaJson;
                generation = other.generation;
                active = other.active;
                rebindSpec();
            }
            return *this;
        }

        Snapshot(Snapshot&& other) noexcept :
            instance(std::move(other.instance)),
            spec(other.spec),
            type(std::move(other.type)),
            configSchemaJson(std::move(other.configSchemaJson)),
            generation(other.generation),
            active(other.active) {
            rebindSpec();
        }

        Snapshot& operator=(Snapshot&& other) noexcept {
            if (this != &other) {
                instance = std::move(other.instance);
                spec = other.spec;
                type = std::move(other.type);
                configSchemaJson = std::move(other.configSchemaJson);
                generation = other.generation;
                active = other.active;
                rebindSpec();
            }
            return *this;
        }

        void rebindSpec() noexcept {
            spec.type = agentxx::plugin::PluginStringView::from(type.data(), type.size());
            spec.config_schema_json = agentxx::plugin::PluginStringView::from(
                configSchemaJson.data(),
                configSchemaJson.size()
            );
        }
    };

    Snapshot snapshot() const {
        std::lock_guard lock(mutex);
        Snapshot out;
        out.instance = instance.lock();
        out.spec = spec;
        out.type = type;
        out.configSchemaJson = configSchemaJson;
        out.rebindSpec();
        out.generation = generation;
        out.active = active;
        return out;
    }

    void activate(
        const std::shared_ptr<PluginInstance>& owner,
        AgentxxPluginGraphNodeTypeSpec          inSpec,
        uint64_t                                inGeneration
    ) {
        std::lock_guard lock(mutex);
        instance = owner;
        // lifetime generation distinguishes plugin instances. The additional
        // activation counter also invalidates already compiled nodes when the
        // same live instance replaces its registration.
        const auto nextGeneration = generation == UINT64_MAX ? 1 : generation + 1;
        generation = std::max(inGeneration, nextGeneration);
        spec = inSpec;
        type.assign(inSpec.type.data ? inSpec.type.data : "", inSpec.type.size);
        configSchemaJson.assign(
            inSpec.config_schema_json.data ? inSpec.config_schema_json.data : "",
            inSpec.config_schema_json.size
        );
        spec.type = agentxx::plugin::PluginStringView::from(type.data(), type.size());
        spec.config_schema_json
            = agentxx::plugin::PluginStringView::from(configSchemaJson.data(), configSchemaJson.size());
        active = true;
    }

    void invalidate(const PluginInstance* owner) {
        std::lock_guard lock(mutex);
        if (owner && instance.lock().get() != owner) {
            return;
        }
        instance.reset();
        spec = {};
        type.clear();
        configSchemaJson.clear();
        active = false;
    }

private:
    mutable std::mutex mutex;
    std::weak_ptr<PluginInstance> instance;
    AgentxxPluginGraphNodeTypeSpec spec{};
    std::string type;
    std::string configSchemaJson;
    uint64_t generation = 0;
    bool active = false;
};

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
        std::string_view                name,
        std::string_view                configJson,
        std::shared_ptr<PluginInstance> instance,
        AgentxxPluginGraphNodeTypeSpec  spec,
        std::shared_ptr<GraphTypeSlot>  slot = {},
        uint64_t                        generation = 0
    );

    ~PluginGraphNode() override;

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput in) override;

    std::string get_name() const override;

private:

    std::string                     name_;
    std::string                     configJson_;
    std::string                     type_;
    std::string                     configSchemaJson_;
    std::shared_ptr<PluginInstance> instance_;
    AgentxxPluginGraphNodeTypeSpec  spec_;
    std::shared_ptr<GraphTypeSlot>   slot_;
    uint64_t                        generation_ = 0;
};

} // namespace plugin
} // namespace agentxx
