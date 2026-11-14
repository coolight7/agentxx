#ifndef AGENTXX_AGENT_TOOL_RENDER_BRIDGE_H
#define AGENTXX_AGENT_TOOL_RENDER_BRIDGE_H

/// 工具特化渲染的 agent 侧一半: 读插件登记表, 替客户端算出内容
///
/// ## 为什么在 agent 侧
///
/// 渲染器由 **agent 侧** 插件注册 (C++ 插件或 JS 脚本), 它们的代码只活在这个进程里;
/// 客户端 (TUI/CLI/远程) 拿到的是登记表快照, 算内容时必须回到这里:
///
/// ```
/// agent: PluginMount::toolRenderers_   --(注册/注销)--> 下发快照
/// client: 缓存未命中 → tool_render_request
/// agent:  [findToolRenderCallable] (io 线程) → [invokeToolRender] (工作线程)
/// client:  tool_render_result → 写缓存 → 重绘
/// ```
///
/// ## 线程
///
/// - [collectToolRenderItems] / [findToolRenderCallable] 读挂载登记表, **只在 agent
///   io 线程**调用 (挂载级状态都在那条线程改);
/// - [invokeToolRender] 在**工作线程**调用 (插件回调可能慢, 不能挡住会话推进),
///   它只碰调用凭据里的字段, 不再访问挂载表; 内部持插件实例租约, 因此回调执行期间
///   插件不会被卸载 (dlclose 不会越过仍在运行的插件代码)。
#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/plugin/api/plugin_api.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {

namespace plugin {
class PluginInstance;
}

namespace agent {

class AgentContext;

/// 单项字段的字节上限 (参数/结果/输出; 超限按"没有内容"处理)
///
/// 与客户端侧同一上限: 两侧都不希望一条渲染请求把消息撑大。
inline constexpr size_t kToolRenderValueMaxBytes = 256 * 1024;

/// 收集本 agent 的渲染登记快照 (没有挂载/表为空时返回空数组)
///
/// - io 线程调用
/// - 空数组表示"没有自定义渲染", 下发后客户端清空远程表
std::vector<WireToolRenderItem> collectToolRenderItems(const AgentContext& ctx);

/// 从登记表取出的一次调用凭据
///
/// 取出后与挂载表无关 (回调指针 + user_data + 实例强引用都在这里), 因此可以带到
/// 工作线程执行。`instance` 为空表示该实例没有自持引用 (单元测试手工构造的伪实例),
/// 此时按"没有租约约束"处理, 直接调用回调。
struct ToolRenderCallable {
    std::string                             toolName;
    AgentxxPluginToolRenderFn               fn       = nullptr;
    void*                                   userData = nullptr;
    /// true = 折叠内容 (一行文本), false = 展开体 (items JSON)
    bool                                    collapsed = true;
    std::shared_ptr<plugin::PluginInstance> instance;
};

/// 取某工具某一部分的调用凭据
///
/// - io 线程调用
/// - 没有登记项 / 登记项没有该部分的回调 → `std::nullopt` (客户端按"没有自定义内容"
///   处理并缓存, 不会反复来问)
///
/// - `args`:
///     - [ctx] 目标 agent 的上下文 (用它找到挂载)
///     - [toolName] 工具名
///     - [collapsed] true = 折叠内容 (一行文本), false = 展开体 (items JSON)
std::optional<ToolRenderCallable> findToolRenderCallable(
    const AgentContext& ctx,
    std::string_view    toolName,
    bool                collapsed
);

/// 执行一次渲染请求 (工作线程)
///
/// - 回调抛异常、返回非 OK、输出超限、实例已进入关闭流程 → `ok = false`
///   (客户端回退默认渲染并缓存, 不会抛到调用方)
/// - `requestId` 原样带回, 客户端据此匹配等待中的请求
///
/// - `args`:
///     - [call] [findToolRenderCallable] 取出的凭据
///     - [req] 客户端发来的请求 (字段均为拥有型拷贝)
WireToolRenderResult invokeToolRender(
    const ToolRenderCallable&         call,
    const WireToolRenderRequest& req
);

} // namespace agent
} // namespace agentxx

#endif // AGENTXX_AGENT_TOOL_RENDER_BRIDGE_H
