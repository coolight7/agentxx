#pragma once

#include "agentxx/agent/io/agent_io_transport.h"
#include <expected>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {

/// 输入 JSON 解析失败的原因
///
/// 分开"JSON 语法错误"与其他参数问题: 调用方通常把前者映射成"格式错误"类错误码,
/// 后者映射成"参数非法"类错误码; 只看错误文本猜是哪一类不可靠。
struct UserInputError {
    /// true = JSON 语法错误; false = 结构/取值不合法
    bool        badJson = false;
    /// 给人看的说明 (含具体字段与限额, 可直接转给调用方)
    std::string message;
};

/// 把一条输入 JSON 解析成 wire 输入 (`WireUserInput`) 的正文部分
///
/// 输入形态 (CLI / FFI / 插件共用同一份):
/// ```json
/// {
///   "text": "……",
///   "attachments": [{"type":"image","displayName":"a.png","pathOrUrl":"D:/a.png"}],
///   "model": "gpt-4o",
///   "delivery": "next-turn"
/// }
/// ```
///
/// 只填 `text` / `attachments` / `model` / `delivery` 四项: 会话、请求序号、等待
/// 回执这类"这条输入属于谁"的信息由调用方补齐 —— 同一条输入 JSON 在不同通道里
/// 属于不同会话, 不该由解析函数决定。
///
/// 校验顺序 (与历史 FFI 行为一致):
/// 1. 必须是合法 JSON 对象;
/// 2. `attachments` 若非空必须是数组;
/// 3. `text` 与 `attachments` 至少一个非空;
/// 4. 附件条数不超过 [kMaxAttachmentsPerMessage];
/// 5. 单个附件不超过该类型的大小上限。
///
/// - `args`:
///     - [inputJson] 输入 JSON 文本 (空串按"缺少内容"处理)
///
/// - `return` 成功给 wire 输入; 失败给可读原因
std::expected<WireUserInput, UserInputError> parseUserInputJson(std::string_view inputJson);

} // namespace agent
} // namespace agentxx
