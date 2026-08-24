/*
 * libexample_js.so —— example_js 插件的 C++ 壳 (统一插件模型示例)
 *
 * 所有插件统一为 C++ 插件: 本壳是 example_js 的 C++ 实现部分,
 * 脚本能力经能力调用 (agentxx.agent.capabilities 接口表 invoke_capability)
 * 委派给 interpreter 引擎插件 (agentxx_javascript_engine), 宿主不参与脚本管理。
 *
 * 加载流程 (entry):
 *   1. COM 风格接口表查询: 一次性查询缓存全部已知接口表
 *   2. 检查能力 interpreter.js 可用 (manifest depends 已保证, 此处防御)
 *   3. get_own_info 拿自身 name/path → 推导同目录 plugin.js
 *   4. invoke_capability("interpreter.js", "load", {name, path})
 *      → 引擎执行脚本; 脚本内 agentxx.registerTool 等经本壳 host
 *        注册到本插件实例 (宿主 detachAll 统一清理)
 *
 * 卸载流程 (unload):
 *   - 宿主 detachAll 摘除全部注册 (工具/钩子/订阅) 后调本回调
 *   - invoke_capability("interpreter.js", "unload", {name}) 通知引擎
 *     释放对应 JSContext (投递式)
 */
#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_iface_helper.h"

#include <cstdio>
#include <cstring>
#include <string>

static const AgentxxHost* g_host = nullptr;
/// 宿主接口表缓存 (entry 时 AgentIfaces::query 一次查询; 进程级静态数据)
static agentxx::plugin::AgentIfaces g_if{};
static std::string                  g_name; ///< 本插件名
static std::string                  g_dir;  ///< 本插件目录

/// 库路径所在目录
static std::string dirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}

/// 文件是否存在 (纯 C ABI 插件无文件系统 API, 用 stdio 探测)
static bool fileExists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) {
        std::fclose(f);
        return true;
    }
    return false;
}

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("example_js"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Example JS plugin (C++ shell + JS via interpreter.js capability)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx) {
    (void)plugin_ctx;
    g_host = host;
    // COM 风格接口表查询: entry 内一次性查询全部已知 IID 并缓存
    static const agentxx::plugin::AgentIfaces s_if = agentxx::plugin::AgentIfaces::query(host);
    g_if = s_if;
    if (!s_if.capabilities || !s_if.plugins || !s_if.json || !s_if.log) {
        return -1; // 核心依赖的接口表缺失 (宿主过简), 无法工作
    }
    auto logE = [&](const std::string& msg) {
        s_if.log->log(host, 4, agentxx_plugin_sv(msg.data(), msg.size()));
    };

    if (!s_if.capabilities->has_capability(host, AGENTXX_SV("interpreter.js"))) {
        logE("example_js: interpreter.js capability not available");
        return -1;
    }

    // 自身信息: name + 库路径 (推导插件目录); 字段解析经宿主 json_get_string
    // (对转义字符/嵌套结构可靠, 替代手写字符串扫描)
    char* info = s_if.plugins->get_own_info(host);
    if (!info) {
        logE("example_js: get_own_info failed");
        return -1;
    }
    auto field = [&](const char* key) -> std::string {
        char* v
            = s_if.json->json_get_string(host, agentxx_plugin_sv_cstr(info), AGENTXX_SV(key));
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
        logE("example_js: own info invalid");
        return -1;
    }
    g_name = name;
    g_dir  = dirOf(libPath);

    // 推导 plugin.js 路径: 默认与库同目录; Windows 多配置布局下 DLL 位于
    // {插件根}/Debug|Release/ 子目录, plugin.js 在插件根 → 上溯一级回退
    std::string scriptPath = g_dir + "/plugin.js";
    if (!fileExists(scriptPath)) {
        auto pos = g_dir.find_last_of("/\\");
        if (pos != std::string::npos) {
            std::string parent = g_dir.substr(0, pos);
            if (fileExists(parent + "/plugin.js")) {
                g_dir      = parent;
                scriptPath = parent + "/plugin.js";
            }
        }
    }

    // 委派加载脚本 (经能力调用 → 引擎执行; 脚本内注册动作挂到本插件实例)
    // - args 经 json_escape 转义字段值, 防止路径含引号/反斜杠破坏 JSON
    char* escName = s_if.json->json_escape(host, agentxx_plugin_sv(name.data(), name.size()));
    char* escPath
        = s_if.json->json_escape(host, agentxx_plugin_sv(scriptPath.data(), scriptPath.size()));
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
    char* err  = nullptr;
    char* resp = s_if.capabilities->invoke_capability(
        host,
        AGENTXX_SV("interpreter.js"),
        AGENTXX_SV("load"),
        agentxx_plugin_sv(args.data(), args.size()),
        &err
    );
    if (!resp) {
        std::string msg  = "example_js: script load failed: ";
        msg             += err ? err : "?";
        logE(msg);
        if (err) {
            host->vtable->free(err);
        }
        return -1;
    }
    host->vtable->free(resp);
    std::string okMsg = "example_js: script loaded (" + scriptPath + ")";
    s_if.log->log(host, 2, agentxx_plugin_sv(okMsg.data(), okMsg.size()));
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* plugin_ctx) {
    (void)plugin_ctx;
    if (!g_host || !g_if.capabilities || g_name.empty()) {
        return;
    }
    // 通知引擎释放本插件的 JSContext (宿主已先 detachAll 摘除全部注册)
    std::string args = "{\"name\":\"" + g_name + "\"}";
    char*       err  = nullptr;
    char*       resp = g_if.capabilities->invoke_capability(
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
