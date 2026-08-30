// ffi_api.cpp —— libagentxx FFI C API 入口实现
//
// 本文件是唯一允许跨语言 (FFI) 调用的导出面: 全部函数为 extern "C" + 纯 C
// 参数/返回 (无 STL/异常出界), 内部统一 catchError 兜底并转错误码; 每个
// 函数可选的 char** log 参数用于向调用方回传执行过程中的错误/日志详情 (宿主
// 用后必须 agentxx_ffi_free 释放)。符号导出白名单见 lib/ffi_symbols.map。

#include "agentxx/ffi_api.h"
#include "agentxx/util/log.h"
#include "ffi_runtime.h"

#include <cstring>
#include <memory>
#include <string>

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

/// 失败: 填 log (如有) 并返回错误码
int ffiFail(int code, const std::string& detail, char** log) {
    if (log != nullptr) {
        *log = agentxx_ffi_strdup_n(detail.data(), detail.size());
    }
    return code;
}

/// 成功: log 置 NULL, 返回 0
int ffiOk(char** log) {
    if (log != nullptr) {
        *log = nullptr;
    }
    return AGENTXX_FFI_OK;
}

/// 按 rc/err 填 log 并返回 rc (非 0 且 err 为空时标注内部错误)
int ffiFinish(int rc, const std::string& err, char** log) {
    if (log == nullptr) {
        return rc;
    }
    *log = rc == 0 ? nullptr
                   : agentxx_ffi_strdup_n(
                       err.empty() ? "internal error (no detail)" : err.data(),
                       err.empty() ? 0 : err.size()
                   );
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

void* agentxx_ffi_malloc(size_t size) {
    return malloc(size);
}

void agentxx_ffi_free(const void* ptr) {
    XX_LOGD("agentxx_ffi_free : {}", ptr);
    // 如果此处出错，也可能是在此之前 ptr 已经越界访问，释放时 debug
    // 检查出存在越界写入
    free(const_cast<void*>(ptr));
}

char* agentxx_ffi_strdup_n(const char* s, size_t size) {
    if (s == nullptr) {
        return nullptr;
    }
    auto* p = static_cast<char*>(agentxx_ffi_malloc(size + 1));
    if (p == nullptr) {
        return nullptr;
    }
    if (size > 0) {
        std::memcpy(p, s, size);
    }
    p[size] = '\0';
    return p;
}

int agentxx_ffi_api_version(void) {
    return AGENTXX_FFI_API_VERSION;
}

const char* agentxx_ffi_library_version(void) {
    return "0.1.0";
}

const char* agentxx_ffi_strerror(int code) {
    switch (code) {
        case AGENTXX_FFI_OK:
            return "success";
        case AGENTXX_FFI_ERR_INVALID:
            return "invalid argument";
        case AGENTXX_FFI_ERR_STATE:
            return "invalid state";
        case AGENTXX_FFI_ERR_JSON:
            return "JSON parse failed";
        case AGENTXX_FFI_ERR_CONFIG:
            return "invalid configuration";
        case AGENTXX_FFI_ERR_INIT:
            return "agent init failed";
        case AGENTXX_FFI_ERR_INTERRUPT:
            return "interrupt id invalid/expired";
        case AGENTXX_FFI_ERR_TIMEOUT:
            return "sync query timeout";
        case AGENTXX_FFI_ERR_OOM:
            return "out of memory";
        case AGENTXX_FFI_ERR_INTERNAL:
            return "internal error";
        default:
            return "unknown error code";
    }
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

AgentxxFFIAgent* agentxx_ffi_create(
    const char*                config_json,
    const char*                model_json,
    const AgentxxFFICallbacks* cb,
    char**                     log
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

int agentxx_ffi_start(AgentxxFFIAgent* a, char** log) {
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

int agentxx_ffi_stop(AgentxxFFIAgent* a, char** log) {
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

int agentxx_ffi_destroy(AgentxxFFIAgent* a, char** log) {
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

int agentxx_ffi_send_input(AgentxxFFIAgent* a, const char* text, char** log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (text == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null text", log);
    }
    std::string err;
    try {
        const int rc = a->impl->sendInput(text, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int agentxx_ffi_cancel(AgentxxFFIAgent* a, char** log) {
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

int agentxx_ffi_select_model(AgentxxFFIAgent* a, const char* model_name, char** log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (model_name == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null model_name", log);
    }
    std::string err;
    try {
        const int rc = a->impl->selectModel(model_name, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int agentxx_ffi_set_permission(
    AgentxxFFIAgent* a,
    const char*      path,
    int              allow,
    int              op,
    char**           log
) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (path == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null path", log);
    }
    std::string err;
    try {
        const int rc = a->impl->setPermission(path, allow, op, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int agentxx_ffi_switch_session(AgentxxFFIAgent* a, const char* sessionId, char** log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (sessionId == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null sessionId", log);
    }
    std::string err;
    try {
        const int rc = a->impl->switchSession(sessionId, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

// ---------------------------------------------------------------------------
// 同步查询
// ---------------------------------------------------------------------------

char* agentxx_ffi_get_model_info(AgentxxFFIAgent* a, char** log) {
    if (a == nullptr || !a->impl) {
        ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
        return nullptr;
    }
    std::string err, out;
    try {
        out = a->impl->getModelInfo(err);
    } catch (...) {
        ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
        return nullptr;
    }
    if (out.empty()) {
        ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
        return nullptr;
    }
    ffiOk(log);
    return agentxx_ffi_strdup_n(out.data(), out.size());
}

char* agentxx_ffi_get_context_messages(AgentxxFFIAgent* a, char** log) {
    if (a == nullptr || !a->impl) {
        ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
        return nullptr;
    }
    std::string err, out;
    try {
        out = a->impl->getContextMessages(err);
    } catch (...) {
        ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
        return nullptr;
    }
    if (out.empty()) {
        ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
        return nullptr;
    }
    ffiOk(log);
    return agentxx_ffi_strdup_n(out.data(), out.size());
}

char* agentxx_ffi_list_sessions(AgentxxFFIAgent* a, char** log) {
    if (a == nullptr || !a->impl) {
        ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
        return nullptr;
    }
    std::string err, out;
    try {
        out = a->impl->listSessions(err);
    } catch (...) {
        ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
        return nullptr;
    }
    if (out.empty()) {
        ffiFail(AGENTXX_FFI_ERR_TIMEOUT, err, log);
        return nullptr;
    }
    ffiOk(log);
    return agentxx_ffi_strdup_n(out.data(), out.size());
}

// ---------------------------------------------------------------------------
// HIL 中断
// ---------------------------------------------------------------------------

int agentxx_ffi_interrupt_respond(
    AgentxxFFIAgent* a,
    int64_t          interrupt_id,
    const char*      values_json,
    char**           log
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

char* agentxx_ffi_drain_logs(AgentxxFFIAgent* a, char** log) {
    if (a == nullptr || !a->impl) {
        ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
        return nullptr;
    }
    std::string out;
    try {
        out = a->impl->drainLogs();
    } catch (...) {
        ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
        return nullptr;
    }
    ffiOk(log);
    return agentxx_ffi_strdup_n(out.data(), out.size());
}

} // extern "C"