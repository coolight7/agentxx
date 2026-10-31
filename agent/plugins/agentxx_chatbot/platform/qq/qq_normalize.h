#pragma once

/// QQ 网关事件 → `InboundEvent` 的归一化 (纯函数, 不碰网络也不碰连接状态)
///
/// 单独拆成一层的理由: 平台对不同附件 (图片/语音/文件/引用) 的字段布局并不一致,
/// 只靠读文档猜很容易整类漏掉 —— 这层的正确性只能靠**真实事件**验证, 所以把它做成
/// 不依赖 asio 的纯函数, 测试可以直接喂仓库里留存的真实事件 JSON 做回归
/// (见 `agent/test/core/test_chatbot_qq.cpp`)。
#include "../../chatbot_types.h"

#include "utilxx_base/json.h"

#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 机器人自己的身份 (用来在全量消息里判断「这条 @ 的是不是自己」)
///
/// 群里的 openid 是**按群**发的 (`member_openid`), 同一个机器人在不同群里不一样,
/// 所以这里放一组。来源可以叠加:
/// - 网关 READY 的 `d.user` (`id` / `union_openid`);
/// - 账号配置的 `selfOpenid` (手工钉住, 见 `AccountConfig`);
/// - 群里自己消息的回声 (`author.member_openid`, 由 `QqPlatform` 自学习)。
struct QqSelfIdentity {
    std::vector<std::string> openids;
    /// 机器人昵称 (识别自己消息回声的依据; 可空)
    std::string username;

    /// 是不是自己的 openid
    bool matches(std::string_view id) const {
        if (id.empty()) {
            return false;
        }
        for (const auto& one : openids) {
            if (one == id) {
                return true;
            }
        }
        return false;
    }

    /// 记一个 openid (去重); 返回 true = 这个是新值
    bool add(std::string_view id) {
        if (id.empty() || matches(id)) {
            return false;
        }
        openids.emplace_back(id);
        return true;
    }
};

/// 一条网关事件 → 归一化结果
///
/// - `eventName` 只认消息类事件 (`GROUP_AT_MESSAGE_CREATE` / `GROUP_MESSAGE_CREATE` /
///   `C2C_MESSAGE_CREATE`), 其它事件返回 false
/// - 解析不出会话 id、机器人自己发的、内容与附件都空的也返回 false (不叫醒模型)
/// - `self` 不是空指针时, 全量消息里还会按它判断「这条 @ 的是不是自己」
/// - 成功时 `out` 被整体重写
bool normalizeQqEvent(
    std::string_view         eventName,
    const utilxx_base::Json& data,
    InboundEvent&            out,
    const QqSelfIdentity*    self = nullptr
);

/// 从下载地址里取文件名 (`.../abc.md?fname=abc.md` → `abc.md`); 取不到返回 `attachment`
std::string qqFileNameOfUrl(std::string_view url);

} // namespace chatbot
} // namespace agentxx
