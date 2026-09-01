// agentxx_math —— 数学计算工具插件
#include "agentxx_math_plugin.h"
#include "math_impl.h"
#include <string>

using namespace agentxx_math_plugin;

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

std::string schemaCalculate(PluginCtx* ctx) {
    auto p = ctx->toolPrompt(kNameCalculate);
    return neograph::json{
        {"type", "object"},
        {
         "properties", {
                {
                    "expression",
                    {
                        {"type", "string"},
                        {
                            "description",
                            agentxx::plugin::toolPromptArgDesc(
                                p,
                                "expression",
                                "The mathematical expression string to evaluate, e.g. '2 + 3 * 4', 'sin(pi / 4) ^ 2', 'sqrt(16) + log10(100)', '5!', 'gcd(48, 18)'."
                            ),
                        },
                    },
                },
                {
                    "precision",
                    {
                        {"type", "integer"},
                        {
                            "description",
                            agentxx::plugin::toolPromptArgDesc(
                                p,
                                "precision",
                                "Optional decimal precision for floating point output (e.g. 2 for 2 decimal places, range 0 to 15)."
                            ),
                        },
                    },
                },
                {
                    "angle_unit",
                    {
                        {"type", "string"},
                        {"enum", neograph::json::array({"rad", "deg"})},
                        {"default", "rad"},
                        {
                            "description",
                            agentxx::plugin::toolPromptArgDesc(
                                p,
                                "angle_unit",
                                "Angle unit for trigonometric functions: 'rad' (radians, default) or 'deg' (degrees)."
                            ),
                        },
                    },
                },
            }, },
        {"required", neograph::json::array({"expression"})},
    }
        .dump();
}

} // namespace

/* ---------------- 插件入口 / 销毁 ---------------- */

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                agentxx_plugin_sv_cstr("agentxx_math"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr(
                    "Mathematical expression evaluator: parse and calculate math expressions"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            raw = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            // agentxx_math_calculate (fast_tool)
            agentxx::plugin::fast_tool(
                *ctx,
                kNameCalculate,
                kDepictCalculate,
                schemaCalculate(ctx.get()),
                [](std::string_view args_json) -> std::string {
                    auto arguments = args_json.empty() ? neograph::json::object()
                                                       : neograph::json::parse(args_json);
                    return mathCalculateExecute(arguments);
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            delete ctx;
        }
    });
}
