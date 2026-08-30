/*
 * libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
 */
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_kit.h"

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

void shellLog(const ShellCtx* ctx, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        ctx->iface.log->log(ctx->host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
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
                AGENTXX_PLUGIN_SV("example_js"),
                AGENTXX_PLUGIN_SV("1.0.0"),
                AGENTXX_PLUGIN_SV("Example JS plugin (C++ shell + JS via interpreter.js capability)"
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
                s_if.log->log(host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
            };

            if (!s_if.capabilities->has_capability(host, AGENTXX_PLUGIN_SV("interpreter.js"))) {
                logE("example_js: interpreter.js capability not available");
                return -1;
            }

            char* info = s_if.plugins->get_own_info(host);
            if (!info) {
                logE("example_js: get_own_info failed");
                return -1;
            }
            auto field = [&](const char* key) -> std::string {
                char* v = s_if.json->json_get_string(
                    host,
                    agentxx_plugin_sv_cstr(info),
                    AGENTXX_PLUGIN_SV(key)
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
                logE("example_js: own info invalid");
                return -1;
            }
            ctx->dir = dirOf(libPath);

            std::string scriptPath = ctx->dir + "/plugin.js";
            if (!fileExists(scriptPath)) {
                auto pos = ctx->dir.find_last_of("/\\");
                if (pos != std::string::npos) {
                    std::string parent = ctx->dir.substr(0, pos);
                    if (fileExists(parent + "/plugin.js")) {
                        ctx->dir   = parent;
                        scriptPath = parent + "/plugin.js";
                    }
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
            std::string args  = "{\"name\":";
            args             += escName ? escName : "\"\"";
            args             += ",\"path\":";
            args             += escPath ? escPath : "\"\"";
            args             += "}";
            if (escName) {
                host->vtable->free(escName);
            }
            if (escPath) {
                host->vtable->free(escPath);
            }
            char* err = nullptr;
            auto* h = s_if.capabilities->invoke_capability_async(
                host,
                AGENTXX_SV("interpreter.js"),
                AGENTXX_SV("load"),
                agentxx_plugin_sv(args.data(), args.size()),
                nullptr, nullptr, &err);
            if (!h) {
                std::string errStr = err ? err : "load script async dispatch failed";
                if (err) host->vtable->free(err);
                logE(std::string("example_js: ") + errStr);
                return -1;
            }
            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<ShellCtx*>(plugin_ctx);
    agentxx::plugin_guard::guardCallVoid(
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
                char* esc = ctx->iface.json->json_escape(
                    host,
                    agentxx_plugin_sv(ctx->name.data(), ctx->name.size())
                );
                std::string args = std::string("{\"name\":") + (esc ? esc : "\"\"") + "}";
                if (esc) host->vtable->free(esc);
                char* err = nullptr;
                auto* h = ctx->iface.capabilities->invoke_capability_async(
                    host,
                    AGENTXX_SV("interpreter.js"),
                    AGENTXX_SV("unload"),
                    agentxx_plugin_sv(args.data(), args.size()),
                    nullptr, nullptr, &err);
                if (!h) { if(err) host->vtable->free(err); }
                else if (err) host->vtable->free(err);
            }
            delete ctx;
        }
    );
}
