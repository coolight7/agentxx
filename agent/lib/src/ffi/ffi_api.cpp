//
// 本文件是唯一允许跨语言 (FFI) 调用的导出面: 全部函数为 extern "C" + 纯 C
// 参数/返回 (无 STL/异常出界), 内部统一 catchError 兜底并转错误码; 每个
// 函数可选的 AgentxxString* log 参数用于向调用方回传执行过程中的错误/日志详情 (宿主
// 用后必须 agentxx_ffi_string_free 释放)。符号导出白名单见 lib/ffi_symbols.map。

#include "agentxx/ffi_api.h"
#include "agentxx/util/log.h"
#include "ffi_runtime.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

using agentxx::ffi::FfiAgentRuntime;

namespace {

/// 捕获当前异常为可读文本 (仅供 C 边界兜底)
std::string cxxErrText() {
    try {
        throw;
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown exception";
    }
}

/// 填充输出 AgentxxString (跨 CRT 堆分配)
void fillString(AgentxxString* out, std::string_view s) {
    if (out == nullptr) {
        return;
    }
    out->data = nullptr;
    out->size = 0;
    if (s.empty()) {
        return;
    }
    AgentxxStringView sv{s.data(), static_cast<uint64_t>(s.size())};
    agentxx_ffi_strdup_n(&sv, out);
}

std::string_view svToCpp(const AgentxxStringView* sv) {
    if (sv == nullptr || sv->data == nullptr || sv->size == 0) {
        return {};
    }
    return std::string_view{sv->data, static_cast<size_t>(sv->size)};
}

/// 失败: 填 log (如有) 并返回错误码
int32_t ffiFail(int32_t code, const std::string& detail, AgentxxString* log) {
    if (log != nullptr) {
        fillString(log, detail);
    }
    return code;
}

/// 成功: log 置空, 返回 0
int32_t ffiOk(AgentxxString* log) {
    if (log != nullptr) {
        log->data = nullptr;
        log->size = 0;
    }
    return AGENTXX_FFI_OK;
}

/// 按 rc/err 填 log 并返回 rc (非 0 且 err 为空时标注内部错误)
int32_t ffiFinish(int32_t rc, const std::string& err, AgentxxString* log) {
    if (log == nullptr) {
        return rc;
    }
    if (rc == 0) {
        log->data = nullptr;
        log->size = 0;
    } else {
        fillString(log, err.empty() ? "internal error (no detail)" : err);
    }
    return rc;
}

} // namespace

// C 句柄实体: 持有运行时 (shared_ptr; 协程/回调经 shared_from_this 保活)。
// 头文件仅有 typedef struct AgentxxFFIAgent AgentxxFFIAgent 前置声明, 此处补充完整定义
// (必须在全局命名空间, 与头文件 typedef 引用同一类型)。
struct AgentxxFFIAgent {
    std::shared_ptr<FfiAgentRuntime> impl;
};

extern "C" {

// ---------------------------------------------------------------------------
// 内存 / 版本
// ---------------------------------------------------------------------------

void* AGENTXX_FFI_CALL agentxx_ffi_malloc(uint64_t size) {
    return malloc(static_cast<size_t>(size));
}

void AGENTXX_FFI_CALL agentxx_ffi_free(const void* ptr) {
    XX_LOGD("agentxx_ffi_free : {}", ptr);
    free(const_cast<void*>(ptr));
}

void AGENTXX_FFI_CALL agentxx_ffi_string_free(AgentxxString* str) {
    if (str != nullptr && str->data != nullptr) {
        agentxx_ffi_free(str->data);
        str->data = nullptr;
        str->size = 0;
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_strdup_n(const AgentxxStringView* s, AgentxxString* out) {
    if (out == nullptr) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    out->data = nullptr;
    out->size = 0;
    if (s == nullptr || s->data == nullptr) {
        return AGENTXX_FFI_OK;
    }
    const uint64_t size = s->size;
    auto*          p    = static_cast<char*>(agentxx_ffi_malloc(size + 1));
    if (p == nullptr) {
        return AGENTXX_FFI_ERR_OOM;
    }
    if (size > 0) {
        std::memcpy(p, s->data, static_cast<size_t>(size));
    }
    p[size]   = '\0';
    out->data = p;
    out->size = size;
    return AGENTXX_FFI_OK;
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_api_version(void) {
    return AGENTXX_FFI_API_VERSION;
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_library_version(AgentxxStringView* out) {
    if (out == nullptr) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    static const char kVersion[] = "0.1.0";
    out->data                    = kVersion;
    out->size                    = sizeof(kVersion) - 1;
    return AGENTXX_FFI_OK;
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_strerror(int32_t code, AgentxxStringView* out) {
    if (out == nullptr) {
        return AGENTXX_FFI_ERR_INVALID;
    }
    const char* str = nullptr;
    switch (code) {
        case AGENTXX_FFI_OK:
            str = "success";
            break;
        case AGENTXX_FFI_ERR_INVALID:
            str = "invalid argument";
            break;
        case AGENTXX_FFI_ERR_STATE:
            str = "invalid state";
            break;
        case AGENTXX_FFI_ERR_JSON:
            str = "JSON parse failed";
            break;
        case AGENTXX_FFI_ERR_CONFIG:
            str = "invalid configuration";
            break;
        case AGENTXX_FFI_ERR_INIT:
            str = "agent init failed";
            break;
        case AGENTXX_FFI_ERR_INTERRUPT:
            str = "interrupt id invalid/expired";
            break;
        case AGENTXX_FFI_ERR_TIMEOUT:
            str = "sync query timeout";
            break;
        case AGENTXX_FFI_ERR_OOM:
            str = "out of memory";
            break;
        case AGENTXX_FFI_ERR_INTERNAL:
            str = "internal error";
            break;
        default:
            str = "unknown error code";
            break;
    }
    out->data = str;
    out->size = static_cast<uint64_t>(std::strlen(str));
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

AgentxxFFIAgent* AGENTXX_FFI_CALL agentxx_ffi_create(
    const AgentxxStringView*   config_json,
    const AgentxxStringView*   model_json,
    const AgentxxFFICallbacks* cb,
    AgentxxString*             log
) {
    std::string err;
    try {
        auto rt = FfiAgentRuntime::create(config_json, model_json, cb, err);
        if (!rt) {
            ffiFail(AGENTXX_FFI_ERR_CONFIG, err, log);
            return nullptr;
        }
        auto* handle = new (std::nothrow) AgentxxFFIAgent();
        if (handle == nullptr) {
            err = "out of memory";
            ffiFail(AGENTXX_FFI_ERR_OOM, err, log);
            return nullptr;
        }
        handle->impl = std::move(rt);
        ffiOk(log);
        return handle;
    } catch (...) {
        ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
        return nullptr;
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_start(AgentxxFFIAgent* a, AgentxxString* log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err;
    try {
        const int rc = a->impl->start(err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_stop(AgentxxFFIAgent* a, AgentxxString* log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err;
    try {
        const int rc = a->impl->stop(err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_destroy(AgentxxFFIAgent* a, AgentxxString* log) {
    if (a == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    int         rc = AGENTXX_FFI_OK;
    std::string err;
    try {
        if (a->impl) {
            rc = a->impl->destroy(err);
        }
    } catch (...) {
        rc  = AGENTXX_FFI_ERR_INTERNAL;
        err = cxxErrText();
    }
    delete a; // 释放句柄 (impl 引用计数随之递减; destroy 已等 io 线程退出)
    return ffiFinish(rc, err, log);
}

// ---------------------------------------------------------------------------
// 会话交互 (异步)
// ---------------------------------------------------------------------------

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_send_input(AgentxxFFIAgent* a, const AgentxxStringView* text, AgentxxString* log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (text == nullptr || text->data == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null text", log);
    }
    std::string err;
    try {
        const int rc = a->impl->sendInput(svToCpp(text), err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_cancel(AgentxxFFIAgent* a, AgentxxString* log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err;
    try {
        const int rc = a->impl->cancel(err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_select_model(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* model_name,
    AgentxxString*           log
) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (model_name == nullptr || model_name->data == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null model_name", log);
    }
    std::string err;
    try {
        const int rc = a->impl->selectModel(svToCpp(model_name), err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_set_permission(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* path,
    int32_t                  allow,
    int32_t                  op,
    AgentxxString*           log
) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (path == nullptr || path->data == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null path", log);
    }
    std::string err;
    try {
        const int rc = a->impl->setPermission(svToCpp(path), allow, op, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL agentxx_ffi_switch_session(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* sessionId,
    AgentxxString*           log
) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (sessionId == nullptr || sessionId->data == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null sessionId", log);
    }
    std::string err;
    try {
        const int rc = a->impl->switchSession(svToCpp(sessionId), err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

// ---------------------------------------------------------------------------
// 同步查询
// ---------------------------------------------------------------------------

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_model_info(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log) {
    if (out == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out pointer", log);
    }
    out->data = nullptr;
    out->size = 0;
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err, res;
    try {
        res = a->impl->getModelInfo(err);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
    if (res.empty()) {
        return ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
    }
    fillString(out, res);
    ffiOk(log);
    return AGENTXX_FFI_OK;
}

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_context_messages(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log) {
    if (out == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out pointer", log);
    }
    out->data = nullptr;
    out->size = 0;
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err, res;
    try {
        res = a->impl->getContextMessages(err);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
    if (res.empty()) {
        return ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
    }
    fillString(out, res);
    ffiOk(log);
    return AGENTXX_FFI_OK;
}

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_list_sessions(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log) {
    if (out == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out pointer", log);
    }
    out->data = nullptr;
    out->size = 0;
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err, res;
    try {
        res = a->impl->listSessions(err);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
    if (res.empty()) {
        return ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
    }
    fillString(out, res);
    ffiOk(log);
    return AGENTXX_FFI_OK;
}

// ---------------------------------------------------------------------------
// HIL 中断
// ---------------------------------------------------------------------------

int32_t AGENTXX_FFI_CALL agentxx_ffi_interrupt_respond(
    AgentxxFFIAgent*         a,
    int64_t                  interrupt_id,
    const AgentxxStringView* values_json,
    AgentxxString*           log
) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err;
    try {
        const int rc = a->impl->interruptRespond(interrupt_id, values_json, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_drain_logs(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log) {
    if (out == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out pointer", log);
    }
    out->data = nullptr;
    out->size = 0;
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string res;
    try {
        res = a->impl->drainLogs();
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
    fillString(out, res);
    ffiOk(log);
    return AGENTXX_FFI_OK;
}

} // extern "C"
