#pragma once

#include "agentxx/agent/code_agent.h"
#include "agentxx/agent/io/jsonl_io_transport.h"
#include <memory>
#include <string>

namespace agentxx {
namespace client {

/// JSONL stdio 一次性运行模式 (计划 PRO-8)
///
/// 与 `server` 模式共用同一套 Wire 协议与同一份会话驱动代码, 只把传输换成 stdin/stdout
/// 的逐行 JSON (见 `agentxx::agent::JsonlAgentIOTransport`):
///
/// ```
///   stdin  : 一行一条 client → server 消息 (hello / user_input / cancel / ...)
///   stdout : 一行一条 server → client 消息 (hello_ack / input_ack / delta /
///            turn_result / ...), 只写协议行, 写后立即 flush
///   stderr : 日志与诊断 (不混进 stdout, 保证 stdout 可逐行解析)
/// ```
///
/// 一次性运行语义:
/// - stdin EOF 后不再受理新输入, 但会等待进行中的轮次与已排队的输入跑完再退出
///   (脚本可以只写一条 `user_input` 就关闭 stdin, 然后从 stdout 读结果)
/// - 收到 SIGINT / SIGTERM 时同样按"输入结束"收尾, 但等待时限很短 (不等长轮次跑完)
/// - **响应不等于轮次完成**: `input_ack` 只表示输入已受理 (started/queued/steered),
///   轮次是否成功结束看 `turn_result` (`hasError` / `interrupted` / `errorMessage`);
///   退出码只反映"进程是否正常跑完", 不反映轮次结果
///
/// - `args`:
///     - [agent]     已构造但未 init 的 CodeAgent (与本进程内 TUI/CLI 模式同一个对象)
///     - [sessionId] 会话 id; 为空时自动生成一个唯一 id, 外部客户端从 `hello_ack`
///                   的 `sessionId` 字段读回, 后续请求都用该 id (也可以留空:
///                   服务端把空会话 id 视为"按当前绑定会话处理")
///     - [lineIo]    行读写注入点; 缺省时读 stdin / 写 stdout (测试用内存缓冲注入)
void runJsonlStdio(
    std::shared_ptr<agent::CodeAgent>          agent,
    std::string                                sessionId = {},
    agent::JsonlAgentIOTransport::LineIo       lineIo    = {}
);

} // namespace client
} // namespace agentxx
