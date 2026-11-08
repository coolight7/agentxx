#pragma once

#include "agentxx/tools/tool.h"
#include "neograph/types.h"
#include "utilxx_base/json.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace plugin {

/// 动态工具注册表 (热插拔插件工具)
///
/// 设计要点:
/// - 统一以 shared_ptr 持有插件工具: 摘除注册后, 执行中调用仍由局部
///   shared_ptr 引用计数保活, 代码段不会在执行中被卸载 (插件卸载须等
///   插件实例 inflight==0, 见 PluginManager)
/// - 仅 io 线程读写 (与 Session 无锁模型一致); 工具执行在主程序线程池,
///   经 find() 取 shared_ptr 后跨线程安全
/// - 内置工具 (engine->own_tools 的 unique_ptr 所有权) 不进入本表,
///   查找顺序: 本表优先 (动态) → ToolcallWrapNode 静态列表回退
/// - 名称冲突检测: 注册时与 [静态工具名集合] 比对, 冲突返回失败
///   (静态工具名由 BaseAgent::init 加载时注入)
class ToolRegistry {
public:

    /// 注册工具; 名称冲突 (表内/静态工具) 返回 false
    bool registerTool(std::string name, std::shared_ptr<agentxx::tools::XXToolBase> tool);

    /// 摘除工具 (按名称); 返回被摘除的工具 (执行中调用靠返回的 shared_ptr 保活)
    std::shared_ptr<agentxx::tools::XXToolBase> unregisterTool(std::string_view name);

    /// 查找工具; 返回 shared_ptr (保持代码段存活, 可跨线程使用)
    std::shared_ptr<agentxx::tools::XXToolBase> find(std::string_view name) const;

    /// 表内是否已注册 (含禁用摘除前判断)
    bool contains(std::string_view name) const;

    /// 追加全部工具定义 (LLM 请求侧工具 schema; 供 ModelCallWrapNode 调用)
    void appendDefinitions(std::vector<neograph::ChatTool>& defs) const;

    /// 当前注册数量
    size_t size() const;

    /// 所有工具名 (供列表展示)
    std::vector<std::string> names() const;

    /// 加载静态工具名集合 (BaseAgent::init 调用; 用于注册冲突检测)
    /// - 传入 engine 已持有所有权的内置工具名; 仅作名称比对, 不持有对象
    void setStaticToolNames(std::vector<std::string> names);

    /// 登记静态工具的参数 schema (BaseAgent::init 调用; 展示侧参数自愈用)
    /// - 只保存名字与参数 schema (`ChatTool::parameters`), 不持有工具对象;
    ///   与 [setStaticToolNames] 是两件事, 后者只用于插件工具注册冲突检测
    /// - 整体替换 (重复调用以最后一次为准)
    void setStaticToolSchemas(std::vector<std::pair<std::string, utilxx_base::Json>> schemas);

    /// 取工具参数 JSON Schema (静态登记表 → 动态插件工具定义)
    /// - 展示侧用它把工具行参数按声明类型自愈 (见
    ///   [agentxx::util::autoFixToolArgsTypes]), 使特化渲染器直接按类型读参数
    /// - 插件工具的定义按名字缓存 (注册/摘除时失效), 避免每一行都构造一次
    ///   `ChatTool`; 仅 io 线程调用 (与表内其它查询一致)
    /// - 未登记返回 false (`out` 保持不变)
    bool parametersOf(std::string_view name, utilxx_base::Json& out) const;

private:

    /// 静态工具名 (内置/MCP/延迟加载工具), 冲突检测用
    std::vector<std::string> staticToolNames_{};
    /// 静态工具参数 schema (名字 → parameters; 展示侧参数自愈用)
    std::map<std::string, utilxx_base::Json, std::less<>> staticToolSchemas_{};
    /// 插件工具定义里的参数 schema 缓存 (get_definition 每次都会构造 ChatTool)
    /// - 注册/摘除该名字时清掉, 因此不会读到旧定义
    mutable std::map<std::string, utilxx_base::Json, std::less<>> dynamicSchemaCache_{};
    /// 动态插件工具表
    std::map<std::string, std::shared_ptr<agentxx::tools::XXToolBase>, std::less<>> tools_{};
};

} // namespace plugin
} // namespace agentxx
