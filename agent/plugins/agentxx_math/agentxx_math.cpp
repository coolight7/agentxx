/// agentxx_math —— 数学计算工具插件
#include "agentxx_math_plugin.h"
#include "math_impl.h"
#include <string>

using namespace agentxx_math_plugin;
using namespace agentxx::plugin;

namespace {

constexpr std::string_view kNameCalculate = "agentxx_math_calculate";
constexpr std::string_view kDepictCalculate =
    R"(Evaluate a mathematical expression and return the computed result.
Supports:
- Arithmetic: `+`, `-`, `*`, `/`, `%` (modulo), `//` (floor division)
- Power: `^` or `**` (e.g. `2^10`, `2**3**2`)
- Factorial: `!` (e.g. `5!`)
- Bitwise: `&`, `|`, `~`, `<<`, `>>`, `xor(a,b)`
- Comparisons & Logic: `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`, `? :`
- Constants: `pi`, `e`, `tau`, `phi`, `inf`, `nan`, `true`, `false`
- Functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `sqrt`, `cbrt`, `exp`, `log`, `ln`, `log10`, `log2`, `abs`, `floor`, `ceil`, `round`, `min`, `max`, `sum`, `avg`, `gcd`, `lcm`, `comb` (nCr), `perm` (nPr), `clamp`, `rad`, `deg`, etc.
- Implicit multiplication: `2pi`, `2(3+4)`, `(1+2)(3+4)`)";

} // namespace

struct MathPluginCtx : public PluginBase {};

/// 注册事务 (mount 的实际内容); 失败由主程序只回滚这一个 agent。
static int32_t mathSetup(MathPluginCtx& ctx, const ScopeRef& ref) {
    auto schema
        = ctx.schema(kNameCalculate)
              .string(
                  "expression",
                  "The mathematical expression string to evaluate, e.g. '2 + 3 * 4', 'sin(pi / 4) ^ 2', 'sqrt(16) + log10(100)', '5!', 'gcd(48, 18)'.",
                  /*required=*/true
              )
              .integer(
                  "precision",
                  "Optional decimal precision for floating point output (e.g. 2 for 2 decimal places, range 0 to 15)."
              )
              .enumString(
                  "angle_unit",
                  "Angle unit for trigonometric functions: 'rad' (radians, default) or 'deg' (degrees).",
                  {"rad", "deg"},
                  false,
                  "rad"
              )
              .build();

    fast_tool(
        ctx,
        ref,
        kNameCalculate,
        kDepictCalculate,
        schema,
        [](std::string_view args_json) -> std::string {
            ArgReader args(args_json);
            return mathCalculateExecute(args.raw());
        }
    );
    return 0;
}

static void*
    mathStart(MathPluginCtx& ctx, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 本插件没有进程级工作 (无连接/线程/定时器): start 只给出完成信号。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    (void)ctx;
    return nullptr;
}

/// 挂载: 每个 agent 一次 (本插件没有 per-agent 状态, 直接注册工具)
static void* mathMount(
    MathPluginCtx&                     ctx,
    const AgentxxPluginAgentMountSpec* spec,
    const PluginxxOperatorNotify*      notify,
    PluginxxString*                    error
) {
    if (!notify) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_math mount: notify required");
        }
        return nullptr;
    }
    const AgentRef ref(*spec);
    if (mathSetup(ctx, ref) != 0) {
        if (error) {
            PluginString::set(ctx.host, error, "agentxx_math mount: registration failed");
        }
        return nullptr;
    }
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* mathUnmount(
    MathPluginCtx&,
    void*,
    const PluginxxOperatorNotify* notify,
    PluginxxString*
) {
    // 注册由主程序在本挂载卸载时自动撤销; per-agent 状态由挂载销毁。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

static void* mathStop(MathPluginCtx&, const PluginxxOperatorNotify* notify, PluginxxString*) {
    // 无自管线程/定时器; 注册记录由主程序在 stop 后统一撤销。
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    return nullptr;
}

AGENTXX_PLUGIN_AGENT_EXPORT(
    MathPluginCtx,
    "agentxx_math",
    "1.0.0",
    "Mathematical expression evaluator: parse and calculate math expressions",
    mathStart,
    mathStop,
    mathMount,
    mathUnmount
);
