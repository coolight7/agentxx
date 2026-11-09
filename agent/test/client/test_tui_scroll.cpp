// 消息列表 (LazyScrollable) 滚动行为测试
//
// 覆盖的回归场景:
// - [抖动回归] 流式输出时内容更新帧 (token 增长后首帧) 与同内容再渲染帧
//   (鼠标移动/任意事件触发) 的渲染结果必须完全一致, 且吸附底部时屏幕最底行
//   展示最新内容 —— 修复前内容帧按估算高度定位子项 (估算偏差约 ±1 行),
//   底部多出空行, 下一帧才回到正确位置, 帧间交替表现为列表上下抖动
// - [吸附跟随] stickToBottom 下内容增长自动跟随到底 (流式追加/消息入列)
// - [滚轮滚动] 滚轮上滚解除吸附并保持视图不动, 滚回底部恢复吸附
// - [本地条数上限清理] 头部删除消息后的下标平移 (视口内容稳定, 可清理区间与
//   历史窗口记账同步)
#include "agentxx-test/client/test_tui_scroll.h"
#include "agentxx-test/client/test_tui_tool_header.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/model/message_window.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "asio/io_context.hpp"
#include "ftxui/component/animation.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "utilxx_base/string_util.h"
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_scroll_passed = 0;
int g_tui_scroll_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_scroll_passed
#define XX_TEST_FAILED g_tui_scroll_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

/// 测试夹具: 固定 100x20 视口的消息列表
namespace {

/// 屏幕某一行的一格一字文本 (宽字符的占位格记一个空格)
///
/// 与屏幕上看到的列一一对应, 所以 ASCII 子串在串里的下标就是它的列号
/// (消息按列对齐, 折叠头的颜色/dim 断言靠这个定位)
std::string screenRowText(const ftxui::Screen& screen, int y) {
    std::string row;
    row.reserve(static_cast<size_t>(screen.dimx()));
    for (int x = 0; x < screen.dimx(); ++x) {
        const auto& ch = screen.PixelAt(x, y).character;
        row += ch.empty() ? ' ' : ch[0];
    }
    return row;
}

/// 夹具: 把一行当作 agent 下发的 insert_message 放进展示历史 (id 自动生成)
void pushRow(TUIRenderState& st, TUIMessage row) {
    static uint64_t seq = 0;
    if (row.id.empty()) {
        row.id = fmt::format("t_row_{}", ++seq);
    }
    agentxx::agent::WireDelta d;
    d.type    = agentxx::agent::WireDelta::Type::InsertMessage;
    d.msgId   = row.id;
    d.message = std::make_shared<TUIMessage>(std::move(row));
    st.replica.applyDelta(d);
}

/// 夹具: 前插一行 (历史分页场景)
void prependRow(TUIRenderState& st, TUIMessage row) {
    static uint64_t seq = 0;
    if (row.id.empty()) {
        row.id = fmt::format("t_prepend_{}", ++seq);
    }
    std::vector<TUIMessage> rows{std::move(row)};
    st.replica.prependRows(rows);
}

/// 夹具: 把尾部流式项设成 (角色 + 文本) 的形态
/// - `epoch` 非 0 时用它当流 id (同一 epoch = 同一条流), 否则每次换一条新流
/// - 真实路径只会追加, 这里的"整段设置"语义仅服务于夹具
void setStream(
    TUIRenderState&   st,
    TUIMessage::Role  role,
    const std::string& text,
    uint64_t          epoch = 0
) {
    static uint64_t seq = 0;
    const auto      id  = fmt::format("t_stream_{}", epoch == 0 ? ++seq : epoch);
    agentxx::agent::WireDelta begin;
    begin.type       = agentxx::agent::WireDelta::Type::StreamBegin;
    begin.msgId      = id;
    begin.streamRole = role;
    st.replica.applyDelta(begin);
    if (!text.empty()) {
        agentxx::agent::WireDelta tok;
        tok.type = (role == TUIMessage::Role::Think) ? agentxx::agent::WireDelta::Type::ThinkToken
                                                     : agentxx::agent::WireDelta::Type::TextToken;
        tok.msgId = id;
        tok.text  = text;
        st.replica.applyDelta(tok);
    }
    st.isStreaming = true;
}

/// 夹具: 丢弃尾部流式项 (流结束)
void clearStream(TUIRenderState& st) {
    st.replica.clearStream();
    st.isStreaming = false;
}

} // namespace

struct ScrollFixture {
    asio::io_context io;
    TUISharedState   sharedState;
    TUITheme         theme = TUITheme::darkTheme();

    TUICtx                                ctx;
    std::shared_ptr<MessageListComponent> comp;

    static constexpr int kWidth  = 100;
    static constexpr int kHeight = 20;

    /// 渲染尺寸 (成员使高视口夹具可覆写; 默认 100x20)
    int width  = kWidth;
    int height = kHeight;

    /// setToken 当前累积的流 id 与序号 (同一 id 内只追加, 变化即换新流)
    std::string tokenStreamId_;
    uint64_t    tokenStreamSeq_ = 0;

    ScrollFixture() {
        sharedState.mutate([&](TUIRenderState& st) {
            st.pluginRegistry = makeTestToolRegistry();
        });
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [] {};
        ctx.theme      = &theme;
        ctx.sessionId  = "s";
        ctx.remoteUrl  = "";
        comp           = std::make_shared<MessageListComponent>(ctx);
    }

    /// 追加一条历史消息 (长文本, 撑高列表使其溢出视口)
    void addHistory(int n = 8) {
        for (int i = 0; i < n; ++i) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            for (int j = 0; j < 80; ++j) {
                m->text += "history line " + std::to_string(j) + " with some words to wrap ";
            }
            sharedState.mutate([&](TUIRenderState& st) {
                pushRow(st, std::move(*m));
            });
        }
    }

    /// 设置流式增量文本 (模拟 onDelta 的 token 追加; 内容变化时换一条新流)
    void setToken(const std::string& tok) {
        sharedState.mutate([&](TUIRenderState& st) {
            if (st.replica.hasStream() && st.replica.stream().id == tokenStreamId_) {
                const auto& cur = *st.replica.stream().text;
                if (tok.size() > cur.size() && tok.compare(0, cur.size(), cur) == 0) {
                    // 同一流的增量追加
                    agentxx::agent::WireDelta d;
                    d.type  = agentxx::agent::WireDelta::Type::TextToken;
                    d.msgId = tokenStreamId_;
                    d.text  = tok.substr(cur.size());
                    st.replica.applyDelta(d);
                    st.isStreaming = true;
                    return;
                }
            }
            tokenStreamId_ = fmt::format("t_token_{}", ++tokenStreamSeq_);
            setStream(st, TUIMessage::Role::Assistant, tok);
        });
    }

    /// 模拟 onDelta 新建流式 token (指定流身份 epoch + 整体替换内容);
    /// 同流追加时保持相同 epoch (与 onDelta 的"同一段流追加"语义一致)
    void setTokenEpoch(
        uint64_t         epoch,
        const std::string& tok,
        TUIMessage::Role role = TUIMessage::Role::Assistant
    ) {
        tokenStreamId_ = fmt::format("t_token_{}", epoch);
        sharedState.mutate([&](TUIRenderState& st) {
            setStream(st, role, tok, epoch);
        });
    }

    /// 模拟 onSync: 整体重建 TUIRenderState (流式项丢弃, 消息历史保留)
    /// 注意: 必须深拷贝消息 (每个消息新建 shared_ptr) —— 真实 onSync 从
    /// WireMessage 反序列化重建 ViewMessage, 消息指针全变, 驱动 itemKey 变化;
    /// 若浅拷贝 (共享指针), key 不变, 无法模拟缓存失效路径
    void rebuildState() {
        sharedState.mutate([&](TUIRenderState& st) {
            // 等价于 onSync 的整体替换: 消息全部换新对象 (渲染缓存随指针失效)
            agentxx::agent::WireSyncPayload p;
            for (const auto& m : st.replica.rows()) {
                p.messages.push_back(*m);
            }
            p.totalMessages = p.messages.size();
            st.replica.reset(p);
            st.isStreaming = false;
        });
    }

    /// 流式结束: 用同一个 id 定稿成正式行 (模拟 agent 的 insert_message)
    void endStream() {
        sharedState.mutate([&](TUIRenderState& st) {
            if (st.replica.hasStream() && !st.replica.stream().text->empty()) {
                auto m  = TUIMessage::makeText(TUIMessage::Role::Assistant, *st.replica.stream().text);
                m.id    = st.replica.stream().id;
                pushRow(st, std::move(m));
            }
            clearStream(st);
        });
    }

    /// 渲染一帧 (模拟 UI 循环: 取快照 -> 渲染)
    std::string render() {
        return renderScreen().ToString();
    }

    /// 渲染一帧并返回屏幕 (逐格样式可查: 颜色 / dim)
    ftxui::Screen renderScreen() {
        ctx.frameState = sharedState.readSnapshot();
        auto el        = comp->Render();
        auto screen    = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return screen;
    }

    /// 在消息列表区域中部发送滚轮事件 (返回是否被处理)
    bool wheel(ftxui::Mouse::Button button) {
        ftxui::Mouse m;
        m.button = button;
        m.motion = ftxui::Mouse::Pressed;
        m.x      = 40;
        m.y      = height / 2;
        return comp->OnEvent(ftxui::Event::Mouse("", m));
    }

    /// 取渲染文本的最后一行 (去除 ANSI 控制序列干扰后判断内容)
    static std::string lastLine(const std::string& screen) {
        size_t pos = screen.rfind('\n');
        return (pos == std::string::npos) ? screen : screen.substr(pos + 1);
    }

    /// 取渲染文本的最后 n 行 (消息子项尾部带空行, 标记可能落在倒数第二行)
    static std::string lastLines(const std::string& screen, int n) {
        std::vector<std::string> lines;
        std::string              s   = screen;
        size_t                   pos = 0;
        while ((pos = s.find('\n')) != std::string::npos) {
            lines.push_back(s.substr(0, pos));
            s.erase(0, pos + 1);
        }
        if (!s.empty()) {
            lines.push_back(s);
        }
        std::string out;
        for (size_t k
             = (lines.size() > static_cast<size_t>(n)) ? lines.size() - static_cast<size_t>(n) : 0;
             k < lines.size();
             ++k) {
            out += lines[k];
            out += '\n';
        }
        return out;
    }
};

/// 高视口夹具 (可见条数 > 消息列表 LazyScrollable 预算 maxItems=64):
/// 用于复现"可见集自身超预算 -> 预算淘汰误伤可见子项" (见场景 17b)
struct TallScrollFixture : ScrollFixture {
    TallScrollFixture() {
        width  = kWidth;
        height = 150; // 折叠消息 2 行/条 -> 可见 ~75 条 > 64
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 历史分页前插锚定 (LazyScrollable::notifyPrepended):
// 头部插入子项后视口内容保持稳定 —— offset 与总高按新增区估算行数同步
// 下移, 既有条目的实测高度/渲染缓存随索引平移保留; 渲染画面 (除滚动条
// gutter 列外) 与前插前完全一致
// ---------------------------------------------------------------------------

/// 规范化屏幕文本: 去掉右侧滚动条/间隔列并 rstrip, 供逐行画面一致性比较
static std::string normalizeScreenForCompare(const std::string& s, int width) {
    const size_t keep = width > 3 ? static_cast<size_t>(width) - 3 : 1;
    std::string  out;
    size_t       pos = 0;
    while (pos <= s.size()) {
        size_t      nl   = s.find('\n', pos);
        std::string line = s.substr(pos, (nl == std::string::npos ? s.size() : nl) - pos);
        if (line.size() > keep) {
            line.resize(keep);
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out += line;
        out += '\n';
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

/// 剥离 ANSI 转义序列 (颜色/样式码), 便于按纯文本断言跨多个文本节点的内容
static std::string stripAnsiCodes(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) {
                ++i;
            }
            continue;
        }
        out += s[i];
    }
    return out;
}

/// 屏幕文本中首个包含 needle 的行号 (0 起; 未找到返回 -1)
/// 用于断言版面行序 (如"正文 / 空行 / 占位提示"三行的相对位置)
static int screenRowOf(std::string_view screen, std::string_view needle) {
    int    row = 0;
    size_t pos = 0;
    while (pos <= screen.size()) {
        const size_t nl  = screen.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? screen.size() : nl;
        if (screen.substr(pos, end - pos).find(needle) != std::string_view::npos) {
            return row;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
        ++row;
    }
    return -1;
}

/// 屏幕文本第 row 行 (0 起; 越界返回空串)
static std::string screenRowText(std::string_view screen, int row) {
    if (row < 0) {
        return {};
    }
    int    cur = 0;
    size_t pos = 0;
    while (pos <= screen.size()) {
        const size_t nl  = screen.find('\n', pos);
        const size_t end = (nl == std::string_view::npos) ? screen.size() : nl;
        if (cur == row) {
            return std::string{screen.substr(pos, end - pos)};
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
        ++cur;
    }
    return {};
}

/// 该行是否为空白行 (无任何可见字符)
static bool isBlankRow(std::string_view row) {
    return row.find_first_not_of(" \t\r") == std::string_view::npos;
}

/// 消息内容区是否为空白行: 忽略右侧留白区 (最后 3 列)
///
/// 最后 3 列是消息列表的版面装饰 (滚动条 gutter + 右侧跳转按钮列 `▲ - + ▼`),
/// 不属于内容 —— 判断"内容之间是否隔了空行"时要把它们排除
static bool isContentBlankRow(std::string_view row, int width) {
    const size_t keep = width > 3 ? static_cast<size_t>(width) - 3 : 1;
    if (row.size() > keep) {
        row = row.substr(0, keep);
    }
    return isBlankRow(row);
}

/// 推进组件树动画 n 帧 (与运行期一致: 每次回调只给上一帧到当前的时间差)
static void advanceAnimation(ftxui::ComponentBase& comp, int frames, float stepSeconds = 0.09F) {
    for (int i = 0; i < frames; ++i) {
        ftxui::animation::Params params{ftxui::animation::Duration{stepSeconds}};
        comp.OnAnimation(params);
    }
}

static void testHistoryPrependAnchoring() {
    // ---- 场景 A: 上滑后前插更早消息, 视口画面与偏移保持稳定 ----
    {
        ScrollFixture f;
        f.addHistory(8);
        // 模拟尾窗同步窗口元数据: 窗口上方还有 100 条更早历史
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.history.reset(100, 108, st.messages().size());
        });
        f.render();
        f.render(); // 两帧建立测量/缓存

        // 向上滚动若干行 (解除吸附, 视口离开底部且远离顶部)
        for (int i = 0; i < 6; ++i) {
            (void)f.wheel(ftxui::Mouse::WheelUp);
        }
        std::string scrolled = f.render();
        const int   offset   = f.comp->scrollOffset();
        const int   total    = f.comp->totalHeight();
        XX_TEST_EXPECT_TRUE(offset > 0);

        // 模拟分页响应: 窗口上方整体前插 4 条更早消息 + 锚定通知
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 4; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                for (int j = 0; j < 40; ++j) {
                    m->text += "older history " + std::to_string(j) + " words to wrap ";
                }
                prependRow(st, std::move(*m));
            }
            st.history.reset(96, 108, st.messages().size());
        });
        f.comp->onHistoryPrepended(4);
        std::string after = f.render();

        // 视口内容逐行一致 (新增区在视口上方, 不影响可见内容)
        XX_TEST_EXPECT_EQ(
            normalizeScreenForCompare(scrolled, ScrollFixture::kWidth),
            normalizeScreenForCompare(after, ScrollFixture::kWidth)
        );
        // 偏移随新增区估算行数下移, 且与总高增量一致 (未触发新测量时严格相等)
        XX_TEST_EXPECT_EQ(f.comp->scrollOffset() - offset, f.comp->totalHeight() - total);
        XX_TEST_EXPECT_TRUE(f.comp->scrollOffset() > offset);
        XX_TEST_EXPECT_TRUE(f.comp->totalHeight() > total);
    }

    // ---- 场景 B: 前插后继续上滑到顶部区域, 实测校正稳定后不破坏视口 ----
    {
        ScrollFixture f;
        f.addHistory(8);
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.history.reset(100, 108, st.messages().size());
        });
        f.render();
        f.render();

        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "PAGED_OLDER_MESSAGE";
            prependRow(st, std::move(*m));
            st.history.reset(99, 108, st.messages().size());
        });
        f.comp->onHistoryPrepended(1);
        f.render();
        f.render();

        // 连续上滑穿过新增区直至顶部: 偏移始终合法 (实测修正会使 offset
        // 数值小幅波动 —— 上方内容实测变高时同步下移以保持视口内容稳定,
        // 这正是锚定的目的, 故不断言数值单调)。上限取总高+冗余, 保证可达顶
        for (int i = 0; i < f.comp->totalHeight() + 100 && f.comp->scrollOffset() > 0; ++i) {
            (void)f.wheel(ftxui::Mouse::WheelUp);
            f.render();
            const int off = f.comp->scrollOffset();
            XX_TEST_EXPECT_TRUE(off >= 0);
            XX_TEST_EXPECT_TRUE(off < f.comp->totalHeight());
        }
        XX_TEST_EXPECT_EQ(f.comp->scrollOffset(), 0);
        // 到达顶部后画面可见前插的更早消息
        XX_TEST_EXPECT_TRUE(
            normalizeScreenForCompare(f.render(), ScrollFixture::kWidth).find("PAGED_OLDER_MESSAGE")
            != std::string::npos
        );

        // 重置锚定状态 (会话切换路径): 幂等无副作用
        f.comp->resetHistoryPagination();
        XX_TEST_EXPECT_TRUE(!f.render().empty());
    }

    // ---- 场景 C: 首屏填充 (前插前列表为空) 不做锚定, 直接展示 ----
    {
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "FIRST_LOADED";
            pushRow(st, std::move(*m));
            st.history.reset(50, 50, st.messages().size());
        });
        f.comp->onHistoryPrepended(1); // 空数组上调用安全 (仅记录条数语义)
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("FIRST_LOADED") != std::string::npos);
    }

    // ---- 场景 D: 本地条数上限清理 (头部删除 k 条, 见 model/message_window.h) ----
    //
    // 客户端在末尾追加消息后, 若超出保留窗口 (max(末尾 100 条, 视口及其上方几条)),
    // 会把**视口上方**的头部消息删掉 (消息数据 + 渲染缓存), 并同步:
    // - 消息列表并行数组与锚点前移 (视口内容零跳变)
    // - 历史窗口起点右移 (被删的一段在服务端仍在, 上滚时可重新分页拉回)
    {
        ScrollFixture f;
        f.addHistory(8); // 每条都很高 -> 吸附底部时视口顶行在末尾几条上
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.history.reset(100, 108, st.messages().size());
        });
        f.render();
        f.render(); // 两帧建立测量/缓存

        const size_t count = 8;
        const size_t safe  = f.comp->trimSafeHeadCount(count);
        XX_TEST_EXPECT_TRUE(safe > 0);
        XX_TEST_EXPECT_TRUE(safe < count); // 视口内的消息不参与清理
        // 判定 (与 applyLocalTrimCheck 同一套规则): 不再看是否吸附底部,
        // 保留窗口 = max(末尾上限, 视口顶行及其后全部 + 视口上方 margin)
        XX_TEST_EXPECT_TRUE(f.comp->isStickToBottom());
        XX_TEST_EXPECT_TRUE(safe > kLocalMessageViewportMargin); // 本夹具前提
        XX_TEST_EXPECT_EQ(
            planLocalMessageTrim(kLocalMessageLimit + 30, safe),
            safe - kLocalMessageViewportMargin // 视口位置比末尾上限更大 -> 按视口保留
        );
        // 上滚到顶后同一套判定: 视口顶行在第 0 条, 没有可清理的消息
        XX_TEST_EXPECT_EQ(planLocalMessageTrim(kLocalMessageLimit + 30, 0), size_t{0});

        const std::string before       = f.render();
        const int         totalBefore  = f.comp->totalHeight();
        const int         offsetBefore = f.comp->scrollOffset();
        const size_t      dropped      = std::min<size_t>(3, safe);

        // 模拟清理: 消息数据删除 + 历史窗口记账 (client 线程) + 组件下标平移
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.replica.trimHead(dropped);
            st.history.noteTrimmedHead(dropped);
        });
        f.comp->onHistoryTrimmed(dropped);
        const std::string after = f.render();

        // 视口内容逐行一致 (被删消息全部位于视口上方; 缓存随索引平移保留)
        XX_TEST_EXPECT_EQ(
            normalizeScreenForCompare(before, ScrollFixture::kWidth),
            normalizeScreenForCompare(after, ScrollFixture::kWidth)
        );
        // 总高度与派生偏移同步减少 (被删区在锚点上方, 只影响滚动条长度)
        XX_TEST_EXPECT_TRUE(f.comp->totalHeight() < totalBefore);
        XX_TEST_EXPECT_TRUE(f.comp->scrollOffset() < offsetBefore);
        // 可清理区间随列表前移 (锚点索引同步减少)
        XX_TEST_EXPECT_EQ(f.comp->trimSafeHeadCount(count - dropped), safe - dropped);
        // 历史窗口: 起点右移 -> 上方重新成为"还有更早历史" (可再次分页拉回)
        {
            auto snap = f.sharedState.readSnapshot();
            XX_TEST_EXPECT_EQ(snap->history.windowStart(), 100 + static_cast<uint64_t>(dropped));
            XX_TEST_EXPECT_EQ(snap->history.totalCount(), uint64_t{108});
            XX_TEST_EXPECT_TRUE(snap->history.hasMore());
        }

        // 上滚到顶 (不再吸附底部): 视口顶行在第 0 条, 没有可清理的消息。
        // 判定不再要求吸附底部, 但视口及其上方 margin 条一律不动
        for (int i = 0; i < 4000; ++i) {
            (void)f.wheel(ftxui::Mouse::WheelUp);
        }
        f.render();
        XX_TEST_EXPECT_FALSE(f.comp->isStickToBottom());
        {
            const size_t scrolled = f.comp->trimSafeHeadCount(count - dropped);
            XX_TEST_EXPECT_EQ(scrolled, size_t{0});
            XX_TEST_EXPECT_EQ(planLocalMessageTrim(kLocalMessageLimit + 30, scrolled), size_t{0});
        }
    }
}

// ---------------------------------------------------------------------------
// 消息列表头部角色标签与 Tip 级别文本的界面语言切换
// ---------------------------------------------------------------------------

/// 头部角色标签 ("[Think]"/"[Tool]"/"[System]") 与 Tip 前缀 ("[Tip] <级别> · ")
/// 随界面语言切换; Tip 折叠态格式为 "+ [Tip] Warn · <单行预览>"
static void testRoleLabelLocalization() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();

    // 中文界面: 四种角色的折叠态消息头部均为中文标签
    settings.setLanguage(TuiLanguage::ZhCn);
    {
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto think       = std::make_shared<TUIMessage>();
            think->role      = TUIMessage::Role::Think;
            think->collapsed = true;
            think->text      = "think body";
            pushRow(st, std::move(*think));

            auto tool                = std::make_shared<TUIMessage>();
            tool->role               = TUIMessage::Role::Tool;
            tool->collapsed          = true;
            tool->text               = R"({"path":"/tmp/a.txt"})";
            tool->tool               = TUIMessage::ToolData{};
            tool->tool->toolName     = "agentxx_filesystem_read";
            tool->tool->toolFinished = true;
            tool->tool->toolResult   = "ok";
            pushRow(st, std::move(*tool));

            auto tip           = std::make_shared<TUIMessage>();
            tip->role          = TUIMessage::Role::Tip;
            tip->collapsed     = true;
            tip->text          = "disk space low";
            tip->tip           = TUIMessage::TipData{};
            tip->tip->tipLevel = TUIMessage::TipLevel::Warning;
            pushRow(st, std::move(*tip));

            auto sys       = std::make_shared<TUIMessage>();
            sys->role      = TUIMessage::Role::System;
            sys->collapsed = true;
            sys->text      = "system note";
            pushRow(st, std::move(*sys));
        });

        f.render();
        // 级别标签与正文颜色不同 (级别色 vs hint), 屏幕文本中间会夹 ANSI 转义,
        // 故先剥离再按纯文本断言
        std::string frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(frame.find("+ [思考]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("+ [工具]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("+ [系统]") != std::string::npos);
        // Tip 折叠态: "+ [提示] 警告 · <单行预览>"
        XX_TEST_EXPECT_TRUE(frame.find("+ [提示] 警告 · disk space low") != std::string::npos);

        // 语言切换 (与 agent_tui 的 onLanguageChange 一致: 清缓存后重建) -> 英文标签
        settings.setLanguage(TuiLanguage::EnUs);
        f.comp->invalidateCache();
        std::string en = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(en.find("+ [Think]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(en.find("+ [Tool]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(en.find("+ [System]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(en.find("+ [Tip] Warn · disk space low") != std::string::npos);
    }

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 事件中枢投递的输入: 专属展示角色 Role::Event (折叠头 `+ [事件] <事件名>`)
//
// 来源是**结构化字段** (`ViewMessage::source`, 取值 `event:<事件名>`), 不是从正文
// 里扒出来的: 正文已经是渲染好的 `[Event]` / `[Info]` / `[Content]` 文本, 再解析一遍会把
// 展示逻辑绑在文本格式上。正文通常很长, 所以默认折叠、点击整块展开。
// ---------------------------------------------------------------------------

void testEventSourceBadge() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();
    settings.setLanguage(TuiLanguage::ZhCn);

    ScrollFixture f;
    f.sharedState.mutate([](TUIRenderState& st) {
        // 正文是中枢渲染好的三段文本 (内容段是值本身 + 后台任务的产物正文)
        auto eventMsg = std::make_shared<TUIMessage>(TUIMessage::makeText(
            TUIMessage::Role::Event,
            "[Event] chat.message\n[Info]\nmode: first\nwait: w-3\nstatus: value\n"
            "[Content]\n{\"asyncId\":\"async-7\",\"kind\":\"tool\",\"name\":\"grep\"}\n"
            "content:\ntext: hi there\ntail: EVBODY9XYZ",
            1000
        ));
        eventMsg->source = std::string{agentxx::agent::kInputSourceEventPrefix} + "chat.message";
        pushRow(st, std::move(*eventMsg));

        auto userMsg  = std::make_shared<TUIMessage>();
        userMsg->role = TUIMessage::Role::User;
        userMsg->text = "typed by the user USERBODY8ABC";
        pushRow(st, std::move(*userMsg));
    });

    // 中文界面的事件头 (i18n 键 msg.eventPrefix, 见 tui_i18n.cpp)
    const std::string header = "+ [事件] chat.message";
    // 折叠预览取 `[Content]` 段第一行 (值本身原样给出, 元信息不重复)
    const std::string preview = "{\"asyncId\":\"async-7\",\"kind\":\"tool\",\"name\":\"grep\"}";

    // 折叠态 (默认): 头部标记 + 事件名 + " · " + 内容预览; 正文尾部标记不入屏
    std::string frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(frame.find(header + " · " + preview) != std::string::npos);
    XX_TEST_EXPECT_TRUE(frame.find("EVBODY9XYZ") == std::string::npos);
    // 事件名只出现一次 (只在那条事件消息的头部)
    {
        size_t first = frame.find(header);
        size_t next  = first == std::string::npos ? std::string::npos
                                                  : frame.find(header, first + 1);
        XX_TEST_EXPECT_TRUE(first != std::string::npos);
        XX_TEST_EXPECT_TRUE(next == std::string::npos);
    }
    // 用户自己输入的消息: `- ` 开头, 正文完整展示 (默认展开)
    XX_TEST_EXPECT_TRUE(frame.find("- typed by the user USERBODY8ABC") != std::string::npos);

    // 折叠头样式: `+` 与 `[事件]` 前缀用高亮色; 事件名与内容预览叠加 dim
    {
        auto        screen = f.renderScreen();
        int         rowY   = -1;
        std::string row;
        for (int y = 0; y < screen.dimy(); ++y) {
            // 逐格文本只保留每个格的首字节 (宽字符断成单字节), 所以用 ASCII 的事件名定位行
            auto text = screenRowText(screen, y);
            if (text.find("chat.message") != std::string::npos) {
                rowY = y;
                row  = std::move(text);
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(rowY >= 0);
        if (rowY >= 0) {
            const auto markAt = row.find('+');
            XX_TEST_EXPECT_TRUE(markAt != std::string::npos);
            if (markAt != std::string::npos) {
                const auto& mark = screen.PixelAt(static_cast<int>(markAt), rowY);
                XX_TEST_EXPECT_TRUE(mark.foreground_color == f.theme.accentColor);
                XX_TEST_EXPECT_FALSE(mark.dim);
            }
            // 名字与内容之间是 " · " (两边各一个空格): 内容从名字后 3 列开始
            const auto nameAt = row.find("chat.message");
            XX_TEST_EXPECT_TRUE(nameAt != std::string::npos);
            const auto prevAt = row.find(preview);
            XX_TEST_EXPECT_TRUE(prevAt != std::string::npos);
            if (nameAt != std::string::npos && prevAt != std::string::npos) {
                XX_TEST_EXPECT_EQ(prevAt, nameAt + 12 + 3);
            }
            if (nameAt != std::string::npos) {
                const auto& name = screen.PixelAt(static_cast<int>(nameAt), rowY);
                XX_TEST_EXPECT_TRUE(name.foreground_color == f.theme.accentColor);
                XX_TEST_EXPECT_TRUE(name.dim);
            }
            if (prevAt != std::string::npos) {
                XX_TEST_EXPECT_TRUE(screen.PixelAt(static_cast<int>(prevAt), rowY).dim);
            }
        }
    }

    // 点击事件消息 -> 展开: 标记变 `-`, 正文全文上屏
    f.render(); // 命中盒由上一帧 visibleBoxes 反推, 再渲染一帧
    bool clicked = false;
    for (const auto& box : f.comp->collapsibleBoxes()) {
        if (box.IsEmpty()) {
            continue;
        }
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = (box.x_min + box.x_max) / 2;
        m.y      = (box.y_min + box.y_max) / 2;
        clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
        break;
    }
    XX_TEST_EXPECT_TRUE(clicked);
    std::string expanded = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(expanded.find("- [事件] chat.message") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("EVBODY9XYZ") != std::string::npos);
    // 展开态: 全文上屏 (含分段标记与产物正文), 事件名不再 dim
    XX_TEST_EXPECT_TRUE(expanded.find("[Info]") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("mode: first") != std::string::npos);
    XX_TEST_EXPECT_TRUE(expanded.find("content:") != std::string::npos);
    {
        auto        screen = f.renderScreen();
        int         rowY   = -1;
        std::string row;
        for (int y = 0; y < screen.dimy(); ++y) {
            auto text = screenRowText(screen, y);
            if (text.find("chat.message") != std::string::npos) {
                rowY = y;
                row  = std::move(text);
                break;
            }
        }
        XX_TEST_EXPECT_TRUE(rowY >= 0);
        // 展开态的标记是 `-`
        XX_TEST_EXPECT_TRUE(rowY >= 0 && row.find('-') != std::string::npos);
        const auto nameAt = rowY >= 0 ? row.find("chat.message") : std::string::npos;
        XX_TEST_EXPECT_TRUE(nameAt != std::string::npos);
        if (nameAt != std::string::npos) {
            const auto& name = screen.PixelAt(static_cast<int>(nameAt), rowY);
            XX_TEST_EXPECT_TRUE(name.foreground_color == f.theme.accentColor);
            XX_TEST_EXPECT_FALSE(name.dim);
        }
    }
    {
        auto snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages().empty() && !snap->messages()[0]->collapsed);
    }

    // 再次点击 -> 折回
    f.render();
    clicked = false;
    for (const auto& box : f.comp->collapsibleBoxes()) {
        if (box.IsEmpty()) {
            continue;
        }
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = (box.x_min + box.x_max) / 2;
        m.y      = (box.y_min + box.y_max) / 2;
        clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
        break;
    }
    XX_TEST_EXPECT_TRUE(clicked);
    frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(frame.find(header) != std::string::npos);
    XX_TEST_EXPECT_TRUE(frame.find("EVBODY9XYZ") == std::string::npos);

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 事件内容被转存到 share store 时的折叠预览: 提示行不算内容
// (正文 `[Content]` 段的第一行是取回提示, 预览应跳过它看后面的开头片段)
// ---------------------------------------------------------------------------

void testEventOffloadPreview() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();
    settings.setLanguage(TuiLanguage::ZhCn);

    ScrollFixture f;
    f.sharedState.mutate([](TUIRenderState& st) {
        auto eventMsg = std::make_shared<TUIMessage>(TUIMessage::makeText(
            TUIMessage::Role::Event,
            "[Event] big.done\n[Info]\nmode: first\nwait: w-1\nstatus: value\n[Content]\n"
            "[Content offloaded. Use the `agentxx_share_store` tool to fetch the content by "
            "ID 3. Total 200 lines, show [1, 66], hide [67, 200].]\n"
            "{\"asyncId\":\"async-7\"}\ncontent:\nPREVIEWLINE7Q",
            1000
        ));
        eventMsg->source = std::string{agentxx::agent::kInputSourceEventPrefix} + "big.done";
        pushRow(st, std::move(*eventMsg));

        // 没有值的投递 (`[Content: empty]`): 折叠头不画预览
        auto quiet       = std::make_shared<TUIMessage>(TUIMessage::makeText(
            TUIMessage::Role::Event,
            "[Event] quiet.evt\n[Info]\nmode: first\nwait: w-2\nstatus: ended-empty\n[Content: empty]",
            2000
        ));
        quiet->source    = std::string{agentxx::agent::kInputSourceEventPrefix} + "quiet.evt";
        pushRow(st, std::move(*quiet));
    });

    const std::string frame = stripAnsiCodes(f.render());
    // 预览是提示行后面的第一行内容
    XX_TEST_EXPECT_TRUE(
        frame.find("+ [事件] big.done · {\"asyncId\":\"async-7\"}") != std::string::npos
    );
    XX_TEST_EXPECT_TRUE(frame.find("Content offloaded") == std::string::npos);
    // `[Content: empty]`: 只有折叠头, 没有 " · " 与预览
    XX_TEST_EXPECT_TRUE(frame.find("+ [事件] quiet.evt") != std::string::npos);
    XX_TEST_EXPECT_TRUE(frame.find("quiet.evt · ") == std::string::npos);

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 用户消息支持展开/折叠: 开头标记与其它可折叠消息统一为 `+` / `-`
// (原来是不可折叠的 `> ` 前缀)
// ---------------------------------------------------------------------------

void testUserMessageCollapse() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();
    settings.setLanguage(TuiLanguage::ZhCn);

    ScrollFixture f;
    f.sharedState.mutate([](TUIRenderState& st) {
        auto userMsg  = std::make_shared<TUIMessage>();
        userMsg->role = TUIMessage::Role::User;
        userMsg->text
            = "first line of the user input USRHEAD7XYZ\nsecond line USRTAIL3ABC";
        pushRow(st, std::move(*userMsg));
    });

    // 默认展开: `- ` 开头, 正文逐行展示
    std::string frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(frame.find("- first line of the user input USRHEAD7XYZ")
                        != std::string::npos);
    XX_TEST_EXPECT_TRUE(frame.find("second line USRTAIL3ABC") != std::string::npos);

    // 点击 -> 折叠: `+ ` 开头 + 单行预览 (只保留首行)
    f.render();
    bool clicked = false;
    for (const auto& box : f.comp->collapsibleBoxes()) {
        if (box.IsEmpty()) {
            continue;
        }
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = (box.x_min + box.x_max) / 2;
        m.y      = (box.y_min + box.y_max) / 2;
        clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
        break;
    }
    XX_TEST_EXPECT_TRUE(clicked);
    frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(frame.find("+ first line of the user input USRHEAD7XYZ")
                        != std::string::npos);
    XX_TEST_EXPECT_TRUE(frame.find("second line USRTAIL3ABC") == std::string::npos);

    // 再点一次 -> 展开
    f.render();
    clicked = false;
    for (const auto& box : f.comp->collapsibleBoxes()) {
        if (box.IsEmpty()) {
            continue;
        }
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = (box.x_min + box.x_max) / 2;
        m.y      = (box.y_min + box.y_max) / 2;
        clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
        break;
    }
    XX_TEST_EXPECT_TRUE(clicked);
    frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(frame.find("second line USRTAIL3ABC") != std::string::npos);

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 消息列表项的附件显示 (统一形态)
//
// - 折叠: 消息行最右侧只显示数量 `n个附件`
// - 展开: 正文之后 (消息末尾) 列出附件
//   `|- n个附件（点击打开）` / `|- 类型: 文件名 大小`
// ---------------------------------------------------------------------------

/// 去掉行尾空白 (屏幕行按整宽输出, 尾部是版面留白)
static std::string trimRowEnd(std::string_view row) {
    size_t end = row.size();
    while (end > 0 && (row[end - 1] == ' ' || row[end - 1] == '\r')) {
        --end;
    }
    return std::string{row.substr(0, end)};
}

void testUserMessageAttachments() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();
    settings.setLanguage(TuiLanguage::ZhCn);

    ScrollFixture f;
    f.sharedState.mutate([](TUIRenderState& st) {
        auto row  = std::make_shared<TUIMessage>();
        row->role = TUIMessage::Role::User;
        row->text = "look at ATTACHPHOTO7";
        agentxx::agent::MediaAttachment photo;
        photo.type        = agentxx::agent::MediaType::Image;
        photo.displayName = "photo.png";
        photo.sizeBytes   = 12345;
        agentxx::agent::MediaAttachment clip;
        clip.type        = agentxx::agent::MediaType::Video;
        clip.displayName = "clip.mp4";
        clip.sizeBytes   = 2 * 1024 * 1024;
        row->attachments = {photo, clip};
        pushRow(st, std::move(*row));
    });

    // ---- 展开 (默认): 正文在上, 附件列表在末尾 ----
    std::string frame     = stripAnsiCodes(f.render());
    const int   textRow   = screenRowOf(frame, "- look at ATTACHPHOTO7");
    const int   headerRow = screenRowOf(frame, "|- 2个附件（点击打开）");
    const int   photoRow  = screenRowOf(frame, "photo.png");
    const int   clipRow   = screenRowOf(frame, "clip.mp4");
    XX_TEST_EXPECT_TRUE(textRow >= 0);
    XX_TEST_EXPECT_TRUE(headerRow >= 0);
    XX_TEST_EXPECT_TRUE(headerRow == textRow + 1); // 正文紧随其后就是附件列表
    XX_TEST_EXPECT_TRUE(photoRow == headerRow + 1);
    XX_TEST_EXPECT_TRUE(clipRow == photoRow + 1);
    // 每条附件带类型与大小 (展开态不再单行重复"点击打开"提示)
    XX_TEST_EXPECT_TRUE(screenRowText(frame, photoRow).find("12.1 KB") != std::string::npos);
    XX_TEST_EXPECT_TRUE(screenRowText(frame, clipRow).find("2.0 MB") != std::string::npos);
    XX_TEST_EXPECT_TRUE(screenRowText(frame, photoRow).find("点击打开") == std::string::npos);
    // 每条附件一行都有命中区 (点击在文件管理器中定位; 命中动作本身不在测试里触发)
    XX_TEST_EXPECT_EQ(f.comp->attachmentHitBoxes().size(), size_t{2});

    // ---- 折叠: 单行预览 + 最右侧的数量, 附件列表隐藏 ----
    f.sharedState.mutate([&f](TUIRenderState& st) {
        f.sharedState.mutableMessage(st, 0).collapsed = true;
    });
    frame = stripAnsiCodes(f.render());
    const int previewRow = screenRowOf(frame, "+ look at ATTACHPHOTO7");
    XX_TEST_EXPECT_TRUE(previewRow >= 0);
    // 数量与预览在同一行, 且该行去尾后以数量结尾 (即贴在本项最右侧);
    // 字节位置还需明显靠右 —— 左对齐时它会紧跟在预览后 (约第 20 列)
    const std::string previewLine = trimRowEnd(screenRowText(frame, previewRow));
    const size_t      countAt     = previewLine.rfind("2个附件");
    XX_TEST_EXPECT_TRUE(countAt != std::string::npos);
    XX_TEST_EXPECT_TRUE(previewLine.ends_with("2个附件"));
    XX_TEST_EXPECT_TRUE(countAt > 40);
    // 附件列表在折叠态不展示
    XX_TEST_EXPECT_TRUE(screenRowOf(frame, "photo.png") < 0);
    XX_TEST_EXPECT_TRUE(screenRowOf(frame, "|- 2个附件") < 0);
    // 折叠态没有附件行命中区 (右侧计数不单独响应点击, 整行点击只是展开/收起)
    XX_TEST_EXPECT_EQ(f.comp->attachmentHitBoxes().size(), size_t{0});

    // ---- 单个附件用单数文案 ----
    f.sharedState.mutate([](TUIRenderState& st) {
        auto row = std::make_shared<TUIMessage>(*st.replica.rows()[0]);
        row->attachments.resize(1);
        row->collapsed = true;
        st.replica.replaceRow(0, row);
    });
    frame = stripAnsiCodes(f.render());
    XX_TEST_EXPECT_TRUE(trimRowEnd(screenRowText(frame, screenRowOf(frame, "+ look at")))
                            .ends_with("1个附件"));

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 附件段与消息角色无关 (所有角色共用 appendAttachmentList / appendAttachmentCount):
// - 可折叠角色 (Tool/Think/System/Tip/Event/User): 折叠时数量贴右, 展开时列表在末尾
// - 不可折叠角色 (Assistant/Interrupt): 没有折叠形态, 总是列出附件列表
// ---------------------------------------------------------------------------

void testRoleSharedAttachmentSection() {
    auto&      settings  = TUISettings::instance();
    const auto savedLang = settings.language();
    settings.setLanguage(TuiLanguage::ZhCn);

    auto photoOf = [](std::string name) {
        agentxx::agent::MediaAttachment att;
        att.type        = agentxx::agent::MediaType::Image;
        att.displayName = std::move(name);
        att.sizeBytes   = 2048;
        return att;
    };

    ScrollFixture f;
    f.sharedState.mutate([&photoOf](TUIRenderState& st) {
        // 1) 工具消息 (折叠, 2 个附件): 数量贴右, 列表不画
        auto tool                = std::make_shared<TUIMessage>();
        tool->id                 = "t_tool_att";
        tool->role               = TUIMessage::Role::Tool;
        tool->text               = "{\"path\":\"a.txt\"}";
        tool->collapsed          = true;
        tool->tool               = TUIMessage::ToolData{};
        tool->tool->toolName     = "my_tool";
        tool->tool->toolCallId   = "call_att";
        tool->tool->toolResult   = "tool result body";
        tool->tool->toolFinished = true;
        tool->attachments        = {photoOf("tool-a.png"), photoOf("tool-b.png")};
        pushRow(st, std::move(*tool));

        // 2) 正文消息 (不可折叠, 1 个附件): 总是把列表放在末尾
        auto asst        = TUIMessage::makeText(TUIMessage::Role::Assistant, "asst body ASSTBODY2");
        asst.id          = "t_asst_att";
        asst.attachments = {photoOf("asst.png")};
        pushRow(st, std::move(asst));

        // 3) 事件消息 (默认折叠, 1 个附件): 数量贴右
        auto ev        = TUIMessage::makeText(TUIMessage::Role::Event, "[Event] EVBODY3");
        ev.id          = "t_ev_att";
        ev.source      = "event:test.evt";
        ev.attachments = {photoOf("ev.png")};
        pushRow(st, std::move(ev));
    });

    std::string frame = stripAnsiCodes(f.render());

    // ---- 工具消息折叠: 头行最右侧是数量, 附件名不出现 ----
    // (角色标签随界面语言切换, 这里按工具名定位行; 行首可见字符应是折叠标记 "+")
    const int         toolRow  = screenRowOf(frame, "my_tool");
    const std::string toolLine = trimRowEnd(screenRowText(frame, toolRow));
    XX_TEST_EXPECT_TRUE(toolRow >= 0);
    const size_t firstVisible = toolLine.find_first_not_of(' ');
    XX_TEST_EXPECT_TRUE(firstVisible != std::string::npos && toolLine[firstVisible] == '+');
    XX_TEST_EXPECT_TRUE(toolLine.ends_with("2个附件"));
    XX_TEST_EXPECT_TRUE(screenRowOf(frame, "tool-a.png") < 0);
    // 折叠行没有附件命中区 (只有展开态的附件行登记命中)
    XX_TEST_EXPECT_EQ(f.comp->attachmentHitBoxes().size(), size_t{1}); // 仅 Assistant 那一行

    // ---- 正文消息 (不可折叠): 正文之后是附件列表 ----
    const int bodyRow   = screenRowOf(frame, "ASSTBODY2");
    const int headerRow = screenRowOf(frame, "|- 1个附件（点击打开）");
    const int fileRow   = screenRowOf(frame, "asst.png");
    XX_TEST_EXPECT_TRUE(bodyRow >= 0);
    XX_TEST_EXPECT_TRUE(headerRow == bodyRow + 1);
    XX_TEST_EXPECT_TRUE(fileRow == headerRow + 1);

    // ---- 事件消息 (默认折叠): 折叠头行同样把数量贴到最右 ----
    // 该行可能与右侧跳转按钮列同高, 因此按"包含"判定而不是行尾
    const int         evRow  = screenRowOf(frame, "EVBODY3");
    const std::string evLine = trimRowEnd(screenRowText(frame, evRow));
    XX_TEST_EXPECT_TRUE(evRow >= 0);
    XX_TEST_EXPECT_TRUE(evLine.find("test.evt") != std::string::npos);
    const size_t evCountAt = evLine.find("1个附件");
    XX_TEST_EXPECT_TRUE(evCountAt != std::string::npos);
    XX_TEST_EXPECT_TRUE(evCountAt > 40); // 靠右 (左对齐时约在第 30 列)
    XX_TEST_EXPECT_TRUE(screenRowOf(frame, "ev.png") < 0);

    // ---- 工具消息展开: 列表出现在末尾 (结果之后) ----
    f.sharedState.mutate([&f](TUIRenderState& st) {
        f.sharedState.mutableMessage(st, 0).collapsed = false;
    });
    frame                = stripAnsiCodes(f.render());
    const int resultRow  = screenRowOf(frame, "tool result body");
    const int toolHeader = screenRowOf(frame, "|- 2个附件（点击打开）");
    const int toolFile   = screenRowOf(frame, "tool-a.png");
    XX_TEST_EXPECT_TRUE(resultRow >= 0);
    XX_TEST_EXPECT_TRUE(toolHeader > resultRow);
    XX_TEST_EXPECT_TRUE(toolFile == toolHeader + 1);
    // 展开后两条工具附件 + 1 条正文附件都有命中区
    XX_TEST_EXPECT_EQ(f.comp->attachmentHitBoxes().size(), size_t{3});

    settings.setLanguage(savedLang);
}

// ---------------------------------------------------------------------------
// 用户消息的初始折叠状态: 换行符超 5 个或超过 200 字时折叠, 否则展开
// (判定发生在 TUI 收到消息时; 用户之后的点击不再被覆盖)
// ---------------------------------------------------------------------------

/// 测试用 TUI 端点: 暴露受保护的 onDelta/onSync (与 test_tui_stream 同一手法)
class FoldTUIClientIO : public TUIClientAgentIO {
public:

    explicit FoldTUIClientIO(asio::io_context& ctx) :
        TUIClientAgentIO(ctx.get_executor(), "fold_session") {}

    void feedDelta(const agentxx::agent::WireDelta& d) {
        onDelta(d);
    }

    void feedSync(const agentxx::agent::WireSyncPayload& p) {
        onSync(p);
    }
};

void testUserMessageFoldOnReceive() {
    // ---- 判据边界 ----
    XX_TEST_EXPECT_FALSE(shouldCollapseUserMessage(""));
    // 5 个换行符 (6 行) 仍展开, 6 个换行符折叠
    XX_TEST_EXPECT_FALSE(shouldCollapseUserMessage("1\n2\n3\n4\n5\n6"));
    XX_TEST_EXPECT_TRUE(shouldCollapseUserMessage("1\n2\n3\n4\n5\n6\n7"));
    // 200 字仍展开, 201 字折叠 (按字符计, 不按字节)
    XX_TEST_EXPECT_FALSE(shouldCollapseUserMessage(std::string(200, 'x')));
    XX_TEST_EXPECT_TRUE(shouldCollapseUserMessage(std::string(201, 'x')));
    std::string zh200;
    while (utilxx_base::utf8GetLength(zh200) < 200) {
        zh200 += "中";
    }
    XX_TEST_EXPECT_FALSE(shouldCollapseUserMessage(zh200));
    XX_TEST_EXPECT_TRUE(shouldCollapseUserMessage(zh200 + "中"));

    // ---- 接收路径: 插入的用户行按规则定初始状态 ----
    asio::io_context io;
    FoldTUIClientIO  client(io);
    auto             rowsOf = [&client] {
        return client.sharedState().snapshot()->replica.rows();
    };
    auto insertUser = [&client](std::string id, std::string text) {
        agentxx::agent::ViewMessage row
            = agentxx::agent::ViewMessage::makeText(agentxx::agent::ViewMessage::Role::User,
                                                   std::move(text));
        row.id                             = std::move(id);
        agentxx::agent::WireDelta d;
        d.type    = agentxx::agent::WireDelta::Type::InsertMessage;
        d.msgId   = row.id;
        d.message = std::make_shared<agentxx::agent::ViewMessage>(std::move(row));
        client.feedDelta(d);
    };

    insertUser("msg_short", "short user input FOLDOK1");
    insertUser("msg_long", std::string(201, 'y'));
    {
        auto rows = rowsOf();
        XX_TEST_EXPECT_EQ(rows.size(), size_t{2});
        XX_TEST_EXPECT_FALSE(rows[0]->collapsed); // 短消息: 展开
        XX_TEST_EXPECT_TRUE(rows[1]->collapsed);  // 长消息: 折叠
        // 文本本身不被改写 (折叠只是展示状态)
        XX_TEST_EXPECT_EQ(rows[0]->text, std::string{"short user input FOLDOK1"});
    }

    // ---- 同步快照里的历史用户行同样按规则定初始状态 ----
    {
        agentxx::agent::WireSyncPayload payload;
        auto shortRow = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::User,
            "history short SYNCROW1"
        );
        shortRow.id = "msg_h1";
        auto longRow = agentxx::agent::ViewMessage::makeText(
            agentxx::agent::ViewMessage::Role::User,
            "line\nline\nline\nline\nline\nline\nline SYNCROW2"
        );
        longRow.id = "msg_h2";
        payload.messages.push_back(shortRow);
        payload.messages.push_back(longRow);
        payload.totalMessages = payload.messages.size();
        client.feedSync(payload);

        auto rows = rowsOf();
        XX_TEST_EXPECT_EQ(rows.size(), size_t{2});
        XX_TEST_EXPECT_FALSE(rows[0]->collapsed);
        XX_TEST_EXPECT_TRUE(rows[1]->collapsed);
    }

    // ---- 其它角色不受规则影响 (agent 给出的折叠状态原样保留) ----
    {
        agentxx::agent::ViewMessage think
            = agentxx::agent::ViewMessage::makeText(agentxx::agent::ViewMessage::Role::Think,
                                                    "thinking text");
        think.id                        = "msg_think";
        think.collapsed                 = false;
        agentxx::agent::WireDelta d;
        d.type    = agentxx::agent::WireDelta::Type::InsertMessage;
        d.msgId   = think.id;
        d.message = std::make_shared<agentxx::agent::ViewMessage>(think);
        client.feedDelta(d);
        auto rows = rowsOf();
        XX_TEST_EXPECT_EQ(rows.size(), size_t{3});
        XX_TEST_EXPECT_FALSE(rows[2]->collapsed);
    }
}

// ---------------------------------------------------------------------------
// 回归: 连接失败 banner 的 [重试] 按钮命中区域随状态出现/消失
//
// 背景: 如果用成员 Box 保存命中区域, 且以 `Box{}` (四个分量为 0) 表示"清空",
// 造成两个问题:
// - 状态从 Failed 变为 Connected 后, 成员 Box 未被清空, 原位置仍能点中"已消失的按钮"
// - `Box{}` 实际是屏幕左上角 (0,0), 会把该处点击当成命中
// 现在命中经 UiHitMap 登记: banner 重建/离开列表时清空, 未渲染则不登记。
// ---------------------------------------------------------------------------

void testBannerRetryButtonHitArea() {
    ScrollFixture f;

    // 连接失败 (无消息, 空状态 banner): [重试] 按钮渲染并登记命中
    f.sharedState.mutate([](TUIRenderState& st) {
        st.connState = ConnState::Failed;
    });
    f.render();

    const ftxui::Box retryBox = f.comp->retryButtonBox();
    XX_TEST_EXPECT_TRUE(!retryBox.IsEmpty());
    const int clickX = (retryBox.x_min + retryBox.x_max) / 2;
    const int clickY = (retryBox.y_min + retryBox.y_max) / 2;

    int retryClicks = 0;
    f.comp->setOnRetryClick([&] {
        ++retryClicks;
    });

    ftxui::Mouse click;
    click.button = ftxui::Mouse::Left;
    click.motion = ftxui::Mouse::Released;
    click.x      = clickX;
    click.y      = clickY;
    XX_TEST_EXPECT_TRUE(f.comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(retryClicks, 1);

    // 连接成功: banner 重建, [重试] 按钮不再渲染 -> 命中区域为空, 原位置点击无效
    f.sharedState.mutate([](TUIRenderState& st) {
        st.connState = ConnState::Connected;
        st.startupProgress.clear();
    });
    f.render();
    XX_TEST_EXPECT_TRUE(f.comp->retryButtonBox().IsEmpty());
    XX_TEST_EXPECT_FALSE(f.comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(retryClicks, 1);

    // 再次失败: 按钮恢复可点
    f.sharedState.mutate([](TUIRenderState& st) {
        st.connState = ConnState::Failed;
    });
    f.render();
    const ftxui::Box retryAgain = f.comp->retryButtonBox();
    XX_TEST_EXPECT_TRUE(!retryAgain.IsEmpty());
    click.x = (retryAgain.x_min + retryAgain.x_max) / 2;
    click.y = (retryAgain.y_min + retryAgain.y_max) / 2;
    XX_TEST_EXPECT_TRUE(f.comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(retryClicks, 2);

    // 有消息后 banner 离开列表 (按钮随之失效)
    f.addHistory(1);
    f.render();
    XX_TEST_EXPECT_TRUE(f.comp->retryButtonBox().IsEmpty());
    XX_TEST_EXPECT_FALSE(f.comp->OnEvent(ftxui::Event::Mouse("", click)));
    XX_TEST_EXPECT_EQ(retryClicks, 2);
}

// ---------------------------------------------------------------------------
// 回归: 内容更新帧与同内容再渲染帧必须完全一致 (流式抖动)
// ---------------------------------------------------------------------------

// 回归: markdown 列表文本的拖选复制 (消息列表层, 含正文左侧留白与列表悬挂缩进)
//
// 背景: 列表项内容左缘在项目符号 (marker) 之后, 续行靠左侧空列 (marker 所占的
// 列) 形成悬挂缩进 —— 那些列没有任何节点绘制。修复前逐行取文本用的是"容器夹取
// 后的端点": 落在这些空列上的端点被夹成"整行选中", 复制结果多出用户没选中的
// 整行文本 (还会补出一串空格), 而每行开头的缩进又取不出来 (用户报告"在列表中的
// 文本会缺失头部、额外复制了末尾之后的字符")。
// 现在每行按"起点行到行尾 / 终点行从行首 / 中间行整行"的区间取文本与高亮
// (见 ftxui::Selection::RowRange), 列表项的悬挂缩进列按空白格取出。
static void testMarkdownListTextCopy() {
    // 视口 40x12: 正文左侧留白 3 列, 列表项前缀 "  • " (缩进 2 + 符号 2)
    ScrollFixture f;
    f.width  = 40;
    f.height = 12;
    f.sharedState.mutate([&](TUIRenderState& st) {
        auto m  = std::make_shared<TUIMessage>();
        m->role = TUIMessage::Role::Assistant;
        m->text = "- alpha bravo charlie delta echo foxtrot golf hotel india juliet\n"
                  "- second\n";
        pushRow(st, std::move(*m));
    });
    f.render();
    f.render();

    /// 拖选复制的结果 (复制文本 + 该帧屏幕, 用于核对高亮与复制一致)
    struct CopyProbe {
        std::string   text;
        ftxui::Screen screen;
    };

    /// 按 App 的帧顺序 (渲染 -> Select -> GetParts) 取一次拖选复制的结果
    auto copyOf = [&](int x0, int y0, int x1, int y1) {
        auto el     = f.comp->Render();
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(f.width),
            ftxui::Dimension::Fixed(f.height)
        );
        ftxui::Selection sel(x0, y0, x1, y1);
        ftxui::Render(screen, el.get(), sel);
        return CopyProbe{sel.GetParts(), std::move(screen)};
    };

    // 屏幕版面 (左起: 3 列留白 + "  • " + 正文, 续行与正文对齐; 留白是版面留白,
    // 不属于内容, 拖选复制不会带出来):
    //   row0 |     • alpha bravo charlie delta        |
    //   row1 |       echo foxtrot golf hotel india    |
    //   row2 |       juliet                            |
    //   row3 |     • second                            |

    // 整体拖选: 项目符号与正文都取出; 续行保留 4 列悬挂缩进, 不夹带消息列表留白
    {
        const auto probe = copyOf(0, 0, 35, 2);
        XX_TEST_EXPECT_EQ(
            probe.text,
            std::string{"  • alpha bravo charlie delta\n"
                        "    echo foxtrot golf hotel india\n"
                        "    juliet"}
        );
        // 悬挂缩进列 (3..6) 与正文一起高亮 —— 高亮与复制一致
        for (int x = 3; x <= 6; ++x) {
            XX_TEST_EXPECT_TRUE(probe.screen.PixelAt(x, 1).inverted);
        }
        XX_TEST_EXPECT_TRUE(probe.screen.PixelAt(7, 1).inverted);
    }

    // 右下 -> 左上拖选: 终点落在续行的悬挂缩进列上。
    // 修复前: 终点被夹成"整行选中" -> 复制出整行正文 (末尾多出大量字符);
    //         起点所在行的左侧空列被补成一串空格 -> 开头多出空白
    { XX_TEST_EXPECT_EQ(copyOf(20, 0, 2, 1).text, std::string{"harlie delta"}); }

    // 终点落在续行正文之中: 只取到终点列, 不多取
    {
        XX_TEST_EXPECT_EQ(
            copyOf(10, 0, 12, 1).text,
            std::string{"ha bravo charlie delta\n    echo f"}
        );
    }

    // 续行内部拖选: 只取该行的对应列 (选区不含缩进列, 不带出悬挂缩进)
    { XX_TEST_EXPECT_EQ(copyOf(14, 1, 23, 1).text, std::string{"xtrot golf"}); }

    // 只选续行的一部分: 从该行行首取到终点列 (含悬挂缩进的空白列)
    { XX_TEST_EXPECT_EQ(copyOf(0, 1, 10, 1).text, std::string{"    echo"}); }

    // 选区完全落在正文左侧留白内: 不取任何正文 (不越界取整行)
    { XX_TEST_EXPECT_EQ(copyOf(0, 0, 2, 1).text, std::string{}); }

    // 第二个列表项 (marker 在正文左侧): 整体拖选取出 marker 与正文
    { XX_TEST_EXPECT_EQ(copyOf(0, 3, 35, 3).text, std::string{"  • second"}); }
}

// ---------------------------------------------------------------------------
// 消息列表右侧跳转按钮 (▲ - + ▼)
//
// 需求: 消息列表右侧竖直排列四个按钮 (按钮之间空一行, 整组居中) —— `▲` 跳到顶部,
// `-` 跳到视口上方最近的一条用户消息, `+` 跳到视口下方最近的一条用户消息,
// `▼` 跳到底部。历史分页可能没加载完, 因此跳转只按内存中已有的消息计算,
// 不要求先加载完整。
// ---------------------------------------------------------------------------

static void testNavStripButtons() {
    using Nav = MessageListComponent::NavButton;

    /// 按钮组占用的行数 (4 个按钮, 相邻按钮之间空 1 行): 与组件内的居中算法一致
    constexpr int kNavGroupRows = 7;

    /// 构造测试会话: U0 A A A A U1 A A A A U2 A A A A U3 A A A A
    /// (助手消息各约 40 行: 保证跳到"最后一条用户消息"之后下方仍有一屏以上内容,
    ///  否则 LazyScrollable 会把视口回夹到底部窗口, 锚点下标断言不成立)
    auto buildList = [](ScrollFixture& f) {
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 4; ++i) {
                auto u  = std::make_shared<TUIMessage>();
                u->role = TUIMessage::Role::User;
                u->text = fmt::format("USER_MARK_{}", i);
                pushRow(st, std::move(*u));
                for (int j = 0; j < 4; ++j) {
                    std::string text = fmt::format("assistant {} msg{} ", j, i);
                    for (int k = 0; k < 20; ++k) {
                        text += fmt::format("filler paragraph line {} of msg{}.", k, i) + "\n\n";
                    }
                    auto a  = std::make_shared<TUIMessage>();
                    a->role = TUIMessage::Role::Assistant;
                    a->text = std::move(text);
                    pushRow(st, std::move(*a));
                }
            }
        });
    };
    constexpr size_t kUserIndex[4] = {0, 5, 10, 15};

    /// 点击指定跳转按钮 (取其命中区域左上角); 返回是否被处理
    auto clickNav = [](ScrollFixture& f, Nav button) {
        const ftxui::Box box = f.comp->navButtonBox(button);
        if (box.IsEmpty()) {
            return false;
        }
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = box.x_min;
        m.y      = box.y_min;
        return f.comp->OnEvent(ftxui::Event::Mouse("", m));
    };

    // ---- 布局: 右侧一列, 自上而下 ▲ - + ▼ (相邻按钮空一行, 整组竖直居中) ----
    {
        ScrollFixture f;
        buildList(f);
        f.render();

        const ftxui::Box boxTop  = f.comp->navButtonBox(Nav::Top);
        const ftxui::Box boxPrev = f.comp->navButtonBox(Nav::PrevUser);
        const ftxui::Box boxNext = f.comp->navButtonBox(Nav::NextUser);
        const ftxui::Box boxBot  = f.comp->navButtonBox(Nav::Bottom);
        XX_TEST_EXPECT_FALSE(boxTop.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxPrev.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxNext.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxBot.IsEmpty());

        // 同一列, 自上而下排列且相邻按钮空 1 行 (按钮字形在各自命中区域的首行)
        XX_TEST_EXPECT_EQ(boxTop.x_min, boxPrev.x_min);
        XX_TEST_EXPECT_EQ(boxTop.x_min, boxNext.x_min);
        XX_TEST_EXPECT_EQ(boxTop.x_min, boxBot.x_min);
        XX_TEST_EXPECT_EQ(boxPrev.y_min - boxTop.y_min, 2);
        XX_TEST_EXPECT_EQ(boxNext.y_min - boxPrev.y_min, 2);
        XX_TEST_EXPECT_EQ(boxBot.y_min - boxNext.y_min, 2);

        // 整组在消息区竖直居中: 20 行视口 + 组跨度 7 行 (4 按钮 + 3 个空行)
        // -> 组顶部在第 6 行 (上方 6 行、下方 7 行, 整数居中)
        XX_TEST_EXPECT_EQ(kNavGroupRows, 7);
        XX_TEST_EXPECT_EQ(f.height, ScrollFixture::kHeight);
        XX_TEST_EXPECT_EQ(boxTop.y_min, 6);
        XX_TEST_EXPECT_EQ(boxBot.y_min, 6 + kNavGroupRows - 1);

        // 位于消息列表右侧 (屏幕最右两列之内 = 右侧留白区), 不占用消息内容宽
        XX_TEST_EXPECT_EQ(boxTop.x_min, f.width - 2);

        // 屏幕上该列自上而下就是 ▲ - + ▼ (字形为多字节, 按字节长度取)
        const std::string frame      = stripAnsiCodes(f.render());
        const auto        hasGlyphAt = [&frame](int row, int col, const std::string& glyph) {
            const std::string line = screenRowText(frame, row);
            return col >= 0 && col + static_cast<int>(glyph.size()) <= static_cast<int>(line.size())
                   && line.compare(static_cast<size_t>(col), glyph.size(), glyph) == 0;
        };
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxTop.y_min, boxTop.x_min, "▲"));
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxPrev.y_min, boxPrev.x_min, "-"));
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxNext.y_min, boxNext.x_min, "+"));
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxBot.y_min, boxBot.x_min, "▼"));
        // 相邻按钮之间的空行不画任何字符
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxTop.y_min + 1, boxTop.x_min, " "));
        XX_TEST_EXPECT_TRUE(hasGlyphAt(boxPrev.y_min + 1, boxPrev.x_min, " "));
    }

    // ---- ^ 跳到顶部: 解除吸附底部, 顶行是最早的一条消息 ----
    {
        ScrollFixture f;
        buildList(f);
        f.render();
        XX_TEST_EXPECT_TRUE(f.comp->isStickToBottom()); // 默认吸附底部

        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::Top));
        std::string frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), size_t{0});
        XX_TEST_EXPECT_FALSE(f.comp->isStickToBottom());
        XX_TEST_EXPECT_TRUE(screenRowText(frame, 0).find("- USER_MARK_0") != std::string::npos);

        // 已解除吸附: 底部新增内容不拉动视图 (顶行仍是同一条消息)
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto a  = std::make_shared<TUIMessage>();
            a->role = TUIMessage::Role::Assistant;
            a->text = "appended tail marker TAILN_TOP";
            pushRow(st, std::move(*a));
        });
        f.render();
        f.render();
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), size_t{0});
        XX_TEST_EXPECT_TRUE(
            screenRowText(stripAnsiCodes(f.render()), 0).find("- USER_MARK_0") != std::string::npos
        );
    }

    // ---- v 跳到底部: 恢复吸附底部, 之后新增内容继续跟随 ----
    {
        ScrollFixture f;
        buildList(f);
        f.render();
        for (int i = 0; i < 20; ++i) {
            f.wheel(ftxui::Mouse::WheelUp);
            f.render();
        }
        XX_TEST_EXPECT_FALSE(f.comp->isStickToBottom());

        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::Bottom));
        std::string frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(f.comp->isStickToBottom());
        // 吸附底部: 最新内容贴底 (消息子项自带尾部空行, 故看最后两行)
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 2).find("filler paragraph line 19") != std::string::npos
        );

        f.sharedState.mutate([&](TUIRenderState& st) {
            auto a  = std::make_shared<TUIMessage>();
            a->role = TUIMessage::Role::Assistant;
            a->text = "newest tail marker TAILN_BOT";
            pushRow(st, std::move(*a));
        });
        f.render();
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(stripAnsiCodes(f.render()), 2).find("TAILN_BOT")
            != std::string::npos
        );
    }

    // ---- - / + 在上一条 / 下一条用户消息之间跳转 ----
    {
        ScrollFixture f;
        buildList(f);
        f.render(); // 吸附底部

        // - : 跳到视口上方最近的用户消息 (= 最后一条用户消息)
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::PrevUser));
        std::string frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), kUserIndex[3]);
        XX_TEST_EXPECT_TRUE(screenRowText(frame, 0).find("- USER_MARK_3") != std::string::npos);
        XX_TEST_EXPECT_FALSE(f.comp->isStickToBottom());

        // 再按一次 -: 继续跳到上一条用户消息
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::PrevUser));
        frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), kUserIndex[2]);
        XX_TEST_EXPECT_TRUE(screenRowText(frame, 0).find("- USER_MARK_2") != std::string::npos);

        // + : 跳到下一条用户消息
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::NextUser));
        frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), kUserIndex[3]);
        XX_TEST_EXPECT_TRUE(screenRowText(frame, 0).find("- USER_MARK_3") != std::string::npos);

        // 下滚几行使顶行落到该用户消息之后 (消息已离开视口顶), 再按 - 回到它
        for (int i = 0; i < 3; ++i) {
            f.wheel(ftxui::Mouse::WheelDown);
        }
        f.render();
        XX_TEST_EXPECT_TRUE(
            screenRowText(stripAnsiCodes(f.render()), 0).find("- USER_MARK_3") == std::string::npos
        );
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::PrevUser));
        frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), kUserIndex[3]);
        XX_TEST_EXPECT_TRUE(screenRowText(frame, 0).find("- USER_MARK_3") != std::string::npos);

        // 已在最后一条用户消息: 下方没有用户消息 -> + 退化为跳到底部 (恢复吸附)
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::NextUser));
        XX_TEST_EXPECT_TRUE(f.comp->isStickToBottom());
    }

    // ---- 列表里没有用户消息时退化: - 跳到顶部, + 跳到底部 ----
    {
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 6; ++i) {
                auto a  = std::make_shared<TUIMessage>();
                a->role = TUIMessage::Role::Assistant;
                a->text = fmt::format("assistant only {}\n\n", i) + std::string(600, 'z');
                pushRow(st, std::move(*a));
            }
        });
        f.render(); // 吸附底部

        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::PrevUser)); // 上方无用户消息 -> 顶部
        f.render();
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), size_t{0});
        XX_TEST_EXPECT_FALSE(f.comp->isStickToBottom());

        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::NextUser)); // 下方无用户消息 -> 底部
        XX_TEST_EXPECT_TRUE(f.comp->isStickToBottom());
    }

    // ---- 按钮点击不得被"折叠消息"吃掉 (命中判定顺序) ----
    {
        ScrollFixture f;
        // 首条为**展开的长 Tip** (可折叠): 它占满整个视口, 因此按钮组所在的行
        // 同时也是该消息的命中行 —— 点按钮必须只跳转, 不被折叠判定吃掉
        f.sharedState.mutate([&](TUIRenderState& st) {
            std::string body;
            for (int i = 0; i < 40; ++i) {
                body += fmt::format("tip body line {} with marker TIPNAV\n", i);
            }
            auto tip       = std::make_shared<TUIMessage>();
            tip->role      = TUIMessage::Role::Tip;
            tip->tip       = TUIMessage::TipData{};
            tip->collapsed = false;
            tip->text      = std::move(body);
            pushRow(st, std::move(*tip));
            for (int i = 0; i < 6; ++i) {
                auto a  = std::make_shared<TUIMessage>();
                a->role = TUIMessage::Role::Assistant;
                a->text = fmt::format("assistant {}\n\n", i) + std::string(600, 'y');
                pushRow(st, std::move(*a));
            }
        });
        f.render();
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::Top)); // 跳到顶部: 长 Tip 占满视口
        f.render();                                 // 落实跳转
        f.render(); // 折叠命中区域按顶部视图重建 (由上一帧可见区域反推)

        const int navRow = f.comp->navButtonBox(Nav::Top).y_min;
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::Top).Contain(f.width - 2, navRow));

        // 按钮所在行同样是 Tip 消息的命中行: 点按钮只跳转, 不折叠该消息
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::Top));
        auto snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages().empty());
        XX_TEST_EXPECT_FALSE(snap->messages()[0]->collapsed);

        // 点消息区域 (非按钮列) 仍然折叠该消息
        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = 10;
        m.y      = navRow;
        XX_TEST_EXPECT_TRUE(f.comp->OnEvent(ftxui::Event::Mouse("", m)));
        snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(snap->messages()[0]->collapsed);
    }

    // ---- 空列表 (banner): 无可跳转目标, 不画按钮也不登记命中 ----
    {
        ScrollFixture f;
        f.render();
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::Top).IsEmpty());
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::PrevUser).IsEmpty());
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::NextUser).IsEmpty());
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::Bottom).IsEmpty());

        ftxui::Mouse m;
        m.button = ftxui::Mouse::Left;
        m.motion = ftxui::Mouse::Released;
        m.x      = f.width - 2; // 有消息时按钮列所在列
        m.y      = 0;
        XX_TEST_EXPECT_FALSE(f.comp->OnEvent(ftxui::Event::Mouse("", m)));
    }

    // ---- 消息区高度不足: 先退化为紧凑排列 (无间隔), 仍放不下时少画下面的按钮 ----
    {
        // 6 行: 放不下带间隔的 7 行, 退化为 4 个按钮紧凑排列 (全部保留)
        ScrollFixture f;
        f.height = 6;
        buildList(f);
        f.render();
        const ftxui::Box boxTop  = f.comp->navButtonBox(Nav::Top);
        const ftxui::Box boxPrev = f.comp->navButtonBox(Nav::PrevUser);
        const ftxui::Box boxNext = f.comp->navButtonBox(Nav::NextUser);
        const ftxui::Box boxBot  = f.comp->navButtonBox(Nav::Bottom);
        XX_TEST_EXPECT_FALSE(boxTop.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxPrev.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxNext.IsEmpty());
        XX_TEST_EXPECT_FALSE(boxBot.IsEmpty());
        XX_TEST_EXPECT_EQ(boxPrev.y_min - boxTop.y_min, 1);  // 紧凑排列 (无空行)
        XX_TEST_EXPECT_EQ(boxTop.y_min, (f.height - 4) / 2); // 仍居中
        XX_TEST_EXPECT_EQ(boxBot.y_min, boxTop.y_min + 3);
    }
    {
        // 3 行: 只能放下前 3 个, 第 4 个 (▼) 不画也不登记命中
        ScrollFixture f;
        f.height = 3;
        buildList(f);
        f.render();
        XX_TEST_EXPECT_FALSE(f.comp->navButtonBox(Nav::Top).IsEmpty());
        XX_TEST_EXPECT_FALSE(f.comp->navButtonBox(Nav::PrevUser).IsEmpty());
        XX_TEST_EXPECT_FALSE(f.comp->navButtonBox(Nav::NextUser).IsEmpty());
        XX_TEST_EXPECT_TRUE(f.comp->navButtonBox(Nav::Bottom).IsEmpty()); // 第 4 行在消息区外

        // 画出来的按钮仍可用
        XX_TEST_EXPECT_TRUE(clickNav(f, Nav::Top));
        f.render();
        XX_TEST_EXPECT_EQ(f.comp->anchorIndex(), size_t{0});
    }
}

TestResult testTuiScroll() {
    XX_TEST_EXPECT_TRUE(true);

    // 消息列表头部角色标签 ([Think]/[Tool]/[System]/[Tip]) 随界面语言切换
    // (见 TuiI18n): 本模块断言英文标签, 固定界面语言为英文, 避免跟随系统语言
    auto&      tuiSettings = TUISettings::instance();
    const auto savedLang   = tuiSettings.language();
    tuiSettings.setLanguage(TuiLanguage::EnUs);

    testHistoryPrependAnchoring();

    testRoleLabelLocalization();

    // 事件中枢投递的输入: 折叠头 `+ [事件] <事件名>`
    testEventSourceBadge();

    // 内容被转存 share store 时预览跳过提示行
    testEventOffloadPreview();

    // 用户消息支持展开/折叠 (`+` / `-` 开头标记)
    testUserMessageCollapse();

    // 消息列表项的附件显示 (折叠: 右侧数量 / 展开: 末尾附件列表)
    testUserMessageAttachments();

    // 附件段与角色无关 (工具折叠贴右 / 正文消息总是末尾列表)
    testRoleSharedAttachmentSection();

    // 用户消息的初始折叠状态 (换行 > 5 或字数 > 200 时折叠)
    testUserMessageFoldOnReceive();

    testBannerRetryButtonHitArea();

    // 消息列表右侧跳转按钮列 (▲ - + ▼)
    testNavStripButtons();

    // markdown 列表文本的拖选复制 (列表悬挂缩进 / 行内空白列上的端点)
    testMarkdownListTextCopy();

    {
        // 场景 1: 流式输出中, 内容更新帧 vs 同内容再渲染帧 (鼠标移动帧)
        ScrollFixture f;
        f.addHistory();
        f.render(); // 首帧建立布局

        std::string tok;
        for (int step = 0; step < 25; ++step) {
            // 每步追加带唯一尾部标记的 token (标记落在流式项最后一行)
            tok += "new token content " + std::to_string(step) + " with some words " + "M"
                   + std::to_string(step) + "END ";
            f.setToken(tok);
            // 内容帧: token 增长后首帧渲染
            std::string contentFrame = f.render();
            // 同内容再渲染帧: 模拟鼠标移动触发的渲染 (内容未变)
            std::string mouseFrame = f.render();

            XX_TEST_EXPECT_TRUE(contentFrame == mouseFrame);
            // 吸附底部: 屏幕最后两行内必须包含最新 token 的尾部标记。
            // 流式子项与消息块一样自带尾部空行 (见 message_list.cpp 的
            // "所有子项都自带尾部空行" 约定), 故标记落在倒数第二行
            XX_TEST_EXPECT_TRUE(
                ScrollFixture::lastLines(contentFrame, 2).find("M" + std::to_string(step) + "END")
                != std::string::npos
            );
        }
    }

    {
        // 场景 2: 流式结束, token 落为正式消息后仍吸附到底部
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.setToken("final answer text with distinctive marker Z9Q7");
        f.render();
        f.endStream();
        f.render(); // 落消息后的首帧
        std::string frame = f.render();
        // 消息子项尾部带空行, 标记在倒数第二行
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 2).find("Z9Q7") != std::string::npos);
    }

    {
        // 场景 3: 滚轮上滚解除吸附, 内容增长时视图保持不动 (不跟随)
        ScrollFixture f;
        f.addHistory();
        f.render();
        for (int i = 0; i < 5; ++i) {
            XX_TEST_EXPECT_TRUE(f.wheel(ftxui::Mouse::WheelUp));
            f.render();
        }
        // 记录当前视图 (滚动位置已上移)
        std::string before = f.render();
        XX_TEST_EXPECT_FALSE(before.find("history line 79") != std::string::npos);
        // 内容在底部增长
        f.setToken("growing content below viewport with marker Q9ZZ");
        for (int i = 0; i < 10; ++i) {
            f.render();
        }
        // 未吸附: 视图不得被底部增长拉动 (两帧渲染一致)
        std::string view1 = f.render();
        std::string view2 = f.render();
        XX_TEST_EXPECT_TRUE(view1 == view2);
        XX_TEST_EXPECT_TRUE(view1 == before);
    }

    {
        // 场景 4: 滚回底部恢复吸附, 内容继续增长时跟随到底
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.wheel(ftxui::Mouse::WheelUp);
        f.render();
        // 一直滚到底部 (视口高 20, 多滚几次必然到达)
        for (int i = 0; i < 200; ++i) {
            f.wheel(ftxui::Mouse::WheelDown);
            f.render();
        }
        // 底部内容增长 -> 跟随
        f.setToken("follow me to the bottom marker F9AA");
        f.render();
        // 流式子项自带尾部空行 (与消息块一致), 最新内容落在屏幕倒数第二行
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(f.render(), 2).find("F9AA") != std::string::npos
        );
    }

    {
        // 场景 5: 估算高度偏差大时 (含长串无空格文本) 内容帧也必须定位准确
        ScrollFixture f;
        f.addHistory(4);
        f.render();
        std::string tok;
        for (int step = 0; step < 15; ++step) {
            // 长串无空格文本 (估算/实测偏差大) + 短标记
            tok += "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                   + std::to_string(step) + " ";
            f.setToken(tok);
            std::string c1 = f.render();
            std::string c2 = f.render();
            XX_TEST_EXPECT_TRUE(c1 == c2);
            // 流式子项自带尾部空行 (与消息块一致), 最新内容落在屏幕倒数第二行
            XX_TEST_EXPECT_TRUE(
                ScrollFixture::lastLines(c1, 2).find(std::to_string(step)) != std::string::npos
            );
        }
    }

    {
        // 场景 6: onSync 重建 state 后新一轮流式首 token 不丢失 (epoch 碰撞回归)
        // 回归链: 首轮流式 (UI 缓存 streamEpoch_=1) -> 流结束 -> onSync (epoch 归 0)
        //   -> 新流首 token (epoch 0->1, 恰与 UI 缓存的 streamEpoch_ 相等)
        //   -> 必须强制重建渲染器; 否则 syncStream 误判"同一流"走增量分支,
        //      而 fedLen 仍是旧流长度, 首 token 被整体跳过 (渲染缺字),
        //      直到 token 超过旧 fedLen 才从错误偏移开始显示
        ScrollFixture f;
        f.addHistory();
        f.render();

        // 第一轮流式: epoch 0 -> 1, UI 缓存 streamEpoch_ = 1
        f.setTokenEpoch(1, "first stream content marker F1ST");
        f.render();
        f.endStream(); // token 落为正式消息
        f.render();    // 无 token 帧 (OnRender 清理)

        // 模拟 onSync: 整体重建 state, epoch 归 0 (保留历史)
        f.rebuildState();
        f.render(); // 无 token 帧

        // 用户发送消息后新一轮首 token: epoch 0 -> 1, 与 UI 缓存的 streamEpoch_ 相等
        f.setTokenEpoch(1, "second stream marker S2ND");
        std::string frame = f.render();
        // 流式子项自带尾部空行 (与消息块一致), 最新内容落在屏幕倒数第二行
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 2).find("S2ND") != std::string::npos);

        // 同流后续 token 追加 (epoch 不变) 仍正常增量显示
        f.setTokenEpoch(1, "second stream marker S2ND with more tail T3ST");
        std::string frame2 = f.render();
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame2, 2).find("T3ST") != std::string::npos);
    }

    {
        // 场景 7: 主题切换 (invalidateCache) 后同内容渲染必须与切换前一致
        // 回归: clearCache 后新主题重建了 Element, 但"缓存命中且 box 相同则
        // 跳过布局"的判定误判 (lastBoxes_ 未清, 新元素从未 SetBox) ->
        // 新元素以未初始化 box_ 渲染, 消息列表消失;
        // 且此后每帧 box 恒同持续跳过, 直到内容变化才恢复
        ScrollFixture f;
        f.addHistory();
        f.render(); // 建立缓存与布局
        std::string before = f.render();

        f.comp->invalidateCache();           // 模拟主题切换: 清空渲染缓存
        std::string afterClear = f.render(); // 缓存重建首帧: 必须重新布局
        XX_TEST_EXPECT_TRUE(before == afterClear);

        // 后续帧 (缓存命中 + box 相同路径) 仍一致 (跳过布局是安全的)
        std::string cachedFrame = f.render();
        XX_TEST_EXPECT_TRUE(before == cachedFrame);

        // 流式结束后再切一次主题 (双路径: 消息 + 流式残留缓存) 仍一致
        f.setToken("streaming text with marker THME");
        f.render();
        f.endStream();
        f.render();
        std::string before2 = f.render();
        f.comp->invalidateCache();
        XX_TEST_EXPECT_TRUE(before2 == f.render());
    }

    {
        // 场景 8: onSync 整体重建 state 后 (消息指针全变), 同内容渲染必须正常
        // 回归: onSync 重建后所有消息 key 变化 -> 阶段1 全部重建新元素,
        // 但 lastBoxes_ 保留旧值 (resize 不重置), 内容相同 + 滚动位置不变时
        // itemBox 与上帧相同 -> 阶段2 误判 "缓存命中且 box 相同" 跳过布局,
        // 新元素从未 SetBox (box_ = {0,0,0,0}) -> 渲染只画首字符/整行消失
        ScrollFixture f;
        f.addHistory();
        f.render(); // 建立缓存与布局

        // 模拟 onSync: 整体重建 state (消息内容不变, 指针全变)
        f.rebuildState();
        f.render(); // 全部重建帧

        // 同内容再渲染帧: 内容未变, box 相同 -> 缓存命中/跳过布局路径
        std::string frame = f.render();
        // 历史消息完整显示 (无整行消失)
        XX_TEST_EXPECT_TRUE(frame.find("history line 79") != std::string::npos);
    }

    {
        // 场景 8b: onSync 重建后发送 user 消息, 新消息必须完整显示
        ScrollFixture f;
        f.addHistory();
        f.render();
        f.rebuildState();
        f.render();

        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hello user message with marker USRM";
            pushRow(st, std::move(*m));
            st.isStreaming = true;
        });
        f.comp->setStickToBottom(true);
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame == f.render());
    }

    {
        // 场景 8c: onSync 重建 + 估算==实测的短消息 (无折行) -> 跳过布局回归
        // 触发条件: 消息短/单行, estimateLines == 实测高度, 滚动偏移不变 ->
        // sameBox=true 误判跳过布局 -> 新元素 box_ 未初始化 {0,0,0,0} ->
        // Text 只画首字符到 (0,0) (用户报告"少开头两个字/整行不显示但有滚动高度")
        ScrollFixture f;
        // 短消息 (单行, 无折行): 估算 == 实测
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 30; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "short line " + std::to_string(i);
                pushRow(st, std::move(*m));
            }
        });
        f.render(); // 建立缓存与布局

        // 模拟 onSync: 整体重建 state (指针全变, 内容相同)
        f.rebuildState();
        std::string rebuilt = f.render(); // 全部重建帧
        if (rebuilt.find("short line 29") == std::string::npos) {
            fprintf(stderr, "[DBG8c] rebuilt missing short line 29, dump:\n");
            // 分行打印 (转义序列干扰行内查找, 但可看行分布)
            std::string s   = rebuilt;
            size_t      pos = 0, lineNo = 0;
            while ((pos = s.find('\n')) != std::string::npos) {
                fprintf(stderr, "  L%02zu: %s\n", lineNo++, s.substr(0, pos).c_str());
                s.erase(0, pos + 1);
            }
            fprintf(stderr, "  L%02zu: %s\n", lineNo, s.c_str());
        }
        // 历史消息完整显示 (无整行消失)
        XX_TEST_EXPECT_TRUE(rebuilt.find("short line 29") != std::string::npos);
        // 同内容再渲染帧一致
        XX_TEST_EXPECT_TRUE(rebuilt == f.render());

        // 发送 user 消息 (估算==实测的单行消息) 后完整显示
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "user msg marker USRM";
            pushRow(st, std::move(*m));
            st.isStreaming = true;
        });
        f.comp->setStickToBottom(true);
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(ScrollFixture::lastLines(frame, 3).find("USRM") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame == f.render());
    }

    {
        // 场景 9: System 消息支持折叠且默认折叠显示; 点击 header 折叠/展开
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m       = std::make_shared<TUIMessage>();
            m->role      = TUIMessage::Role::Tip;
            m->tip       = TUIMessage::TipData{};
            m->collapsed = true; // 默认折叠 (与 makeText(Role::Tip) 语义一致)
            m->text      = "tip tip body line that is long enough to be truncated "
                           "in the collapsed preview with unique tail marker "
                           "SYSM_TAIL_9XYZ";
            pushRow(st, std::move(*m));
        });

        // 点击命中区域由上一帧 visibleBoxes 反推: 渲染两帧建立布局与命中盒
        f.render();
        f.render();

        // 折叠态: header 显示 "+" 折叠标记 + "[Tip]" 前缀 + 单行预览,
        // 正文尾部标记 (超出 preview 截断) 不显示;
        // Info 级别不附加级别文本 (levelKey 为空), 故 "[Tip]" 后直接接预览分隔符 "· ";
        // 预览叠加了 dim 属性, 屏幕文本中间夹 ANSI 转义, 故先剥离再断言
        std::string collapsed1 = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(collapsed1.find("+ [Tip] · ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(collapsed1.find("SYSM_TAIL_9XYZ") == std::string::npos);

        // 模拟点击 header → 展开 (处理事件, 且消息折叠状态翻转)
        bool clicked = false;
        for (const auto& box : f.comp->collapsibleBoxes()) {
            if (box.IsEmpty()) {
                continue;
            }
            ftxui::Mouse m;
            m.button = ftxui::Mouse::Left;
            m.motion = ftxui::Mouse::Released;
            m.x      = (box.x_min + box.x_max) / 2;
            m.y      = (box.y_min + box.y_max) / 2;
            clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
            break;
        }
        XX_TEST_EXPECT_TRUE(clicked);

        // 展开态: "-" 展开标记 + "[Tip]" 前缀 + 正文完整显示 (尾部标记可见)
        std::string expanded = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(expanded.find("- [Tip] ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(expanded.find("SYSM_TAIL_9XYZ") != std::string::npos);

        // 状态确实更新为展开
        auto snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages().empty() && !snap->messages()[0]->collapsed);

        // 再次点击 → 重新折叠 (先渲染刷新命中区域: 展开后消息更高, box 更大)
        f.render();
        clicked = false;
        for (const auto& box : f.comp->collapsibleBoxes()) {
            if (box.IsEmpty()) {
                continue;
            }
            ftxui::Mouse m;
            m.button = ftxui::Mouse::Left;
            m.motion = ftxui::Mouse::Released;
            m.x      = (box.x_min + box.x_max) / 2;
            m.y      = (box.y_min + box.y_max) / 2;
            clicked  = f.comp->OnEvent(ftxui::Event::Mouse("", m));
            break;
        }
        XX_TEST_EXPECT_TRUE(clicked);
        std::string collapsed2 = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(collapsed2.find("+ [Tip] · ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(collapsed2.find("SYSM_TAIL_9XYZ") == std::string::npos);
        snap = f.sharedState.readSnapshot();
        XX_TEST_EXPECT_TRUE(!snap->messages().empty() && snap->messages()[0]->collapsed);
    }

    {
        // 场景 9b: System 消息折叠态按提示级别显示对应前缀 (Warning/Error)
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m           = std::make_shared<TUIMessage>();
            m->role          = TUIMessage::Role::Tip;
            m->tip           = TUIMessage::TipData{};
            m->tip->tipLevel = TUIMessage::TipLevel::Warning;
            m->collapsed     = true;
            m->text          = "warning tip body with marker WRN_TAIL_7K";
            pushRow(st, std::move(*m));

            auto m2           = std::make_shared<TUIMessage>();
            m2->role          = TUIMessage::Role::Tip;
            m2->tip           = TUIMessage::TipData{};
            m2->tip->tipLevel = TUIMessage::TipLevel::Error;
            m2->collapsed     = true;
            m2->text          = "error tip body with marker ERR_TAIL_8M";
            pushRow(st, std::move(*m2));

            auto m3       = std::make_shared<TUIMessage>();
            m3->role      = TUIMessage::Role::Tip;
            m3->tip       = TUIMessage::TipData{};
            m3->collapsed = true;
            m3->text      = "info tip body with marker INF_TAIL_6N";
            pushRow(st, std::move(*m3));
        });
        f.render();
        f.render();
        // 头部前缀与正文颜色不同 (级别色 vs hint), 屏幕文本中间会夹 ANSI 转义,
        // 故先剥离再按纯文本断言
        std::string frame = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(frame.find("[Tip] Warn · ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("[Tip] Error · ") != std::string::npos);
        // Info 级别不附加级别文本 (levelKey 为空): 前缀为 "[Tip] " 后直接接 "· ",
        // 即 "[Tip]" 与 "·" 之间只有前缀自带的一个空格 (井号分隔符已去掉)
        XX_TEST_EXPECT_TRUE(frame.find("[Tip] · ") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("[Tip] Info") == std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("[Tip] #") == std::string::npos);
        // 三条 System 消息均渲染 (折叠态预览含完整短文本 marker)
        XX_TEST_EXPECT_TRUE(frame.find("WRN_TAIL_7K") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("ERR_TAIL_8M") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("INF_TAIL_6N") != std::string::npos);
    }

    {
        // 场景 9c: 运行中的提示消息 (如"正在压缩上下文") 折叠标记处显示加载动画
        //
        // 帧循环由 hasRunningMessage() 维持: 该判定原先只看流式思考与未完成的
        // 工具消息, 提示消息运行期间两者都不存在, 点阵会停在首帧不动。
        // 结束后 (更新消息带 running=false) 标记回到静态 +/-。
        auto&      settings = TUISettings::instance();
        const auto origAnim = settings.animationLevel();
        settings.setAnimationLevel(AnimationLevel::High);

        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m          = std::make_shared<TUIMessage>();
            m->role         = TUIMessage::Role::Tip;
            m->tip          = TUIMessage::TipData{};
            m->tip->running = true;
            m->collapsed    = true;
            m->text         = "Summarizing LLM Context... RUN_TIP_1A";
            pushRow(st, std::move(*m));
        });
        // 首帧: 点阵为 spinner 首帧 (测试环境无帧循环, 帧索引停在首帧)
        XX_TEST_EXPECT_TRUE(stripAnsiCodes(f.render()).find("⠋ [Tip] · ") != std::string::npos);

        // 推进动画: 经组件树转发 OnAnimation 到 runSpinner_ (运行中的提示此时是
        // 唯一的运行中条目, 帧循环由本判定维持)
        advanceAnimation(*f.comp, 1);
        XX_TEST_EXPECT_TRUE(stripAnsiCodes(f.render()).find("⠙ [Tip] · ") != std::string::npos);

        // 压缩结束 (更新消息带 running=false): 折叠标记回到静态 "+" (折叠态)
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m         = std::make_shared<TUIMessage>();
            m->role        = TUIMessage::Role::Tip;
            m->tip         = TUIMessage::TipData{};
            m->collapsed   = true;
            m->text        = "Summarized LLM Context 100->50/200 · 0.8s";
            m->id          = st.replica.rows()[0]->id; // 同一行按 id 更新
            st.replica.replaceRow(0, std::move(m));
        });
        std::string done = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(done.find("+ [Tip] · Summarized") != std::string::npos);
        XX_TEST_EXPECT_TRUE(done.find("⠙") == std::string::npos);

        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 10: 多行单换行 (cmark softbreak) 的 markdown 消息 —— 高度估算
        // 必须与渲染语义一致 (段内换行合并为空格), 不得按 \n 计数。
        //
        // 回归: estimateLines 按 \n 硬换行计数, 而 DomBuilder 把段内单个换行
        // (softbreak) 合并为空格 (仅空行分隔的段落间插入 1 行空行) —— 对
        // "多行短句无空行"文本 (LLM 输出常见), 每条估算 >> 实测 (40 行 x
        // 25 字符在 97 列下估算 41 行/条, 实际合并折行 ~13 行/条)。总高度
        // 虚高 -> stickToBottom 滚动偏移过大, 顶部消息被推离视口, 视口内
        // 可见内容减少 (用户报告 "上半几条消息不渲染/像可用高度变小"),
        // 且被推出视口的项永不进入视口实测 -> 高估持续存在
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 10; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                for (int j = 0; j < 40; ++j) {
                    m->text += "softbreak line " + std::to_string(j) + " msg" + std::to_string(i)
                               + "\n";
                }
                pushRow(st, std::move(*m));
            }
        });
        // 多渲染几帧: stickToBottom 下底部可见项被实测修正, 总高度逼近
        // 真实值附近 ((40 行 x ~27 字符 / 97 列 ≈ 12 行 + 1 空行) x 10 条
        // ≈ 130 行); 修复前按 \n 计数 ≈ 40*10 = 400+ 行
        for (int i = 0; i < 5; ++i) {
            f.render();
        }
        const int totalH = f.comp->totalHeight();
        if (totalH >= 250 || totalH <= 60) {
            fprintf(stderr, "[DBG10] softbreak totalHeight=%d (expect ~130)\n", totalH);
        }
        XX_TEST_EXPECT_TRUE(totalH < 250);
        XX_TEST_EXPECT_TRUE(totalH > 60);
        // 吸附底部: 最后一条消息完整可见 (底部贴底, 消息尾部带空行标记在倒数第二行)
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(f.render(), 2).find("msg9") != std::string::npos
        );
    }

    {
        // 场景 10b: softbreak 消息与折叠系统消息混合 —— 估算修正后滚动
        // 定位正常, 底部最新内容可见; 修复前折叠消息之后的多条 softbreak
        // 消息 (视口外高估) 会把最新消息推出视口底部
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 折叠的 System/Tip 消息 (单行 header)
            auto tip       = std::make_shared<TUIMessage>();
            tip->role      = TUIMessage::Role::Tip;
            tip->tip       = TUIMessage::TipData{};
            tip->collapsed = true;
            tip->text      = "collapsed tip header";
            pushRow(st, std::move(*tip));
            // 8 条 softbreak assistant 消息
            for (int i = 0; i < 8; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                for (int j = 0; j < 30; ++j) {
                    m->text
                        += "soft line " + std::to_string(j) + " mixed" + std::to_string(i) + "\n";
                }
                pushRow(st, std::move(*m));
            }
        });
        f.render();
        f.render();
        f.render();
        // 底部最新消息可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(f.render(), 2).find("mixed7") != std::string::npos
        );
    }

    {
        // 场景 11 (诊断): 混合消息类型长列表 —— 从顶部逐行滚到底部,
        // 每条消息的 marker 必须至少被渲染到屏幕一次; 若某消息在
        // 滚动过程中从未出现, 说明该消息被虚拟列表误判为不可见
        // (高度估算与实际渲染偏差导致 continue/break 误判), 即用户
        // 报告的"某些消息显示为空白, 滑动到某些位置又正常"
        ScrollFixture            f;
        std::vector<std::string> markers;
        auto                     mk = [&](int i, const char* tag) {
            return fmt::format("MMK{}_{}_", i, tag);
        };

        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 24; ++i) {
                auto              m      = std::make_shared<TUIMessage>();
                const std::string marker = mk(i, "A");
                markers.push_back(marker);
                switch (i % 6) {
                    case 0: { // 普通 markdown 段落
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "plain paragraph with marker " + marker
                                  + "\n\n"
                                    "second paragraph with some more text to wrap around.\n";
                        break;
                    }
                    case 1: { // mermaid 状态图 (planning 常用)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "```mermaid\nstateDiagram-v2\n"
                                  "    [*] --> phase_a\n"
                                  "    phase_a --> phase_b\n"
                                  "    phase_b --> phase_c\n"
                                  "    phase_c --> [*]\n"
                                  "```\n"
                                  "after mermaid marker "
                                  + marker + "\n";
                        break;
                    }
                    case 2: { // 表格 (单元格长文本 -> 受限列宽自动换行)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = "| col1 | col2 |\n|---|---|\n"
                                  "| "
                                  + marker + " | " + marker + " |\n";
                        break;
                    }
                    case 3: { // 长串无空格文本 (flexbox 不折行, 按列估算会高估)
                        m->role = TUIMessage::Role::Assistant;
                        m->text = std::string(200, 'x') + " " + marker + "\n";
                        break;
                    }
                    case 4: { // User 多行
                        m->role = TUIMessage::Role::User;
                        m->text = "user line 1\nuser line 2 marker " + marker + "\nuser line 3\n";
                        break;
                    }
                    case 5: { // Tool 展开 (多行 JSON 参数 + 长 result)
                        m->role               = TUIMessage::Role::Tool;
                        m->tool               = TUIMessage::ToolData{};
                        m->tool->toolName     = "agentxx_filesystem_read";
                        m->tool->toolFinished = true;
                        m->tool->toolResult   = "line1 result marker " + marker
                                              + "\n"
                                                "line2 with some words to wrap if narrow\n"
                                                "line3\n";
                        m->text      = "{\n  \"path\": \"/a/b/c\",\n  \"offset\": 0,\n"
                                       "  \"limit\": 100\n}";
                        m->collapsed = false;
                        break;
                    }
                }
                pushRow(st, std::move(*m));
            }
        });

        f.render(); // 建立缓存与布局 (stickToBottom 默认)

        // 滚到顶部 (每次滚 1 行, 直到不再变化)
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 2000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break; // 已到顶部
                }
                lastOffset = off;
            }
        }

        // 从顶部逐行向下滚动, 收集每条消息 marker 是否出现过
        std::set<std::string> unseen(markers.begin(), markers.end());
        for (int off = 0; off < 4000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break; // 滚不动 (已到底)
            }
        }
        // 最后一帧 (底部)
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (!unseen.empty()) {
            fprintf(stderr, "[DBG11] %zu messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(stderr, "[DBG11] totalHeight=%d\n", f.comp->totalHeight());
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 12 (诊断): mermaid 状态图消息 (估算按代码块行数, 实际图形
        // 高度远大于源行数 -> 严重低估) + 长对话 + stickToBottom ——
        // 底部最新消息必须完整可见; 若 mermaid 低估, totalHeight 偏小,
        // offset 偏小, 底部最新消息被推出视口 (用户报告的"某些消息
        // 显示为空白, 滑动到某些位置又正常")
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 前面 300 条普通段落消息 + mermaid 在中间 + 后面 5 条普通消息:
            // mermaid 估算位置 (源 8 行) 落在视口上方被 continue 跳过 (不实测),
            // 但其实际高度 24 行 -> 总高度低估 -> 底部内容被推出视口,
            // 且 mermaid 区域 (视口内) 显示为空白
            auto addHistory = [&](int n, int base) {
                for (int i = 0; i < n; ++i) {
                    auto m  = std::make_shared<TUIMessage>();
                    m->role = TUIMessage::Role::Assistant;
                    m->text = "history paragraph " + std::to_string(base + i)
                              + " with enough words to wrap around the width nicely "
                                "and fill several lines of the screen area.\n";
                    pushRow(st, std::move(*m));
                }
            };
            addHistory(300, 0);
            auto mm  = std::make_shared<TUIMessage>();
            mm->role = TUIMessage::Role::Assistant;
            mm->text = "```mermaid\nstateDiagram-v2\n"
                       "    [*] --> phase_a\n"
                       "    phase_a --> phase_b\n"
                       "    phase_b --> phase_c\n"
                       "    phase_c --> [*]\n"
                       "```\n";
            pushRow(st, std::move(*mm));
            addHistory(5, 1000);
            // 最后一条普通消息 (marker 必须在底部可见)
            auto last  = std::make_shared<TUIMessage>();
            last->role = TUIMessage::Role::Assistant;
            last->text = "final message with unique marker LAST_MK_9ZQ\n";
            pushRow(st, std::move(*last));
        });

        for (int i = 0; i < 8; ++i) {
            f.render(); // stickToBottom 贴底稳定
        }
        std::string frame = f.render();
        // mermaid 状态图必须渲染在视口中 (修复前整图缺失 -> 视口内一段空白;
        // 图中任一节点/箭头可见即说明被布局渲染; 视口只显示图的一部分)
        XX_TEST_EXPECT_TRUE(frame.find("[*]") != std::string::npos);
        // mermaid 之后的普通消息必须正常显示 (不被 mermaid 低估的高度错位)
        XX_TEST_EXPECT_TRUE(frame.find("history paragraph 1000") != std::string::npos);
        // 底部最新消息必须完整可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 4).find("LAST_MK_9ZQ") != std::string::npos
        );
        if (frame.find("LAST_MK_9ZQ") == std::string::npos) {
            fprintf(
                stderr,
                "[DBG12] last message NOT visible! totalHeight=%d\n",
                f.comp->totalHeight()
            );
            fprintf(stderr, "[DBG12] frame:\n%s\n", frame.c_str());
        }
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 4).find("LAST_MK_9ZQ") != std::string::npos
        );
    }

    {
        // 场景 13: 单条消息的估算 == 实测高度 (估算算法与渲染语义一致性回归)
        // - 普通段落消息: 内容折行 + 尾部空行 = 3 行
        // - mermaid 状态图: 源 7 行 -> 图形 24 行 (估算公式 源行数×3+3 = 24)
        // - softbreak 多行: 段内换行合并为空格, 1 行 + 尾部空行 = 2 行
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "history paragraph 0 with enough words to wrap around the width nicely "
                      "and fill several lines of the screen area.\n";
            pushRow(st, std::move(*m));
        });
        f.render();
        XX_TEST_EXPECT_TRUE(f.comp->totalHeight() == 3);

        ScrollFixture f2;
        f2.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "```mermaid\nstateDiagram-v2\n"
                      "    [*] --> phase_a\n"
                      "    phase_a --> phase_b\n"
                      "    phase_b --> phase_c\n"
                      "    phase_c --> [*]\n"
                      "```\n";
            pushRow(st, std::move(*m));
        });
        f2.render();
        XX_TEST_EXPECT_TRUE(f2.comp->totalHeight() == 24);

        ScrollFixture f3;
        f3.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::Assistant;
            m->text = "line one\nline two\nline three\nline four\nline five\n";
            pushRow(st, std::move(*m));
        });
        f3.render();
        XX_TEST_EXPECT_TRUE(f3.comp->totalHeight() == 2);
    }

    {
        // 场景 14 (诊断): filesystem_edit Tool 消息展开 —— 实际渲染走
        // renderEditToolDiff (按 oldStr/newStr 差异逐行渲染), 而估算按
        // args JSON 行数 + toolResult 行数 —— 大 diff 时严重低估。
        // 与场景 12 相同机制: 低估 -> totalHeight 偏低 -> stickToBottom
        // 偏移偏小 -> 底部最新消息被推出视口 / edit Tool 被 continue 跳过
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            // 普通历史消息 (估算准确, 撑高列表)
            for (int i = 0; i < 300; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "history paragraph " + std::to_string(i)
                          + " with enough words to wrap around the width nicely "
                            "and fill several lines of the screen area.\n";
                pushRow(st, std::move(*m));
            }
            // 中间一条 filesystem_edit Tool 消息 (展开, 大 diff)
            auto et                = std::make_shared<TUIMessage>();
            et->role               = TUIMessage::Role::Tool;
            et->tool               = TUIMessage::ToolData{};
            et->tool->toolName     = "agentxx_filesystem_edit";
            et->tool->toolFinished = true;
            et->tool->toolResult   = "success"; // 成功 (非错误前缀)
            et->collapsed          = false;
            std::string oldStr, newStr;
            for (int j = 0; j < 14; ++j) {
                oldStr += "old line " + std::to_string(j) + "\n";
                newStr += "new line " + std::to_string(j) + "\n";
            }
            newStr += "diff marker DL_MK_7ZZ\n";
            // 构造合法 JSON args (换行必须转义为 \\n, 否则解析失败回退)
            std::string argsJson = "{\"path\":\"/a/b.txt\",\"old_str\":\"";
            for (char c : oldStr) {
                if (c == '\n') {
                    argsJson += "\\n";
                } else {
                    argsJson += c;
                }
            }
            argsJson += "\",\"new_str\":\"";
            for (char c : newStr) {
                if (c == '\n') {
                    argsJson += "\\n";
                } else {
                    argsJson += c;
                }
            }
            argsJson += "\"}";
            et->text  = std::move(argsJson);
            // diff 内容由**插件自己的工具渲染器**提供 (主程序不再按工具名特化):
            // 这里模拟插件推送的装饰 (Diff 组件项), 覆盖"大 diff 消息"的滚动场景
            {
                auto reg
                    = st.pluginRegistry
                          ? std::make_shared<agentxx::plugin::ClientUiRegistry>(*st.pluginRegistry)
                          : std::make_shared<agentxx::plugin::ClientUiRegistry>();
                auto& decor                = reg->toolDecors.emplace_back();
                decor.plugin               = "agentxx_filesystem";
                decor.toolCallId           = "call_edit_scroll_1";
                decor.displayName          = "Edit";
                utilxx_base::Json diffItem = utilxx_base::Json::object();
                diffItem["kind"]           = "Diff";
                diffItem["path"]           = "/a/b.txt";
                diffItem["oldStr"]         = oldStr;
                diffItem["newStr"]         = newStr;
                decor.items                = utilxx_base::Json::array({std::move(diffItem)});
                st.pluginRegistry          = std::move(reg);
                et->tool->toolCallId       = "call_edit_scroll_1";
            }
            pushRow(st, std::move(*et));
            // 后面 5 条历史 + last
            for (int i = 0; i < 5; ++i) {
                auto m  = std::make_shared<TUIMessage>();
                m->role = TUIMessage::Role::Assistant;
                m->text = "tail paragraph " + std::to_string(i)
                          + " with enough words to wrap around the width nicely.\n";
                pushRow(st, std::move(*m));
            }
            auto last  = std::make_shared<TUIMessage>();
            last->role = TUIMessage::Role::Assistant;
            last->text = "final message with unique marker LAST_MK_8XY\n";
            pushRow(st, std::move(*last));
        });

        for (int i = 0; i < 8; ++i) {
            f.render(); // stickToBottom 贴底稳定
        }
        std::string frame = f.render();
        // filesystem_edit 的 diff 内容必须渲染在视口中 (修复前整块丢失 -> 空白)
        XX_TEST_EXPECT_TRUE(frame.find("DL_MK_7ZZ") != std::string::npos);
        // 底部最新消息必须完整可见
        XX_TEST_EXPECT_TRUE(
            ScrollFixture::lastLines(frame, 4).find("LAST_MK_8XY") != std::string::npos
        );
    }

    {
        // 场景 15a (诊断): 单条 Tool 折叠消息的渲染结果
        ScrollFixture g;
        g.sharedState.mutate([&](TUIRenderState& st) {
            auto m                = std::make_shared<TUIMessage>();
            m->collapsed          = true;
            m->role               = TUIMessage::Role::Tool;
            m->tool               = TUIMessage::ToolData{};
            m->tool->toolName     = "agentxx_filesystem_read";
            m->tool->toolFinished = true;
            m->tool->toolResult   = "some result with marker COLX_ZZ";
            m->text               = "{\"path\":\"/a/b/c\"}";
            pushRow(st, std::move(*m));
        });
        g.render();
        std::string gframe = g.render();
        fprintf(
            stderr,
            "[DBG15a] single collapsed Tool: totalH=%d frame:\n%s\n",
            g.comp->totalHeight(),
            gframe.c_str()
        );
    }

    {
        // 场景 15 (诊断): 全折叠消息列表 (Think/Tool/System 折叠, 仅 header) +
        // 正常上下滚动 —— 所有折叠 header 必须渲染; 复现用户报告
        // "消息都是折叠的, 上下滚动到某些位置时连续几条不显示, 但可以
        // 点击展开, 滚动超过一段距离又好了"。折叠消息估算 2 行 == 实测,
        // 若仍出现缺失, 说明问题不在高度估算, 而在布局/绘制环节
        ScrollFixture            f;
        std::vector<std::string> markers;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 60; ++i) {
                const std::string mk = "COL_" + std::to_string(i) + "_ZQ";
                markers.push_back(mk);
                auto m       = std::make_shared<TUIMessage>();
                m->collapsed = true; // 全部折叠
                if (i % 3 == 0) {
                    // Think 折叠: header "[Think] <时长>" + 折叠预览(正文单行截断)
                    m->role       = TUIMessage::Role::Think;
                    m->durationMs = 1000 + i;
                    m->text       = "think body content with marker " + mk
                              + " and lots of "
                                "text to make preview truncate at the right edge ...";
                } else if (i % 3 == 1) {
                    // Tool 折叠: header 显示摘要 (buildToolHeaderSummary);
                    // marker 放入 path, 摘要 "Read · /a/COL_x_ZQ" 会显示出来
                    m->role               = TUIMessage::Role::Tool;
                    m->tool               = TUIMessage::ToolData{};
                    m->tool->toolName     = "agentxx_filesystem_read";
                    m->tool->toolFinished = true;
                    m->tool->toolResult   = "some result text";
                    m->text               = "{\"path\":\"/a/" + mk + "\"}";
                } else {
                    // System/Tip 折叠
                    m->role = TUIMessage::Role::Tip;
                    m->tip  = TUIMessage::TipData{};
                    m->text = "system tip message with marker " + mk;
                }
                pushRow(st, std::move(*m));
            }
        });

        f.render();
        // 从顶部逐行滚动到底部, 收集每个折叠 header 的 marker
        std::set<std::string> unseen(markers.begin(), markers.end());
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 2000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break;
                }
                lastOffset = off;
            }
        }
        for (int off = 0; off < 4000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break;
            }
        }
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!unseen.empty()) {
            fprintf(stderr, "[DBG15] %zu folded messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(
                stderr,
                "[DBG15] totalHeight=%d scrollOffset=%d\n",
                f.comp->totalHeight(),
                f.comp->scrollOffset()
            );
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 16 (诊断): 大量折叠消息 + 间隔插入的展开高消息 (mermaid/edit
        // diff) —— 模拟真实长对话; 逐行滚动遍历, 所有折叠 header 必须渲染
        // (折叠消息估算准确, 若高消息低估导致位置错位, 后续连续折叠消息
        //  会在某些滚动位置显示为空白, 滚动大段距离后恢复 —— 用户报告)
        ScrollFixture            f;
        std::vector<std::string> markers;
        f.sharedState.mutate([&](TUIRenderState& st) {
            int msgNo = 0;
            for (int i = 0; i < 96; ++i) {
                // 每 16 条插入一条展开的高消息 (低估源)
                if (i > 0 && i % 16 == 0) {
                    auto hm  = std::make_shared<TUIMessage>();
                    hm->role = TUIMessage::Role::Assistant;
                    if (i % 32 == 0) {
                        // mermaid 状态图 (展开)
                        hm->text = "```mermaid\nstateDiagram-v2\n"
                                   "    [*] --> p_a\n    p_a --> p_b\n    p_b --> p_c\n"
                                   "    p_c --> [*]\n```\n";
                    } else {
                        // filesystem_edit diff (展开)
                        hm->role               = TUIMessage::Role::Tool;
                        hm->tool               = TUIMessage::ToolData{};
                        hm->tool->toolName     = "agentxx_filesystem_edit";
                        hm->tool->toolFinished = true;
                        hm->tool->toolResult   = "success";
                        hm->collapsed          = false;
                        std::string os, ns;
                        for (int j = 0; j < 10; ++j) {
                            os += "old line " + std::to_string(j) + "\n";
                            ns += "new line " + std::to_string(j) + "\n";
                        }
                        std::string aj = "{\"path\":\"/a/f.txt\",\"old_str\":\"";
                        for (char c : os) {
                            if (c == '\n') {
                                aj += "\\n";
                            } else {
                                aj += c;
                            }
                        }
                        aj += "\",\"new_str\":\"";
                        for (char c : ns) {
                            if (c == '\n') {
                                aj += "\\n";
                            } else {
                                aj += c;
                            }
                        }
                        aj       += "\"}";
                        hm->text  = std::move(aj);
                    }
                    pushRow(st, std::move(*hm));
                    continue;
                }
                // 折叠消息
                const std::string mk = "MX" + std::to_string(msgNo++) + "_KQ";
                markers.push_back(mk);
                auto m       = std::make_shared<TUIMessage>();
                m->collapsed = true;
                if (msgNo % 3 == 0) {
                    m->role       = TUIMessage::Role::Think;
                    m->durationMs = 1000 + msgNo;
                    m->text       = "think body with marker " + mk + " ...";
                } else if (msgNo % 3 == 1) {
                    m->role               = TUIMessage::Role::Tool;
                    m->tool               = TUIMessage::ToolData{};
                    m->tool->toolName     = "agentxx_filesystem_read";
                    m->tool->toolFinished = true;
                    m->tool->toolResult   = "ok";
                    m->text               = "{\"path\":\"/a/" + mk + "\"}";
                } else {
                    m->role = TUIMessage::Role::Tip;
                    m->tip  = TUIMessage::TipData{};
                    m->text = "tip with marker " + mk;
                }
                pushRow(st, std::move(*m));
            }
        });

        f.render();
        std::set<std::string> unseen(markers.begin(), markers.end());
        f.comp->setStickToBottom(false);
        {
            int lastOffset = -1;
            for (int i = 0; i < 3000; ++i) {
                f.render();
                f.wheel(ftxui::Mouse::WheelUp);
                const int off = f.comp->scrollOffset();
                if (off == lastOffset && off == 0) {
                    break;
                }
                lastOffset = off;
            }
        }
        for (int off = 0; off < 6000; ++off) {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
            if (unseen.empty()) {
                break;
            }
            if (!f.wheel(ftxui::Mouse::WheelDown)) {
                break;
            }
        }
        {
            std::string frame = f.render();
            for (auto it = unseen.begin(); it != unseen.end();) {
                if (frame.find(*it) != std::string::npos) {
                    it = unseen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!unseen.empty()) {
            fprintf(stderr, "[DBG16] %zu folded messages never rendered:\n", unseen.size());
            for (const auto& m : unseen) {
                fprintf(stderr, "  missing: %s\n", m.c_str());
            }
            fprintf(stderr, "[DBG16] totalHeight=%d\n", f.comp->totalHeight());
        }
        XX_TEST_EXPECT_TRUE(unseen.empty());
    }

    {
        // 场景 17 (回归): 预算淘汰不得移除"本帧已确保/待渲染"的可见子项
        //
        // 机制: buildItem 对内容较大的子项按源字节 ×64 上报 sourceBytes
        // (消息列表: 见 buildMessageItem), 当视口内可见子项的总估算字节
        // 超过 maxBytes (长代码 / JSON / 工具结果 / diff) —— 或可见条数
        // 超过 maxItems (高终端) —— 时, ensureElement 插入新子项触发的
        // evictIfNeeded 会持续从 LRU 尾部淘汰直至预算达标。尾部先消耗
        // 视口外的旧缓存, 耗尽后即命中"本帧定位阶段已处理过、仍待渲染"
        // 的可见子项: hasCache_ 被置 false 但索引仍在 visibleIndices_
        // (visibleBoxes_ 有盒 -> 点击折叠/展开仍有效), 渲染时 elementAt
        // 回退空 text -> 连续多条消息显示为空白; 预算由可见集自身超限,
        // 每帧稳定复现, 滚动改变可见集后消失/恢复 (用户报告: "滚动到
        // 一定位置时连续几条消息不显示, 但可以点击展开和折叠, 再滚动
        // 就恢复")。修复前本场景的视口顶部连续标记缺失。
        //
        // 直接构造 LazyScrollable: 预算 (maxItems=8, maxBytes=2000,
        // byteExemptThreshold=0) 远小于可见集 (30 行视口, 单行子项 x ~42
        // 项 x sourceBytes=200 = ~8400), key 恒定无内容变更干扰, 使淘汰
        // 必然触及可见子项
        auto scrollable = std::make_shared<LazyScrollable>(
            [] {
                return static_cast<size_t>(60);
            }, // 60 个子项 (列表 60 行)
            [](size_t i) {
                return 0x1000ULL + i;
            }, // key 恒定
            [](size_t, int) {
                return 1;
            }, // 单行子项 (估算==实测)
            [](size_t i) {
                LazyBuiltItem b;
                b.element = ftxui::text(
                    "MT17 item " + std::to_string(i) + " marker=" + std::to_string(i)
                );
                b.sourceBytes = 200; // 模拟 x64 折算后的较大渲染树
                return b;
            },
            LazyScrollable::CacheBudget{8, 2000, 0}, // maxItems, maxBytes, exempt=0
            nullptr
        );
        scrollable->setStickToBottom(false); // 固定视口在顶部 (offset=0)
        auto el = scrollable->Render() | ftxui::flex;
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen, el);
        std::string out = screen.ToString();
        // 视口 (offset 0) 内 0..29 条全部必须渲染 (顶部连续标记缺失即回归)
        std::string missing;
        for (int i = 0; i < 30; ++i) {
            const std::string marker = "marker=" + std::to_string(i);
            if (out.find(marker) == std::string::npos) {
                missing += (missing.empty() ? "" : ",") + std::to_string(i);
            }
        }
        if (!missing.empty()) {
            fprintf(stderr, "[DBG17] missing visible markers: %s\n", missing.c_str());
        }
        XX_TEST_EXPECT_TRUE(missing.empty());
    }

    {
        // 场景 17b (回归): 高视口 (可见条数 > maxItems=64) 全折叠消息列表,
        // 顶部连续折叠消息必须渲染 —— 消息列表级复现场景 17 的同一机制
        // (count 预算淘汰误伤可见子项), 用户报告的正是折叠消息场景
        TallScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            for (int i = 0; i < 200; ++i) {
                const std::string mk = "T17B_" + std::to_string(i) + "_END";
                auto              m  = std::make_shared<TUIMessage>();
                m->collapsed         = true;
                m->role              = TUIMessage::Role::Tip;
                m->tip               = TUIMessage::TipData{};
                m->text              = "tip " + mk;
                pushRow(st, std::move(*m));
            }
        });
        f.comp->setStickToBottom(false); // 固定视口在顶部 (offset=0)
        f.render();                      // 建立缓存与布局
        f.render();                      // 再渲染 (全部缓存命中路径)
        std::string frame = f.render();
        // 折叠消息 2 行/条, 150 行视口可见 0..74; 顶部消息被预算淘汰 ->
        // 空白 (修复前 T17B_0..T17B_10 丢失)
        std::string missing;
        for (int i = 0; i < 75; ++i) {
            const std::string mk = "T17B_" + std::to_string(i) + "_END";
            if (frame.find(mk) == std::string::npos) {
                missing += (missing.empty() ? "" : ",") + std::to_string(i);
            }
        }
        if (!missing.empty()) {
            fprintf(stderr, "[DBG17b] missing visible collapsed markers: %s\n", missing.c_str());
        }
        XX_TEST_EXPECT_TRUE(missing.empty());
    }

    {
        // 场景 18: TailThinkingMode 流式与末尾折叠预览测试
        // 注: 本场景验证折叠形态与预览截取逻辑, 与加载动画无关 —— 先将动画
        // 等级临时降为 Low (< High), 使运行中流式 Think 头部回退静态 +/- 标识,
        // 断言按静态形态编写; 动画形态由场景 19 单独覆盖
        auto&      settings = TUISettings::instance();
        const auto origMode = settings.tailThinkingMode();
        const auto origAnim = settings.animationLevel();
        settings.setAnimationLevel(AnimationLevel::Low);

        // 18a: SingleLine 模式下流式 Think 显示单行折叠并截取末尾字符
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Thinking step one\nThinking step two\nThinking step three final tail marker THK_TAIL_18A");
            });
            std::string frame = f.render();
            // 单行折叠标志 "+ [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            // 包含末尾字符 marker
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18A") != std::string::npos);
            // 不应为展开标志 "- [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") == std::string::npos);
        }

        // 18b: AutoExpand 模式下流式 Think 自动展开显示
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Thinking step one auto expand header\nThinking step two\nThinking step three THK_TAIL_18B");
            });
            std::string frame = f.render();
            // 自动展开标志 "- [Think]"
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") != std::string::npos);
            // 包含正文内容
            XX_TEST_EXPECT_TRUE(
                frame.find("Thinking step one auto expand header") != std::string::npos
            );
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18B") != std::string::npos);
        }

        // 18c: 历史末尾 Think 消息在 SingleLine 模式下折叠预览显示末尾截取
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto m       = std::make_shared<TUIMessage>();
                m->role      = TUIMessage::Role::Think;
                m->collapsed = true;
                m->text
                    = "First line head of thinking that is very long\nSecond line\nTail line with marker THK_TAIL_18C";
                pushRow(st, std::move(*m));
            });
            std::string frame = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18C") != std::string::npos);
        }

        // 点击流式末尾 Think 的辅助: 点击第一个非空命中区中心 (返回是否处理)
        auto clickFirstCollapsible = [](ScrollFixture& fx) -> bool {
            for (const auto& box : fx.comp->collapsibleBoxes()) {
                if (box.IsEmpty()) {
                    continue;
                }
                ftxui::Mouse m;
                m.button = ftxui::Mouse::Left;
                m.motion = ftxui::Mouse::Released;
                m.x      = (box.x_min + box.x_max) / 2;
                m.y      = (box.y_min + box.y_max) / 2;
                return fx.comp->OnEvent(ftxui::Event::Mouse("", m));
            }
            return false;
        };

        // 18d: SingleLine 模式下点击流式末尾 Think → 展开, 再点击 → 折叠
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Thinking head line hidden when collapsed THK18D_HEAD\n" "Thinking step two\nThinking step three tail THK_TAIL_18D", 7);
            });
            // 两帧建立布局与命中区域
            std::string frame = f.render();
            frame             = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK18D_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18D") != std::string::npos);
            // 流式区子项登记为可点击命中区 (isStream 标记)
            XX_TEST_EXPECT_TRUE(f.comp->collapsibleBoxes().size() == 1);
            XX_TEST_EXPECT_TRUE(f.comp->collapsibleIsStream(0));

            // 点击 → 展开 (显示 "- [Think]" 与全文头部)
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string expanded = f.render();
            XX_TEST_EXPECT_TRUE(expanded.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(expanded.find("THK18D_HEAD") != std::string::npos);

            // 刷新命中区后再点击 → 折叠回单行预览
            f.render();
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string collapsedAgain = f.render();
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("THK18D_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsedAgain.find("THK_TAIL_18D") != std::string::npos);
        }

        // 18e: AutoExpand 模式下点击流式末尾 Think → 折叠, 再点击 → 展开
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Auto expand head line THK18E_HEAD\n" "Thinking step two\nThinking step three tail THK_TAIL_18E", 8);
            });
            std::string frame = f.render();
            frame             = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK18E_HEAD") != std::string::npos);

            // 点击 → 折叠为单行末尾截取预览 (头部隐藏, 尾部可见)
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string collapsed = f.render();
            XX_TEST_EXPECT_TRUE(collapsed.find("+ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsed.find("THK18E_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(collapsed.find("THK_TAIL_18E") != std::string::npos);

            // 刷新命中区后再点击 → 恢复展开
            f.render();
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string expandedAgain = f.render();
            XX_TEST_EXPECT_TRUE(expandedAgain.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(expandedAgain.find("THK18E_HEAD") != std::string::npos);
        }

        // 18f: 流结束提交后覆盖态重置 —— 新一轮思考回到设置模式默认展示
        settings.setTailThinkingMode(TailThinkingMode::AutoExpand);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Stream one head THK18F_HEAD\nStream one tail THK_TAIL_18F", 3);
            });
            f.render();
            f.render();
            // 点击折叠流式 think
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            XX_TEST_EXPECT_TRUE(f.render().find("+ [Think]") != std::string::npos);

            // 流结束: token 提交为正式 Think 行 (agent 端定稿 insert_message,
            // 默认 collapsed=true), 覆盖态应随流结束重置
            f.sharedState.mutate([&](TUIRenderState& st) {
                // 定稿: 同一 id 落成正式行 (流式项随之丢弃), 默认折叠
                auto m       = std::make_shared<TUIMessage>();
                m->role      = TUIMessage::Role::Think;
                m->collapsed = true;
                m->text      = *st.replica.stream().text;
                m->id        = st.replica.stream().id;
                pushRow(st, std::move(*m));
                clearStream(st);
            });
            std::string committedFrame = f.render();
            XX_TEST_EXPECT_TRUE(committedFrame.find("+ [Think]") != std::string::npos);

            // 新一轮思考开始 (新 epoch): 不沿用上一流的折叠覆盖态, 自动展开
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "Stream two head THK18F_HEAD2\nStream two tail THK_TAIL_18F2", 9);
            });
            std::string nextStreamFrame = f.render();
            nextStreamFrame             = f.render();
            XX_TEST_EXPECT_TRUE(nextStreamFrame.find("- [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(nextStreamFrame.find("THK18F_HEAD2") != std::string::npos);
        }

        // 18g: 流式思考在 Ultra 动画等级下走增量多行渲染, 点击展开后不会被
        // 折回 —— 回归: 折叠期间 syncStream 提前返回 (不记账流身份), 首次展开
        // 被误判为"新流"并清除用户点击覆盖态, 表现为展开一帧后又折回单行
        // (需再点一次才正常展开)
        settings.setTailThinkingMode(TailThinkingMode::SingleLine);
        settings.setAnimationLevel(AnimationLevel::Ultra);
        {
            ScrollFixture f;
            f.setTokenEpoch(
                11,
                "Thinking head line THK18G_HEAD\nThinking tail THK_TAIL_18G",
                TUIMessage::Role::Think
            );
            // 两帧建立布局与命中区域
            f.render();
            std::string frame = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("THK18G_HEAD") == std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_18G") != std::string::npos);

            // 点击 → 展开 (显示全文头部)
            XX_TEST_EXPECT_TRUE(clickFirstCollapsible(f));
            std::string expanded = f.render();
            XX_TEST_EXPECT_TRUE(expanded.find("THK18G_HEAD") != std::string::npos);

            // 后续帧保持展开 (修复前第二帧即折回单行预览)
            std::string next = f.render();
            XX_TEST_EXPECT_TRUE(next.find("THK18G_HEAD") != std::string::npos);

            // 同一流继续增长 (epoch 不变) 仍保持展开, 新内容可见
            f.setTokenEpoch(
                11,
                "Thinking head line THK18G_HEAD\nThinking tail THK_TAIL_18G\nAddendum THK_18G_ADD",
                TUIMessage::Role::Think
            );
            std::string grown = f.render();
            XX_TEST_EXPECT_TRUE(grown.find("THK18G_HEAD") != std::string::npos);
            XX_TEST_EXPECT_TRUE(grown.find("THK_18G_ADD") != std::string::npos);
        }

        // 恢复原始设置
        settings.setTailThinkingMode(origMode);
        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 19: 运行中 tool/think 头部加载动画 (动画等级 >= High 时以
        // spinner braille 点阵替代 "+/-" 静态标识, 与输入框前缀动画同款)
        auto&      settings = TUISettings::instance();
        const auto origAnim = settings.animationLevel();

        // 19a: 动画等级 High, 流式 Think 展开态头部显示 spinner 首帧 (无静态 "-")
        settings.setAnimationLevel(AnimationLevel::High);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "thinking THK19A", 1);
            });
            std::string frame = f.render();
            // spinner 默认帧序列首帧; 测试环境无 ScreenInteractive 动画循环,
            // 帧索引恒为首帧 "⠋"
            XX_TEST_EXPECT_TRUE(frame.find("⠋ [Think]") != std::string::npos);
            // 不再出现静态 +/- 标识
            XX_TEST_EXPECT_TRUE(frame.find("- [Think]") == std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") == std::string::npos);
        }

        // 19b: 动画等级 Ultra, 流式 Think 折叠态头部同样使用 spinner
        settings.setAnimationLevel(AnimationLevel::Ultra);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                setStream(st, TUIMessage::Role::Think, "head THK19B_HEAD\ntail THK_TAIL_19B", 2);
            });
            f.render();
            std::string frame = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("⠋ [Think]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("+ [Think]") == std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("THK_TAIL_19B") != std::string::npos);
        }

        // 19c: 运行中 tool (未完成) 折叠头部以 spinner 替代 "+"; 完成后回退
        // 静态 "+" (消息指针变化驱动缓存失效重建)
        settings.setAnimationLevel(AnimationLevel::High);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto m            = std::make_shared<TUIMessage>();
                m->role           = TUIMessage::Role::Tool;
                m->text           = R"({"path": "/tmp/a.txt"})";
                m->tool           = TUIMessage::ToolData{};
                m->tool->toolName = "agentxx_filesystem_read";
                m->collapsed      = true;
                pushRow(st, std::move(*m));
            });
            std::string runningFrame = f.render();
            XX_TEST_EXPECT_TRUE(runningFrame.find("⠋ [Tool]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(runningFrame.find("+ [Tool]") == std::string::npos);

            // 工具完成: mutableMessage 复制消息 (指针变化 -> itemKey 失效重建),
            // 头部回退静态标识
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto nm                = std::make_shared<TUIMessage>(*st.replica.rows().back());
                nm->tool->toolFinished = true;
                nm->tool->toolResult   = "ok";
                st.replica.replaceRow(st.replica.rows().size() - 1, std::move(nm));
            });
            std::string doneFrame = f.render();
            XX_TEST_EXPECT_TRUE(doneFrame.find("+ [Tool]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(doneFrame.find("⠋ [Tool]") == std::string::npos);
        }

        // 19d: 动画等级 Low (<High) 时运行中 tool 保持原静态 "+" 标识
        settings.setAnimationLevel(AnimationLevel::Low);
        {
            ScrollFixture f;
            f.sharedState.mutate([&](TUIRenderState& st) {
                auto m            = std::make_shared<TUIMessage>();
                m->role           = TUIMessage::Role::Tool;
                m->text           = R"({"path": "/tmp/b.txt"})";
                m->tool           = TUIMessage::ToolData{};
                m->tool->toolName = "agentxx_filesystem_read";
                m->collapsed      = true;
                pushRow(st, std::move(*m));
            });
            std::string frame = f.render();
            XX_TEST_EXPECT_TRUE(frame.find("+ [Tool]") != std::string::npos);
            XX_TEST_EXPECT_TRUE(frame.find("⠋ [Tool]") == std::string::npos);
        }

        // 恢复原始设置
        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 20 (崩溃回归): 生成占位提示 (GenStatus) 出现时的子项高度估算
        //
        // 背景: 模型节点开始执行到首个可见输出之间, agent 下发 GenStatus 增量,
        // 消息列表末尾出现占位项 (见 docs/zh-cn/design/tui.md §2.13)。此时
        // isStreaming 为真但流式项尚未建立 (首个 token 未到达), 且
        // streamUseIncremental_ 为假 —— 估算走"流式区降级路径"。
        // 修复前 quickHeight 的占位项判定写在降级路径之后 (不可达): 占位项下标
        // 落入降级分支解引空的流式文本指针构造 string_view, 进程立即崩溃
        // (用户报告: TUI 会话中一发送消息就退出, 且无 ASan 报告 —— 崩溃在
        //  消息列表渲染帧内, 终端处于全屏界面, 报告不可见)
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hello gen placeholder";
            pushRow(st, std::move(*m));
            // GenStatus(Waiting) 到达后的客户端状态: 生成中, 尚无 token
            st.isStreaming = true;
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::Waiting;
        });

        // 占位项 = " - [Think] waiting for response..." (英文界面, 各段为独立
        // 文本节点, 屏幕文本中节点间会插入样式转义序列, 故按段断言):
        // 子项数 = 1 条用户消息 + 1 条占位项, 高度 = 2 + 2
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("[Think]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("waiting for response...") != std::string::npos);
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), 4);
        // 占位提示不是消息: 不进入消息列表
        XX_TEST_EXPECT_EQ(f.sharedState.readSnapshot()->messages().size(), (size_t)1);

        // 工具调用阶段 (已解析出工具名): 展示工具名与阶段说明
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "read_file";
        });
        frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("[Tool]") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("read_file") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("receiving tool call...") != std::string::npos);
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), 4);

        // 生成结束 (Done): 占位项清除, 高度回到用户消息本身
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase = agentxx::agent::WireDelta::GenPhase::Done;
            st.genToolName.clear();
            st.isStreaming = false;
        });
        frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("waiting for response...") == std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("receiving tool call...") == std::string::npos);
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), 2);
    }

    {
        // 场景 20b: 上一流以增量渲染结束 (Ultra 动画等级下的思考流) 后进入新轮次
        //
        // 流结束后流式区子项数必须归零 (OnRender 的无 token 分支): 否则生成占位
        // 提示的下标被推到列表末尾之外 —— 占位提示不显示, 该位置留下一条空白的
        // "[Think]" 头部行 (用户看到的"等待响应"提示从第二条消息起消失)
        auto&      settings = TUISettings::instance();
        const auto origAnim = settings.animationLevel();
        settings.setAnimationLevel(AnimationLevel::Ultra);

        ScrollFixture g;
        g.sharedState.mutate([&](TUIRenderState& st) {
            setStream(st, TUIMessage::Role::Think, "thinking THK20_STALE", 21);
        });
        g.render(); // 思考流走增量渲染 (子项数 = 头部 1 + 尾部块 1)
        g.sharedState.mutate([&](TUIRenderState& st) {
            clearStream(st); // 流结束 (提交落盘 + 复位流式状态)
        });
        g.render(); // 无 token 帧

        // 新轮次: GenStatus(Waiting) 到达, 尚无 token
        g.sharedState.mutate([&](TUIRenderState& st) {
            st.isStreaming = true;
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::Waiting;
        });
        std::string next = g.render();
        XX_TEST_EXPECT_TRUE(next.find("waiting for response...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(next.find("THK20_STALE") == std::string::npos);

        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 21 (回归): 生成占位提示紧跟流式正文时, 与正文之间必须有空行
        //
        // 背景: 列表里每个子项自带尾部空行 (消息块/增量稳定块), 但流式区末尾
        // 子项 (仍在增长的尾部块 / 降级整段) 后面原本没有内容, 因此不带尾部
        // 空行。生成占位提示紧跟其后时就会贴着正文显示 (用户报告: 正文之后
        // "正在调用工具"提示缺少边距); 思考内容后面因为思考流结算时提交为
        // 消息块 (自带尾部空行), 所以看起来正常
        auto&      settings = TUISettings::instance();
        const auto origAnim = settings.animationLevel();
        settings.setAnimationLevel(AnimationLevel::High); // 走增量渲染 (尾部块路径)

        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hi";
            pushRow(st, std::move(*m));
            // 流式正文已积累 (增量渲染), 随后上游开始接收工具调用
            setStream(st, TUIMessage::Role::Assistant, "answer text MARK21_BODY", 31);
        });
        f.render(); // 首帧: 建立流式渲染 (内容行已上屏)
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "read_file";
        });
        std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("MARK21_BODY") != std::string::npos);
        XX_TEST_EXPECT_TRUE(frame.find("receiving tool call...") != std::string::npos);
        // 正文行与提示行之间必须隔一个空白行
        const int bodyRow  = screenRowOf(stripAnsiCodes(frame), "MARK21_BODY");
        const int placeRow = screenRowOf(stripAnsiCodes(frame), "receiving tool call...");
        XX_TEST_EXPECT_TRUE(bodyRow >= 0);
        XX_TEST_EXPECT_EQ(placeRow - bodyRow, 2);
        XX_TEST_EXPECT_TRUE(isContentBlankRow(
            screenRowText(stripAnsiCodes(frame), bodyRow + 1),
            ScrollFixture::kWidth
        ));

        // 降级路径 (动画等级不足, 流式整段 paragraph 单子项) 同样要有空行
        settings.setAnimationLevel(AnimationLevel::Disabled);
        ScrollFixture g;
        g.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hi";
            pushRow(st, std::move(*m));
            setStream(st, TUIMessage::Role::Assistant, "fallback text MARK21_FALLBACK");
        });
        g.render();
        g.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "read_file";
        });
        std::string fallbackFrame = g.render();
        const int   fBodyRow      = screenRowOf(stripAnsiCodes(fallbackFrame), "MARK21_FALLBACK");
        const int fPlaceRow = screenRowOf(stripAnsiCodes(fallbackFrame), "receiving tool call...");
        XX_TEST_EXPECT_TRUE(fBodyRow >= 0);
        XX_TEST_EXPECT_EQ(fPlaceRow - fBodyRow, 2);

        // 上方是消息块 (自带尾部空行) 时不多出空行: 占位项仍只占 2 行
        ScrollFixture h;
        h.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hi";
            pushRow(st, std::move(*m));
            st.isStreaming = true;
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::Waiting;
        });
        h.render();
        XX_TEST_EXPECT_EQ(h.comp->totalHeight(), 4); // 用户消息 2 行 + 占位项 2 行
        const int userRow = screenRowOf(stripAnsiCodes(h.render()), "- hi");
        const int waitRow = screenRowOf(stripAnsiCodes(h.render()), "waiting for response...");
        XX_TEST_EXPECT_TRUE(userRow >= 0);
        XX_TEST_EXPECT_EQ(waitRow - userRow, 2);

        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 22 (回归): 生成占位提示行首的加载动画必须随动画帧转动
        //
        // isActive 判定 (现名 hasRunningMessage) 原先只看流式思考输出与未完成的
        // 工具消息: 占位提示出现时两者都不存在 (既无 token 也无工具消息), 帧循环
        // 从未启动 —— 该行点阵停在首帧不动 (用户报告"等待响应/正在调用工具的
        // 加载中动画卡住")。补上占位判定后帧循环正常推进
        auto&      settings = TUISettings::instance();
        const auto origAnim = settings.animationLevel();
        settings.setAnimationLevel(AnimationLevel::High);

        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hi";
            pushRow(st, std::move(*m));
            st.isStreaming = true;
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::Waiting;
        });
        // 首帧: 点阵为 spinner 首帧 (测试环境无帧循环, 帧索引停在首帧)
        std::string first = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(first.find("⠋ [Think]") != std::string::npos);

        // 推进动画: 经组件树转发 OnAnimation 到 runSpinner_ (占位行此时是
        // 唯一的运行中条目, 帧循环由本判定维持)
        advanceAnimation(*f.comp, 1);
        std::string next = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(next.find("⠋ [Think]") == std::string::npos);
        XX_TEST_EXPECT_TRUE(next.find("⠙ [Think]") != std::string::npos);

        // 工具调用阶段的占位行 (角色标签 [Tool]) 同样转动
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
            st.genToolName = "read_file";
        });
        f.render();
        advanceAnimation(*f.comp, 1);
        std::string toolRow = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(toolRow.find("⠹ [Tool]") != std::string::npos);

        // 占位提示清除后 (无运行中条目): 帧循环不再推进
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase = agentxx::agent::WireDelta::GenPhase::Done;
            st.genToolName.clear();
            st.isStreaming = false;
        });
        f.render();
        advanceAnimation(*f.comp, 3);
        std::string idle = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(idle.find("waiting for response...") == std::string::npos);
        XX_TEST_EXPECT_TRUE(idle.find("receiving tool call...") == std::string::npos);

        settings.setAnimationLevel(origAnim);
    }

    {
        // 场景 23 (回归): 占位提示的工具名换成工具特化显示名
        //
        // 占位行原先直接显示 genToolName (如 "agentxx_filesystem_list"), 与
        // 正常工具消息的折叠头 ("List") 不一致。现在经同一套工具特化渲染查询
        // (queryToolRender) 换显示名, 未命中渲染器时回退原始工具名
        ScrollFixture f;
        f.sharedState.mutate([&](TUIRenderState& st) {
            auto m  = std::make_shared<TUIMessage>();
            m->role = TUIMessage::Role::User;
            m->text = "hi";
            pushRow(st, std::move(*m));
            st.isStreaming = true;
            st.genPhase    = agentxx::agent::WireDelta::GenPhase::ToolCall;
        });

        // 已知工具 (模板类渲染器): 显示名 "List"，不出现原始工具名
        // (显示名与阶段说明是相邻的两个文本节点, 样式不同 -> 屏幕文本中夹 ANSI
        //  转义序列, 故先剥离再按纯文本断言)
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genToolName = "agentxx_filesystem_list";
        });
        XX_TEST_EXPECT_TRUE(
            stripAnsiCodes(f.render()).find("List receiving tool call...") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(
            stripAnsiCodes(f.render()).find("agentxx_filesystem_list") == std::string::npos
        );

        // 内置工具渲染器同样生效 (agentxx_share_store -> "Store")
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genToolName = "agentxx_share_store";
        });
        XX_TEST_EXPECT_TRUE(
            stripAnsiCodes(f.render()).find("Store receiving tool call...") != std::string::npos
        );

        // 未注册渲染器的工具: 回退原始工具名
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genToolName = "unknown_tool_xyz";
        });
        XX_TEST_EXPECT_TRUE(
            stripAnsiCodes(f.render()).find("unknown_tool_xyz receiving tool call...")
            != std::string::npos
        );

        // 等待响应阶段不显示工具名
        f.sharedState.mutate([&](TUIRenderState& st) {
            st.genPhase = agentxx::agent::WireDelta::GenPhase::Waiting;
            st.genToolName.clear();
        });
        std::string waiting = stripAnsiCodes(f.render());
        XX_TEST_EXPECT_TRUE(waiting.find("[Think] waiting for response...") != std::string::npos);
        XX_TEST_EXPECT_TRUE(waiting.find("List ") == std::string::npos);
    }

    // 视觉为空的消息 (模型把零宽占位符当正文输出) 不进副本:
    // agent 端在定稿处就过滤一次, 客户端收到后也按不可见丢弃 —— 不会出现
    // "只有一行空白" 的消息把相邻消息顶开
    {
        ScrollFixture f;
        (void)f.render();
        const int baseHeight = f.comp->totalHeight(); // 空列表 = 视口高度

        auto blank  = std::make_shared<TUIMessage>();
        blank->role = TUIMessage::Role::Assistant;
        blank->text = "\xE2\x80\x8B"; // U+200B 零宽空格
        f.sharedState.mutate([&](TUIRenderState& st) { pushRow(st, *blank); });
        (void)f.render();
        XX_TEST_EXPECT_EQ(f.sharedState.snapshot()->messages().size(), size_t{0});
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), baseHeight);

        auto normal  = std::make_shared<TUIMessage>();
        normal->role = TUIMessage::Role::Assistant;
        normal->text = "hello";
        f.sharedState.mutate([&](TUIRenderState& st) { pushRow(st, *normal); });
        (void)f.render();
        const int afterNormal = f.comp->totalHeight();
        XX_TEST_EXPECT_EQ(afterNormal, 2); // 内容 1 行 + 尾部空行 1 行

        // 只有耗时、正文全空白的思考不算有内容 (判据见 ViewMessage::hasVisibleContent):
        // 这种行渲染出来只剩一个 `[思考]` 头部, 看上去就是一条空消息 -> 副本丢弃
        auto think        = std::make_shared<TUIMessage>();
        think->role       = TUIMessage::Role::Think;
        think->text       = " ";
        think->durationMs = 10900;
        think->collapsed  = true;
        f.sharedState.mutate([&](TUIRenderState& st) { pushRow(st, *think); });
        (void)f.render();
        XX_TEST_EXPECT_EQ(f.sharedState.snapshot()->messages().size(), size_t{1});
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), afterNormal);

        // 带加密思考统计的空正文 Think 有内容 (头部会展示 token 统计):
        // 头部 1 行 + 尾部空行 1 行
        auto enc      = std::make_shared<TUIMessage>();
        enc->role     = TUIMessage::Role::Think;
        enc->text     = " ";
        enc->collapsed = true;
        enc->think     = TUIMessage::ThinkData{};
        enc->think->reasoningTokens = 128;
        f.sharedState.mutate([&](TUIRenderState& st) { pushRow(st, *enc); });
        (void)f.render();
        XX_TEST_EXPECT_EQ(f.comp->totalHeight(), afterNormal + 2);
    }

    // 恢复原始界面语言
    tuiSettings.setLanguage(savedLang);

    return TestResult{g_tui_scroll_passed, g_tui_scroll_failed};
}

} // namespace test
} // namespace agentxx
