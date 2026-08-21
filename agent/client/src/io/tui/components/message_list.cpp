#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/agent_tui.h" // formatDurationMilliseconds / oneLinePreview
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx/util/diff_util.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "markdown/dom_builder.hpp"
#include "markdown/parser.hpp"
#include "markdown/state_diagram.hpp"
#include "markdown/text_utils.hpp"

using namespace ftxui;

namespace {

/// 渲染 markdown 为 ftxui Element; 其中 ```mermaid 代码块由 DomBuilder 渲染为
/// 状态图 (见 markdown::build_code_block), 其余按 markdown 主题渲染
std::pair<Element, std::unique_ptr<markdown::DomBuilder>> renderMarkdown(
    std::string_view       content,
    Color                  color,
    markdown::Theme const& mdTheme,
    int                    maxWidth = 0
) {
    if (content.empty()) {
        return {ftxui::text(""), nullptr};
    }
    auto parser  = markdown::make_cmark_parser();
    auto ast     = parser->parse(content);
    auto builder = std::make_unique<markdown::DomBuilder>();
    if (maxWidth > 0) {
        builder->set_max_width(maxWidth);
    }
    auto el = builder->build(ast, -1, mdTheme);
    return {el | ftxui::color(color), std::move(builder)};
}

/// 估算文本显示行数 (换行符计数 + 按显示宽度折行估算)。
/// 仅用于不可见子项的高度估算 (影响滚动条/滚动定位), 子项进入视口后实测修正。
/// 宽字符 (CJK/emoji 等) 按 2 列计, 与 markdown::utf8_display_width 一致,
/// 修复旧实现按 UTF-8 码点计宽 (宽字符算 1 列) 导致 CJK 文本高度低估、
/// 滚动定位抖动的问题。线性扫描, 不整串调用 utf8_display_width (避免 O(n²))。
size_t estimateLines(std::string_view s, int width) {
    if (s.empty()) {
        return 1;
    }
    size_t useWidth = (width <= 0) ? 80 : static_cast<size_t>(width);
    size_t lines    = 1;
    size_t col      = 0;
    size_t i        = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            ++lines;
            col = 0;
            ++i;
            continue;
        }
        size_t len  = markdown::utf8_byte_length(s[i]);
        len         = std::min(len, s.size() - i);
        const int w = markdown::codepoint_width(markdown::utf8_codepoint(s.data() + i, len));
        if (w > 0) {
            // 组合字符/零宽字符不占列, 不触发折行
            col += w;
            if (col >= useWidth) {
                ++lines;
                col = 0;
            }
        }
        i += len;
    }
    return lines;
}

/// 估算 markdown 渲染高度 (行), 与 renderMarkdown (cmark-gfm + DomBuilder)
/// 的渲染语义对齐 (仅用于未进入视口的消息; 进入视口后实测修正)。
///
/// 背景: estimateLines 把每个 \n 都当硬换行, 但 DomBuilder 对普通段落把段内
/// 单个换行 (cmark softbreak) 合并为空格, 段落只按宽度折行 (仅空行分隔的
/// 段落间插入 1 行空行)。对"多行短句"文本 (LLM 输出常见, 如每行一个要点
/// 但无空行分隔), 按 \n 计数会严重高估 —— 例: 80 行 × 40 字符在 97 列下
/// 估算 80 行, 实际合并折行仅 ~33 行。不可见项高估即总高度虚高 ->
/// stickToBottom 滚动偏移偏大, 顶部消息被推出视口显示空白, 且不可见项
/// 永不进入视口实测 -> 空白持续 (用户报告"上半几条消息不渲染/可用高度
/// 变小")。
///
/// 估算规则 (近似, 尽量不高估):
/// - 普通段落: 段内换行折叠为单个空格, 再按宽度折行 (与
///   build_wrapping_container 的 ftxui::paragraph 合并语义一致)
/// - 行首标记行 (标题 # / 引用 > / 列表 - * + 数字. / 表格 |): 每源行渲染
///   1+ 行, 按去除标记后内容宽度折行估算 (build_list_item/blockquote 等
///   均为每源行一行, 内容处再按段落折行)
/// - ``` / ~~~ 围栏代码块: 开始围栏 1 行 + 内容每行 1 行 (含围栏内空行) +
///   结束围栏 1 行 (build_code_block 逐行渲染)
/// - ```mermaid 围栏: 渲染为状态图 (节点框 + 箭头), 图形高度与源行数无关,
///   实测约为源行数 × 3 + 3 (4 节点 5 边 TB 图: 7 源行 -> 24 行)。若按
///   普通代码块估算 (每行 1 行), 严重低估 (7 -> 8), 视口外消息总高度偏低,
///   滚动偏移偏小, 底部内容被推出视口且该 mermaid 消息被 continue 跳过
///   永不实测 -> 视口内显示空白 (用户报告"某些消息显示为空白, 滑动到
///   某些位置又正常")。故按 源行数 × 3 + 3 估算, 残余偏差由
///   LazyScrollable 的可见性容错 (kEstimateSlack) 提前实测自愈
/// - 块级元素间空行: 与 build_document 的 vbox({text(""), ...}) 一致,
///   第 2 个块起每块前 +1 行
size_t estimateMarkdownLines(std::string_view s, int width) {
    if (s.empty()) {
        return 1;
    }
    const size_t useWidth       = (width <= 0) ? 80 : static_cast<size_t>(width);
    size_t       total          = 0;
    size_t       blocks         = 0; // 渲染块计数 (块间空行 +1, build_document 语义)
    bool         inFence        = false;
    bool         fenceIsMermaid = false; // 当前围栏是否为 ```mermaid (图形估算)
    size_t       fenceLines     = 0;     // 当前围栏源行数 (含开始/结束围栏)

    std::string para; // 普通段落累积 (softbreak -> 空格合并)
    auto        flushParagraph = [&]() {
        if (para.empty()) {
            return;
        }
        total += estimateLines(para, static_cast<int>(useWidth));
        para.clear();
        ++blocks;
    };

    /// 围栏信息串是否为 mermaid (大小写不敏感, 容忍首尾空白) —— 与
    /// dom_builder 的 is_mermaid_fence 语义一致
    auto isMermaidInfo = [](std::string_view info) {
        size_t b = info.find_first_not_of(" \t");
        size_t e = info.find_last_not_of(" \t");
        if (b == std::string_view::npos) {
            return false;
        }
        info = info.substr(b, e - b + 1);
        return info.size() >= 7 && info.substr(0, 7) == "mermaid";
    };

    const size_t n = s.size();
    size_t       i = 0;
    while (i < n) {
        const size_t     eol     = s.find('\n', i);
        const size_t     lineEnd = (eol == std::string_view::npos) ? n : eol;
        std::string_view line    = s.substr(i, lineEnd - i);
        const size_t     b       = line.find_first_not_of(" \t");
        const size_t     e       = line.find_last_not_of(" \t");
        line = (b == std::string_view::npos) ? std::string_view{} : line.substr(b, e - b + 1);
        if (line.empty()) {
            // 空行: 段落终止 (围栏内空行属于代码内容, 渲染 1 行)
            if (inFence) {
                ++total;
                ++fenceLines;
            } else {
                flushParagraph();
            }
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        if (inFence) {
            ++total;
            ++fenceLines;
            if (line.size() >= 3 && (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~")) {
                inFence = false; // 结束围栏 (已计 1 行)
                if (fenceIsMermaid) {
                    // 图形高度估算: 源行数 × 3 + 3 (实测 4 节点 5 边 TB 图
                    // 7 源行 = 24 行); 已按普通行计 fenceLines 行, 补足差额
                    total += fenceLines * 2 + 3;
                }
                fenceIsMermaid = false;
                fenceLines     = 0;
            }
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        const bool isFenceStart
            = line.size() >= 3 && (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~");
        if (isFenceStart) {
            flushParagraph();
            ++total; // 开始围栏 1 行
            ++blocks;
            inFence        = true;
            fenceIsMermaid = isMermaidInfo(line.substr(3));
            fenceLines     = 1;
            i              = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        // 块级标记行 (标题/引用/列表/分隔线/表格): 每源行渲染 1+ 行
        const char c0           = line[0];
        const bool isMarkerLine = (c0 == '#') || (c0 == '>') || (c0 == '-') || (c0 == '*')
                                  || (c0 == '+') || (c0 == '|') || (c0 == '=');
        const bool isOrderedList
            = (line.size() >= 2 && c0 >= '0' && c0 <= '9' && (line[1] == '.' || line[1] == ')'));
        if (isMarkerLine || isOrderedList) {
            flushParagraph();
            // 去除行首标记序列后按内容折行估算 (渲染时内容宽度更窄, 已偏低估)
            const size_t cs       = line.find_first_not_of("#>-*+|= .");
            const auto   content  = (cs == std::string_view::npos || cs >= line.size())
                                        ? std::string_view{}
                                        : line.substr(cs);
            total                += estimateLines(content, static_cast<int>(useWidth));
            ++blocks;
            i = (eol == std::string_view::npos) ? n : eol + 1;
            continue;
        }
        // 普通文本行: 并入段落 (softbreak 合并, 行间以单个空格连接)
        if (!para.empty()) {
            para += ' ';
        }
        para += line;
        i     = (eol == std::string_view::npos) ? n : eol + 1;
    }
    flushParagraph();
    // 块间空行 (build_document: 第 2 个块起每块前 1 行空行)
    if (blocks > 1) {
        total += blocks - 1;
    }
    return std::max<size_t>(1, total);
}

/// 将工具调用参数 JSON 缩进格式化 (2 空格) 便于展开阅读, 例如:
/// {
///   "path": "/a/b",
///   "line_offset": 0
/// }
/// 参数解析失败或非对象时回退返回原始文本 (截断/异常参数)
std::string formatToolArgs(std::string_view argsText) {
    return agentxx::util::catchError<std::string>(
        [&]() -> std::string {
            auto j = neograph::json::parse(argsText);
            if (!j.is_object()) {
                return std::string{argsText};
            }
            return j.dump(2);
        },
        [&](std::string) -> std::string {
            return std::string{argsText};
        }
    );
}

/// 工具结果文本是否表示失败
/// 与服务端工具错误约定一致: 工具返回 "[Error] ..." / "[Exception aborted: ...]" 文本,
/// 中断为 "[Interrupt]"; 成功结果 (如 "Success, Replace N hits") 不以这些前缀开头
static bool isToolResultError(std::string_view result) {
    return result.starts_with("[Error]") || result.starts_with("[Exception")
           || result.starts_with("[Interrupt]") || result.contains("Permission");
}

} // namespace

MessageListComponent::MessageListComponent(TUICtx& ctx) :
    ctx_(ctx) {
    LazyScrollable::CacheBudget budget;
    // 渲染树内存放大: FTXUI text()/paragraph() 按 glyph/词拆对象, 实测渲染树
    // 为源文本 30~70 倍 (memprof 实测: 1MB 英文 -> text() 30.7MB, markdown
    // 全量渲染 173MB)。因此 sourceBytes 按"渲染树估算字节"(源 × 64 系数) 上报
    // (见 buildMessageItem), 使 maxBytes 直接约束真实驻留内存 —— 旧实现按
    // 源文本字节计 (16MiB 源 ≈ 0.5~1GB 渲染树), 预算形同虚设, 实测 100K/200K
    // 上下文时消息列表渲染树缓存即占 10+ MB。
    budget.maxItems = 64;              // 条数预算: 可见 ~30 条 + 少量滚动余量
    budget.maxBytes = 4 * 1024 * 1024; // 渲染树估算字节预算: 4MiB
    // 字节预算豁免: sourceBytes ≤64KB (即源 ≤1KB 的短消息) 不计入字节预算,
    // 只受 maxItems 条数约束 (64 条 × ~64KB ≈ 4MB 封顶) —— 短消息渲染树
    // 重建成本低, 无需挤占长消息的字节预算; 若连条数预算都不设, 短消息会
    // 无限堆叠 (旧 byteExemptThreshold=1024 按源字节计, 配合旧 sourceBytes
    // 语义, 短消息不计预算但依然缓存, 最多 256 条 × 64KB ≈ 16MB 常驻)
    budget.byteExemptThreshold = 64 * 1024;
    scrollable_                = std::make_shared<LazyScrollable>(
        [this] {
            return itemCount();
        },
        [this](size_t index) {
            return itemKey(index);
        },
        [this](size_t index, int width) {
            return estimateHeight(index, width);
        },
        [this](size_t index) {
            return buildItem(index);
        },
        budget,
        [this](size_t index) {
            return fillViewport(index);
        }
    );
    Add(scrollable_);
}

void MessageListComponent::invalidateCache() {
    scrollable_->clearCache();
    // 主题/宽度变化: 稳定块的着色/布局已过时, 令增量渲染器重建缓存
    if (streamRenderer_) {
        streamRenderer_->invalidateCache();
    }
}

Element MessageListComponent::OnRender() {
    // 流式已结束 (无流式 token): 及时释放增量渲染器缓存的稳定块/文本, 避免常驻内存。
    // 同时重置流身份缓存 (streamEpoch_/streamFedLen_): 使下一流强制走重建分支。
    // 为什么必须重置: onSync 会整体重建 TUIRenderState (currentTokenEpoch 归 0),
    // 若此处保留旧 streamEpoch_, 新流首 token 递增后的 epoch 可能与旧值恰好相等,
    // syncStream 误判为"同一流"而走增量分支, 而 fedLen 仍是旧流长度 —— 首 token
    // 内容被整体跳过 (渲染缺字), 直到 token 超过旧 fedLen 才从错误偏移开始显示。
    // (须在构建/布局前判断; 流式进行中时 hasStreamingToken 为真, 不会重置)
    if (ctx_.frameState && !hasStreamingToken(*ctx_.frameState)) {
        if (streamRenderer_ && !streamRenderer_->text().empty()) {
            streamRenderer_->reset();
        }
        streamFedLen_ = 0;
        streamEpoch_  = ~0ULL;
    }

    // 由上一帧 viewport 可见区域反推可折叠消息的鼠标命中区域。
    // 必须在 scrollable_->Render() 之前计算: 此时 visibleBoxes() 为上一帧数据,
    // 与用户当前看到的屏幕内容一致。
    // (注意: 本组件采用懒构建, OnRender 阶段不构建任何子项, 仅产出布局节点)
    collapsibleBoxes_.clear();
    collapsibleIndices_.clear();
    if (ctx_.frameState) {
        const auto& vboxes = scrollable_->visibleBoxes();
        const auto& msgs   = ctx_.frameState->messages;
        for (size_t i = 0; i < vboxes.size() && i < msgs.size(); ++i) {
            const auto& msg = *msgs[i];
            // 可折叠消息: Think / Tool / System (点击 header 折叠/展开)
            const bool collapsible
                = (msg.role == TUIMessage::Role::Think || msg.role == TUIMessage::Role::Tool
                   || msg.role == TUIMessage::Role::System || msg.role == TUIMessage::Role::Tip);
            if (!collapsible || vboxes[i].IsEmpty()) {
                continue;
            }
            collapsibleBoxes_.push_back(vboxes[i]);
            collapsibleIndices_.push_back(i);
        }
    }

    // 中断控件命中区域: 清空后由本帧 scrollable_->Render() 中构建可见
    // Interrupt 消息时填充 (buildInterruptControl), 供下一帧点击命中检测
    interruptHits_.clear();

    return hbox({
               text("   "),
               scrollable_->Render() | bold | flex,
               text("   "),
           })
           | reflect(areaBox_);
}

bool MessageListComponent::OnEvent(Event event) {
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (handleCollapsibleClick(mouse)) {
            return true;
        }
        if (handleInterruptClick(mouse)) {
            return true;
        }
        return scrollable_->OnEvent(event);
    }
    // 键盘: 优先作用于当前激活的中断消息 (输入框编辑/选中切换/确认/取消)
    if (activeInterruptMsg_ != static_cast<size_t>(-1)) {
        if (handleInterruptKey(event)) {
            return true;
        }
    }
    return false;
}

bool MessageListComponent::handleCollapsibleClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (areaBox_.x_max < areaBox_.x_min) {
        return false; // 尚未布局
    }
    if (mouse.x < areaBox_.x_min || mouse.x > areaBox_.x_max) {
        return false;
    }
    for (size_t k = 0; k < collapsibleBoxes_.size() && k < collapsibleIndices_.size(); ++k) {
        if (mouse.y < collapsibleBoxes_[k].y_min || mouse.y > collapsibleBoxes_[k].y_max) {
            continue;
        }
        const size_t mi      = collapsibleIndices_[k];
        bool         handled = false;
        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi < st.messages.size()) {
                auto& msg     = ctx_.state->mutableMessage(st, mi);
                msg.collapsed = !msg.collapsed;
                handled       = true;
            }
        });
        if (handled) {
            ctx_.postRedraw();
        }
        return handled;
    }
    return false;
}

// ---------------------------------------------------------------------------
// LazyScrollable 回调
// ---------------------------------------------------------------------------

bool MessageListComponent::hasStreamingToken(const TUIRenderState& st) const {
    return st.isStreaming && st.currentToken && !st.currentToken->empty();
}

size_t MessageListComponent::itemCount() {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        return 1; // 空状态 banner (fillViewport)
    }
    size_t n = st.messages.size();
    if (hasStreamingToken(st)) {
        syncStream(st); // 每帧同步流式增量状态 (feed 新 token / harvest 稳定块)
        if (streamUseIncremental_) {
            n += streamItemCount();
        } else {
            n += 1; // 降级: 整段 paragraph 单子项
        }
    }
    return n;
}

size_t MessageListComponent::streamItemCount() const {
    size_t n = streamHeaderCount_;
    if (streamRenderer_) {
        n += streamRenderer_->stableBlockCount();
        if (streamRenderer_->frontierStart() < streamRenderer_->text().size()) {
            ++n; // 尾部 (仍增长) 块
        }
    }
    return n;
}

uint64_t MessageListComponent::itemKey(size_t index) {
    const auto& st      = *ctx_.frameState;
    auto        combine = [](uint64_t seed, uint64_t v) -> uint64_t {
        return seed ^ (v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    if (st.messages.empty() && !hasStreamingToken(st)) {
        // banner 内容随连接状态 + 启动进度 (startupProgress) 变化:
        // 两者计入 key 使 LazyScrollable 缓存失效重建 —— 若 key 恒定,
        // 缓存的是首次渲染的 banner 文本, 连接状态切换/启动步骤更新
        // 都不会反映到屏幕 (启动进度永远停在第一步)
        uint64_t h = 1;
        h          = combine(h, static_cast<uint64_t>(st.connState));
        for (const char c : st.startupProgress) {
            h = combine(h, static_cast<uint64_t>(static_cast<uint8_t>(c)));
        }
        return h;
    }
    if (index < st.messages.size()) {
        // 消息内容变化必然伴随消息指针变化 (见 TUISharedState::mutableMessage /
        // onSync 整体替换), 故以指针为主 key; 附加廉价 O(1) 特征 (长度/标志)
        // 防止地址重用 (ABA) —— 内容变则指针变, 长度几乎必然同步变化
        uint64_t    h = reinterpret_cast<uint64_t>(st.messages[index].get());
        const auto& m = *st.messages[index];
        h             = combine(h, static_cast<uint64_t>(m.role));
        h             = combine(h, m.collapsed);
        h             = combine(h, m.tool ? m.tool->toolFinished : false);
        h             = combine(h, static_cast<uint64_t>(m.durationMs));
        h             = combine(h, static_cast<uint64_t>(m.startTimeMs));
        h             = combine(h, m.text.size());
        h             = combine(h, m.tool ? m.tool->toolResult.size() : size_t{0});
        h             = combine(h, m.tool ? m.tool->toolName.size() : size_t{0});
        // 中断消息: 状态变化经 mutableMessage 复制 (指针已变), 此处附加
        // 结构特征进一步防 ABA
        h = combine(
            h,
            static_cast<uint64_t>(
                m.interrupt ? m.interrupt->interruptStatus : TUIMessage::InterruptStatus::Waiting
            )
        );
        h = combine(h, m.interrupt ? static_cast<uint64_t>(m.interrupt->inputIndex) : uint64_t{0});
        h = combine(h, m.interrupt ? m.interrupt->interruptResult.size() : size_t{0});
        // 中断 UI 状态 (编辑文本/选中项/提示) 变化经 version 递增反映到 key,
        // 触发高度重估 (tip 增删影响估算行数)
        if (m.role == TUIMessage::Role::Interrupt) {
            InterruptKey key;
            if (interruptKeyOf(m, key)) {
                auto it = interruptUi_.find(key);
                h       = combine(h, it != interruptUi_.end() ? it->second.version : 0);
            }
        }
        return h;
    }
    // ---- 流式区 ----
    if (!streamUseIncremental_) {
        // 降级路径: 单个 paragraph 项, 以 (指针, 长度, role) 作为 key 触发高度重估
        uint64_t h = reinterpret_cast<uint64_t>(st.currentToken.get());
        h          = combine(h, st.currentToken ? st.currentToken->size() : 0);
        h          = combine(h, 0xDEAD0000ull);
        return h;
    }
    const size_t si = index - st.messages.size();
    // 头部项 (thinking 时长行): key 稳定, 帧间缓存
    if (si < streamHeaderCount_) {
        return combine(streamGen_, 0xFACE0000ull);
    }
    const size_t bi = si - streamHeaderCount_;
    if (streamRenderer_ && bi < streamRenderer_->stableBlockCount()) {
        // 稳定块: 内容不可变, key = (代次, 块序号) 稳定, LazyScrollable 缓存布局
        return combine(streamGen_, 0xC0FFEE00ull + static_cast<uint64_t>(bi));
    }
    // 尾部块: 每帧内容变化, key 随 (token 长度, 尾部起点) 变化触发重建
    uint64_t h = combine(streamGen_, 0xFEED0000ull);
    h          = combine(h, st.currentToken ? st.currentToken->size() : 0);
    h          = combine(h, streamRenderer_ ? streamRenderer_->frontierStart() : 0);
    return h;
}

size_t MessageListComponent::estimateHeight(size_t index, int width) {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        return 1; // banner 为 fillViewport, 高度由 LazyScrollable 置为视口高度
    }
    if (index < st.messages.size()) {
        const auto& msg = *st.messages[index];
        // 注意: 所有消息分支的估算高度 = buildMessageBlock 内容行数 + 1 (尾部空行)。
        // buildMessageItem 产出 vbox({block, text("")}), 实测高度恒比内容多 1 行;
        // 若估算漏掉该空行, 不可见项 (未测量) 高度恒偏低 1 行/条, 总高度偏低
        // -> stickToBottom 滚动偏移偏小, 底部最新消息被推出视口 (整行不显示
        // 但滚动条/总高度仍存在)。此偏差还使"估算==实测"恒不成立, 每帧触发
        // corrected 重算, 浪费且无法收敛到精确总高度。
        switch (msg.role) {
            case TUIMessage::Role::User:
                return estimateLines(msg.text, width) + 1;
            case TUIMessage::Role::Assistant:
                // Assistant 走 renderMarkdown (cmark-gfm + DomBuilder): 段内
                // 单换行 (softbreak) 合并为空格, 按此语义估算 (estimateLines
                // 按 \n 硬换行计数, 对"多行单换行"文本严重高估 -> 顶部消息
                // 被推出视口空白, 见 estimateMarkdownLines 注释)
                return estimateMarkdownLines(msg.text, width) + 1;
            case TUIMessage::Role::System:
                // 折叠: 仅 header 行 + 空行; 展开: header + 内容 + 空行
                return (msg.collapsed ? 1 : 1 + estimateLines(msg.text, width)) + 1;
            case TUIMessage::Role::Think:
                // 折叠: 仅 header 行 + 空行; 展开: header + 内容 + 空行
                // (展开内容走 renderMarkdown, 同 Assistant 用 markdown 语义估算)
                return (msg.collapsed ? 1 : 1 + estimateMarkdownLines(msg.text, width)) + 1;
            case TUIMessage::Role::Tip:
                // 折叠: 仅 header 行 + 空行; 展开: header + 内容 + 空行
                return (msg.collapsed ? 1 : 1 + estimateLines(msg.text, width)) + 1;
            case TUIMessage::Role::Tool: {
                if (msg.collapsed) {
                    return 1 + 1; // header 行 + 空行
                }
                // 注意: 渲染 (buildMessageBlock) 对 filesystem_edit 工具特化为
                // diff 展示 (appendEditToolBody -> renderEditToolDiff), 行数 =
                // old_str/new_str 差异行数, 与 args JSON 行数/toolResult 行数无关。
                // 若按 args/result 估算, 大 diff 时严重低估 -> edit 消息被 continue
                // 跳过 (视口区域空白), 且总高度偏低 -> stickToBottom 底部内容被推出
                // 视口 (用户报告"某些消息显示为空白")。故对 edit 工具解析参数,
                // 用 computeLineDiff 精确估算 diff 行数 (仅 key 变化/宽度变化时调用,
                // 成本可接受)。
                const bool isEditTool = msg.tool && msg.tool->toolName == "agentxx_filesystem_edit";
                const bool isPlanTool = msg.tool && msg.tool->toolName == "agentxx_planning_write";
                const bool finished   = msg.tool && msg.tool->toolFinished;
                if (isEditTool && finished && !isToolResultError(msg.tool->toolResult)) {
                    size_t diffLines = 0;
                    bool   hasPath   = false;
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            auto args = neograph::json::parse(msg.text);
                            hasPath   = !args.value("path", std::string{}).empty();
                            diffLines = agentxx::util::computeLineDiff(
                                            args.value("old_str", std::string{}),
                                            args.value("new_str", std::string{})
                            )
                                            .size();
                            return true;
                        },
                        [](std::string) -> bool {
                            return false;
                        }
                    );
                    // header + (file 行) + diff 行 + 尾部空行
                    return static_cast<int>(1 + (hasPath ? 1 : 0) + diffLines) + 1;
                }
                if (isPlanTool) {
                    size_t planLines = 1; // header
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            auto       args    = neograph::json::parse(msg.text);
                            const auto roadmap = args.value("roadmap", std::string{});
                            if (!roadmap.empty()) {
                                planLines      += 1; // "State Diagram:"
                                size_t rmLines  = 1;
                                for (char ch : roadmap) {
                                    if (ch == '\n') {
                                        ++rmLines;
                                    }
                                }
                                planLines += rmLines * 3 + 3;
                            }
                            if (args.contains("todos") && args["todos"].is_array()) {
                                planLines += 1; // "Todos:"
                                for (const auto& td : args["todos"]) {
                                    planLines += 1;
                                    if (td.is_object()
                                        && !td.value("summary", std::string{}).empty()) {
                                        planLines += 1;
                                    }
                                }
                            }
                            if (args.contains("notes")) {
                                planLines += 1; // "Notes:"
                                if (args["notes"].is_string()) {
                                    planLines
                                        += estimateLines(args["notes"].get<std::string>(), width);
                                } else if (args["notes"].is_array()) {
                                    planLines += args["notes"].size();
                                }
                            }
                            return true;
                        },
                        [](std::string) -> bool {
                            return false;
                        }
                    );
                    return static_cast<int>(planLines) + 1; // +1: 尾部空行
                }
                size_t lines = 1; // header
                if (!msg.text.empty()) {
                    // 与渲染一致: 参数按 JSON 缩进格式化后的行数估算
                    lines += estimateLines(formatToolArgs(msg.text), width);
                }
                lines += finished ? estimateLines(msg.tool->toolResult, width) : 1;
                return lines + 1; // +1: 尾部空行
            }
            case TUIMessage::Role::Interrupt: {
                // 粗略估算 (进入视口后实测修正): 头行 + label + depict +
                // 控件区 + 提示/状态行 + 尾部空行
                // (编辑文本/选中项高度恒定, 不参与估算)
                const bool waiting
                    = msg.interrupt
                      && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting;
                if (waiting) {
                    size_t lines = 4;
                    auto   type  = msg.interrupt->inputType;
                    if (type == "enum") {
                        // 枚举项全部渲染 (buildInterruptControl 逐项输出), 不能截断:
                        // 截断估算 (如 min(n,5)) 使 >5 项的中断消息严重低估 ->
                        // 被 continue 跳过 (消息区空白), 同 Tool diff/mermaid 机制
                        lines += msg.interrupt->inputEnums.size();
                    }
                    InterruptKey key;
                    if (interruptKeyOf(msg, key)) {
                        auto it = interruptUi_.find(key);
                        if (it != interruptUi_.end() && !it->second.tip.empty()) {
                            ++lines;
                        }
                    }
                    return static_cast<int>(lines) + 1; // +1: 尾部空行
                }
                // 非 waiting: 状态行 1 行 + 尾部空行 = 2
                return 2;
            }
        }
        return 2; // 未知角色兜底: 内容 1 行 + 空行
    }
    // ---- 流式区 ----
    if (!streamUseIncremental_) {
        // 降级路径: 单个 paragraph 项
        return 1 + estimateLines(*st.currentToken, width);
    }
    const size_t si = index - st.messages.size();
    if (si < streamHeaderCount_) {
        return 1; // thinking 头部单行
    }
    const size_t bi = si - streamHeaderCount_;
    if (streamRenderer_) {
        if (bi < streamRenderer_->stableBlockCount()) {
            // 稳定块 + 尾部空行分隔 (与 buildStreamingStable 的 vbox{block, text("")} 对应)
            // 稳定块内容为 markdown 块 (段落 softbreak 合并语义), 同消息估算
            return 1 + estimateMarkdownLines(streamRenderer_->stableBlockSource(bi), width);
        }
        // 尾部块 (仍增长, markdown 语义: softbreak 合并/围栏等, 同消息估算)
        const auto   t = streamRenderer_->text();
        const size_t f = streamRenderer_->frontierStart();
        if (f < t.size()) {
            return std::max(static_cast<size_t>(1), estimateMarkdownLines(t.substr(f), width));
        }
    }
    return 1;
}

bool MessageListComponent::fillViewport(size_t index) {
    const auto& st = *ctx_.frameState;
    return index == 0 && st.messages.empty() && !hasStreamingToken(st);
}

LazyBuiltItem MessageListComponent::buildItem(size_t index) {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        LazyBuiltItem out;
        out.element   = buildBanner();
        out.cacheable = true;
        return out;
    }
    if (index < st.messages.size()) {
        return buildMessageItem(*st.messages[index], index);
    }
    // ---- 流式区 ----
    if (!streamUseIncremental_) {
        return buildStreamingItem(st);
    }
    const size_t si = index - st.messages.size();
    if (si < streamHeaderCount_) {
        return buildStreamingHeader(st);
    }
    const size_t bi = si - streamHeaderCount_;
    if (streamRenderer_ && bi < streamRenderer_->stableBlockCount()) {
        return buildStreamingStable(st, bi);
    }
    return buildStreamingFrontier(st);
}

Element MessageListComponent::buildBanner() {
    const auto& theme = *ctx_.theme;
    const auto& st    = *ctx_.frameState;

    // 连接状态行 (banner 下半部):
    // - Connecting: agent-io 正在启动, 下方逐步显示当前正在执行的启动
    //   操作 (如"加载 MCP server: xxx"), 并提示输入将在连接完成后自动发送
    // - Failed:     连接失败提示 + 可点击的 [重试] 按钮 (TUIClientAgentIO 全局
    //               鼠标事件经 retryButtonBox 命中检测, 点击重新发起连接)
    // - Connected:  启动完成提示 + 默认按键提示
    Element statusLine;
    switch (st.connState) {
        case ConnState::Connecting: {
            Elements els;
            els.push_back(text("输入消息将在连接完成后自动发送") | color(theme.hintColor) | center);
            els.push_back(text("agent-io 正在启动中 ...") | color(theme.hintColor) | center);
            if (!st.startupProgress.empty()) {
                // 当前正在执行的启动步骤 (agent 线程逐步上报)
                els.push_back(
                    text(fmt::format("~ {}", st.startupProgress)) | color(theme.accentColor)
                    | center
                );
            }
            statusLine = vbox(std::move(els));
            break;
        }
        case ConnState::Failed: {
            // 重置上一帧命中区域 (避免缓存命中的旧 Box 残留; 元素重建后 reflect 重新填充)
            retryButtonBox_ = ftxui::Box{};
            statusLine      = hbox({
                filler(),
                text("  agent-io 连接失败  ") | color(theme.errorColor),
                text("  [ 重试 ]  ") | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor)
                    | bold | reflect(retryButtonBox_),
                filler(),
            });
            break;
        }
        default: // Connected: 启动完成提示 + 按键提示
            statusLine = vbox({
                text(R"(Type a message to start. [Esc] cancel, [Ctrl+C] quit.)")
                    | color(theme.hintColor) | center,
            });
            break;
    }

    return vbox({
        filler(),
        text(R"_(
    ___   _____________   ________
   /   | / ____/ ____/ | / /_  __/    __    __
  / /| |/ / __/ __/ /  |/ / / /    __/ /___/ /_
 / ___ / /_/ / /___/ /|  / / /    /_  __/_  __/
/_/  |_\____/_____/_/ |_/ /_/      /_/   /_/
)_") | bold | color(theme.accentColor)
            | center,
        statusLine,
        filler(),
    });
}

LazyBuiltItem MessageListComponent::buildMessageItem(const TUIMessage& msg, size_t index) {
    const int maxWidth = std::max(1, scrollable_->contentWidth());

    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
    auto block = buildMessageBlock(msg, index, maxWidth, builders);

    LazyBuiltItem out;
    out.element           = vbox({std::move(block), text("")});
    const size_t srcBytes = msg.text.size() + (msg.tool ? msg.tool->toolResult.size() : 0)
                            + (msg.tool ? msg.tool->toolName.size() : 0);
    // 渲染树内存估算: FTXUI 渲染树为源文本 ~30-70 倍 (text() 按 glyph 拆
    // std::string, paragraph() 按词拆元素), 按 64 系数折算上报, 使
    // LazyScrollable 的字节预算 (maxBytes) 约束真实驻留内存而非源文本字节
    // (见构造函数预算注释; 系数取实测范围上沿, 宁紧勿松)
    out.sourceBytes = srcBytes * 64;
    // 中断消息不缓存: 每帧重建以刷新控件 reflect 命中区域 (interruptHits_),
    // 否则缓存命中时控件 Box 丢失, 点击无法命中; 中断消息数量少, 成本可忽略
    out.cacheable = (msg.role != TUIMessage::Role::Interrupt);
    // markdown DomBuilder 生命周期与 Element 绑定
    // (Element 内 reflect 的链接 Box 指向 builder 内部容器)
    for (auto& b : builders) {
        out.attachments.push_back(std::move(b));
    }
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingItem(const TUIRenderState& st) {
    // 降级路径 (动画等级不足, 未启用增量渲染): 整段 paragraph 单子项
    const auto& theme = *ctx_.theme;

    LazyBuiltItem out;
    out.cacheable   = false;
    out.sourceBytes = st.currentToken ? st.currentToken->size() : 0;

    Element block;
    if (st.currentTokenRole == TUIMessage::Role::Think) {
        Elements lines;
        Elements header;
        header.push_back(text("- [Think] ") | color(theme.thinkingColor));
        const TUIMessage* currentMsg = nullptr;
        for (size_t i = st.messages.size(); i > 0; --i) {
            if (st.messages[i - 1]->role == st.currentTokenRole) {
                currentMsg = st.messages[i - 1].get();
                break;
            }
        }
        if (currentMsg && currentMsg->durationMs > 0) {
            header.push_back(
                text(agentxx::util::formatDurationMilliseconds(currentMsg->durationMs) + " ")
                | color(theme.thinkingColor)
            );
        }
        lines.push_back(hbox(std::move(header)));
        lines.push_back(paragraph(*st.currentToken) | color(theme.thinkingColor));
        block = vbox(std::move(lines));
    } else {
        block = paragraph(*st.currentToken) | color(theme.normalColor);
    }
    out.element = std::move(block);
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingHeader(const TUIRenderState& st) {
    // thinking 头部项: "[Think] <时长>" 单行, 可缓存 (key 稳定)
    const auto& theme = *ctx_.theme;
    Elements    header;
    header.push_back(text("- [Think] ") | color(theme.thinkingColor));
    const TUIMessage* currentMsg = nullptr;
    for (size_t i = st.messages.size(); i > 0; --i) {
        if (st.messages[i - 1]->role == st.currentTokenRole) {
            currentMsg = st.messages[i - 1].get();
            break;
        }
    }
    if (currentMsg && currentMsg->durationMs > 0) {
        header.push_back(
            text(agentxx::util::formatDurationMilliseconds(currentMsg->durationMs) + " ")
            | color(theme.thinkingColor)
        );
    }
    LazyBuiltItem out;
    out.element     = hbox(std::move(header));
    out.cacheable   = true;
    out.sourceBytes = 0;
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingStable(const TUIRenderState& st, size_t bi) {
    // 已闭合顶层块: 构建一次后由 LazyScrollable 缓存 (key 稳定, 仅可见项被布局)
    const auto&        theme    = *ctx_.theme;
    const int          maxWidth = std::max(1, scrollable_->contentWidth());
    const bool         thinking = (st.currentTokenRole == TUIMessage::Role::Think);
    const ftxui::Color c        = thinking ? theme.thinkingColor : theme.normalColor;

    LazyBuiltItem out;
    out.cacheable = true;
    if (streamRenderer_) {
        out.element = vbox({
            streamRenderer_->stableBlockElement(bi, theme.markdownTheme, maxWidth) | color(c),
            text(""), // 块间空行分隔 (与整篇解析一致)
        });
        // 渲染树估算 (×64, 同 buildMessageItem): 稳定块 Element 同样按 glyph
        // 拆 std::string, 内存放大 ~30-70 倍
        out.sourceBytes = streamRenderer_->stableBlockSource(bi).size() * 64;
    } else {
        out.element = text("");
    }
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingFrontier(const TUIRenderState& st) {
    // 尾部 (仍增长) 块: 每帧重建
    const auto&        theme    = *ctx_.theme;
    const int          maxWidth = std::max(1, scrollable_->contentWidth());
    const bool         thinking = (st.currentTokenRole == TUIMessage::Role::Think);
    const ftxui::Color c        = thinking ? theme.thinkingColor : theme.normalColor;

    LazyBuiltItem out;
    out.cacheable   = false;
    out.sourceBytes = st.currentToken ? st.currentToken->size() : 0;
    if (streamRenderer_) {
        std::unique_ptr<markdown::DomBuilder> fb;
        auto el = streamRenderer_->renderFrontier(theme.markdownTheme, maxWidth, fb);
        if (fb) {
            out.attachments.push_back(std::move(fb));
        }
        if (!el) {
            el = text("");
        }
        out.element = el | color(c);
    } else {
        out.element = text("");
    }
    return out;
}

void MessageListComponent::syncStream(const TUIRenderState& st) {
    streamUseIncremental_ = false;
    streamHeaderCount_    = 0;

    if (!hasStreamingToken(st)) {
        // 流式结束: 释放渲染器缓存 (OnRender 也会在无 token 时重置)
        if (streamRenderer_ && !streamRenderer_->text().empty()) {
            streamRenderer_->reset();
            ++streamGen_;
        }
        streamFedLen_ = 0;
        // 下一流强制全量重建 (防御: 即使 future 代码在重建 currentToken 时
        // 忘记递增 epoch, 此处兜底也能保证渲染器不与新流串用)
        streamEpoch_ = ~0ULL;
        return;
    }

    // 增量渲染仅在动画等级满足时启用; 否则降级为整段 paragraph 单子项。
    // 降级期间不 feed 渲染器、不更新 fedLen/epoch —— renderer 内容与 fedLen
    // 的一致性保持 (renderer text == token[0..fedLen)), 恢复增量后按 fedLen
    // 追加缺失的增量即可, 无需重建 (与"新流"路径区分)。
    const bool inc = (st.currentTokenRole == TUIMessage::Role::Think)
                         ? TUISettings::instance().isAnimationEnabled(AnimationLevel::Ultra)
                         : TUISettings::instance().isAnimationEnabled(AnimationLevel::Low);
    if (!inc) {
        return;
    }
    streamUseIncremental_ = true;
    streamHeaderCount_    = (st.currentTokenRole == TUIMessage::Role::Think) ? 1 : 0;

    if (!streamRenderer_) {
        streamRenderer_ = std::make_unique<markdown::IncrementalRenderer>();
    }
    const auto& tok = *st.currentToken;
    if (st.currentTokenEpoch != streamEpoch_) {
        // 新流 (token 被重置/更换): 重建渲染器并全量 feed。
        // 判定依据: client 线程仅在新建 currentToken 时递增 epoch (COW 复制
        // 不递增, 内容仍是同一流延续), 故 epoch 相同即"同一流", 无需再对
        // 整段累积文本做前缀比较
        streamRenderer_->reset();
        ++streamGen_;
        streamEpoch_ = st.currentTokenEpoch;
        if (!tok.empty()) {
            streamRenderer_->append(tok);
        }
        streamFedLen_ = tok.size();
    } else if (tok.size() > streamFedLen_) {
        // 同一流: 仅 feed 新增字节 (fedLen 不变量: renderer text == token[0..fedLen))
        streamRenderer_->append(std::string_view(tok).substr(streamFedLen_));
        streamFedLen_ = tok.size();
    }
}

// ---------------------------------------------------------------------------
// 工具调用头部摘要 (TUI 特化渲染)
// ---------------------------------------------------------------------------

struct ToolHeaderSummary {
    std::string toolName;
    std::string argsSummary;
};

/// 将工具调用的参数 JSON 摘要为单行头部, 例如:
/// - agentxx_filesystem_read  -> "Read", " · [0, 100] /path/file" (运行中 " · [running] [0, 100]
/// /path/file")
/// - agentxx_filesystem_write      -> "Write", " · /path/file" (运行中 " · [running] /path/file")
/// - agentxx_web_search                 -> "Search", " · <query>" (运行中 " · [running] <query>")
/// 未知工具 / 参数解析失败返回空 toolName, 调用方回退显示原始 toolName
static ToolHeaderSummary
    buildToolHeaderSummary(std::string_view toolName, std::string_view argsText, bool running) {
    // 参数 JSON 解析失败 (截断/异常) 或解析结果非对象时回退显示原始 toolName
    bool           parseOk = true;
    neograph::json args    = agentxx::util::catchError<neograph::json>(
        [&]() -> neograph::json {
            auto j = neograph::json::parse(argsText);
            if (!j.is_object()) {
                parseOk = false;
            }
            return j;
        },
        [&](std::string) -> neograph::json {
            parseOk = false;
            return {};
        }
    );
    if (!parseOk || !args.is_object()) {
        return {};
    }

    auto getStr = [&args](std::string_view key) -> std::string {
        return args.value(std::string(key), std::string{});
    };
    auto getStrList = [&args](std::string_view key) -> std::vector<std::string> {
        return args.value(std::string(key), std::vector<std::string>{});
    };

    /// 拼接 "{action}" 与 " · [running] [{params}] {target}" (params 可空)
    auto make = [&](std::string_view action, std::string_view params, std::string_view target
                ) -> ToolHeaderSummary {
        std::string argsSummary = " ·";
        if (running) {
            argsSummary += " [running]";
        }
        if (!params.empty()) {
            argsSummary += " [";
            argsSummary += params;
            argsSummary += "]";
        }
        if (!target.empty()) {
            argsSummary += " ";
            argsSummary += target;
        }
        return ToolHeaderSummary{
            .toolName    = std::string(action),
            .argsSummary = std::move(argsSummary),
        };
    };

    /// "[offset, limit]" 区间参数摘要 (默认 -1/缺省表示不过滤, 不显示)
    auto range = [&args](const char* offKey, const char* limKey) -> std::string {
        const int64_t off = args.value(offKey, int64_t{-1});
        const int64_t lim = args.value(limKey, int64_t{-1});
        if (off <= 0 && lim <= 0) {
            return {};
        }
        if (off <= 0) {
            return fmt::format("0, {}", lim);
        }
        if (lim <= 0) {
            return fmt::format("{}", off);
        }
        return fmt::format("{}, {}", off, lim);
    };

    /// 字符串列表摘要: 最多展示 maxShow 项, 超出以 ", ..." 收尾
    auto joinList = [](const std::vector<std::string>& items, size_t maxShow = 2) -> std::string {
        std::string  out;
        const size_t n = (items.size() < maxShow) ? items.size() : maxShow;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                out += ", ";
            }
            out += items[i];
        }
        if (items.size() > maxShow) {
            out += (n > 0 ? ", ..." : "...");
        }
        return out;
    };

    // TODO: 操作失败时显示失败内容
    if (toolName == "agentxx_filesystem_list") {
        return make("List", {}, getStr("path"));
    }
    if (toolName == "agentxx_filesystem_read") {
        return make("Read", range("line_offset", "line_limit"), getStr("path"));
    }
    if (toolName == "agentxx_filesystem_write") {
        return make("Write", {}, getStr("path"));
    }
    if (toolName == "agentxx_filesystem_edit") {
        return make("Edit", {}, getStr("path"));
    }
    if (toolName == "agentxx_filesystem_glob") {
        return make("Glob", {}, joinList(getStrList("file_patterns")));
    }
    if (toolName == "agentxx_filesystem_grep") {
        // 匹配模式 (引号包裹) 作为参数区, 文件模式作为主参数
        const auto  patterns = getStrList("text_patterns");
        const auto  files    = getStrList("file_patterns");
        std::string quoted;
        for (size_t i = 0; i < patterns.size() && i < 2; ++i) {
            if (i > 0) {
                quoted += ", ";
            }
            quoted += '"';
            quoted += oneLinePreview(patterns[i], 50);
            quoted += '"';
        }
        if (patterns.size() > 2) {
            quoted += ", ...";
        }
        return make("Grep", quoted, joinList(files));
    }
    if (toolName == "agentxx_web_search") {
        return make("Search", {}, oneLinePreview(getStr("query"), 100));
    }
    if (toolName == "agentxx_web_fetch") {
        return make("Fetch", {}, oneLinePreview(getStr("url"), 100));
    }
    if (toolName == "agentxx_web_fetch_markdown") {
        return make("FetchMD", {}, oneLinePreview(getStr("url"), 100));
    }
    // execute 系列 (bash/windows/python/javascript): 统一缩略名 Bash, 内容为命令
    if (toolName == "agentxx_execute_bash_command" || toolName == "agentxx_execute_windows_command"
        || toolName == "agentxx_execute_python_command"
        || toolName == "agentxx_execute_javascript_command") {
        return make("Bash", {}, oneLinePreview(getStr("command"), 100));
    }
    // planning_write: 缩略名称 Plan, 缩略内容取 todos 格式化为一行并用 ; 隔开
    if (toolName == "agentxx_planning_write") {
        std::string todosSummary;
        if (args.contains("todos") && args["todos"].is_array()) {
            for (const auto& td : args["todos"]) {
                std::string item;
                if (td.is_object()) {
                    const auto state   = td.value("state", std::string{});
                    const auto content = td.value("content", std::string{});
                    if (content.empty()) {
                        continue;
                    }
                    std::string icon = "[ ]";
                    if (state == "in_progress") {
                        icon = "[~]";
                    } else if (state == "completed") {
                        icon = "[#]";
                    } else if (state == "failed") {
                        icon = "[!]";
                    }
                    item = fmt::format("{} {}", icon, content);
                } else if (td.is_string()) {
                    item = td.get<std::string>();
                }
                if (item.empty()) {
                    continue;
                }
                if (!todosSummary.empty()) {
                    todosSummary += "; ";
                }
                todosSummary += item;
            }
        }
        return make("Plan", {}, todosSummary);
    }
    return {};
}

// ---------------------------------------------------------------------------
// 消息块构建 (与旧实现一致的视觉呈现)
// ---------------------------------------------------------------------------

Element MessageListComponent::buildMessageBlock(
    const TUIMessage&                                   msg,
    size_t                                              msgIndex,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    const auto& theme = *ctx_.theme;

    switch (msg.role) {
        case TUIMessage::Role::User:
            // 内容超宽时 xflex_shrink 使段落吸收剩余宽度换行/裁剪,
            // 避免 hbox 按比例压缩前缀 "> " (见 ftxui box_helper::ComputeShrinkHard)
            return hbox({
                text("> ") | color(theme.userColor),
                paragraph(msg.text) | color(theme.userColor) | xflex_shrink,
            });
        case TUIMessage::Role::Assistant: {
            auto [el, builder]
                = renderMarkdown(msg.text, theme.assistantColor, theme.markdownTheme, maxWidth);
            if (builder) {
                mdBuilders.push_back(std::move(builder));
            }
            return el;
        }
        case TUIMessage::Role::System: {
            ftxui::Color tipColor = theme.systemColor;
            // 可折叠: header 行带 +/- 折叠标记与单行预览 (折叠态), 展开态显示全文
            // (与 Think 消息同一折叠模式; 默认折叠见创建处 makeText / onDelta)
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "- " : "+ ") | color(tipColor));
            header.push_back(text("[System] ") | color(tipColor));
            if (!expanded) {
                // 预览可能超出宽度: xflex_shrink 使预览吸收剩余宽度并在右缘裁剪,
                // 避免 hbox 把 "- [System] " 前缀一并压缩 (向左覆盖压缩)
                header.push_back(
                    text(oneLinePreview(msg.text)) | color(tipColor) | dim | xflex_shrink
                );
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                lines.push_back(paragraph(msg.text) | color(tipColor));
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Tip: {
            // 按提示级别区分前缀与颜色 (Info/Warning/Error)
            std::string  prefix   = "# ";
            ftxui::Color tipColor = theme.hintColor;
            const auto   tipLevel = msg.tip ? msg.tip->tipLevel : TUIMessage::TipLevel::Info;
            switch (tipLevel) {
                case TUIMessage::TipLevel::Warning:
                    prefix   = "# [Warn] ";
                    tipColor = theme.thinkingColor;
                    break;
                case TUIMessage::TipLevel::Error:
                    prefix   = "# [Error] ";
                    tipColor = theme.errorColor;
                    break;
                case TUIMessage::TipLevel::Info:
                    break;
            }
            // 可折叠: header 行带 +/- 折叠标记与单行预览 (折叠态), 展开态显示全文
            // (与 Think 消息同一折叠模式; 默认折叠见创建处 makeText / onDelta)
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "- " : "+ ") | color(tipColor));
            header.push_back(text(prefix) | color(tipColor));
            if (!expanded) {
                // 同 System: 预览超宽时右缘裁剪, 不压缩前缀
                header.push_back(text(oneLinePreview(msg.text)) | color(tipColor) | xflex_shrink);
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                lines.push_back(paragraph(msg.text) | color(tipColor));
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Think: {
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "- " : "+ ") | color(theme.thinkingColor));
            header.push_back(text("[Think] ") | color(theme.thinkingColor));
            if (msg.durationMs > 0) {
                header.push_back(
                    text(agentxx::util::formatDurationMilliseconds(msg.durationMs))
                    | color(theme.thinkingColor)
                );
                header.push_back(text(" "));
            }
            if (!expanded) {
                // 同 System: 预览超宽时右缘裁剪, 不压缩前缀
                std::string previewText;
                if (!msg.text.empty()) {
                    previewText = oneLinePreview(msg.text);
                } else if (msg.think && msg.think->reasoningTokens > 0) {
                    previewText = fmt::format("思考 {} tokens", msg.think->reasoningTokens);
                } else if (msg.think && msg.think->isEncrypted) {
                    previewText = "思考内容被加密";
                }
                if (!previewText.empty()) {
                    header.push_back(
                        text(std::move(previewText)) | color(theme.thinkingColor) | dim
                        | xflex_shrink
                    );
                }
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                if (!msg.text.empty()) {
                    auto [el, builder] = renderMarkdown(
                        msg.text,
                        theme.thinkingColor,
                        theme.markdownTheme,
                        maxWidth
                    );
                    if (builder) {
                        mdBuilders.push_back(std::move(builder));
                    }
                    lines.push_back(std::move(el));
                } else {
                    std::string infoText;
                    if (msg.think && msg.think->reasoningTokens > 0) {
                        infoText = fmt::format("思考 {} tokens", msg.think->reasoningTokens);
                    } else if (msg.think && msg.think->isEncrypted) {
                        infoText = "思考内容被加密";
                    }
                    if (!infoText.empty()) {
                        lines.push_back(
                            text(std::move(infoText)) | color(theme.thinkingColor) | dim
                        );
                    }
                }
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Tool: {
            // 防御: 类型不变量下 tool 应非空 (fromJson/构造均保证), 缺失时跳过渲染
            if (!msg.tool) {
                return text("");
            }
            const bool expanded   = !msg.collapsed;
            const bool isEditTool = msg.tool && msg.tool->toolName == "agentxx_filesystem_edit";
            const bool isPlanTool = msg.tool && msg.tool->toolName == "agentxx_planning_write";
            const bool finished   = msg.tool && msg.tool->toolFinished;
            // TUI 特化: 已知工具头部渲染为 "动词 · 参数摘要"
            // (如 "Read · [0, 100] /path" / "Write · /path"), 未知工具回退原始 toolName
            Elements lines;
            Elements header;
            {
                header.push_back(
                    text(fmt::format("{} [Tool] ", expanded ? "-" : "+")) | color(theme.toolColor)
                );
            }
            if (!expanded) {
                // 折叠状态, 特化渲染
                auto summary = buildToolHeaderSummary(msg.tool->toolName, msg.text, !finished);
                std::string displayName;
                std::string argsSummary;
                if (!summary.toolName.empty()) {
                    displayName = std::move(summary.toolName);
                    argsSummary = std::move(summary.argsSummary);
                } else {
                    displayName = msg.tool->toolName;
                    if (!finished) {
                        argsSummary = " · [running]";
                        if (!msg.text.empty()) {
                            argsSummary += " " + oneLinePreview(msg.text, 80);
                        }
                    } else {
                        auto resPreview = oneLinePreview(msg.tool->toolResult);
                        if (!resPreview.empty()) {
                            argsSummary = " " + std::move(resPreview);
                        }
                    }
                }

                // toolName: 运行中高亮
                if (!finished) {
                    header.push_back(
                        text(std::move(displayName)) | color(theme.accentColor) | bold
                    );
                } else {
                    header.push_back(text(std::move(displayName)) | color(theme.toolColor) | dim);
                }

                if (!argsSummary.empty()) {
                    header.push_back(
                        text(std::move(argsSummary)) | color(theme.toolColor) | dim | xflex_shrink
                    );
                }
            } else {
                if (!finished) {
                    header.push_back(text(msg.tool->toolName) | color(theme.accentColor) | bold);
                } else {
                    header.push_back(text(msg.tool->toolName) | color(theme.toolColor));
                }
            }

            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                if (isEditTool) {
                    appendEditToolBody(msg, lines);
                } else if (isPlanTool) {
                    appendPlanToolBody(msg, lines, maxWidth);
                } else {
                    if (!msg.text.empty()) {
                        // 参数 JSON 缩进格式化 (2 空格) 便于阅读; 解析失败回退原文
                        lines.push_back(hbox({
                            text("  args: ") | color(theme.toolColor),
                            paragraph(formatToolArgs(msg.text)) | color(theme.toolColor)
                                | xflex_shrink,
                        }));
                    }
                    if (finished) {
                        lines.push_back(hbox({
                            text("  result: ") | color(theme.toolColor),
                            paragraph(msg.tool->toolResult) | color(theme.toolColor) | xflex_shrink,
                        }));
                    } else {
                        lines.push_back(text("  running...") | color(theme.toolColor));
                    }
                }
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Interrupt: {
            // 中断输入消息: 内嵌交互控件, 直接渲染在消息列表中
            Elements lines;

            const bool waiting
                = msg.interrupt
                  && msg.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting;
            if (waiting) {
                // 头行: 类型 + 进度
                Elements header;
                header.push_back(
                    text(fmt::format(
                        "! [Interrupt] Input {}/{}: ",
                        msg.interrupt->inputIndex,
                        msg.interrupt->inputTotal
                    ))
                    | color(theme.accentColor) | bold
                );
                header.push_back(
                    text(msg.interrupt->inputLabel) | color(theme.accentColor) | xflex_shrink
                );
                lines.push_back(hbox(std::move(header)));

                if (!msg.interrupt->inputDepict.empty()) {
                    lines.push_back(hbox({
                        text("  ") | color(theme.hintColor),
                        text(msg.interrupt->inputDepict) | color(theme.hintColor) | xflex_shrink,
                    }));
                }

                lines.push_back(text(" "));
                lines.push_back(buildInterruptControl(msg, msgIndex));

                // 校验失败提示: 从 UI 状态表读取 (消息结构不承载 UI 状态)
                InterruptKey key;
                if (interruptKeyOf(msg, key)) {
                    auto it = interruptUi_.find(key);
                    if (it != interruptUi_.end() && !it->second.tip.empty()) {
                        lines.push_back(hbox({
                            text("  ") | color(theme.hintColor),
                            text(it->second.tip) | color(theme.errorColor) | xflex_shrink,
                        }));
                    }
                }
            } else {
                lines.push_back(buildInterruptStatusLine(msg));
            }

            return vbox(std::move(lines));
        }
    }
    return text("");
}

// ---------------------------------------------------------------------------
// agentxx_filesystem_edit 特化渲染
// ---------------------------------------------------------------------------

void MessageListComponent::appendEditToolBody(const TUIMessage& msg, Elements& lines) {
    const auto& theme = *ctx_.theme;
    // 操作失败: 渲染错误信息, 不渲染基于请求参数的 diff (避免误导: 文件实际未被修改)
    if (msg.tool && msg.tool->toolFinished && isToolResultError(msg.tool->toolResult)) {
        lines.push_back(hbox({
            text("  result: ") | color(theme.toolColor),
            paragraph(msg.tool->toolResult) | color(theme.errorColor) | xflex_shrink,
        }));
        return;
    }
    std::string path, oldStr, newStr;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto args = neograph::json::parse(msg.text);
            path      = args.value("path", std::string{});
            oldStr    = args.value("old_str", std::string{});
            newStr    = args.value("new_str", std::string{});
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
    if (!path.empty()) {
        lines.push_back(hbox({
            text("  file: ") | color(theme.hintColor),
            text(path) | color(theme.toolColor) | xflex_shrink,
        }));
    }
    lines.push_back(renderEditToolDiff(oldStr, newStr));
}

Element MessageListComponent::renderEditToolDiff(std::string_view oldStr, std::string_view newStr) {
    using agentxx::util::DiffLineType;
    const auto& theme = *ctx_.theme;
    auto        diff  = agentxx::util::computeLineDiff(oldStr, newStr);
    if (diff.empty()) {
        return text("  (no changes)") | color(theme.hintColor);
    }

    const int  screenW    = Terminal::Size().dimx;
    const bool sideBySide = screenW >= 100;

    auto trunc = [](std::string_view s, size_t maxChars) -> std::string {
        const auto idx = agentxx::util::findIndexByUtf8Length(s, maxChars);
        if (idx > 0 && idx < s.size()) {
            return fmt::format("{}...", s.substr(0, idx));
        }
        return std::string{s};
    };

    if (!sideBySide) {
        const size_t maxChars = static_cast<size_t>(std::max(20, screenW - 6));
        Elements     lines;
        for (const auto& l : diff) {
            Color       c      = theme.toolColor;
            std::string prefix = " ";
            if (l.type == DiffLineType::Add) {
                c      = theme.accentColor;
                prefix = "+";
            } else if (l.type == DiffLineType::Delete) {
                c      = theme.errorColor;
                prefix = "-";
            }
            lines.push_back(hbox({
                text(prefix) | color(c),
                text(" ") | color(theme.hintColor),
                text(trunc(l.text, maxChars)) | color(c),
            }));
        }
        return vbox(std::move(lines));
    }

    const int colW  = std::max(20, (screenW - 3) / 2);
    const int textW = std::max(8, colW - 6);

    Elements leftLines;
    Elements rightLines;
    auto     emptyCell = [&]() {
        return text(" ") | color(theme.hintColor);
    };
    auto makeCell = [&](std::string_view sign, int no, std::string_view txt, Color c) {
        std::string noStr = (no > 0) ? std::to_string(no) : std::string{};
        return hbox({
            text(sign) | color(c),
            text(noStr) | color(theme.hintColor) | size(WIDTH, EQUAL, 4),
            text(" "),
            text(trunc(txt, static_cast<size_t>(textW))) | color(c) | xflex_shrink,
        });
    };

    size_t i = 0;
    while (i < diff.size()) {
        if (diff[i].type == DiffLineType::Context) {
            leftLines.push_back(makeCell(" ", diff[i].oldLineNo, diff[i].text, theme.toolColor));
            rightLines.push_back(makeCell(" ", diff[i].newLineNo, diff[i].text, theme.toolColor));
            ++i;
            continue;
        }
        std::vector<const agentxx::util::DiffLine*> dels;
        std::vector<const agentxx::util::DiffLine*> adds;
        while (i < diff.size() && diff[i].type == DiffLineType::Delete) {
            dels.push_back(&diff[i]);
            ++i;
        }
        while (i < diff.size() && diff[i].type == DiffLineType::Add) {
            adds.push_back(&diff[i]);
            ++i;
        }
        const size_t maxk = std::max(dels.size(), adds.size());
        for (size_t k = 0; k < maxk; ++k) {
            leftLines.push_back(
                (k < dels.size())
                    ? makeCell("-", dels[k]->oldLineNo, dels[k]->text, theme.errorColor)
                    : emptyCell()
            );
            rightLines.push_back(
                (k < adds.size())
                    ? makeCell("+", adds[k]->newLineNo, adds[k]->text, theme.accentColor)
                    : emptyCell()
            );
        }
    }

    return hbox({
        vbox(std::move(leftLines)) | flex,
        separator(),
        vbox(std::move(rightLines)) | flex,
    });
}

// ---------------------------------------------------------------------------
// agentxx_planning_write 特化渲染
// ---------------------------------------------------------------------------

void MessageListComponent::appendPlanToolBody(
    const TUIMessage& msg,
    Elements&         lines,
    int               maxWidth
) {
    const auto& theme = *ctx_.theme;

    // 操作失败: 渲染错误信息
    if (msg.tool && msg.tool->toolFinished && isToolResultError(msg.tool->toolResult)) {
        lines.push_back(hbox({
            text("  result: ") | color(theme.toolColor),
            paragraph(msg.tool->toolResult) | color(theme.errorColor) | xflex_shrink,
        }));
        return;
    }

    neograph::json args;
    bool           parseOk = agentxx::util::catchError<bool>(
        [&]() -> bool {
            args = neograph::json::parse(msg.text);
            return args.is_object();
        },
        [](std::string) -> bool {
            return false;
        }
    );

    if (!parseOk) {
        if (!msg.text.empty()) {
            lines.push_back(hbox({
                text("  args: ") | color(theme.toolColor),
                paragraph(msg.text) | color(theme.toolColor) | xflex_shrink,
            }));
        }
        return;
    }

    // ---- Block 1: 状态图 (Roadmap / State Diagram) ----
    const auto roadmap = args.value("roadmap", std::string{});
    if (!roadmap.empty()) {
        auto diagram = markdown::parseMermaidStateDiagram(roadmap);
        if (!diagram.nodes.empty()) {
            lines.push_back(hbox({
                text("  State Diagram:") | color(theme.accentColor) | bold,
            }));
            const int diagW  = (maxWidth > 0) ? std::max(20, maxWidth - 4) : 0;
            auto      diagEl = markdown::renderMermaidStateDiagram(
                diagram,
                diagW,
                theme.normalColor,
                markdown::diagramNodeColor(theme.markdownTheme)
            );
            lines.push_back(hbox({
                text("    "),
                diagEl | flex,
            }));
        }
    }

    // ---- Block 2: Todo 列表 ----
    if (args.contains("todos") && args["todos"].is_array() && !args["todos"].empty()) {
        lines.push_back(hbox({
            text("  Todos:") | color(theme.accentColor) | bold,
        }));
        for (const auto& td : args["todos"]) {
            if (td.is_object()) {
                const auto  state   = td.value("state", std::string{});
                const auto  content = td.value("content", std::string{});
                const auto  summary = td.value("summary", std::string{});
                std::string icon    = "[ ]";
                Color       c       = theme.hintColor;
                if (state == "in_progress") {
                    icon = "[~]";
                    c    = theme.thinkingColor;
                } else if (state == "completed") {
                    icon = "[#]";
                    c    = theme.accentColor;
                } else if (state == "failed") {
                    icon = "[!]";
                    c    = theme.errorColor;
                }
                lines.push_back(hbox({
                    text(fmt::format("    {} ", icon)) | color(c) | bold,
                    paragraph(content) | color(c) | xflex_shrink,
                }));
                if (!summary.empty()) {
                    lines.push_back(hbox({
                        text("        - ") | color(theme.hintColor) | dim,
                        paragraph(summary) | color(theme.hintColor) | dim | xflex_shrink,
                    }));
                }
            } else if (td.is_string()) {
                lines.push_back(hbox({
                    text("    [ ] ") | color(theme.hintColor) | bold,
                    paragraph(td.get<std::string>()) | color(theme.hintColor) | xflex_shrink,
                }));
            }
        }
    }

    // ---- Block 3: Note 列表 / 备忘 ----
    if (args.contains("notes")) {
        const auto& notesVal = args["notes"];
        if (notesVal.is_string()) {
            const auto notes = notesVal.get<std::string>();
            if (!notes.empty()) {
                lines.push_back(hbox({
                    text("  Notes:") | color(theme.accentColor) | bold,
                }));
                lines.push_back(hbox({
                    text("    "),
                    paragraph(notes) | color(theme.hintColor) | xflex_shrink,
                }));
            }
        } else if (notesVal.is_array() && !notesVal.empty()) {
            lines.push_back(hbox({
                text("  Notes:") | color(theme.accentColor) | bold,
            }));
            for (const auto& n : notesVal) {
                std::string noteStr = n.is_string() ? n.get<std::string>() : n.dump();
                if (!noteStr.empty()) {
                    lines.push_back(hbox({
                        text("    • ") | color(theme.hintColor),
                        paragraph(noteStr) | color(theme.hintColor) | xflex_shrink,
                    }));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 中断输入消息: 控件渲染 + 交互
// ---------------------------------------------------------------------------

namespace {

/// double 步进后的显示格式: 整数值按 "1.0" 风格, 非整数保留有效精度
std::string formatStepDouble(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        return fmt::format("{:.1f}", v);
    }
    return fmt::format("{:.10g}", v);
}

} // namespace

Element MessageListComponent::buildInterruptStatusLine(const TUIMessage& msg) {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    const auto& it = *msg.interrupt;
    switch (it.interruptStatus) {
        case TUIMessage::InterruptStatus::Confirmed:
            return hbox({
                text(fmt::format("! [Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(fmt::format("已确认 {}: {}", it.inputLabel, it.interruptResult))
                    | color(theme.accentColor) | dim | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Cancelled:
            return hbox({
                text(fmt::format("! [Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(fmt::format("{}: 已取消", it.inputLabel)) | color(theme.errorColor) | dim
                    | xflex_shrink,
            });
        case TUIMessage::InterruptStatus::Expired:
            return hbox({
                text(fmt::format("! [Interrupt] Input {}/{}: ", it.inputIndex, it.inputTotal))
                    | color(theme.hintColor),
                text(fmt::format("{}: 已过期", it.inputLabel)) | color(theme.errorColor) | dim
                    | xflex_shrink,
            });
        default:
            return text("");
    }
}

Element MessageListComponent::buildInterruptControl(const TUIMessage& msg, size_t msgIndex) {
    const auto& theme = *ctx_.theme;
    if (!msg.interrupt) {
        return text("");
    }
    // UI 状态 (编辑文本/选中项): 惰性创建并读取 (渲染是 UI 线程独占路径)
    const auto& ui = uiStateFor(msg);
    const auto& id = *msg.interrupt;

    // 记录命中区域: box 经 shared_ptr 持有, reflect 在布局 (SetBox) 时写回,
    // 点击检测读到的是最新布局位置 (构建阶段记录值会拿到空 Box)
    auto hit = [this, msgIndex](uint8_t kind, int sub, const std::shared_ptr<Box>& box) {
        InterruptHitBox hb;
        hb.msgIndex = msgIndex;
        hb.kind     = kind;
        hb.sub      = sub;
        hb.box      = box;
        interruptHits_.push_back(std::move(hb));
    };
    auto mkBox = []() {
        return std::make_shared<Box>();
    };

    // 按钮样式
    auto btn = [&theme](std::string label, bool active) {
        if (active) {
            return text(label) | bgcolor(theme.buttonActiveBgColor)
                   | color(theme.buttonActiveTextColor) | bold;
        }
        return text(label) | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
    };

    // 确认 / 取消按钮 (共用)
    auto confirmBox = mkBox();
    auto cancelBox  = mkBox();
    auto confirmBtn = btn(" [确认] ", false) | reflect(*confirmBox);
    auto cancelBtn  = text(" ✕ ") | color(theme.errorColor) | reflect(*cancelBox);
    hit(kHitConfirm, 0, confirmBox);
    hit(kHitCancel, 0, cancelBox);

    Element control;
    if (id.inputType == "bool") {
        // 权限询问 (rememberable): 额外显示"记住"开关 —— 勾选后确认时按本次
        // 允许/拒绝注册路径规则, 后续访问该路径或其子目录不再询问
        const bool rememberable = [&]() -> bool {
            auto it = interruptChannels_.find(id.interruptId);
            return it != interruptChannels_.end() && it->second.rememberable;
        }();

        auto yesBox = mkBox();
        auto noBox  = mkBox();
        auto yes    = btn(" 是 ", ui.selected == 0) | reflect(*yesBox);
        auto no     = btn(" 否 ", ui.selected == 1) | reflect(*noBox);
        hit(kHitBoolYes, 0, yesBox);
        hit(kHitBoolNo, 0, noBox);
        Elements row;
        row.push_back(yes);
        row.push_back(text(" "));
        row.push_back(no);
        if (rememberable) {
            auto remBox = mkBox();
            auto rem = btn(ui.remember ? " 记住✓ " : " 记住 ", ui.remember) | reflect(*remBox);
            hit(kHitRemember, 0, remBox);
            row.push_back(text("  "));
            row.push_back(rem);
        }
        row.push_back(text("  "));
        row.push_back(confirmBtn);
        row.push_back(text("  "));
        row.push_back(cancelBtn);
        control = hbox(std::move(row));
    } else if (id.inputType == "int" || id.inputType == "double") {
        auto minusBox = mkBox();
        auto plusBox  = mkBox();
        auto editBox  = mkBox();
        auto minus    = btn(" - ", false) | reflect(*minusBox);
        auto plus     = btn(" + ", false) | reflect(*plusBox);
        hit(kHitNumMinus, 0, minusBox);
        hit(kHitNumPlus, 0, plusBox);
        hit(kHitEdit, 0, editBox);
        control = hbox({
            minus,
            text(" "),
            text(" " + ui.editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox) | xflex_shrink,
            text(" "),
            plus,
            text("  "),
            confirmBtn,
            text("  "),
            cancelBtn,
        });
    } else if (id.inputType == "enum") {
        // 枚举项竖直列表 (选中项高亮)
        enumBoxes_.resize(id.inputEnums.size());
        Elements items;
        for (size_t i = 0; i < id.inputEnums.size(); ++i) {
            if (!enumBoxes_[i]) {
                enumBoxes_[i] = mkBox();
            }
            auto entry = text(fmt::format(
                " {} {}",
                (static_cast<int>(i) == ui.selected) ? "▸" : " ",
                id.inputEnums[i]
            ));
            if (static_cast<int>(i) == ui.selected) {
                entry = entry | bgcolor(theme.buttonActiveBgColor)
                        | color(theme.buttonActiveTextColor) | bold;
            } else {
                entry = entry | color(theme.buttonTextColor);
            }
            entry = entry | reflect(*enumBoxes_[i]);
            hit(kHitEnumItem, static_cast<int>(i), enumBoxes_[i]);
            items.push_back(entry);
        }
        // 底部操作行: 确认 + 取消
        control = vbox({
            vbox(std::move(items)),
            text(" "),
            hbox({
                confirmBtn,
                text("  "),
                cancelBtn,
            }),
        });
    } else { // string
        auto editBox = mkBox();
        hit(kHitEdit, 0, editBox);
        control = hbox({
            text(" " + ui.editText + " ") | bgcolor(theme.inputBgColor)
                | color(theme.inputTextColor) | reflect(*editBox) | xflex_shrink,
            text("  "),
            confirmBtn,
            text("  "),
            cancelBtn,
        });
    }
    return control;
}

// ---------------------------------------------------------------------------
// 中断 UI 状态表 (UI 线程独占; key = (interruptId, inputIndex))
// ---------------------------------------------------------------------------

void MessageListComponent::attachInterruptChannel(
    int64_t                                 wireId,
    std::shared_ptr<InterruptResultChannel> ch,
    bool                                    rememberable
) {
    interruptChannels_[wireId] = InterruptChannelInfo{std::move(ch), rememberable};
}

void MessageListComponent::releaseInterruptChannel(int64_t wireId) {
    interruptChannels_.erase(wireId);
    for (auto it = interruptUi_.begin(); it != interruptUi_.end();) {
        if (it->first.id == wireId) {
            it = interruptUi_.erase(it);
        } else {
            ++it;
        }
    }
}

void MessageListComponent::clearInterruptUiState() {
    interruptUi_.clear();
    interruptChannels_.clear();
}

bool MessageListComponent::interruptKeyOf(const TUIMessage& msg, InterruptKey& out) {
    if (msg.role != TUIMessage::Role::Interrupt || !msg.interrupt) {
        return false;
    }
    out.id    = msg.interrupt->interruptId;
    out.index = msg.interrupt->inputIndex;
    return true;
}

MessageListComponent::InterruptUIState& MessageListComponent::uiStateFor(const TUIMessage& msg) {
    InterruptKey key;
    if (!interruptKeyOf(msg, key)) {
        // 非中断消息不应请求 UI 状态; 返回静态兜底条目 (调用方按角色分支, 不会走到)
        static InterruptUIState fallback;
        return fallback;
    }
    auto [it, inserted] = interruptUi_.try_emplace(key);
    if (inserted) {
        // 惰性初始化: 编辑文本 = 默认值 (数值无默认时 "0"/"0.0"), string 无默认为空;
        // 选中项 = bool 按默认值 (false/no/n → "否"), enum 按默认值匹配 (无匹配首项)
        const auto& id = *msg.interrupt;
        auto&       ui = it->second;
        ui.editText    = id.inputDefault;
        if ((id.inputType == "int" || id.inputType == "double") && id.inputDefault.empty()) {
            ui.editText = (id.inputType == "double") ? "0.0" : "0";
        }
        if (id.inputType == "bool") {
            std::string def = id.inputDefault;
            agentxx::util::toLowerSelf(def);
            ui.selected = (def == "false" || def == "no" || def == "n") ? 1 : 0;
        } else if (id.inputType == "enum") {
            for (size_t i = 0; i < id.inputEnums.size(); ++i) {
                if (id.inputEnums[i] == id.inputDefault) {
                    ui.selected = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    return it->second;
}

MessageListComponent::InterruptUIState&
    MessageListComponent::mutateInterruptUiState(const TUIMessage& msg) {
    auto& ui = uiStateFor(msg);
    ++ui.version; // 驱动 itemKey 变化 → 懒列表缓存失效 (高度/滚动重估)
    return ui;
}

MessageListComponent::InterruptUIState MessageListComponent::interruptUiState(size_t msgIndex
) const {
    const auto& st = *ctx_.frameState;
    if (msgIndex >= st.messages.size()) {
        return {};
    }
    const auto&  msg = *st.messages[msgIndex];
    InterruptKey key;
    if (!interruptKeyOf(msg, key)) {
        return {};
    }
    auto it = interruptUi_.find(key);
    return it == interruptUi_.end() ? InterruptUIState{} : it->second;
}

// ---------------------------------------------------------------------------
// 中断输入消息交互
// ---------------------------------------------------------------------------

bool MessageListComponent::handleInterruptClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (areaBox_.x_max < areaBox_.x_min) {
        return false; // 尚未布局
    }
    if (mouse.x < areaBox_.x_min || mouse.x > areaBox_.x_max) {
        return false;
    }
    // 命中检测: 后渲染的控件优先 (enum 项与底部按钮同帧注册, 无重叠; 直接顺序查找)
    for (const auto& h : interruptHits_) {
        if (!h.box) {
            continue;
        }
        const auto& box = *h.box;
        if (mouse.y < box.y_min || mouse.y > box.y_max || mouse.x < box.x_min
            || mouse.x > box.x_max) {
            continue;
        }
        switch (h.kind) {
            case kHitBoolYes:
                // 点击是/否直接确认 (先设置选中, 再确认)
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex < st.messages.size()) {
                        auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                        ui.selected = 0;
                        ui.tip.clear();
                    }
                });
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitBoolNo:
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex < st.messages.size()) {
                        auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                        ui.selected = 1;
                        ui.tip.clear();
                    }
                });
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitNumMinus:
                setInterruptActive(h.msgIndex);
                stepInterrupt(h.msgIndex, -1.0);
                return true;
            case kHitNumPlus:
                setInterruptActive(h.msgIndex);
                stepInterrupt(h.msgIndex, 1.0);
                return true;
            case kHitEnumItem: {
                // 选中枚举项
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex >= st.messages.size()) {
                        return;
                    }
                    auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                    ui.selected = h.sub;
                    ui.tip.clear();
                });
                setInterruptActive(h.msgIndex);
                ctx_.postRedraw();
                return true;
            }
            case kHitRemember: {
                // 权限询问: 切换"记住本次选择"
                ctx_.state->mutate([&](TUIRenderState& st) {
                    if (h.msgIndex >= st.messages.size()) {
                        return;
                    }
                    auto& ui    = mutateInterruptUiState(*st.messages[h.msgIndex]);
                    ui.remember = !ui.remember;
                });
                setInterruptActive(h.msgIndex);
                ctx_.postRedraw();
                return true;
            }
            case kHitEdit:
                setInterruptActive(h.msgIndex);
                return true;
            case kHitConfirm:
                setInterruptActive(h.msgIndex);
                confirmInterrupt(h.msgIndex);
                return true;
            case kHitCancel:
                cancelInterrupt(h.msgIndex);
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool MessageListComponent::handleInterruptKey(Event event) {
    const size_t mi = activeInterruptMsg_;

    // 校验 active 消息仍存在且可交互
    std::string type;
    bool        valid = false;
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi < st.messages.size()) {
            const auto& m = *st.messages[mi];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                valid = true;
                type  = m.interrupt->inputType;
            }
        }
    });
    if (!valid) {
        activeInterruptMsg_ = static_cast<size_t>(-1);
        return false;
    }

    if (event == Event::Escape) {
        // 退出编辑态 (不取消中断)
        activeInterruptMsg_ = static_cast<size_t>(-1);
        ctx_.postRedraw();
        return true;
    }
    if (event == Event::Return) {
        confirmInterrupt(mi);
        return true;
    }
    if (type == "bool") {
        if (event == Event::ArrowLeft || event == Event::ArrowRight) {
            ctx_.state->mutate([&](TUIRenderState& st) {
                if (mi < st.messages.size()) {
                    auto& ui    = mutateInterruptUiState(*st.messages[mi]);
                    ui.selected = 1 - ui.selected;
                }
            });
            ctx_.postRedraw();
            return true;
        }
        return false;
    }
    if (type == "enum") {
        int delta = 0;
        if (event == Event::ArrowUp) {
            delta = -1;
        } else if (event == Event::ArrowDown) {
            delta = 1;
        }
        if (delta != 0) {
            ctx_.state->mutate([&](TUIRenderState& st) {
                if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
                    return;
                }
                auto& ui    = mutateInterruptUiState(*st.messages[mi]);
                int   size  = static_cast<int>(st.messages[mi]->interrupt->inputEnums.size());
                ui.selected = std::clamp(ui.selected + delta, 0, size - 1);
            });
            ctx_.postRedraw();
            return true;
        }
        return false;
    }
    if (type == "int" || type == "double") {
        if (event == Event::ArrowUp) {
            stepInterrupt(mi, 1.0);
            return true;
        }
        if (event == Event::ArrowDown) {
            stepInterrupt(mi, -1.0);
            return true;
        }
    }
    // 数值/string: 文本编辑
    if (event.is_character() || event == Event::Backspace || event == Event::Delete
        || event == Event::ArrowLeft || event == Event::ArrowRight) {
        ctx_.state->mutate([&](TUIRenderState& st) {
            if (mi >= st.messages.size()) {
                return;
            }
            auto& ui = mutateInterruptUiState(*st.messages[mi]);
            if (event.is_character()) {
                if (!ui.edited) {
                    // 首次输入替换默认值 (与输入框激活时保留默认值的语义一致)
                    ui.editText.clear();
                    ui.edited = true;
                }
                ui.editText += event.character();
            } else if (event == Event::Backspace && !ui.editText.empty()) {
                ui.editText.pop_back();
                ui.edited = true;
            } else if (event == Event::Delete && !ui.editText.empty()) {
                // 简化: 与 Backspace 同义 (单行输入无光标定位)
                ui.editText.pop_back();
                ui.edited = true;
            }
            ui.tip.clear();
        });
        ctx_.postRedraw();
        return true;
    }
    return false;
}

void MessageListComponent::setInterruptActive(size_t mi) {
    activeInterruptMsg_ = mi;
    // 数值/string: 激活时确保 UI 状态已初始化 (编辑文本 = 默认值);
    // 首次编辑前 edited=false, 编辑后不再覆盖
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size()) {
            return;
        }
        uiStateFor(*st.messages[mi]); // 惰性创建 (初始化默认编辑文本/选中项)
    });
    ctx_.postRedraw();
}

void MessageListComponent::confirmInterrupt(size_t mi) {
    std::string value;
    bool        confirmed = false;
    bool remember = false; // 权限询问: 是否记住本次选择 (mutate 内收集, 锁外发送)
    InterruptKey key;      // 确认成功时记录 (mutate 内收集, 锁外发送结果)
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        const auto& src = *st.messages[mi];
        if (src.role != TUIMessage::Role::Interrupt
            || src.interrupt->interruptStatus != TUIMessage::InterruptStatus::Waiting) {
            return;
        }
        const std::string& type = src.interrupt->inputType;
        auto&              ui   = uiStateFor(src);
        if (type == "bool") {
            value     = (ui.selected == 0) ? "true" : "false";
            confirmed = true;
        } else if (type == "enum") {
            if (ui.selected >= 0
                && ui.selected < static_cast<int>(src.interrupt->inputEnums.size())) {
                value     = src.interrupt->inputEnums[static_cast<size_t>(ui.selected)];
                confirmed = true;
            }
        } else if (type == "int" || type == "double") {
            std::string errTip;
            if (type == "int") {
                int64_t num = 0;
                auto    r   = agentxx::util::parseNumberFromString(ui.editText, num);
                if (r.ec != std::errc{}) {
                    errTip = "Invalid integer, please input again.";
                }
            } else {
                double num = 0.0;
                auto   r   = agentxx::util::parseNumberFromString(ui.editText, num);
                if (r.ec != std::errc{}) {
                    errTip = "Invalid number, please input again.";
                }
            }
            if (!errTip.empty()) {
                ui.tip = std::move(errTip);
                ctx_.postRedraw();
                return;
            }
            value     = ui.editText;
            confirmed = true;
        } else { // string
            value     = ui.editText;
            confirmed = true;
        }
        if (confirmed) {
            // 先取 key (src 引用随后被 mutableMessage 替换失效)
            interruptKeyOf(src, key);
            remember = ui.remember; // 权限询问"记住本次选择"标记
            // 确认结果写入消息 (跨线程共享的展示状态); UI 状态表保留编辑残留
            auto& mm                      = ctx_.state->mutableMessage(st, mi);
            mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Confirmed;
            mm.interrupt->interruptResult = value;
        }
    });
    if (confirmed) {
        // 发送结果到 client 线程 (channel 线程安全):
        // 通道从 interruptChannels_ 取最新 (同请求共享, 经 attachInterruptChannel 注入)
        auto chIt = interruptChannels_.find(key.id);
        if (chIt != interruptChannels_.end() && chIt->second.ch) {
            chIt->second.ch->async_send(
                neograph_asio_error_code{},
                key.index,
                std::optional<std::string>(value),
                remember,
                [](neograph_asio_error_code) {}
            );
        }
        ctx_.postRedraw();
    }
}

void MessageListComponent::cancelInterrupt(size_t mi) {
    int64_t      id = 0;
    InterruptKey key; // mutate 内收集, 锁外发送整体取消
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        id = st.messages[mi]->interrupt->interruptId;
        interruptKeyOf(*st.messages[mi], key);
        // 标记同请求所有未操作消息为 Cancelled
        for (size_t i = 0; i < st.messages.size(); ++i) {
            const auto& m = *st.messages[i];
            if (m.role == TUIMessage::Role::Interrupt && m.interrupt
                && m.interrupt->interruptId == id
                && m.interrupt->interruptStatus == TUIMessage::InterruptStatus::Waiting) {
                auto& mm                      = ctx_.state->mutableMessage(st, i);
                mm.interrupt->interruptStatus = TUIMessage::InterruptStatus::Cancelled;
            }
        }
        // 清理同请求的 UI 状态 (消息已固定状态, 编辑残留不再需要)
        for (auto it = interruptUi_.begin(); it != interruptUi_.end();) {
            if (it->first.id == id) {
                it = interruptUi_.erase(it);
            } else {
                ++it;
            }
        }
    });
    // 整体取消: inputIndex = -1, value = nullopt (经同请求共享通道发送)
    auto chIt = interruptChannels_.find(id);
    if (chIt != interruptChannels_.end() && chIt->second.ch) {
        chIt->second.ch->async_send(
            neograph_asio_error_code{},
            -1,
            std::optional<std::string>(),
            false,
            [](neograph_asio_error_code) {}
        );
    }
    activeInterruptMsg_ = static_cast<size_t>(-1);
    ctx_.postRedraw();
}

void MessageListComponent::stepInterrupt(size_t mi, double delta) {
    ctx_.state->mutate([&](TUIRenderState& st) {
        if (mi >= st.messages.size() || !st.messages[mi]->interrupt) {
            return;
        }
        const auto& src = *st.messages[mi];
        if (src.role != TUIMessage::Role::Interrupt
            || src.interrupt->interruptStatus != TUIMessage::InterruptStatus::Waiting) {
            return;
        }
        const std::string& type = src.interrupt->inputType;
        if (type != "int" && type != "double") {
            return;
        }
        auto&  ui  = uiStateFor(src);
        double val = 0.0;
        if (type == "int") {
            int64_t num = 0;
            auto    r   = agentxx::util::parseNumberFromString(ui.editText, num);
            if (r.ec != std::errc{}) {
                return; // 编辑值非法时步进无效
            }
            val = static_cast<double>(num);
        } else {
            double num = 0.0;
            auto   r   = agentxx::util::parseNumberFromString(ui.editText, num);
            if (r.ec != std::errc{}) {
                return;
            }
            val = num;
        }
        val += delta;
        // 注意: src 为快照引用, 修改 UI 状态表不改变消息, 引用保持有效
        if (type == "int") {
            ui.editText = fmt::format("{}", static_cast<int64_t>(val));
        } else {
            ui.editText = formatStepDouble(val);
        }
        ui.edited = true;
        ui.tip.clear();
    });
    ctx_.postRedraw();
}
