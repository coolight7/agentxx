/*
 * libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
 */
#include "agentxx/plugin/api/plugin_api.h"
#include "agentxx/plugin/api/plugin_guard.h"
#include "agentxx/plugin/api/plugin_iface_helper.h"
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

void shellLog(const ShellCtx* ctx, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        auto sv = agentxx_plugin_sv(msg.data(), msg.size());
        ctx->iface.log->log(ctx->host, 4, &sv);
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
                AGENTXX_PLUGIN_API_VERSION, 0,
                agentxx_plugin_sv_cstr("example_js"),
                agentxx_plugin_sv_cstr("1.0.0"),
                agentxx_plugin_sv_cstr(
                    "Example JS plugin (C++ shell + JS via interpreter.js capability)"
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
            shellLog(raw, msg ? msg : "");
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
                auto sv = agentxx_plugin_sv(msg.data(), msg.size());
                s_if.log->log(host, 4, &sv);
            };

            auto capSv = agentxx_plugin_sv_cstr("interpreter.js");
            if (!s_if.capabilities->has_capability(host, &capSv)) {
                logE("example_js: interpreter.js capability not available");
                return -1;
            }

            AgentxxPluginString info{nullptr, 0};
            s_if.plugins->get_own_info(host, &info);
            if (!info.data) {
                logE("example_js: get_own_info failed");
                return -1;
            }
            auto field = [&](const char* key) -> std::string {
                AgentxxPluginString v{nullptr, 0};
                auto infoSv = agentxx_plugin_string_to_sv(&info);
                auto keySv  = agentxx_plugin_sv_cstr(key);
                s_if.json->json_get_string(host, &infoSv, &keySv, &v);
                if (!v.data) {
                    return {};
                }
                std::string s(v.data, static_cast<size_t>(v.size));
                agentxx_plugin_string_free(host, &v);
                return s;
            };
            std::string libPath = field("path");
            ctx->name           = field("name");
            agentxx_plugin_string_free(host, &info);
            if (ctx->name.empty() || libPath.empty()) {
                logE("example_js: own info invalid");
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

            AgentxxPluginString escName{nullptr, 0};
            auto nameSv = agentxx_plugin_sv(ctx->name.data(), ctx->name.size());
            s_if.json->json_escape(host, &nameSv, &escName);
            AgentxxPluginString escPath{nullptr, 0};
            auto pathSv = agentxx_plugin_sv(scriptPath.data(), scriptPath.size());
            s_if.json->json_escape(host, &pathSv, &escPath);
            std::string args = fmt::format(
                "{{\"name\":{},\"path\":{}}}",
                escName.data ? escName.data : "\"\"",
                escPath.data ? escPath.data : "\"\""
            );
            if (escName.data) {
                agentxx_plugin_string_free(host, &escName);
            }
            if (escPath.data) {
                agentxx_plugin_string_free(host, &escPath);
            }
            AgentxxPluginString err{nullptr, 0};
            auto capSv2 = agentxx_plugin_sv_cstr("interpreter.js");
            auto loadSv = agentxx_plugin_sv_cstr("load");
            auto argsSv = agentxx_plugin_sv(args.data(), args.size());
            auto* h     = s_if.capabilities->invoke_capability_async(
                host,
                &capSv2,
                &loadSv,
                &argsSv,
                nullptr,
                nullptr,
                &err
            );
            if (!h) {
                std::string errStr = err.data ? std::string(err.data, static_cast<size_t>(err.size)) : "load script async dispatch failed";
                if (err.data) {
                    agentxx_plugin_string_free(host, &err);
                }
                logE(fmt::format("example_js: {}", errStr));
                return -1;
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
            shellLog(ctx, msg ? msg : "");
        },
        [&] {
            if (!ctx || !ctx->host) {
                delete ctx;
                return;
            }
            const AgentxxPluginHost* host = ctx->host;
            if (ctx->iface.capabilities && !ctx->name.empty() && ctx->iface.json) {
                AgentxxPluginString esc{nullptr, 0};
                auto nameSv = agentxx_plugin_sv(ctx->name.data(), ctx->name.size());
                ctx->iface.json->json_escape(host, &nameSv, &esc);
                std::string args = fmt::format("{{\"name\":{}}}", esc.data ? esc.data : "\"\"");
                if (esc.data) {
                    agentxx_plugin_string_free(host, &esc);
                }
                AgentxxPluginString err{nullptr, 0};
                auto capSv = agentxx_plugin_sv_cstr("interpreter.js");
                auto unloadSv = agentxx_plugin_sv_cstr("unload");
                auto argsSv   = agentxx_plugin_sv(args.data(), args.size());
                auto* h       = ctx->iface.capabilities->invoke_capability_async(
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
                        agentxx_plugin_string_free(host, &err);
                    }
                } else if (err.data) {
                    agentxx_plugin_string_free(host, &err);
                }
            }
            delete ctx;
        }
    );
}
