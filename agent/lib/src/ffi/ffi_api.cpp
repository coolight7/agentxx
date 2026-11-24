/// 本文件是唯一允许跨语言 (FFI) 调用的导出面: 全部函数为 extern "C" + 纯 C
/// 参数/返回 (无 STL/异常出界), 内部统一交给 catchError 处理并转错误码; 每个
/// 函数可选的 AgentxxString* log 参数用于向调用方回传执行过程中的错误/日志详情
/// (嵌入方程序用后必须 agentxx_ffi_string_free 释放)。符号导出白名单见
/// lib/ffi_symbols.map。
///
/// 导出面只有三类:
/// - 内存 / 版本 / 错误码 (含两个本地同步查询: get_capabilities / drain_logs)
/// - 生命周期 (create / start / stop / destroy)
/// - 调用 (call / call_cancel): 所有业务操作都在 call 的 op 里, 新增能力不加符号
#include "agentxx/ffi_api.h"
#include "agentxx/version.h"
#include "ffi_ops.h"
#include "ffi_runtime.h"
#include "utilxx_base/log.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

using agentxx::ffi::FfiAgentRuntime;
using agentxx::ffi::toSv;

namespace {

/// 捕获当前异常为可读文本 (仅供 C 边界保护)
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

/// 成功但带一条说明 (如 call_cancel 命中"已完成"时的幂等提示)
int32_t ffiOkWithInfo(const std::string& info, AgentxxString* log) {
    if (log != nullptr) {
        fillString(log, info);
    }
    return AGENTXX_FFI_OK;
}

/// 校验回调结构体: struct_size 必须够大 (允许更大, 后续版本扩展字段)
bool callbacksValid(const AgentxxFFICallbacks* cb, std::string& err) {
    if (cb == nullptr) {
        return true; // headless
    }
    if (cb->struct_size < sizeof(AgentxxFFICallbacks)) {
        err = "callbacks.struct_size 必须 >= sizeof(AgentxxFFICallbacks)";
        return false;
    }
    if (cb->reserved != 0) {
        err = "callbacks.reserved 必须为 0";
        return false;
    }
    return true;
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
    if (ptr != nullptr) {
        free(const_cast<void*>(ptr));
    }
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
    out->data = agentxx::kVersion.data();
    out->size = static_cast<uint64_t>(agentxx::kVersion.size());
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
            str = "call timeout";
            break;
        case AGENTXX_FFI_ERR_OOM:
            str = "out of memory";
            break;
        case AGENTXX_FFI_ERR_CANCELED:
            str = "call canceled";
            break;
        case AGENTXX_FFI_ERR_REJECTED:
            str = "rejected by server";
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

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_get_capabilities(AgentxxFFIAgent* a, AgentxxString* out, AgentxxString* log) {
    if (out == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out pointer", log);
    }
    out->data = nullptr;
    out->size = 0;
    try {
        std::string capsJson;
        if (a != nullptr && a->impl) {
            capsJson = a->impl->getCapabilities();
        } else {
            // 句柄为空时给同一份清单 (本库编译进来的那一套)
            utilxx_base::Json ops = utilxx_base::Json::array();
            for (const auto& def : agentxx::ffi::opsDefinition()) {
                ops.push_back(std::string{def.name});
            }
            utilxx_base::Json j = {
                {"apiVersion",     AGENTXX_FFI_API_VERSION               },
                {"libraryVersion", std::string{agentxx::kVersion}        },
                {"ops",            std::move(ops)                       },
                {"features",
                 utilxx_base::Json::array(
                     {"call_api", "call_cancel", "event_owned_payload", "async_stop", "op_ack"}
                 )                                                   },
            };
            capsJson = j.dump();
        }
        fillString(out, capsJson);
        return AGENTXX_FFI_OK;
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
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
        if (!callbacksValid(cb, err)) {
            ffiFail(AGENTXX_FFI_ERR_INVALID, err, log);
            return nullptr;
        }
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
            if (rc != AGENTXX_FFI_OK) {
                // 状态不允许: 不释放句柄 (调用方还要用, 先 stop 再等 EVT_STOPPED)
                return ffiFinish(rc, err, log);
            }
        }
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
    delete a; // 释放句柄 (impl 引用计数随之递减)
    return ffiOk(log);
}

// ---------------------------------------------------------------------------
// 调用 (提交 + 回执)
// ---------------------------------------------------------------------------

int32_t AGENTXX_FFI_CALL agentxx_ffi_call(
    AgentxxFFIAgent*         a,
    const AgentxxStringView* request_json,
    uint64_t*                out_req_id,
    AgentxxString*           log
) {
    if (out_req_id == nullptr) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null out_req_id", log);
    }
    *out_req_id = 0;
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    if (request_json == nullptr || request_json->data == nullptr || request_json->size == 0) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null or empty request_json", log);
    }
    std::string err;
    try {
        const int rc = a->impl->call(toSv(request_json), out_req_id, err);
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

int32_t AGENTXX_FFI_CALL
    agentxx_ffi_call_cancel(AgentxxFFIAgent* a, uint64_t req_id, AgentxxString* log) {
    if (a == nullptr || !a->impl) {
        return ffiFail(AGENTXX_FFI_ERR_INVALID, "null handle", log);
    }
    std::string err;
    try {
        const int rc = a->impl->callCancel(req_id, err);
        if (rc == AGENTXX_FFI_OK) {
            // 正常取消时 err 为空; 幂等命中 (已完成) 时带一条说明
            return err.empty() ? ffiOk(log) : ffiOkWithInfo(err, log);
        }
        return ffiFinish(rc, err, log);
    } catch (...) {
        return ffiFail(AGENTXX_FFI_ERR_INTERNAL, cxxErrText(), log);
    }
}

// ---------------------------------------------------------------------------
// 本地同步
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
    return ffiOk(log);
}

} // extern "C"
