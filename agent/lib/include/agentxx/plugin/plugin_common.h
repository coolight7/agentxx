/*
 * agentxx/plugin/plugin_common.h —— 插件系统公共设施 (agent/client 两侧共享)
 *
 * 背景: plugin_manager.cpp (agent 侧) 与 client_plugin_manager.cpp (client 侧)
 * 存在大量重复基建 (插件名推导 / manifest 解析 / entry 路径解析 / 拓扑排序 /
 * 反向依赖收集 / io 线程同步等待 / C ABI 异常兜底宏)。提取到本头避免两侧
 * 行为漂移 —— 历史上 client 侧多次"漏掉 agent 侧已修的问题" (见
 * plugins.md 13.x 记录), 公共化后修复只做一次。
 *
 * 内容:
 * - XX_PLUGIN_CATCH_*: C ABI 边界异常兜底宏 (宏定义, 无 ODR 问题)
 * - pluginNameFromPath / parsePluginManifest / resolvePluginEntryPath:
 *   插件名推导与目录 manifest 解析 (平台扩展名修正 + 配置子目录回退)
 * - topoSortPlugins<T>: Kahn 拓扑排序 (依赖者排在被依赖者之后)
 * - ioCallSync / ioCallSyncVoid: 跨线程投递到 io 线程同步等待 (duck typing,
 *   Mgr 须提供 isIoThread()/postToIo())
 * - collectReverseRequiredDeps: 反向必选依赖收集 (模板, Plugin 须含
 *   depends/enabled 成员)
 *
 * 线程约定: 本头内非模板函数均可在任意线程调用 (纯函数); 模板函数按
 * 各 manager 的线程模型工作。
 */
#pragma once

#include "agentxx/util/log.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// C ABI 边界异常兜底宏: vtable 函数内部不得让 C++ 异常逃逸 (跨边界 UB),
/// 统一捕获转日志并按失败返回值返回。用法:
///   XX_PLUGIN_CATCH_BEGIN
///   ... 函数体 ...
///   XX_PLUGIN_CATCH_END(失败返回值)     // 有返回值函数
///   XX_PLUGIN_CATCH_END_VOID()          // void 函数
#define XX_PLUGIN_CATCH_BEGIN try {
#define XX_PLUGIN_CATCH_END(ret)                          \
    }                                                     \
    catch (const std::exception& e) {                     \
        XX_LOGE("plugin vtable exception: {}", e.what()); \
        return (ret);                                     \
    }                                                     \
    catch (...) {                                         \
        XX_LOGE("plugin vtable unknown exception");       \
        return (ret);                                     \
    }
#define XX_PLUGIN_CATCH_END_VOID()                        \
    }                                                     \
    catch (const std::exception& e) {                     \
        XX_LOGE("plugin vtable exception: {}", e.what()); \
        return;                                           \
    }                                                     \
    catch (...) {                                         \
        XX_LOGE("plugin vtable unknown exception");       \
        return;                                           \
    }

namespace agentxx {
namespace plugin {

/// 从库文件名推断插件名 (libfoo.so → foo; foo.dll → foo; libfoo.so.1.2 → foo;
/// my.plugin.so → my.plugin)
/// - 扩展名剥离用 rfind (兼容文件名内含扩展名片段, 如 my.plugin.so → my.plugin)
/// - 仅当剥离过扩展名后才去 lib 前缀: 无扩展名的库/目录 (如插件目录
///   `libanalysis`) 保持原名, 避免误剥
std::string pluginNameFromPath(const std::string& path);

/// 解析插件目录 plugin.yaml 清单 (name/entry/depends/optional_depends)
/// - 返回 false 表示解析失败 (目录无 plugin.yaml 或 yaml 非法/缺字段);
///   YAML 非法时记日志, 无 manifest 不记 (调用方决定日志策略)
bool parsePluginManifest(
    const std::filesystem::path& dir,
    std::string&                 name,
    std::string&                 entry,
    std::vector<std::string>&    depends,
    std::vector<std::string>&    optionalDepends
);

/// 目录插件 entry 相对路径 → 平台化绝对库路径:
/// - entry 按 Linux 书写 (libfoo.so), Windows/macOS 下修正扩展名 (.dll/.dylib)
/// - 多配置生成器 (MSVC Debug/Release) 产物位于配置子目录: {dir}/{entry}
///   找不到时回退 {dir}/{Debug|Release|RelWithDebInfo|MinSizeRel}/{entry}
/// - 返回绝对路径 (可能不存在, 由调用方处理)
std::string resolvePluginEntryPath(const std::filesystem::path& dir, const std::string& entry);

/// 拓扑排序项 (调用方 Item 须含 path/name/depends 三个成员, 可附带其他字段)
struct PluginSortItem {
    std::string              path;
    std::string              name; ///< 空 = 无法推导 (不影响排序)
    std::vector<std::string> depends;
};

/// 拓扑排序 (Kahn): 依赖者排在被依赖者之后, 避免配置顺序导致必选依赖缺失
/// - 配置列表中且未放置的依赖 → 未满足; 不在配置列表的依赖视为已满足 (已加载)
/// - 无进展 (环/缺失) 时剩余项按原序附后, 由加载路径的依赖检查报错
template<typename T>
std::vector<T> topoSortPlugins(std::vector<T> items) {
    std::vector<T>   ordered;
    ordered.reserve(items.size());
    std::vector<bool> placed(items.size(), false);
    size_t            placedCount = 0;
    while (placedCount < items.size()) {
        size_t progress = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (placed[i]) {
                continue;
            }
            bool depsOk = true;
            for (const auto& d : items[i].depends) {
                for (size_t j = 0; j < items.size(); ++j) {
                    if (!placed[j] && items[j].name == d) {
                        depsOk = false;
                        break;
                    }
                }
                if (!depsOk) {
                    break;
                }
            }
            if (depsOk) {
                ordered.push_back(std::move(items[i]));
                placed[i] = true;
                ++progress;
            }
        }
        if (progress == 0) {
            // 环或依赖缺失: 剩余项附后 (加载时依赖检查报错)
            for (size_t i = 0; i < items.size(); ++i) {
                if (!placed[i]) {
                    ordered.push_back(std::move(items[i]));
                    placed[i] = true;
                }
            }
            break;
        }
        placedCount += progress;
    }
    return ordered;
}

/// 在 io 线程执行并同步等待结果 (调用方为 io 线程时直接执行)
/// - 供 vtable 的 io 线程约束操作跨线程调用 (JS 线程/宿主线程池) 使用;
///   调用方线程阻塞等待, io 线程为事件循环 (挂起而非忙等), 无死锁风险
/// - Mgr 须提供 isIoThread()/postToIo(); T 为第一个显式模板参数, Mgr 推导
template<typename T, typename Mgr>
T ioCallSync(Mgr* mgr, std::function<T()> fn) {
    if (!mgr) {
        throw std::runtime_error("plugin manager released");
    }
    if (mgr->isIoThread()) {
        return fn();
    }
    std::promise<T> p;
    auto            fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            p.set_value(fn());
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

/// ioCallSync 的 void 特化
template<typename Mgr>
void ioCallSyncVoid(Mgr* mgr, std::function<void()> fn) {
    if (!mgr) {
        return;
    }
    if (mgr->isIoThread()) {
        fn();
        return;
    }
    std::promise<void> p;
    auto               fut = p.get_future();
    mgr->postToIo([&p, fn = std::move(fn)]() mutable {
        try {
            fn();
            p.set_value();
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    fut.get();
}

/// 收集必选依赖 target 的插件名 (io 线程)
/// - onlyEnabled=true: 仅统计 enabled 的插件 (卸载/禁用级联)
/// - onlyEnabled=false: 全部统计 (启用级联: 需恢复被级联禁用的插件)
/// - PluginMap 元素须含 name(键)/depends/enabled 成员
template<typename PluginMap>
std::vector<std::string>
    collectReverseRequiredDeps(const PluginMap& plugins, const std::string& target, bool onlyEnabled) {
    std::vector<std::string> out;
    for (const auto& [name, inst] : plugins) {
        if (name == target || (onlyEnabled && !inst->enabled)) {
            continue;
        }
        if (std::find(inst->depends.begin(), inst->depends.end(), target) != inst->depends.end()) {
            out.push_back(name);
        }
    }
    return out;
}

} // namespace plugin
} // namespace agentxx
