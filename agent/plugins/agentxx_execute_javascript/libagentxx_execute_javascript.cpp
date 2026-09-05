/// libagentxx_execute_javascript.so —— JS 执行工具插件的 C++ 壳
/// 仿 example_js / agentxx_execute_command 结构：
/// - 本体为 C++ 插件，entry 指向 libagentxx_execute_javascript.so
/// - create 阶段经 interpreter.js 能力把同目录 plugin.js 交给 QuickJS 引擎执行
/// - plugin.js 内注册 agentxx_execute_javascript 工具（仿照 agentxx_execute_command）

#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_kit.h"

#include "fmt/format.h"
#include <filesystem>
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
        auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
        ctx->iface.log->log(ctx->host, level, &sv);
    }
}

std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

bool fileExists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    return agentxx::plugin::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
            static const AgentxxPluginInfo info{
                AGENTXX_PLUGIN_API_VERSION,
                0,
                agentxx::plugin::PluginStringView::fromCstr("agentxx_execute_javascript"),
                agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
                agentxx::plugin::PluginStringView::fromCstr(
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
    return agentxx::plugin::guardCall(
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
                auto sv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
                s_if.log->log(host, 4, &sv);
            };

            auto capSv = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
            if (!s_if.capabilities->has_capability(host, &capSv)) {
                logE(
                    "agentxx_execute_javascript: interpreter.js capability not available (need agentxx_javascript_engine)"
                );
                return -1;
            }

            AgentxxPluginString info{nullptr, 0};
            s_if.plugins->get_own_info(host, &info);
            if (!info.data) {
                logE("agentxx_execute_javascript: get_own_info failed");
                return -1;
            }
            auto field = [&](const char* key) -> std::string {
                AgentxxPluginString v{nullptr, 0};
                auto                infoSv = agentxx::plugin::PluginStringView::toSv(&info);
                auto                keySv  = agentxx::plugin::PluginStringView::fromCstr(key);
                s_if.json->json_get_string(host, &infoSv, &keySv, &v);
                if (!v.data) {
                    return {};
                }
                std::string s(v.data, static_cast<size_t>(v.size));
                agentxx::plugin::PluginString::free(host, &v);
                return s;
            };
            std::string libPath = field("path");
            ctx->name           = field("name");
            agentxx::plugin::PluginString::free(host, &info);
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

            AgentxxPluginString escName{nullptr, 0};
            auto                nameSv
                = agentxx::plugin::PluginStringView::from(ctx->name.data(), ctx->name.size());
            s_if.json->json_escape(host, &nameSv, &escName);
            AgentxxPluginString escPath{nullptr, 0};
            auto                pathSv
                = agentxx::plugin::PluginStringView::from(scriptPath.data(), scriptPath.size());
            s_if.json->json_escape(host, &pathSv, &escPath);
            std::string args = fmt::format(
                "{{\"name\":{},\"path\":{}}}",
                escName.data ? escName.data : "\"\"",
                escPath.data ? escPath.data : "\"\""
            );
            if (escName.data) {
                agentxx::plugin::PluginString::free(host, &escName);
            }
            if (escPath.data) {
                agentxx::plugin::PluginString::free(host, &escPath);
            }
            {
                auto msg
                    = fmt::format("agentxx_execute_javascript: load scriptPath={}", scriptPath);
                auto msgSv = agentxx::plugin::PluginStringView::from(msg.data(), msg.size());
                s_if.log->log(host, 2, &msgSv);
            }

            AgentxxPluginString err{nullptr, 0};
            auto  capSv2 = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
            auto  loadSv = agentxx::plugin::PluginStringView::fromCstr("load");
            auto  argsSv = agentxx::plugin::PluginStringView::from(args.data(), args.size());
            auto* h      = s_if.capabilities->invoke_capability_async(
                host,
                &capSv2,
                &loadSv,
                &argsSv,
                [](void* ud, int32_t status, const AgentxxPluginStringView* payload) {
                    auto*            c = static_cast<ShellCtx*>(ud);
                    std::string_view pl
                        = payload && payload->data
                                   ? std::string_view(payload->data, static_cast<size_t>(payload->size))
                                   : "";
                    if (status != AGENTXX_PLUGIN_OPERATOR_OK) {
                        shellLog(
                            c,
                            4,
                            fmt::format(
                                "agentxx_execute_javascript: interpreter load failed: {}",
                                pl.empty() ? "unknown" : pl
                            )
                        );
                    } else {
                        shellLog(
                            c,
                            2,
                            fmt::format("agentxx_execute_javascript: interpreter load ok: {}", pl)
                        );
                    }
                },
                ctx.get(),
                &err
            );
            if (!h) {
                std::string errStr = err.data ? std::string(err.data, static_cast<size_t>(err.size))
                                              : "load script async dispatch failed";
                if (err.data) {
                    agentxx::plugin::PluginString::free(host, &err);
                }
                logE(fmt::format("agentxx_execute_javascript: {}", errStr));
                return -1;
            }
            // 可选：立即日志，避免静默
            {
                auto msgSv = agentxx::plugin::PluginStringView::fromCstr(
                    "agentxx_execute_javascript: load plugin.js dispatched to interpreter.js"
                );
                s_if.log->log(host, 2, &msgSv);
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(
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
                AgentxxPluginString esc{nullptr, 0};
                auto                nameSv
                    = agentxx::plugin::PluginStringView::from(ctx->name.data(), ctx->name.size());
                ctx->iface.json->json_escape(host, &nameSv, &esc);
                std::string args = fmt::format("{{\"name\":{}}}", esc.data ? esc.data : "\"\"");
                if (esc.data) {
                    agentxx::plugin::PluginString::free(host, &esc);
                }
                AgentxxPluginString err{nullptr, 0};
                auto  capSv    = agentxx::plugin::PluginStringView::fromCstr("interpreter.js");
                auto  unloadSv = agentxx::plugin::PluginStringView::fromCstr("unload");
                auto  argsSv   = agentxx::plugin::PluginStringView::from(args.data(), args.size());
                auto* h        = ctx->iface.capabilities->invoke_capability_async(
                    host,
                    &capSv,
                    &unloadSv,
                    &argsSv,
                    nullptr,
                    nullptr,
                    &err
                );
                if (!h) {
                    if (err.data) {
                        agentxx::plugin::PluginString::free(host, &err);
                    }
                } else if (err.data) {
                    agentxx::plugin::PluginString::free(host, &err);
                }
            }
            delete ctx;
        }
    );
}
