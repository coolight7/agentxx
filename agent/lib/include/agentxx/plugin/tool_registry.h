#pragma once

#include "agentxx/tools/tool.h"
#include "neograph/types.h"
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace plugin {

/// 动态工具注册表 (热插拔插件工具)
///
/// 设计要点:
/// - 统一以 shared_ptr 持有插件工具: 摘除注册后, 在途调用仍由局部
///   shared_ptr 引用计数保活, 代码段不会在执行中被卸载 (插件卸载须等
///   插件实例 inflight==0, 见 PluginManager)
/// - 仅 io 线程读写 (与 Session 无锁模型一致); 工具执行在宿主线程池,
///   经 find() 取 shared_ptr 后跨线程安全
/// - 内置工具 (engine->own_tools 的 unique_ptr 所有权) 不进入本表,
///   查找顺序: 本表优先 (动态) → ToolcallWrapNode 静态列表回退
/// - 名称冲突检测: 注册时与 [静态工具名集合] 比对, 冲突返回失败
///   (静态工具名由 BaseAgent::init 装配时注入)
class ToolRegistry {
public:

    /// 注册工具; 名称冲突 (表内/静态工具) 返回 false
    bool registerTool(std::string name, std::shared_ptr<agentxx::tools::XXToolBase> tool);

    /// 摘除工具 (按名称); 返回被摘除的工具 (在途调用靠返回的 shared_ptr 保活)
    std::shared_ptr<agentxx::tools::XXToolBase> unregisterTool(const std::string& name);

    /// 查找工具; 返回 shared_ptr (保持代码段存活, 可跨线程使用)
    std::shared_ptr<agentxx::tools::XXToolBase> find(const std::string& name) const;

    /// 表内是否已注册 (含禁用摘除前判断)
    bool contains(const std::string& name) const;

    /// 追加全部工具定义 (LLM 请求侧工具 schema; 供 ModelCallWrapNode 调用)
    void appendDefinitions(std::vector<neograph::ChatTool>& defs) const;

    /// 当前注册数量
    size_t size() const;

    /// 所有工具名 (供列表展示)
    std::vector<std::string> names() const;

    /// 装配静态工具名集合 (BaseAgent::init 调用; 用于注册冲突检测)
    /// - 传入 engine 已持有所有权的内置工具名; 仅作名称比对, 不持有对象
    void setStaticToolNames(std::vector<std::string> names);

private:

    /// 静态工具名 (内置/MCP/延迟加载工具), 冲突检测用
    std::vector<std::string> staticToolNames_{};
    /// 动态插件工具表
    std::map<std::string, std::shared_ptr<agentxx::tools::XXToolBase>, std::less<>> tools_{};
};

} // namespace plugin
} // namespace agentxx
