// agentxx_screen_capture —— 屏幕捕获插件 (Windows)
// - 从 agentxx_computer_use 拆分独立: 单帧/全部屏幕/鼠标屏/流式推送
// - 注册工具: agentxx_screen_capture (合并 capture_all / capture_mouse / capture_screen /
// get_screen_count / streaming)
// - 流式帧经 publish 事件推送 (topic "agentxx_screen_capture.frame")
// - 像素数据不进入会话消息: 捕获帧经 WIC 编码为 PNG 落盘到宿主 dataDir 的
//   captures/ 目录, 工具结果只包含元信息 + 文件路径 (消息保持 KB 级);
//   避免数 MB 的 base64 像素直接写入 tool 消息 (上下文爆炸/持久化放大/LLM 不可读)
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include "screen_capture.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include "screen_capture_plugin.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_screen_capture_plugin {

// =====================================================================
// 工具注册辅助
// =====================================================================

// =====================================================================
// 每实例上下文 (多实例契约: 原进程级 static 存储/捕获单例/目录缓存全部移入)
// =====================================================================

/// 注册工具执行闭包条目 (经 user_data 传递; unique_ptr 保证地址稳定;
/// 实例生命周期内有效)
struct ToolEntry {
    std::function<std::string(SimpleJson&)> fn;
    const AgentxxHost*                      host = nullptr;
};

struct ScreenCaptureHolder; ///< 前置声明 (完整定义见下方流式采集节)

/// 每实例上下文
struct PluginCtx {
    const AgentxxHost*           host  = nullptr;
    agentxx::plugin::AgentIfaces iface {};
    /// spec 字符串存储 + 工具闭包 + 垫片适配器 (随实例销毁释放)
    std::vector<std::string> storage;
    std::vector<std::unique_ptr<ToolEntry>> tool_entries;
    std::vector<std::unique_ptr<AgentxxSyncToolShim>> sync_tool_shims;
    /// 截图 PNG 落盘目录 {dataDir}/captures (空 = 禁用落盘; create 时装配)
    std::string captures_dir;
    /// XX_LOG* 宏路由 Sink 闭包存储 (create 时装配并发布到 g_log_sink;
    /// destroy 时若全局指针仍指向此处则清除 —— 见 screen_capture_plugin.h 注释)
    PluginLogSink log_sink;
    /// 流式采集 (原函数级 static 单例会把帧发到首实例宿主 —— 已修为每实例)
    std::unique_ptr<ScreenCaptureHolder> holder;
};

static std::string readToolDepict(const PluginCtx& ctx, const std::string& toolName) {
    if (!ctx.host || !ctx.iface.config || !ctx.iface.config->get_tool_prompt) {
        return {};
    }
    char* json = ctx.iface.config->get_tool_prompt(
        ctx.host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    ctx.host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return {};
    }
    std::string depict;
    jsonGetString(j.doc().at_pointer("/depict"), depict);
    return depict;
}

static void registerTool(
    PluginCtx&                              ctx,
    const char*                             name,
    const char*                             defaultDepict,
    const std::string&                      schema,
    std::function<std::string(SimpleJson&)> fn,
    int                                     flags = 0
) {
    std::string depict = readToolDepict(ctx, name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    ctx.storage.push_back(std::move(depict));
    ctx.storage.push_back(schema);

    auto  entry    = std::make_unique<ToolEntry>();
    entry->host    = ctx.host;
    entry->fn      = std::move(fn);
    auto* entryPtr = entry.get();
    ctx.tool_entries.push_back(std::move(entry));

    // 垫片适配器: 实例内嵌存储 (随实例销毁释放; 多实例契约)
    ctx.sync_tool_shims.push_back(std::make_unique<AgentxxSyncToolShim>());
    auto* shim = ctx.sync_tool_shims.back().get();

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        ctx.storage[ctx.storage.size() - 2].data(),
        ctx.storage[ctx.storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(ctx.storage.back().data(), ctx.storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    // 阻塞委托型: 屏幕捕获为慢同步操作 (offload 池线程执行)
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       volatile int*,
                       char** err) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }
            return pluginStrdup(e->host, e->fn(args).c_str());
        } catch (const std::exception& ex) {
            if (err) {
                *err = pluginStrdup(e->host, ex.what());
            }
            return nullptr;
        } catch (...) {
            if (err) {
                *err = pluginStrdup(e->host, "unknown exception");
            }
            return nullptr;
        }
    };
    if (agentxx_register_sync_tool(ctx.host, &spec, shim) != 0) {
        pluginLog(ctx.host, ctx.iface.log, 3,
                  fmt::format("agentxx_screen_capture: register tool {} failed", name));
    }
}

// =====================================================================
// agentxx_screen_capture
// =====================================================================

// 前置声明 (ScreenCaptureHolder 流式回调引用; 定义见下方)
static codegraph::Json frameToJson(PluginCtx& ctx, const agentxx_screen_capture_plugin::ScreenFrame& f, bool saveImages);
static std::string framesResult(PluginCtx& ctx,
                                const std::vector<agentxx_screen_capture_plugin::ScreenFrame>& frames,
                                bool saveImages);

/// 流式采集 (ScreenCapture 内部自管线程; destroy 时停止)。
/// 原函数级 static 单例在多实例下会把帧发到首实例宿主 —— 已修为每实例成员,
/// 帧回调捕获本实例裸指针 (线程存活期 ⊆ 实例存活期: destroy 先 stopStreaming)
struct ScreenCaptureHolder {
    bool startStreaming(int rate) {
        if (capture_.isStreaming()) {
            return false;
        }
        rate = std::clamp(rate, 1, 30);
        // 异常守卫: 帧回调运行在采集线程, 异常逃逸会 std::terminate 进程
        return capture_.startStreaming(
            rate,
            [ctx = this->ctx](const std::vector<agentxx_screen_capture_plugin::ScreenFrame>& frames) {
                try {
                    if (!ctx || !ctx->host || !ctx->iface.events || !ctx->iface.events->publish) {
                        return;
                    }
                    codegraph::Json j = codegraph::Json::object();
                    j["frames"]       = codegraph::Json::array();
                    for (const auto& f : frames) {
                        // 流式帧只推元信息, 不落盘不携带像素
                        j["frames"].push_back(frameToJson(*ctx, f, false));
                    }
                    std::string payload = j.dump();
                    ctx->iface.events->publish(
                        ctx->host,
                        AGENTXX_SV("agentxx_screen_capture.frame"),
                        agentxx_plugin_sv(payload.data(), payload.size())
                    );
                } catch (...) {
                    pluginLog(ctx ? ctx->host : nullptr,
                              ctx ? ctx->iface.log : nullptr,
                              4,
                              "streaming frame publish");
                }
            }
        );
    }

    void stopStreaming() {
        capture_.stopStreaming();
    }

    agentxx_screen_capture_plugin::ScreenCapture capture_;
    PluginCtx*                                   ctx = nullptr; ///< 事件发布归属实例
};

/// 生成截图文件路径: {capturesDir}/capture_{yyyyMMdd_HHmmss_mmm}_{screen}.png
static std::string buildCapturePath(PluginCtx& ctx, int screenIndex) {
    const auto now = std::chrono::system_clock::now();
    const auto tt  = std::chrono::system_clock::to_time_t(now);
    const auto millis
        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
          % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return fmt::format(
        "{}/capture_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}_{:03d}_{}.png",
        ctx.captures_dir,
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        static_cast<int>(millis),
        screenIndex
    );
}

/// 屏幕帧 → 元信息 JSON (像素不入消息: 可选保存 PNG 文件, 返回文件路径)
/// - saveImages=true 且捕获目录可用时: 编码 PNG 落盘, 结果含 image_path;
///   编码/落盘失败在 image_error 标记, 不影响其余元信息返回
/// - 结果体积控制在 KB 级 (元信息仅数百字节), 不会污染会话上下文
static codegraph::Json frameToJson(PluginCtx& ctx, const agentxx_screen_capture_plugin::ScreenFrame& f, bool saveImages) {
    codegraph::Json j = codegraph::Json::object();
    j["width"]        = f.width;
    j["height"]       = f.height;
    j["offset_x"]     = f.offsetX;
    j["offset_y"]     = f.offsetY;
    j["screen_index"] = f.screenIndex;
    j["screen_name"]  = f.screenName;
    j["is_primary"]   = f.isPrimary;
    j["pixel_bytes"]  = static_cast<int64_t>(f.pixelData.size());
    if (saveImages && !f.pixelData.empty() && !ctx.captures_dir.empty()) {
        std::string path = buildCapturePath(ctx, f.screenIndex);
        if (ctx.holder->capture_.saveFramePng(f, path)) {
            j["image_path"]   = path;
            j["image_format"] = "png";
        } else {
            j["image_error"] = "failed to save png";
        }
    }
    return j;
}

/// 帧数组 → JSON (空帧标记失败)
static std::string
    framesResult(PluginCtx& ctx, const std::vector<agentxx_screen_capture_plugin::ScreenFrame>& frames, bool saveImages) {
    if (frames.empty()) {
        return R"({"ok":false,"error":"capture failed"})";
    }
    codegraph::Json arr = codegraph::Json::array();
    for (const auto& f : frames) {
        arr.push_back(frameToJson(ctx, f, saveImages));
    }
    codegraph::Json j = codegraph::Json::object();
    j["ok"]           = true;
    j["frames"]       = arr;
    return j.dump();
}

static const char* kScreenCaptureDefaultDepict
    = "Capture screen frames or control streaming on Windows: capture all screens, mouse screen, or a specific screen; "
      "get screen count; start/stop streaming (streamed frames are pushed as plugin events to topic 'agentxx_screen_capture.frame'). "
      "Captured frames are saved as PNG files under the host dataDir 'captures/' directory; "
      "the result only contains frame metadata (size/offset/screen) plus the image file path — "
      "pixel data never enters the conversation.";

static void registerScreenCaptureTool(PluginCtx& ctx) {
    codegraph::Json cmd = codegraph::Json::object();
    cmd["type"]         = "string";
    cmd["description"]
        = "Operation to perform: capture_all (default), capture_mouse, capture_screen, "
          "get_screen_count, start_streaming, stop_streaming.";
    cmd["enum"] = codegraph::Json::array(
        {codegraph::Json("capture_all"),
         codegraph::Json("capture_mouse"),
         codegraph::Json("capture_screen"),
         codegraph::Json("get_screen_count"),
         codegraph::Json("start_streaming"),
         codegraph::Json("stop_streaming")}
    );
    codegraph::Json schema               = codegraph::Json::object();
    schema["type"]                       = "object";
    schema["properties"]                 = codegraph::Json::object();
    schema["properties"]["command"]      = cmd;
    schema["properties"]["screen_index"] = codegraph::Json({
        {"type",        "integer"                                                               },
        {"description",
         "Optional 0-based screen index for capture_screen (or default capture when specified)."}
    });
    schema["properties"]["frame_rate"]   = codegraph::Json({
        {"type",        "integer"                                                  },
        {"description", "Target frame rate (1-30) for start_streaming. Default: 5."}
    });
    schema["properties"]["save_images"]  = codegraph::Json({
        {"type",        "boolean"                  },
        {"description",
         "Save each captured frame as a PNG file under the host dataDir "
           "'captures/' directory and return its file path. Pixels never "
           "enter the conversation. Default: true."}
    });

    registerTool(
        ctx,
        "agentxx_screen_capture",
        kScreenCaptureDefaultDepict,
        schema.dump(),
        [&ctx](SimpleJson& args) -> std::string {
            ScreenCaptureHolder& capture = *ctx.holder;
            std::string command;
            bool        hasCommand = jsonGetString(args.doc().at_pointer("/command"), command);

            bool saveImages = true;
            jsonGetBool(args.doc().at_pointer("/save_images"), saveImages);

            int64_t idx    = -1;
            bool    hasIdx = jsonGetInt(args.doc().at_pointer("/screen_index"), idx);

            if (!hasCommand || command.empty() || command == "capture"
                || command == "capture_all") {
                if (!hasCommand && hasIdx && idx >= 0) {
                    command = "capture_screen";
                } else {
                    return framesResult(ctx, capture.capture_.captureAllScreens(), saveImages);
                }
            }

            if (command == "capture_all") {
                return framesResult(ctx, capture.capture_.captureAllScreens(), saveImages);
            }
            if (command == "capture_mouse") {
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                auto                                      f = capture.capture_.captureMouseScreen();
                if (f.width > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(ctx, frames, saveImages);
            }
            if (command == "capture_screen") {
                int screenCount = capture.capture_.getScreenCount();
                if (!hasIdx) {
                    idx = 0;
                }
                if (idx < 0 || idx >= screenCount) {
                    return fmt::format(
                        R"json({{"ok":false,"error":"screen index {} out of range (total screens: {})"}})json",
                        idx,
                        screenCount
                    );
                }
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                auto f = capture.capture_.captureScreen(static_cast<int>(idx));
                if (f.width > 0 && f.height > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(ctx, frames, saveImages);
            }
            if (command == "get_screen_count") {
                codegraph::Json j = codegraph::Json::object();
                j["ok"]           = true;
                j["count"]        = capture.capture_.getScreenCount();
                return j.dump();
            }
            if (command == "start_streaming") {
                int64_t rate = 5;
                jsonGetInt(args.doc().at_pointer("/frame_rate"), rate);
                bool            ok = capture.startStreaming(static_cast<int>(rate));
                codegraph::Json j  = codegraph::Json::object();
                j["ok"]            = ok;
                j["rate"]          = rate;
                return j.dump();
            }
            if (command == "stop_streaming") {
                capture.stopStreaming();
                return R"({"ok":true})";
            }
            return R"({"ok":false,"error":"unknown command"})";
        },
        // 防御: 结果意外超长 (如未来新增字段) 时自动经 share_store 截断压缩
        AGENTXX_TOOL_FLAG_AUTO_SUMMARY
    );
}

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无该条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptInHost(PluginCtx& ctx) {
    if (!ctx.host || !ctx.iface.prompt || !ctx.iface.prompt->get_prompt
        || !ctx.iface.prompt->set_prompt) {
        return;
    }
    char* json = ctx.iface.prompt->get_prompt(ctx.host);
    if (!json) {
        return;
    }
    std::string s{json};
    ctx.host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return;
    }

    codegraph::Json patch = codegraph::Json::object();
    patch["toolPrompt"]   = codegraph::Json::object();
    bool needUpdate       = false;

    if (j.doc().at_pointer("/toolPrompt/agentxx_screen_capture").error()) {
        codegraph::Json tp   = codegraph::Json::object();
        tp["depict"]         = std::string{kScreenCaptureDefaultDepict};
        codegraph::Json args = codegraph::Json::object();
        args["command"]
            = "Command to execute: capture_all (default), capture_mouse, capture_screen, "
              "get_screen_count, start_streaming, stop_streaming.";
        args["screen_index"]
            = "Optional 0-based screen index for capture_screen (or default capture when specified).";
        args["frame_rate"] = "Target frame rate for start_streaming (1-30). Default: 5.";
        args["save_images"]
            = "Save each captured frame as PNG under the host dataDir 'captures/' and "
              "return the file path (pixels never enter the conversation). Default: true.";
        tp["args"]                                    = args;
        patch["toolPrompt"]["agentxx_screen_capture"] = tp;
        needUpdate                                    = true;
    }

    if (needUpdate) {
        std::string payload = patch.dump();
        if (ctx.iface.prompt->set_prompt(
                ctx.host, agentxx_plugin_sv(payload.data(), payload.size()))
            != 0) {
            pluginLog(ctx.host, ctx.iface.log, 3,
                      "agentxx_screen_capture: set_prompt failed");
        }
    }
}

} // namespace agentxx_screen_capture_plugin

using namespace agentxx_screen_capture_plugin;

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_screen_capture"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV(
                "Screen capture on Windows: all screens, mouse screen, specific screen, and streaming"
            ),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: create 含目录创建/注册等可抛操作, 异常返回 -1;
    // 守卫日志闭包捕获局部裸指针 (ctx 装配前置空 → 异常路径静默丢弃)
    auto ctx   = std::make_unique<PluginCtx>();
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* m) noexcept {
            pluginLog(raw ? raw->host : nullptr, raw ? raw->iface.log : nullptr, 4, m ? m : "");
        },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();
        // 装配 XX_LOG* 宏路由 Sink (screen_capture.cpp 经 g_log_sink 转发宿主日志)
        ctx->log_sink = PluginLogSink([host, logIf = ctx->iface.log](int level,
                                                                     const std::string& msg) {
            pluginLog(host, logIf, level, msg);
        });
        g_log_sink.store(&ctx->log_sink, std::memory_order_release);
        // 读取宿主 dataDir (io 线程), 初始化截图落盘目录 {dataDir}/captures
        // - get_config 仅 io 线程可用, execute 运行在线程池, 故此处缓存供后续只读
        // - dataDir 不可用 (旧宿主) 时 captures_dir 为空, 捕获只返回元信息不落盘
        if (ctx->iface.config && ctx->iface.config->get_config) {
            char* json = ctx->iface.config->get_config(host);
            if (json) {
                std::string sv{json};
                host->vtable->free(json);
                SimpleJson j(sv);
                if (j.ok()) {
                    std::string dataDir;
                    if (jsonGetString(j.doc().at_pointer("/dataDir"), dataDir) && !dataDir.empty()) {
                        ctx->captures_dir = dataDir + "/captures";
                        std::error_code ec;
                        if (std::filesystem::create_directories(ctx->captures_dir, ec) || !ec) {
                            pluginLog(ctx->host,
                                      ctx->iface.log,
                                      2,
                                      fmt::format(
                                          "agentxx_screen_capture: capture dir ready: {}",
                                          ctx->captures_dir
                                      )
                            );
                        } else {
                            pluginLog(ctx->host,
                                      ctx->iface.log,
                                      3,
                                      fmt::format(
                                          "agentxx_screen_capture: create captures dir failed: {}",
                                          ec.message()
                                      )
                            );
                            ctx->captures_dir.clear();
                        }
                    }
                }
            }
        }
        ensureToolPromptInHost(*ctx);

        // 流式采集: 每实例持有 (destroy 先停线程再释放, 见 destroy 注释)
        ctx->holder      = std::make_unique<ScreenCaptureHolder>();
        ctx->holder->ctx = ctx.get();

        registerScreenCaptureTool(*ctx);
        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_screen_capture loaded (1 tool)");
        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
        if (!ctx) {
            return;
        }
        // 在宿主 FreeLibrary 之前显式释放 DXGI/D3D11/GDI 资源。
        // 这些资源由 ScreenCaptureHolder 持有, 若留到卸载时的静态析构中释放,
        // D3D11 设备销毁会在 Windows loader lock 下执行, 显卡驱动内部线程无法
        // 退出 → 主线程 GetExitCodeThread 无限自旋挂死 (实测 AMD atidxx64 100%
        // 复现)。此处运行于宿主 destroy 回调 (正常上下文, 无 loader lock),
        // 且每实例独立 holder —— 先停流再释放, 与其他实例互不影响。
        if (ctx->holder) {
            ctx->holder->capture_.shutdown();
        }
        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_screen_capture unloaded");
        // 全局宏路由指针仍指向本实例 Sink 时清除 (多实例下可能已指向后装配者)
        const PluginLogSink* expected = &ctx->log_sink;
        g_log_sink.compare_exchange_strong(expected, nullptr);
        delete ctx; // storage/tool_entries/shims/holder 均为 ctx 成员
        });
}
