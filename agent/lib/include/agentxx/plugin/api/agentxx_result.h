#pragma once

// agentxx 插件业务表的返回码（唯一一份定义）
//
// 约定（与 `pluginxx/api/result.h` 一致）：
// - 成功 = 0，就是 `PLUGINXX_RESULT_OK`；
// - 失败一律落在 pluginxx 分给**应用自定义**的 `-5000 ~ -9999` 段，主程序按区间给出的类别是
//   `app_custom`，名字由 agentxx 自己解释（`agentxx::plugin::resultName`）；
// - 一张表只用它真的会返回的码：**每个数字都必须有返回点**；
// - 数字只到 C / FFI 边界：日志与 JSON 里写名字（`resultName`），不写数字；
// - 句柄类返回值（channel id / trigger id / timer id 等）的 `-1` 表示"没拿到 id"，
//   不是返回码，不在这张表里。
//
// 所有表共用同一份码表（同义同值），和 pluginxx 通用段的做法一样；插件从哪张表拿到这个码，
// 由它调用的入口决定，码本身不需要带表名前缀。
//
// 分段：
// - `0`            成功
// - `-5000 ~ -5099` 通用失败（任何表都可能返回）
// - `-5100 ~ ...`   预留给将来某张表独有的失败（目前没有；加之前先确认真的无法共用）

#include "pluginxx/api/result.h"

#include <stdint.h>

/* ==================== 成功 ==================== */

/// 成功（与插件框架的成功码是同一个值）
#define AGENTXX_RESULT_OK PLUGINXX_RESULT_OK

/* ==================== 通用失败段 (-5000 ~ -5099) ==================== */

#define AGENTXX_RESULT_ERR_ARG (-5000)
///< 参数非法：空指针 / 空字符串 / 枚举越界 / `struct_size` 低于基线布局

#define AGENTXX_RESULT_ERR_JSON (-5001)
///< 参数里的 JSON 文本非法：解析失败 / 不是对象 / 不是期望的形态

#define AGENTXX_RESULT_ERR_STATE (-5002)
///< 当前状态不允许：未就绪 / 关闭中 / 已停止 / 在 `mount` 回调里做只能在 `start` 做的事

#define AGENTXX_RESULT_ERR_NOT_FOUND (-5003)
///< 目标不存在：工具 / 会话 / 资源 / 图 / 挂载 / 根 / 通道 / 触发器 / 定时器 / 任务 / 键位 / 句柄

#define AGENTXX_RESULT_ERR_NO_AGENT (-5004)
///< agent 引用无效：引用缺失 / 指向已卸载的挂载 / 序号不匹配（注册类接口返回它，且无副作用）

#define AGENTXX_RESULT_ERR_NOT_OWNER (-5005)
///< 对象不属于本实例：注销别人的工具或权限声明、替别人的工具声明权限、图定义已被别的实例占用

#define AGENTXX_RESULT_ERR_CONFLICT (-5006)
///< 名字或 id 冲突：工具 / 节点类型重名，同一 `sessionId` 开了两个通道，键位已被占用

#define AGENTXX_RESULT_ERR_LIMIT (-5007)
///< 超过上限：根数 / 通道数 / 触发器 / 定时器 / 键位 / UI 描述大小

#define AGENTXX_RESULT_ERR_UNSUPPORTED (-5008)
///< 主程序没提供这项能力：权限中间件未装配 / 运行时容器未 attach / 客户端没接该子能力

#define AGENTXX_RESULT_ERR_FAILED (-5009)
///< 执行失败：落盘 / 投递 / 目标侧拒绝 / init 失败（具体原因看日志）

#define AGENTXX_RESULT_ERR_INTERNAL (-5010)
///< 实现内部错误（不该发生：异常逃逸 / 不变量被破坏）

#ifdef __cplusplus
namespace agentxx {
namespace plugin {

/// 返回码的名字（日志 / JSON 用；不属于这张表时返回 `nullptr`）
///
/// `inline`：头文件即用，主程序与各插件各编译一份字面量，值相同。
/// 名字与插件框架 `pluginxx::result::nameOf` 用的是同一套用词。
inline const char* resultName(int32_t code) {
    switch (code) {
        case AGENTXX_RESULT_OK:
            return "ok";
        case AGENTXX_RESULT_ERR_ARG:
            return "invalid_argument";
        case AGENTXX_RESULT_ERR_JSON:
            return "invalid_json";
        case AGENTXX_RESULT_ERR_STATE:
            return "invalid_state";
        case AGENTXX_RESULT_ERR_NOT_FOUND:
            return "not_found";
        case AGENTXX_RESULT_ERR_NO_AGENT:
            return "invalid_agent";
        case AGENTXX_RESULT_ERR_NOT_OWNER:
            return "not_owner";
        case AGENTXX_RESULT_ERR_CONFLICT:
            return "conflict";
        case AGENTXX_RESULT_ERR_LIMIT:
            return "limit_exceeded";
        case AGENTXX_RESULT_ERR_UNSUPPORTED:
            return "unsupported";
        case AGENTXX_RESULT_ERR_FAILED:
            return "failed";
        case AGENTXX_RESULT_ERR_INTERNAL:
            return "internal";
        default:
            return nullptr;
    }
}

} // namespace plugin
} // namespace agentxx

static_assert(INT32_C(AGENTXX_RESULT_OK) == PLUGINXX_RESULT_OK, "成功码必须与插件框架一致");
#endif // __cplusplus
