/// agentxx 插件跨边界结构体 (spec / input) 的长度校验
///
/// 统一规则一句话: **填充方填 `struct_size` (自己写入的字节数), 读取方按它 gate**。
/// 方向有两个, 规则同一条:
///
/// | 方向 | 结构体 | 填充方 | 读取方 | 何时需要 gate |
/// |---|---|---|---|---|
/// | 插件填 → 主程序读 | spec (`AgentxxPluginToolSpec` / `AgentxxOverlaySpec` …) | 插件 | 主程序 | 老插件 (短) + 新主程序 (长): 读尾部字段前必须判长度 |
/// | 主程序填 → 插件读 | input (`AgentxxToolRenderInput` / `AgentxxUiActionContext`) | 主程序 | 插件 | 新插件 (认识长布局) + 老主程序 (填得短): 读尾部字段前必须判长度 |
///
/// (反方向都天然安全: 读取方只认识自己的前缀字段, 偏移与填充方逐字节一致。)
///
/// 判定 (读取方):
/// - `0` = 填充方按**基线布局**填写 —— 零初始化的结构体因此是安全默认值;
/// - `>= 基线` = 接受; 读追加字段前必须用 [specHas] 判定;
/// - `< 基线` = 拒绝 (连基线字段都缺, 无法定义语义)。
///
/// 接口表 (Iface) 的判定是另一套 (IID + 版本对 + 基线), 见
/// `pluginxx/api/iface_meta.h` 与 `agentxx/plugin/api/plugin_iface_meta.h`;
/// 结构体不是接口表, 因此没有 IID / `MIN_VERSION` 这些项。
#ifndef AGENTXX_PLUGIN_API_PLUGIN_SPEC_H
#define AGENTXX_PLUGIN_API_PLUGIN_SPEC_H

#include "agentxx/plugin/api/client_plugin_api.h"
#include "agentxx/plugin/api/plugin_api.h"
#include "pluginxx/api/iface_meta.h"

#include <cstdint>

namespace agentxx {
namespace plugin {

/// 跨边界结构体的基线字节数 (缺省 = 完整尺寸; 各结构体在本头下方显式特化)
///
/// 基线 = 第一个追加字段的偏移; 结构体当前还没有追加字段时 = `sizeof(自身)`。
/// 以后往结构体尾部加可选字段时: 把对应常量改成 `offsetof(结构体, 新字段)` 并保持
/// 其余不动, 读取方用 [specHas] 判定。
template<typename Spec>
struct SpecBaseline {
    static constexpr uint32_t value = static_cast<uint32_t>(sizeof(Spec));
};

/// 跨边界结构体长度是否可接受 (读取方)
///
/// - `0` = 填充方按基线布局填写 (零填充是安全默认);
/// - `>= 基线` = 接受;
/// - 其它 = 拒绝。
template<typename Spec>
constexpr bool specSizeOk(uint32_t struct_size) noexcept {
    return struct_size == 0 || struct_size >= SpecBaseline<Spec>::value;
}

/// 追加字段可读判定 (读取方): 对方给的长度是否覆盖该字段
///
/// - 与接口表的 `pluginxx::ifaceHas` 同一思路, 区别只在"长度由对方给"而不是"我方看表";
/// - 基线以内的字段不需要它 (长度门槛已保证);
/// - 用法: `specHas(&AgentxxOverlaySpec::extra_json, spec->struct_size)`
///   (`Spec` 由成员指针自动推出)。
template<typename Spec, typename M>
inline bool specHas(M Spec::* member, uint32_t struct_size) noexcept {
    return struct_size >= pluginxx::memberEndOf(member);
}

/* ==================== 基线常量特化 (每处一行, 便于清点) ==================== */

/// 定义一条基线特化并自检: 基线不得小于结构体头部以外的任何对齐空洞起点, 也不得超过完整尺寸
#define AGENTXX_DECLARE_SPEC_BASELINE(Spec, Baseline)                                              \
    template<>                                                                                     \
    struct SpecBaseline<Spec> {                                                                    \
        static constexpr uint32_t value = static_cast<uint32_t>(Baseline);                          \
        static_assert((Baseline) >= 8u);                                                            \
        static_assert((Baseline) <= sizeof(Spec));                                                  \
    }

AGENTXX_DECLARE_SPEC_BASELINE(AgentxxPluginToolSpec, AGENTXX_PLUGIN_TOOL_SPEC_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxPluginHookSpec, AGENTXX_PLUGIN_HOOK_SPEC_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginHookSpecEx,
    AGENTXX_PLUGIN_HOOK_SPEC_EX_BASELINE_SIZE
);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginToolPermissionSpec,
    AGENTXX_PLUGIN_TOOL_PERMISSION_SPEC_BASELINE_SIZE
);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginPermissionPathQuery,
    AGENTXX_PLUGIN_PERMISSION_PATH_QUERY_BASELINE_SIZE
);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginGraphNodeTypeSpec,
    AGENTXX_PLUGIN_GRAPH_NODE_TYPE_SPEC_BASELINE_SIZE
);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginFeaturePointSpec,
    AGENTXX_PLUGIN_FEATURE_POINT_SPEC_BASELINE_SIZE
);
AGENTXX_DECLARE_SPEC_BASELINE(
    AgentxxPluginFeatureImplSpec,
    AGENTXX_PLUGIN_FEATURE_IMPL_SPEC_BASELINE_SIZE
);

AGENTXX_DECLARE_SPEC_BASELINE(AgentxxToolRenderInput, AGENTXX_TOOL_RENDER_INPUT_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxToolRenderSpec, AGENTXX_TOOL_RENDER_SPEC_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxUiActionContext, AGENTXX_UI_ACTION_CONTEXT_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxOverlaySpec, AGENTXX_OVERLAY_SPEC_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxTimerSpec, AGENTXX_TIMER_SPEC_BASELINE_SIZE);
AGENTXX_DECLARE_SPEC_BASELINE(AgentxxKeybindSpec, AGENTXX_KEYBIND_SPEC_BASELINE_SIZE);

#undef AGENTXX_DECLARE_SPEC_BASELINE

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_API_PLUGIN_SPEC_H */
