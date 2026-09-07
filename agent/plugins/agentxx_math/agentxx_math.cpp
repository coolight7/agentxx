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

AGENTXX_PLUGIN_AGENT_EXPORT(
    MathPluginCtx,
    "agentxx_math",
    "1.0.0",
    "Mathematical expression evaluator: parse and calculate math expressions",
    [](MathPluginCtx& ctx) -> int32_t {
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
);
