/// 工具结果两段帧的主程序侧收端 (见 `agentxx/plugin/tool_result_frame.h`)
#include "agentxx/plugin/tool_result_frame.h"

#include "pluginxx/kit/kit.h"
#include "pluginxx/runtime/op_driver.h"

namespace agentxx {
namespace plugin {

namespace {

/// C ABI 的字符串视图 -> 本地视图 (指针可为 NULL, 表示空)
std::string_view viewOf(const PluginxxStringView* view) {
    if (!view || !view->data) {
        return {};
    }
    return std::string_view{view->data, static_cast<size_t>(view->size)};
}

} // namespace

void PLUGINXX_CALL toolNotifyDone(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result,
    const PluginxxStringView* metadata
) noexcept {
    // 直接在栈上拼帧: 框架的完成包在 done 内同步复制, 返回后本缓冲即可释放
    std::string frame;
    try {
        frame = encodeToolResultFrame(viewOf(result), viewOf(metadata));
    } catch (...) {
        // 分配失败: 交给框架一个空结果, 至少保证操作能终结 (否则会话会一直等)
        frame.clear();
    }
    const auto frameView = pluginxx::PluginStringView::from(frame.data(), frame.size());
    // 完成端点的处理本身是 noexcept 的; 重复/迟到的 done 由框架记为忽略
    pluginxx::OpCore::onEndpointDone(ud, status, &frameView);
}

void PLUGINXX_CALL toolNotifyDoneSingle(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* result
) noexcept {
    const PluginxxStringView empty{nullptr, 0};
    toolNotifyDone(ud, status, result, &empty);
}

void PLUGINXX_CALL toolNotifyFrame(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* frame
) noexcept {
    // 已经是帧, 不再包装; 非帧内容由收端按正文处理
    const PluginxxStringView empty{nullptr, 0};
    pluginxx::OpCore::onEndpointDone(ud, status, frame ? frame : &empty);
}

} // namespace plugin
} // namespace agentxx
