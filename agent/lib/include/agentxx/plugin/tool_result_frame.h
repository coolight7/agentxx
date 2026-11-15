/// 工具结果两段 (正文 + 元数据) 在主程序内部的封装与拆解
///
/// 背景: 插件按 `AgentxxPluginToolNotify` 把工具结果分两段交上来 (正文原样字节,
/// 元数据 JSON), 但插件框架的"操作完成包" (`PluginxxOperatorNotify` /
/// `OpCore`) 只有一个字符串。要保证正文不进 JSON (否则非 UTF-8 字节会被换成
/// U+FFFD, 再也无法做编码探测与无损转码), 这里的做法是把两段按**长度前缀的裸字节**
/// 拼成一段:
///
/// ```text
/// [uint32 正文长度][uint32 元数据长度][正文][元数据]
/// ```
///
/// 这不是 JSON: 不做转义、不校验编码, 原样字节直接拼。字节只在本进程内流转,
/// 因此直接用本机字节序与 `uint32`。
///
/// 用到的两处:
/// - 工具执行 (`PluginTool::execute_async` → `mapPluginOpResult`): 收端拆开并
///   对正文做编码校正;
/// - 插件互调 (`call_tool_async` 的完成回调): 收端拆开后按两段交给调用方。
///
/// 相关: 两段在插件侧的产出见 `plugin_kit.h` 的 `toolBodyResultTo`。
#ifndef AGENTXX_PLUGIN_TOOL_RESULT_FRAME_H
#define AGENTXX_PLUGIN_TOOL_RESULT_FRAME_H

#include "agentxx/plugin/api/plugin_api.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace agentxx {
namespace plugin {

/// 帧头长度 (两个 uint32)
inline constexpr size_t kToolResultFrameHeaderSize = 2 * sizeof(uint32_t);

/// 把两段拼成一段裸字节帧 (见文件头)
/// - `args`:
///     - [result] 结果正文 (原样字节)
///     - [metadata] 元数据 JSON (可空)
/// - `return` 帧字节
inline std::string encodeToolResultFrame(std::string_view result, std::string_view metadata) {
    const auto resultSize   = static_cast<uint32_t>(result.size());
    const auto metadataSize = static_cast<uint32_t>(metadata.size());
    std::string frame;
    frame.resize(kToolResultFrameHeaderSize + result.size() + metadata.size());
    std::memcpy(frame.data(), &resultSize, sizeof(resultSize));
    std::memcpy(frame.data() + sizeof(uint32_t), &metadataSize, sizeof(metadataSize));
    if (!result.empty()) {
        std::memcpy(frame.data() + kToolResultFrameHeaderSize, result.data(), result.size());
    }
    if (!metadata.empty()) {
        std::memcpy(
            frame.data() + kToolResultFrameHeaderSize + result.size(),
            metadata.data(),
            metadata.size()
        );
    }
    return frame;
}

/// 拆开帧后的两个视图 (都指向输入缓冲区, 不拥有内存)
struct ToolResultFrameView {
    std::string_view result;   ///< 结果正文
    std::string_view metadata; ///< 元数据 JSON (可空)
};

/// 拆开帧
/// - 长度对不上 (不是本函数产出的帧, 例如插件违约/截断) 时**把整段当正文**返回,
///   元数据留空: 宁可少一段元数据, 也不能把工具结果丢掉
/// - `args`:
///     - [frame] `encodeToolResultFrame` 产出的字节
/// - `return` 两段视图 (指向 `frame`)
inline ToolResultFrameView decodeToolResultFrame(std::string_view frame) noexcept {
    if (frame.size() < kToolResultFrameHeaderSize) {
        return ToolResultFrameView{frame, {}};
    }
    uint32_t resultSize   = 0;
    uint32_t metadataSize = 0;
    std::memcpy(&resultSize, frame.data(), sizeof(resultSize));
    std::memcpy(&metadataSize, frame.data() + sizeof(uint32_t), sizeof(metadataSize));
    const size_t total = static_cast<size_t>(resultSize) + static_cast<size_t>(metadataSize);
    if (frame.size() != kToolResultFrameHeaderSize + total) {
        return ToolResultFrameView{frame, {}};
    }
    const auto* base = frame.data() + kToolResultFrameHeaderSize;
    return ToolResultFrameView{
        std::string_view{base, resultSize},
        std::string_view{base + resultSize, metadataSize}
    };
}

/// 主程序侧的两段完成回调 (符合 `AgentxxPluginToolNotify.done`)
///
/// - `host_ud` 是插件框架的完成端点 (`OpCore` 的 completionEndpoint), 两段在这里
///   合成帧后交给它: "恰好一次"与"迟到的重复 done"由框架统一处理
/// - 可从任意线程调用 (框架内部投递回 io 线程)
void PLUGINXX_CALL toolNotifyDone(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result,
    const PluginxxStringView* metadata
) noexcept;

/// 单段完成回调 (元数据恒为空)
///
/// 用到它的地方: 插件框架的协程根只传一段正文 (`PluginxxOperatorNotify`),
/// 而 Task 型工具 (`plugin_kit.h` 的 `tool`) 本来就只能给正文。
void PLUGINXX_CALL toolNotifyDoneSingle(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result
) noexcept;

/// 整帧完成回调: `frame` 已经是 [encodeToolResultFrame] 的产物
///
/// 用到它的地方: 受控轮询工具 (`plugin_kit.h` 的 `polled_tool`) 在插件侧把两段
/// 拼成帧, 经 `PolledRoot::notifyHost` 的单一字符串通道交上来。
/// 非帧内容 (例如框架放弃根时给的失败原因) 同样走这里, 收端会当作正文处理。
void PLUGINXX_CALL toolNotifyFrame(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* frame
) noexcept;

/// 由插件框架的完成通知器构造工具的两段通知器
/// - 三个入口都在主程序侧实现 (插件只挂函数指针, 不引用主程序符号)
/// - `args`:
///     - [notify] `execute_start` 收到的单段通知器
/// - `return` 交给 `AgentxxPluginToolSpec::execute_start` 的通知器
inline AgentxxPluginToolNotify
    makeToolNotify(const PluginxxOperatorNotify& notify) noexcept {
    return AgentxxPluginToolNotify{
        &toolNotifyDone,
        &toolNotifyDoneSingle,
        &toolNotifyFrame,
        notify.host_ud
    };
}

} // namespace plugin
} // namespace agentxx

#endif /* AGENTXX_PLUGIN_TOOL_RESULT_FRAME_H */
