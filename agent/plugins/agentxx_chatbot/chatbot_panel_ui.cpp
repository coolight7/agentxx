#include "chatbot_panel_ui.h"

#include <pluginxx/ui/build.h>

#include <string>

namespace agentxx {
namespace chatbot {

namespace {

/// 面板是窄栏: 名字太长会挤掉右侧的排队数, 这里先截一道
constexpr size_t kTitleMaxCodePoints  = 24;
constexpr size_t kPromptMaxCodePoints = 48;

/// 列表项前缀列宽 (`|- ` 三个字符 ≈ 24u, 终端 1 格 8u)
constexpr double kBulletCols = 24.0;
/// 会话状态标记列宽 (加载帧或 `·` 一个字符 + 一格间距)
constexpr double kMarkerCols = 16.0;
/// 右侧排队数一列的宽度 (最多三位数)
constexpr double kQueueCols = 24.0;

/// 列表项前缀文本 (`|- `)
pluginxx::ui::Item listBullet() {
    pluginxx::ui::Item item = pluginxx::ui::build::text("|- ");
    item.tone               = "hint";
    item.wrap               = false;
    return item;
}

/// 减淡的辅助文字 (副标题/说明/计数)
///
/// 不用 `build::caption`: 它除 hint 色之外还会叠加一层 dim, 在深色终端上比
/// `|- ` 前缀明显更暗, 同一栏里两种"减淡"看起来不一致; 这里只要 hint 色。
pluginxx::ui::Item hintText(std::string value) {
    pluginxx::ui::Item item = pluginxx::ui::build::text(value);
    item.tone               = "hint";
    return item;
}

/// 段标题文本 (`标题: 数量`)
std::string sectionHeaderText(std::string_view title, size_t count) {
    return std::string{title} + ": " + std::to_string(count);
}

} // namespace

const std::array<std::string_view, 10>& panelSpinnerFrames() {
    static const std::array<std::string_view, 10> kFrames{
        "⠋",
        "⠙",
        "⠹",
        "⠸",
        "⠼",
        "⠴",
        "⠦",
        "⠧",
        "⠇",
        "⠏",
    };
    return kFrames;
}

std::string_view panelSpinnerFrame(uint32_t tick) {
    const auto& frames = panelSpinnerFrames();
    return frames[tick % frames.size()];
}

std::vector<pluginxx::ui::Item> panelItemsOf(const PanelSnapshot& snapshot) {
    using namespace pluginxx::ui;

    std::vector<Item> items;

    const bool hasData = !snapshot.sessions.empty() || !snapshot.personas.empty()
                         || !snapshot.accounts.empty();
    if (!hasData) {
        // agent 侧插件没加载, 或者还没推送过: 给一行说明, 别留一个空白栏
        items.push_back(hintText("等待服务端数据…"));
        return items;
    }

    // 段与段之间留一行空白 (终端里 Gap 的缺省值约 1 行), 不画分隔线
    auto pushSectionGap = [&items]() {
        if (!items.empty()) {
            items.push_back(build::gap());
        }
    };

    /* ---------- 会话 (面板主体) ---------- */

    // 段标题统一 `标题: 数量`
    items.push_back(build::title(sectionHeaderText("会话", snapshot.sessions.size())));
    if (snapshot.sessions.empty()) {
        items.push_back(hintText("还没有会话 (收到群/私聊消息后才创建)"));
    }
    for (const auto& session : snapshot.sessions) {
        Item title = build::text(panelTruncate(session.title, kTitleMaxCodePoints));

        Item subtitle = hintText(panelSessionSubtitle(session));

        // 状态标记: 正在执行一轮就是加载中 (点阵逐帧转动), 空闲时用一个点占位,
        // 保证各行标题从同一列开始
        Item marker = build::text(
            session.running ? std::string{panelSpinnerFrame(snapshot.tick)} : std::string{"·"}
        );
        marker.tone = session.running ? "accent" : "hint";
        marker.wrap = false;

        // 右侧: 排队消息条数 (有积压时用强调色), 靠右对齐
        Item queueText = build::text(panelSessionQueueText(session));
        queueText.tone  = session.queued > 0 ? "accent" : "hint";
        queueText.align = "end";
        queueText.wrap  = false;

        // 固定宽度的三列 (前缀 / 状态 / 条数) 让每行标题落在同一列;
        // 中间的 Expanded 吃掉剩余宽度, 条数才会贴在最右边
        Item row = build::row(
            {
                build::sizedBox(kBulletCols, 0.0, listBullet()),
                build::sizedBox(kMarkerCols, 0.0, marker),
                build::expanded(build::column({title, subtitle})),
                build::sizedBox(kQueueCols, 0.0, queueText),
            }
        );
        row.cross = "center";
        items.push_back(std::move(row));
    }

    /* ---------- 人设信息 ---------- */

    if (!snapshot.personas.empty()) {
        pushSectionGap();
        items.push_back(build::title(sectionHeaderText("人设", snapshot.personas.size())));
        for (const auto& persona : snapshot.personas) {
            Item name = build::text(panelTruncate(persona.name, kTitleMaxCodePoints));

            // 模型与用量同一行 (窄栏里右对齐放不下, 直接接在小字后面)
            Item detail = hintText(panelPersonaDetail(persona));

            // 提示词摘录放在同一个人设块里, 与名字左对齐
            std::vector<Item> lines{name, detail};
            if (!persona.promptHead.empty()) {
                lines.push_back(hintText(
                    panelTruncate(persona.promptHead, kPromptMaxCodePoints)
                ));
            }

            Item block = build::node("Column");
            block.children = std::move(lines);

            Item row = build::row(
                {
                    build::sizedBox(kBulletCols, 0.0, listBullet()),
                    build::expanded(std::move(block)),
                }
            );
            row.cross = "center";
            items.push_back(std::move(row));
        }
    }

    /* ---------- 账号 ---------- */

    if (!snapshot.accounts.empty()) {
        pushSectionGap();
        items.push_back(build::title(sectionHeaderText("账号", snapshot.accounts.size())));
        for (const auto& account : snapshot.accounts) {
            Item row = build::row(
                {
                    build::sizedBox(kBulletCols, 0.0, listBullet()),
                    build::expanded(build::text(panelAccountItemText(account))),
                }
            );
            items.push_back(std::move(row));
        }
    }

    /* ---------- 收发计数 (一行小字) ---------- */

    if (snapshot.received > 0 || snapshot.sent > 0 || snapshot.failed > 0) {
        pushSectionGap();
        std::string stats = "收 " + std::to_string(snapshot.received) + " · 发 "
                            + std::to_string(snapshot.sent);
        if (snapshot.failed > 0) {
            stats += " · 失败 " + std::to_string(snapshot.failed);
        }
        items.push_back(hintText(stats));
    }

    return items;
}

} // namespace chatbot
} // namespace agentxx
