#pragma once

#include "agentxx/agent/io/embedded_client_agent_io.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/ffi_api.h"
#include "asio/any_io_executor.hpp"
#include "utilxx_base/json.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

namespace agentxx {
namespace ffi {

/// 把一条事件交给嵌入方回调
///
/// - payload 用 `agentxx_ffi_malloc` 分配 (跨 CRT 堆), **所有权交给回调**:
///   回调返回前自己 `agentxx_ffi_string_free`, 这里不回收
/// - `cb.on_event` 为空 (headless) 时直接返回, 不做任何序列化
/// - 该函数本身不判线程: 调用方负责在正确的线程上调它
void emitOwnedEvent(const AgentxxFFICallbacks& cb, int32_t type, std::string json);

/// FFI client 端点: 自定义 AgentIOBase client 端点 (与 TUIClientAgentIO /
/// StdIOClientAgentIO 同级), 把服务端产出的事件转成 C 回调 (JSON payload),
/// 并把 FFI 侧的调用请求转成 Wire 消息。
///
/// 应答分流 (提交 + 回执模型):
/// - 应答里 `reqId > 0`: 交给运行期按 reqId 结算对应调用 (不另发广播事件)
/// - 应答里 `reqId == 0`: 属于广播 (别的客户端的请求结果、服务端主动推送),
///   按类型发成 EVT_MODEL_INFO / EVT_PERMISSION_STATE / EVT_COMPONENTS 等
/// - 其余不认识的类型委托基类处理
///
/// 线程约定 (独立 client io 线程):
/// - 本端点状态 (pendingHostToolCalls_ 等) 只在该线程读写
/// - 所有被动回调与对外事件回调都在该线程执行
class FfiClientAgentIO : public agent::EmbeddedClientAgentIO,
                         public std::enable_shared_from_this<FfiClientAgentIO> {
public:

    FfiClientAgentIO(asio::any_io_executor ex, AgentxxFFICallbacks callbacks);

    ~FfiClientAgentIO() override;

    /// 是否在收集事件 (headless 时为 false)
    bool wantsEvents() const {
        return callbacks_.on_event != nullptr;
    }

    /// 上报内部错误事件 (EVT_ERROR); 任意线程可调用 (自动投递到 client io 线程)
    void notifyError(int code, std::string name, std::string message);

    /// 发一条调用回执事件 (EVT_RESULT); **必须在 client io 线程调用**
    /// - [op] op 名; [ok]/[code]/[errorName]/[message]/[result] 为回执内容
    void emitCallResult(
        uint64_t          reqId,
        std::string_view  op,
        bool              ok,
        int32_t           code,
        std::string_view  errorName,
        std::string_view  message,
        const utilxx_base::Json& result
    );

    /// 发一条事件 (EVT_*); **必须在 client io 线程调用**
    void emitEvent(int32_t type, std::string json);

    /// 失败全部挂起中断 (client io 线程调用; 停止或析构时清理)
    /// - 在基类清理之上, 额外为尚未应答的嵌入方程序工具调用上报取消事件
    void failAllPendingInterrupts() override;

    /// 提交嵌入方程序工具调用应答 (client io 线程)
    bool submitHostToolResponse(int64_t callId, int32_t isError, std::string resultJson);

    // -----------------------------------------------------------------------
    // 回执分流 (由运行层注入; 均在 client io 线程调用)
    // -----------------------------------------------------------------------

    /// 一次调用的回执
    /// - [result] 成功时给结果 (`resp:*` 类 op 为去掉 type/reqId 的应答 JSON)
    /// - [ok] false 时 [reason] 是机器可读原因, [message] 是给人看的说明
    std::function<void(
        uint64_t                  reqId,
        utilxx_base::Json         result,
        bool                      ok,
        std::string               reason,
        std::string               message
    )>
        onCallReply;

    /// 收尾: 把仍在等待的调用全部失败掉 (client io 线程, 端点 join 之前调用)
    std::function<void()> onStopping;

protected:

    // ---- AgentIOBase 被动接收回调 (均在 client io 线程执行) ----

    void onSync(const agent::WireSyncPayload& payload) override;

    void onTurnResult(const agent::WireTurnResult& result) override;

    void onContextStats(const agent::WireContextStats& stats) override;

    /// 服务端就绪 (触发 EVT_READY)
    void onServerReady() override;

    /// 处理对端消息: 回执分流 + 事件转换, 其余委托基类
    void onPeerMessage(agent::WireMessage msg) override;

    /// 关闭流程开始: 失败挂起中断 + 冲刷合批 (基类), 再把未完成的调用交给运行期
    void onRuntimeStopping() override;

private:

    /// 合批后的增量文本发出 (EVT_DELTA_TEXT_BATCH; 同一消息 id 内才会合批)
    void emitDeltaText(
        std::string_view msgId,
        std::string_view kind,
        std::string_view text
    ) override;

    /// 非合批的增量发出 (EVT_DELTA)
    void emitDeltaEvent(const agent::WireDelta& delta) override;

    /// 回执分流: reqId > 0 交给运行期结算, 0 按广播处理
    void dispatchReply(uint64_t reqId, utilxx_base::Json json, bool ok, std::string reason, std::string message);

    static std::string dump(const utilxx_base::Json& j);

    /// 事件回调 (值拷贝, 保持有效)
    AgentxxFFICallbacks callbacks_;

    /// 已下发 EVT_HOST_TOOL_CALL 但嵌入方程序尚未应答的调用 id (用于停止时上报取消)
    std::set<int64_t> pendingHostToolCalls_;
};

} // namespace ffi
} // namespace agentxx
