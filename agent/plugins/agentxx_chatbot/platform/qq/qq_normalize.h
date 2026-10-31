#pragma once

/// QQ 网关事件 → `InboundEvent` 的归一化 (纯函数, 不碰网络也不碰连接状态)
///
/// 单独拆成一层的理由: 平台对不同附件 (图片/语音/文件/引用) 的字段布局并不一致,
/// 只靠读文档猜很容易整类漏掉 —— 这层的正确性只能靠**真实事件**验证, 所以把它做成
/// 不依赖 asio 的纯函数, 测试可以直接喂仓库里留存的真实事件 JSON 做回归
/// (见 `agent/test/core/test_chatbot_qq.cpp`)。
#include "../../chatbot_types.h"

#include "utilxx_base/json.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 机器人自己的身份 (用来在全量消息里判断「这条 @ 的是不是自己」)
///
/// 群里的 openid 是**按群**发的 (`member_openid`), 同一个机器人在不同群里不一样,
/// 所以 `openids` 是一组并集, `groupOpenids` 另记「哪个群对应哪个 openid」(诊断与
/// 学习用)。来源可以叠加:
/// - 网关 READY 的 `d.user` (`id` / `union_openid`, 以及它的昵称 `username`);
/// - 账号配置的 `selfOpenid` (手工钉住, 见 `AccountConfig`);
/// - 群里自己消息的回声 (`author.member_openid`, 由 `QqPlatform` 自学习);
/// - `mentions[]` 里昵称到自己的那一项 (命中时把它的 openid 学进对应群)。
struct QqSelfIdentity {
    std::vector<std::string> openids;
    /// 群 openid → 自己在那个群的 member_openid (同一个机器人在不同群里不一样)
    std::map<std::string, std::string, std::less<>> groupOpenids;
    /// 机器人昵称 (识别自己消息回声、以及 `mentions[]` 里“@ 的就是我”的依据; 可空)
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

    /// 昵称是不是机器人自己 (去首尾空白后逐字相等; `username` 为空时永远 false)
    ///
    /// 这是自身 openid 还没学到时的兜底: 群里 @ 机器人时 `mentions[]` 里的 `username`
    /// 就是机器人昵称。与 openid 判定相比它弱一档 (同名成员会误判), 所以
    /// `MentionSelfSource::Nickname` 排在 `Openid` 之前。
    bool matchesName(std::string_view name) const;

    /// 记一个 openid (去重); 返回 true = 这个是新值
    bool add(std::string_view id) {
        if (id.empty() || matches(id)) {
            return false;
        }
        openids.emplace_back(std::string{id});
        return true;
    }

    /// 把一个 openid 记到具体群上 (同时进并集); 返回 true = 新值
    bool addGroup(std::string_view groupId, std::string_view id);

    /// 某个群里已知的自身 openid (空 = 还没学到)
    std::string groupOpenidOf(std::string_view groupId) const {
        const auto it = groupOpenids.find(std::string{groupId});
        return it == groupOpenids.end() ? std::string{} : it->second;
    }
};

/// 一条网关事件 → 归一化结果
///
/// - `eventName` 只认消息类事件 (`GROUP_AT_MESSAGE_CREATE` / `GROUP_MESSAGE_CREATE` /
///   `C2C_MESSAGE_CREATE`), 其它事件返回 false
/// - 解析不出会话 id、机器人自己发的、内容与附件都空的也返回 false (不叫醒模型)
/// - `self` 不是空指针时, 全量消息里还会按它判断「这条 @ 的是不是自己」:
///   先比 `mentions[]` / 正文标记里的 openid; 比不到再比昵称 (自身 openid 还没学到的
///   兜底), 命中昵称时会把那个 openid 学进 `self->groupOpenids` (调用方写回身份表)
/// - `mentions[]` 里出现 `bot: true` **不**等于「@ 的是自己」: 群里还可能有别的机器人
/// - 成功时 `out` 被整体重写
bool normalizeQqEvent(
    std::string_view         eventName,
    const utilxx_base::Json& data,
    InboundEvent&            out,
    QqSelfIdentity*          self = nullptr
);

/// 从下载地址里取文件名 (`.../abc.md?fname=abc.md` → `abc.md`); 取不到返回 `attachment`
std::string qqFileNameOfUrl(std::string_view url);

} // namespace chatbot
} // namespace agentxx
