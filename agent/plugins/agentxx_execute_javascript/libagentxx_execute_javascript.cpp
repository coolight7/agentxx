/*
 * libagentxx_execute_javascript.so —— JS 执行工具插件的 C++ 壳
 * 仿 example_js / agentxx_execute_command 结构：
 * - 本体为 C++ 插件，entry 指向 libagentxx_execute_javascript.so
 * - create 阶段经 interpreter.js 能力把同目录 plugin.js 交给 QuickJS 引擎执行
 * - plugin.js 内注册 agentxx_execute_javascript 工具（对标 bash 版）
 */

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_kit.h"

#include "fmt/format.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

struct ShellCtx {
    const AgentxxPluginHost*     host = nullptr;
    agentxx::plugin::AgentIfaces iface{};
    std::string                  name;
    std::string                  dir;
};

void shellLog(const ShellCtx* ctx, int level, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        ctx->iface.log->log(ctx->host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) {
        std::fclose(f);
        return true;
    }
    return false;
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                agentxx_plugin_sv_cstr("agentxx_execute_javascript"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr(
                    "Execute JavaScript code (JS equivalent of bash command) via QuickJS"
                ),
            };
            return &info;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    ShellCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* msg) noexcept {
            shellLog(raw, 4, msg ? msg : "");
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx   = std::make_unique<ShellCtx>();
            ctx->host  = host;
            ctx->iface = agentxx::plugin::AgentIfaces::query(host);
            raw        = ctx.get();
            if (!ctx->iface.capabilities || !ctx->iface.plugins || !ctx->iface.json
                || !ctx->iface.log) {
                return -1;
            }
            const auto& s_if = ctx->iface;
            auto        logE = [&](const std::string& msg) {
                s_if.log->log(host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
            };

            if (!s_if.capabilities
                     ->has_capability(host, agentxx_plugin_sv_cstr("interpreter.js"))) {
                logE(
                    "agentxx_execute_javascript: interpreter.js capability not available (need agentxx_javascript_engine)"
                );
                return -1;
            }

            char* info = s_if.plugins->get_own_info(host);
            if (!info) {
                logE("agentxx_execute_javascript: get_own_info failed");
                return -1;
            }
            auto field = [&](const char* key) -> std::string {
                char* v = s_if.json->json_get_string(
                    host,
                    agentxx_plugin_sv_cstr(info),
                    agentxx_plugin_sv_cstr(key)
                );
                if (!v) {
                    return {};
                }
                std::string s = v;
                host->vtable->free(v);
                return s;
            };
            std::string libPath = field("path");
            ctx->name           = field("name");
            host->vtable->free(info);
            if (ctx->name.empty() || libPath.empty()) {
                logE("agentxx_execute_javascript: own info invalid");
                return -1;
            }
            ctx->dir = dirOf(libPath);

            std::string scriptPath = fmt::format("{}/plugin.js", ctx->dir);
            if (!fileExists(scriptPath)) {
                auto pos = ctx->dir.find_last_of("/\\");
                if (pos != std::string::npos) {
                    std::string parent = ctx->dir.substr(0, pos);
                    if (fileExists(fmt::format("{}/plugin.js", parent))) {
                        ctx->dir   = parent;
                        scriptPath = fmt::format("{}/plugin.js", parent);
                    }
                }
            }
            // 开发期兜底：若按 get_own_info 推导的 plugin.js 不存在，尝试源码目录
            if (!fileExists(scriptPath)) {
                // 尝试源码树相对路径（独立构建/源码运行）
                // host 传入的 path 在内置模式下可能为虚路径，此分支仅为本地调试兜底
                std::string fallback = "agent/plugins/agentxx_execute_javascript/plugin.js";
                if (fileExists(fallback)) {
                    scriptPath = fallback;
                }
            }

            char* escName = s_if.json->json_escape(
                host,
                agentxx_plugin_sv(ctx->name.data(), ctx->name.size())
            );
            char* escPath = s_if.json->json_escape(
                host,
                agentxx_plugin_sv(scriptPath.data(), scriptPath.size())
            );
            std::string args = fmt::format(
                "{{\"name\":{},\"path\":{}}}",
                escName ? escName : "\"\"",
                escPath ? escPath : "\"\""
            );
            if (escName) {
                host->vtable->free(escName);
            }
            if (escPath) {
                host->vtable->free(escPath);
            }
            s_if.log->log(
                host,
                2,
                agentxx_plugin_sv_cstr(
                    fmt::format("agentxx_execute_javascript: load scriptPath={}", scriptPath)
                        .c_str()
                )
            );

            char* err = nullptr;
            auto* h   = s_if.capabilities->invoke_capability_async(
                host,
                agentxx_plugin_sv_cstr("interpreter.js"),
                agentxx_plugin_sv_cstr("load"),
                agentxx_plugin_sv(args.data(), args.size()),
                [](void* ud, int status, char* payload) {
                    auto* c = static_cast<ShellCtx*>(ud);
                    if (status != AGENTXX_PLUGIN_OPERATOR_OK) {
                        shellLog(
                            c,
                            4,
                            fmt::format(
                                "agentxx_execute_javascript: interpreter load failed: {}",
                                payload ? payload : "unknown"
                            )
                        );
                    } else {
                        shellLog(
                            c,
                            2,
                            fmt::format(
                                "agentxx_execute_javascript: interpreter load ok: {}",
                                payload ? payload : ""
                            )
                        );
                    }
                    if (payload && c && c->host && c->host->vtable && c->host->vtable->free) {
                        c->host->vtable->free(payload);
                    }
                },
                ctx.get(),
                &err
            );
            if (!h) {
                std::string errStr = err ? err : "load script async dispatch failed";
                if (err) {
                    host->vtable->free(err);
                }
                logE(fmt::format("agentxx_execute_javascript: {}", errStr));
                return -1;
            }
            // 可选：立即日志，避免静默
            s_if.log->log(
                host,
                2,
                agentxx_plugin_sv_cstr(
                    "agentxx_execute_javascript: load plugin.js dispatched to interpreter.js"
                )
            );
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* msg) noexcept {
            shellLog(ctx, 4, msg ? msg : "");
        },
        [&] {
            if (!ctx || !ctx->host) {
                delete ctx;
                return;
            }
            const AgentxxPluginHost* host = ctx->host;
            if (ctx->iface.capabilities && !ctx->name.empty() && ctx->iface.json) {
                char* esc = ctx->iface.json->json_escape(
                    host,
                    agentxx_plugin_sv(ctx->name.data(), ctx->name.size())
                );
                std::string args = fmt::format("{{\"name\":{}}}", esc ? esc : "\"\"");
                if (esc) {
                    host->vtable->free(esc);
                }
                char* err = nullptr;
                auto* h   = ctx->iface.capabilities->invoke_capability_async(
                    host,
                    agentxx_plugin_sv_cstr("interpreter.js"),
                    agentxx_plugin_sv_cstr("unload"),
                    agentxx_plugin_sv(args.data(), args.size()),
                    nullptr,
                    nullptr,
                    &err
                );
                if (!h) {
                    if (err) {
                        host->vtable->free(err);
                    }
                } else if (err) {
                    host->vtable->free(err);
                }
            }
            delete ctx;
        }
    );
}
