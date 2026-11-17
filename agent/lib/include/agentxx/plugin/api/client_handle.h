/// client 业务表句柄与"完成回调 payload 文本"之间的转换 (C++ 侧工具, header-only)
///
/// client 侧六张表都走异步操作契约, 注册类成员产出的句柄 (状态栏项 / 面板 /
/// Info 段落 / 定时器 / 快捷键) 只能经完成回调的 `payload` 交回。句柄由主程序持有到
/// 实例卸载, 所以传"进程内地址的十进制文本"就够: 主程序用
/// [agentxx::plugin::clientHandleToText] 生成, 插件侧用
/// [agentxx::plugin::clientHandleFromText] 原样转回。
///
/// 为什么用文本而不是结构体或二进制: 跨边界只传字符串, 脚本引擎 (JS) 与其它语言写的
/// 插件也能自己拼; 与广播订阅句柄 (`pluginxx/api/broadcast_handle.h`) 同一套做法,
/// 两侧共用一份实现, 解析规则不用写两遍。
#ifndef AGENTXX_PLUGIN_API_CLIENT_HANDLE_H
#define AGENTXX_PLUGIN_API_CLIENT_HANDLE_H

#include "agentxx/plugin/api/client_plugin_api.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace agentxx::plugin {

/// 句柄 → 完成回调 `payload` 文本 (十进制地址)
template<typename Handle>
inline std::string clientHandleToText(Handle* handle) {
    return std::to_string(reinterpret_cast<uintptr_t>(handle));
}

/// `payload` 文本 → 句柄 (空 / 非法文本给 `nullptr`)
template<typename Handle>
inline Handle* clientHandleFromText(std::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    uintptr_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return nullptr;
        }
        // 溢出按非法处理 (地址文本远短于此上限)
        if (value > (UINTPTR_MAX - 9) / 10) {
            return nullptr;
        }
        value = value * 10 + static_cast<uintptr_t>(c - '0');
    }
    return reinterpret_cast<Handle*>(value);
}

} // namespace agentxx::plugin

#endif // AGENTXX_PLUGIN_API_CLIENT_HANDLE_H
