/*
 * libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
 *
 * 所有插件统一为 C++ 插件: 本壳是 example_js 的 C++ 实现部分,
 * 脚本能力经能力调用 (invoke_capability) 委派给 interpreter 引擎插件
 * (agentxx_javascript_engine), 宿主不参与脚本管理。
 *
 * 加载流程 (entry):
 *   1. 检查能力 interpreter.js 可用 (manifest depends 已保证, 此处防御)
 *   2. get_own_info 拿自身 name/path → 推导同目录 plugin.js
 *   3. invoke_capability("interpreter.js", "load", {name, path})
 *      → 引擎执行脚本; 脚本内 agentxx.registerTool 等经本壳 host
 *        注册到本插件实例 (宿主 detachAll 统一清理)
 *
 * 卸载流程 (unload):
 *   - 宿主 detachAll 摘除全部注册 (工具/钩子/订阅) 后调本回调
 *   - invoke_capability("interpreter.js", "unload", {name}) 通知引擎
 *     释放对应 JSContext (投递式)
 */
#include "agentxx/plugin/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <string>

static const AgentxxHost* g_host = nullptr;
static std::string        g_name; ///< 本插件名
static std::string        g_dir;  ///< 本插件目录

/// 库路径所在目录
static std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("example_js"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Example JS plugin (C++ shell + JS via interpreter.js capability)"),
    };
    return &info;
}

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    (void)plugin_ctx;
    g_host = host;
    if (!host->vtable->has_capability(host, AGENTXX_SV("interpreter.js"))) {
        host->vtable->log(host, 4, AGENTXX_SV("example_js: interpreter.js capability not available"));
        return -1;
    }

    // 自身信息: name + 库路径 (推导插件目录); 字段解析经宿主 json_get_string
    // (对转义字符/嵌套结构可靠, 替代手写字符串扫描)
    char* info = host->vtable->get_own_info(host);
    if (!info) {
        host->vtable->log(host, 4, AGENTXX_SV("example_js: get_own_info failed"));
        return -1;
    }
    auto field = [&](const char* key) -> std::string {
        char* v = host->vtable->json_get_string(host, agentxx_plugin_sv_cstr(info), AGENTXX_SV(key));
        if (!v) {
            return {};
        }
        std::string s = v;
        host->vtable->free(v);
        return s;
    };
    std::string libPath = field("path");
    std::string name    = field("name");
    host->vtable->free(info);
    if (name.empty() || libPath.empty()) {
        host->vtable->log(host, 4, AGENTXX_SV("example_js: own info invalid"));
        return -1;
    }
    g_name = name;
    g_dir  = dirOf(libPath);

    // 委派加载脚本 (经能力调用 → 引擎执行; 脚本内注册动作挂到本插件实例)
    // - args 经 json_escape 转义字段值, 防止路径含引号/反斜杠破坏 JSON
    std::string scriptPath = g_dir + "/plugin.js";
    char*       escName    = host->vtable->json_escape(host, agentxx_plugin_sv(name.data(), name.size()));
    char*       escPath    = host->vtable->json_escape(host, agentxx_plugin_sv(scriptPath.data(), scriptPath.size()));
    std::string args       = "{\"name\":";
    args                  += escName ? escName : "\"\"";
    args                  += ",\"path\":";
    args                  += escPath ? escPath : "\"\"";
    args                  += "}";
    if (escName) {
        host->vtable->free(escName);
    }
    if (escPath) {
        host->vtable->free(escPath);
    }
    char* err = nullptr;
    char* resp = host->vtable->invoke_capability(
        host,
        AGENTXX_SV("interpreter.js"),
        AGENTXX_SV("load"),
        agentxx_plugin_sv(args.data(), args.size()),
        &err
    );
    if (!resp) {
        std::string msg  = "example_js: script load failed: ";
        msg             += err ? err : "?";
        host->vtable->log(host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
        if (err) {
            host->vtable->free(err);
        }
        return -1;
    }
    host->vtable->free(resp);
    std::string okMsg = "example_js: script loaded (" + scriptPath + ")";
    host->vtable->log(host, 2, agentxx_plugin_sv(okMsg.data(), okMsg.size()));
    return 0;
}

extern "C" void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_host || g_name.empty()) {
        return;
    }
    // 通知引擎释放本插件的 JSContext (宿主已先 detachAll 摘除全部注册)
    std::string args = "{\"name\":\"" + g_name + "\"}";
    char*       err  = nullptr;
    char* resp = g_host->vtable->invoke_capability(
        g_host,
        AGENTXX_SV("interpreter.js"),
        AGENTXX_SV("unload"),
        agentxx_plugin_sv(args.data(), args.size()),
        &err
    );
    if (resp) {
        g_host->vtable->free(resp);
    }
    if (err) {
        g_host->vtable->free(err);
    }
    g_host = nullptr;
}
