#include "agentxx-client/io/tui/components/message_list.h"
#include "agentxx-client/io/tui/agent_tui.h" // formatDurationMilliseconds / oneLinePreview
#include "agentxx-client/io/tui/components/interrupt_view.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/framework/tui_settings.h"
#include "agentxx-client/io/tui/markdown_block.h"
#include "agentxx-client/io/tui/plugin_ui_items.h"
#include "agentxx-client/io/tui/text_layout.h"
#include "agentxx-client/io/tui/ui_items_render.h"
#include "agentxx/plugin/client_plugin_manager.h" // ClientToolDecor 完整定义 (头文件中仅前置声明)
#include "agentxx/util/exception.h"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include "markdown/dom_builder.hpp"
#include "markdown/parser.hpp"
#include "markdown/state_diagram.hpp"
#include "markdown/text_utils.hpp"
#include "utilxx/diff_util.h"
#include "utilxx_base/string_util.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace agentxx::client {

using namespace ftxui;

namespace {

/// 插件装饰 items 的基础缩进列数 (与历史渲染 "    " 一致)
constexpr int kDecorItemIndent = 4;

/// 查找工具调用装饰 (client 插件经 update_tool_decor 推送的语义层渲染声明;
/// 每帧快照 pluginRegistry, 无锁读取; 无插件管理器/无装饰返回空)
/// - TUI 不感知任何具体工具: 装饰内容 (显示名/摘要/items) 完全由插件定义
const agentxx::plugin::ClientToolDecor*
    findToolDecor(const TUIRenderState& st, std::string_view toolCallId) {
    if (!st.pluginRegistry || toolCallId.empty()) {
        return nullptr;
    }
    for (const auto& d : st.pluginRegistry->toolDecors) {
        if (d.toolCallId == toolCallId) {
            return &d;
        }
    }
    return nullptr;
}

/// 工具语义渲染统一入口 (UI 线程):
/// - decor / 预设模版由宿主直接计算 (纯宿主数据)
/// - 自定义 renderer 只读 [ClientToolRenderCache] 的语义快照; 未命中时提交一次
///   后台渲染请求, 本帧用通用回退; 结果写入缓存后 adapter 触发重绘, 下一帧
///   由缓存命中上屏 (不在 UI 线程执行插件回调, plugin.md 第 8.2 节)
agentxx::plugin::ClientToolRenderResult queryToolRender(
    const TUICtx&         ctx,
    const TUIRenderState& st,
    std::string_view      toolCallId,
    std::string_view      toolName,
    std::string_view      argsJson,
    std::string_view      resultText,
    bool                  isFinished,
    bool                  isError,
    int                   maxWidth
) {
    namespace plugin = agentxx::plugin;
    auto cache       = ctx.pluginManager ? ctx.pluginManager->toolRenderCache() : nullptr;
    auto res         = plugin::renderClientTool(
        st.pluginRegistry.get(),
        cache.get(),
        toolCallId,
        toolName,
        argsJson,
        resultText,
        isFinished,
        isError,
        maxWidth
    );
    if (!res.pendingRender || !ctx.pluginManager) {
        return res;
    }
    plugin::ClientToolRenderRequest req;
    req.toolCallId = std::string{toolCallId};
    req.toolName   = std::string{toolName};
    req.argsJson   = std::string{argsJson};
    req.resultText = std::string{resultText};
    req.isFinished = isFinished;
    req.isError    = isError;
    req.maxWidth   = maxWidth;
    // 同一输入特征的重复请求在缓存内去重; UI 线程与 client io 线程为同一线程时
    // 请求会内联执行完成, 此时直接使用刚写入的结果
    if (auto entry = ctx.pluginManager->requestToolRender(req); entry && entry->matched) {
        res.matched       = true;
        res.pendingRender = false;
        res.displayName   = entry->displayName;
        res.summary       = entry->summary;
        res.items         = entry->items;
    }
    return res;
}

// markdown 渲染与行数估算已抽取到共享实现 (message_list 与中断视图共用):
// 见 agentxx-client/io/tui/markdown_block.h (renderMarkdown / estimateMarkdownLines)

/// 将工具调用参数 JSON 缩进格式化 (2 空格) 便于展开阅读, 例如:
/// {
///   "path": "/a/b",
///   "line_offset": 0
/// }
/// 参数解析失败或非对象时回退返回原始文本 (截断/异常参数)
std::string formatToolArgs(std::string_view argsText) {
    return agentxx::util::catchError<std::string>(
        [&]() -> std::string {
            auto j = utilxx_base::Json::parse(argsText);
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
           || result.starts_with("[Interrupt]") || result.starts_with("[Permission");
}

/// 多模态附件大小文本 (文件顶部匿名命名空间内，供 buildMessageBlock 卡片使用)
/// - 提前声明：定义在文件尾部匿名命名空间（与落盘/打开辅助同处）
std::string attachmentSizeText(uint64_t bytes);
} // namespace

MessageListComponent::MessageListComponent(TUICtx& ctx) :
    // 成员初始化顺序与声明顺序一致: 中断通用视图 (渲染/估算/交互/结果组装;
    // 形态由中断 UI 描述数据决定) 先于 ctx_ (两者都只持有外部对象的引用)
    interruptView_(ctx),
    ctx_(ctx) {
    LazyScrollable::CacheBudget budget;
    // 渲染树内存放大: FTXUI text()/paragraph() 按 glyph/词拆对象, 实测渲染树
    // 为源文本 30~70 倍 (memprof 实测: 1MB 英文 -> text() 30.7MB, markdown
    // 全量渲染 173MB)。因此 sourceBytes 按"渲染树估算字节"(源 × 64 系数) 上报
    // (见 buildMessageItem), 使 maxBytes 直接约束真实驻留内存 —— 实测 100K/200K
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

    // 运行中 tool/think 头部加载动画 (动画等级 >= High 时替代 "+/-" 静态标识,
    // 与输入框前缀同款 braille 旋转点阵):
    // - requiredLevel=High: 等级不足时组件内部自动降级, 渲染分支同步回退静态标识
    // - isActive 查询本帧快照: 是否存在运行中条目 (无运行中条目时帧循环自然终止)
    // - 注册为子项: OnAnimation 经组件树转发至此, 帧循环才能持续推进
    SpinnerComponent::Config spinnerCfg;
    spinnerCfg.requiredLevel = AnimationLevel::High;
    spinnerCfg.isActive      = [this] {
        return ctx_.frameState && hasRunningToolOrThink();
    };
    runSpinner_ = std::make_shared<SpinnerComponent>(std::move(spinnerCfg));
    Add(runSpinner_);

    // 初始化加载提示的加载动画 (startupProgress 行首 "~" → 旋转点阵):
    // - 与 runSpinner_/输入框前缀同款 braille 帧, 门槛同为 High;
    //   等级不足时 buildBanner 回退静态 "~"
    // - isActive 仅在 Connecting 且存在启动进度文本时为真, 避免空转
    SpinnerComponent::Config startupCfg;
    startupCfg.requiredLevel = AnimationLevel::High;
    startupCfg.isActive      = [this] {
        return ctx_.frameState && ctx_.frameState->connState == ConnState::Connecting
               && !ctx_.frameState->startupProgress.empty();
    };
    // decorate 留空: buildBanner 按 theme.accentColor 在外部着色 (与原 "~" 一致)
    startupSpinner_ = std::make_shared<SpinnerComponent>(std::move(startupCfg));
    Add(startupSpinner_);
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
        // 流结束: 清除流式折叠的用户点击覆盖态 (已提交的 Think 消息由
        // msg.collapsed 管理), 下一次思考回到设置模式的默认展示。
        // 必须在此处重置: 无 token 时 itemCount 不调用 syncStream,
        // OnRender 是每帧必经路径
        streamThinkOverride_ = -1;
    }

    // 由上一帧 viewport 可见区域反推可折叠消息的鼠标命中区域。
    // 必须在 scrollable_->Render() 之前计算: 此时 visibleBoxes() 为上一帧数据,
    // 与用户当前看到的屏幕内容一致。
    // (注意: 本组件采用懒构建, OnRender 阶段不构建任何子项, 仅产出布局节点)
    collapsibleBoxes_.clear();
    collapsibleIndices_.clear();
    collapsibleIsStream_.clear();
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
            collapsibleIsStream_.push_back(0);
        }
        // 末尾正在输出的流式 Think 子项同样支持点击折叠/展开:
        // 流式区子项索引 >= msgs.size(), 登记其上一帧可见区域并以 isStream
        // 标记区分 (点击切换 streamThinkOverride_ 而非消息 collapsed)。
        // 非 Think 流式内容 (Assistant 正文) 不可折叠, 不登记。
        if (hasStreamingToken(*ctx_.frameState)
            && ctx_.frameState->currentTokenRole == TUIMessage::Role::Think) {
            for (size_t i = msgs.size(); i < vboxes.size(); ++i) {
                if (vboxes[i].IsEmpty()) {
                    continue;
                }
                collapsibleBoxes_.push_back(vboxes[i]);
                collapsibleIndices_.push_back(i);
                collapsibleIsStream_.push_back(1);
            }
        }
    }

    // 连接失败 banner 的 [重试] 按钮命中: banner (空状态) 不在本帧列表中时清空 ——
    // 按钮消失后不再占用那块区域。
    // 注意: 不能每帧无条件清空 —— banner 元素跨帧缓存时不重建, 若每帧清空则会
    // 丢失登记 (按钮变成点不动); 缓存期间命中项连同其 Box 由 reflect 每帧更新。
    // 判定口径与 itemCount/fillViewport 一致: "无消息且无流式内容" 才渲染 banner
    if (ctx_.frameState
        && !(ctx_.frameState->messages.empty() && !hasStreamingToken(*ctx_.frameState))) {
        bannerHits_.beginFrame();
    }

    // 中断控件命中区域: 由本帧 scrollable_->Render() 中构建可见 Interrupt 消息
    // 时填充 (InterruptView::build), 供下一帧点击命中检测
    interruptView_.beginFrame();

    // decor 按钮命中: 同中断控件命中区域生命期 (OnRender 清空 + 构建期填充)
    decorHits_.clear();

    // 附件卡片命中: 同 decorHits_ 生命期
    attachmentHits_.clear();

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
        if (handleRetryClick(mouse)) {
            return true;
        }
        if (handleDecorButtonClick(mouse)) {
            return true;
        }
        if (handleAttachmentClick(mouse)) {
            return true;
        }
        if (handleCollapsibleClick(mouse)) {
            return true;
        }
        // 中断输入项控件 (值按钮/枚举/数值步进/输入框/勾选项/确认/取消):
        // 语义完全由中断 UI 描述数据决定 (InterruptView 通用实现)
        if (interruptView_.handleClick(mouse, areaBox_)) {
            return true;
        }
        const bool handled = scrollable_->OnEvent(event);
        // 历史分页: 滚动接近窗口顶部时预取更早历史 (滚轮驱动滚动后才可能
        // 接近顶部; 点击等其余鼠标事件无需检查)
        if (mouse.button == Mouse::WheelUp || mouse.button == Mouse::WheelDown) {
            maybeRequestMoreHistory();
        }
        return handled;
    }
    // 键盘: 优先作用于当前激活的中断消息 (输入框编辑/选中切换/确认/取消)
    if (interruptView_.activeMsg() != static_cast<size_t>(-1)) {
        if (interruptView_.handleKey(event)) {
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
    for (size_t k = 0; k < collapsibleBoxes_.size() && k < collapsibleIndices_.size()
                       && k < collapsibleIsStream_.size();
         ++k) {
        if (mouse.y < collapsibleBoxes_[k].y_min || mouse.y > collapsibleBoxes_[k].y_max) {
            continue;
        }
        // 流式末尾 Think 子项: 切换流式折叠覆盖态。
        // 命中区域为上一帧数据, 若期间流已提交为正式消息 (索引落入当前
        // messages 范围), 回落按普通消息处理 (切换 msg.collapsed), 避免
        // 提交瞬间的点击被吞掉
        if (collapsibleIsStream_[k] != 0) {
            const size_t si = collapsibleIndices_[k];
            const bool   committedToMessage
                = ctx_.frameState && si < ctx_.frameState->messages.size();
            if (!committedToMessage) {
                toggleStreamThinkCollapsed();
                return true;
            }
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
// 流式末尾 Think 折叠/展开
// ---------------------------------------------------------------------------

bool MessageListComponent::streamThinkCollapsed() const {
    // 用户点击覆盖优先 (仅对当前流生效); 未点击时跟随全局末尾思考模式设置
    if (streamThinkOverride_ >= 0) {
        return streamThinkOverride_ == 0;
    }
    return TUISettings::instance().tailThinkingMode() == TailThinkingMode::SingleLine;
}

void MessageListComponent::toggleStreamThinkCollapsed() {
    streamThinkOverride_ = streamThinkCollapsed() ? 1 : 0;
    ctx_.postRedraw();
}

// ---------------------------------------------------------------------------
// LazyScrollable 回调
// ---------------------------------------------------------------------------

bool MessageListComponent::hasStreamingToken(const TUIRenderState& st) const {
    return st.isStreaming && st.currentToken && !st.currentToken->empty();
}

bool MessageListComponent::hasRunningToolOrThink() const {
    if (!ctx_.frameState) {
        return false;
    }
    const auto& st = *ctx_.frameState;
    // 流式 think 输出中 (流式区 "[Think]" 头部正在推进)
    if (hasStreamingToken(st) && st.currentTokenRole == TUIMessage::Role::Think) {
        return true;
    }
    // 存在未完成的 Tool 消息 (串行执行下即当前运行的工具; 全量扫描仅读标志位,
    // 每帧一次成本可忽略)
    for (const auto& m : st.messages) {
        if (m && m->role == TUIMessage::Role::Tool && m->tool && !m->tool->toolFinished) {
            return true;
        }
    }
    return false;
}

Element MessageListComponent::runningHeaderMark(bool expanded) const {
    // 动画等级 >= High: spinner 当前帧 (decorate 留空, 由调用方按角色着色)
    if (runSpinner_->animationEnabled()) {
        return runSpinner_->Render();
    }
    // 等级不足: 回退原静态 +/- 标识
    return text(expanded ? "-" : "+");
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
        // 动画等级切换 (High 阈值) 影响 banner 首字符形态 ("~" ↔ spinner 帧):
        // 计入 key 使缓存失效重建, 避免等级热切换后仍显示旧形态的缓存快照
        h = combine(h, (startupSpinner_ && startupSpinner_->animationEnabled()) ? 1ULL : 0ULL);
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
        h = combine(h, static_cast<uint64_t>(m.interrupt ? m.interrupt->interruptId : 0));
        h = combine(h, m.interrupt ? m.interrupt->interruptResult.size() : size_t{0});
        // 中断表单状态 (编辑文本/选中项/勾选项/提示) 变化经 version 递增反映到
        // key, 触发高度重估 (提示增删影响估算行数)
        if (m.role == TUIMessage::Role::Interrupt) {
            h = combine(h, interruptView_.stateVersion(m));
        }
        if (m.role == TUIMessage::Role::Think) {
            h = combine(h, static_cast<uint64_t>(TUISettings::instance().tailThinkingMode()));
            const bool isTailMsg = (index + 1 == st.messages.size() && !hasStreamingToken(st));
            h                    = combine(h, isTailMsg ? 1 : 0);
        }
        // Tool/Think 消息头部可能使用加载动画 (动画等级 >= High), 计入动画标志:
        // 运行期间热切换动画等级时使缓存失效重建 (静态 +/- ↔ spinner 点阵),
        // 避免缓存残留旧形态的渲染快照
        if (m.role == TUIMessage::Role::Tool || m.role == TUIMessage::Role::Think) {
            h = combine(h, runSpinner_->animationEnabled() ? 1ULL : 0ULL);
        }
        // 工具消息装饰版本: 装饰在消息创建后由插件异步推送 (tool_start 事件
        // 先建消息, 随后 update_tool_decor 到达), 消息指针不变 —— 版本号变化
        // 触发该块重建, 保证装饰内容及时上屏
        if (m.role == TUIMessage::Role::Tool) {
            if (const auto* d = findToolDecor(st, m.tool ? m.tool->toolCallId : std::string{})) {
                h = combine(h, d->version);
            }
            // 自定义 renderer 的语义结果异步写入缓存 (client io 线程): 版本号变化
            // 触发该块重建, 从"通用回退"切换为插件语义内容
            if (ctx_.pluginManager && m.tool) {
                const auto cache = ctx_.pluginManager->toolRenderCache();
                h                = combine(
                    h,
                    cache->version(agentxx::plugin::ClientToolRenderRequest::keyFor(
                        m.tool->toolCallId,
                        m.tool->toolName
                    ))
                );
            }
        }
        return h;
    }
    // ---- 流式区 ----
    if (!streamUseIncremental_) {
        // 降级路径: 单个 paragraph 项, 以 (指针, 长度, role) 作为 key 触发高度重估。
        // 折叠/展开状态计入 key: 点击切换时若 token 长度未变 (帧间无新 token),
        // 仅凭指针+长度无法使缓存失效, 渲染内容将不随点击更新
        uint64_t h = reinterpret_cast<uint64_t>(st.currentToken.get());
        h          = combine(h, st.currentToken ? st.currentToken->size() : 0);
        h          = combine(h, static_cast<uint64_t>(st.currentTokenRole));
        h          = combine(h, static_cast<uint64_t>(st.pendingTokenDurationMs));
        h          = combine(h, static_cast<uint64_t>(TUISettings::instance().tailThinkingMode()));
        h          = combine(h, streamThinkCollapsed() ? 1ULL : 0ULL);
        h          = combine(h, 0xDEAD0000ull);
        return h;
    }
    const size_t si = index - st.messages.size();
    // 头部项 (thinking 时长行): key 稳定, 帧间缓存;
    // 动画标志计入 key: 热切换动画等级时使缓存失效重建 (静态 "-" ↔ spinner 点阵)
    if (si < streamHeaderCount_) {
        uint64_t h = combine(streamGen_, 0xFACE0000ull);
        h          = combine(h, runSpinner_->animationEnabled() ? 1ULL : 0ULL);
        return h;
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
                return estimateLines(msg.text, width) + msg.attachments.size() + 1;
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
                const bool finished  = msg.tool && msg.tool->toolFinished;
                const bool isError   = finished && isToolResultError(msg.tool->toolResult);
                auto       renderRes = queryToolRender(
                    ctx_,
                    st,
                    msg.tool ? msg.tool->toolCallId : "",
                    msg.tool ? msg.tool->toolName : "",
                    msg.text,
                    msg.tool ? msg.tool->toolResult : "",
                    finished,
                    isError,
                    width
                );
                if (renderRes.matched) {
                    size_t decorLines = 1; // header 行
                    if (finished && isError) {
                        decorLines += estimateLines(msg.tool->toolResult, width);
                    } else if (!renderRes.items.empty()) {
                        // 装饰 items 行数走共享块渲染层 (与渲染同一套判定; 见
                        // ui_items_render.h): 内容块/按钮/diff/状态图逐项累加
                        UiRenderCtx rc;
                        rc.theme  = ctx_.theme;
                        rc.width  = width;
                        rc.indent = kDecorItemIndent;
                        for (const auto& it : renderRes.items) {
                            if (auto item = uiItemFromPluginJson(it)) {
                                decorLines += measureUiItem(*item, rc);
                            }
                        }
                    } else {
                        if (!msg.text.empty()) {
                            decorLines += estimateLines(formatToolArgs(msg.text), width);
                        }
                        if (finished) {
                            decorLines += estimateLines(msg.tool->toolResult, width);
                        }
                    }
                    return static_cast<int>(decorLines) + 1; // +1: 尾部空行
                }
                size_t lines = 1; // header
                if (!msg.text.empty()) {
                    // 与渲染一致: 参数按 JSON 缩进格式化后的行数估算
                    lines += estimateLines(formatToolArgs(msg.text), width);
                }
                lines += finished ? estimateLines(msg.tool->toolResult, width) : 1;
                return lines + 1; // +1: 尾部空行
            }
            case TUIMessage::Role::Interrupt:
                // 中断消息: 形态由消息携带的 UI 描述决定 (InterruptView 通用
                // 实现), 估算与渲染同一套项判定 —— 本处不再按询问类型分支。
                // 注意 enter 视口后仍会实测修正, 估算偏差不影响正确性
                return static_cast<int>(interruptView_.estimate(msg, width)) + 1; // +1: 尾部空行
        }
        return 2; // 未知角色兜底: 内容 1 行 + 空行
    }
    // ---- 流式区 ----
    if (!streamUseIncremental_) {
        if (st.currentTokenRole == TUIMessage::Role::Think && streamThinkCollapsed()) {
            // 单行折叠流式 thinking: 1 行 header + 1 行尾部空行
            return 2;
        }
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

void MessageListComponent::maybeRequestMoreHistory() {
    if (!ctx_.requestMoreHistory) {
        return; // 未装配历史分页钩子 (如测试环境)
    }
    if (!ctx_.frameState || !ctx_.frameState->hasMoreHistory()) {
        return; // 无更早历史 (全量同步 / 已加载到会话开头)
    }
    // 首帧布局前视口未测量 (高度/偏移未就绪), 不触发
    if (scrollable_->viewportHeight() <= 0) {
        return;
    }
    if (scrollable_->scrollOffset() > kHistoryPrefetchRows) {
        return; // 距窗口顶部尚远
    }
    // 执行中去重与边界校验在实现方内部完成 (historyLoading 原子判定)
    ctx_.requestMoreHistory();
}

LazyBuiltItem MessageListComponent::buildItem(size_t index) {
    const auto& st = *ctx_.frameState;
    if (st.messages.empty() && !hasStreamingToken(st)) {
        LazyBuiltItem out;
        out.element = buildBanner();
        // 启动进度行使用加载动画时不可缓存: 缓存命中的旧 Element 是静止帧快照,
        // 点阵不会随动画推进转动; 每帧重建仅此一条 banner, 成本可忽略。
        // 动画等级不足 (静态 "~") 时保持可缓存, 与原行为一致
        const bool bannerAnimating = startupSpinner_ && startupSpinner_->animationEnabled()
                                     && st.connState == ConnState::Connecting
                                     && !st.startupProgress.empty();
        out.cacheable = !bannerAnimating;
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

    // banner 命中表: 本次构建重新登记 (上一帧的 [重试] 按钮登记先清空,
    // 因此状态切到非 Failed 后该按钮立即失去命中区域)
    bannerHits_.beginFrame();

    // 连接状态行 (banner 下半部):
    // - Connecting: server-io 正在启动, 下方逐步显示当前正在执行的启动
    //   操作 (如"加载 MCP server: xxx"), 并提示输入将在连接完成后自动发送
    // - Failed:     连接失败提示 + 可点击的 [重试] 按钮 (命中经 bannerHits_ 登记,
    //               点击经 onRetryClick_ 回调重新发起连接)
    // - Connected:  启动完成提示 + 默认按键提示
    Element statusLine;
    switch (st.connState) {
        case ConnState::Connecting: {
            Elements els;
            els.push_back(text(tr("banner.connecting")) | color(theme.hintColor) | center);
            if (!st.startupProgress.empty()) {
                // 当前正在执行的启动步骤 (agent 线程逐步上报):
                // 动画等级 >= High 时行首 "~" 替换为 braille 旋转加载动画,
                // 等级不足时保持静态 "~" (与 runSpinner_/输入框前缀同门槛)
                if (startupSpinner_ && startupSpinner_->animationEnabled()) {
                    els.push_back(
                        hbox({
                            startupSpinner_->Render() | color(theme.accentColor),
                            text(" " + st.startupProgress) | color(theme.accentColor),
                        })
                        | center
                    );
                } else {
                    els.push_back(
                        text(fmt::format("~ {}", st.startupProgress)) | color(theme.accentColor)
                        | center
                    );
                }
            }
            statusLine = vbox(std::move(els));
            break;
        }
        case ConnState::Failed: {
            statusLine = hbox({
                filler(),
                text(tr("banner.failed")) | color(theme.errorColor),
                bannerHits_.add(
                    text(tr("banner.retry")) | bgcolor(theme.buttonBgColor)
                        | color(theme.buttonTextColor) | bold,
                    std::string{}
                ),
                filler(),
            });
            break;
        }
        default: // Connected: 启动完成提示 + 按键提示
            statusLine = vbox({
                text(tr("banner.connected")) | color(theme.hintColor) | center,
            });
            break;
    }

    return vbox({
        filler(),
        text(R"_(

 █████╗  ██████╗ ███████╗███╗   ██╗████████╗      ╔══╗     ╔══╗
██╔══██╗██╔════╝ ██╔════╝████╗  ██║╚══██╔══╝   ╔══╬══╬═════╬══╬══╗
███████║██║  ███╗█████╗  ██╔██╗ ██║   ██║    ╔═╬  ║++║     ║++║  ╬═╗
██╔══██║██║   ██║██╔══╝  ██║╚██╗██║   ██║    ╚═╬       \_/       ╬═╝
██║  ██║╚██████╔╝███████╗██║ ╚████║   ██║      ╚═══════   ═══════╝
╚═╝  ╚═╝ ╚═════╝ ╚══════╝╚═╝  ╚═══╝   ╚═╝         ╚══╝     ╚══╝   

)_") | bold | color(theme.accentColor)
            | center,
        statusLine,
        filler(),
    });
}

LazyBuiltItem MessageListComponent::buildMessageItem(const TUIMessage& msg, size_t index) {
    const int maxWidth = std::max(1, scrollable_->contentWidth());

    std::vector<std::unique_ptr<markdown::DomBuilder>> builders;
    const size_t                                       decorHitsBefore = decorHits_.size();
    const size_t interruptHitsBefore  = interruptView_.hitBoxes().size();
    const size_t attachmentHitsBefore = attachmentHits_.size();
    auto         block                = buildMessageBlock(msg, index, maxWidth, builders);
    const bool   hasDecorHits         = (decorHits_.size() > decorHitsBefore);
    const bool   hasInterruptHits     = (interruptView_.hitBoxes().size() > interruptHitsBefore);
    const bool   hasAttachmentHits    = (attachmentHits_.size() > attachmentHitsBefore);

    LazyBuiltItem out;
    out.element           = vbox({std::move(block), text("")});
    const size_t srcBytes = msg.text.size() + (msg.tool ? msg.tool->toolResult.size() : 0)
                            + (msg.tool ? msg.tool->toolName.size() : 0);
    // 渲染树内存估算: FTXUI 渲染树为源文本 ~30-70 倍 (text() 按 glyph 拆
    // std::string, paragraph() 按词拆元素), 按 64 系数折算上报, 使
    // LazyScrollable 的字节预算 (maxBytes) 约束真实驻留内存而非源文本字节
    // (见构造函数预算注释; 系数取实测范围上沿, 宁紧勿松)
    out.sourceBytes = srcBytes * 64;
    // 中断消息不缓存: 每帧重建以刷新控件 reflect 命中区域 (InterruptView),
    // 否则缓存命中时控件 Box 丢失, 点击无法命中; 中断消息数量少, 成本可忽略
    // 带有可点击 decor 按钮的工具消息同样不缓存: 每帧重建以刷新 decorHits_,
    // 避免缓存命中时 reflect 持有的 Box 随 decorHits_.clear() 被释放导致 UAF,
    // 且保证每帧点击命中区域有效
    // 运行中的 Tool 消息在头部使用加载动画时同样不缓存: 缓存命中的旧 Element
    // 是静止帧快照, 点阵不会随动画推进转动; 每帧重建仅此一条消息, 成本可忽略
    // (动画等级不足时保持可缓存, 与原行为一致)
    const bool runToolAnimating = msg.role == TUIMessage::Role::Tool && msg.tool
                                  && !msg.tool->toolFinished && runSpinner_->animationEnabled();
    out.cacheable = (msg.role != TUIMessage::Role::Interrupt) && !hasInterruptHits
                    && !runToolAnimating && !hasDecorHits && !hasAttachmentHits;
    // markdown DomBuilder 生命周期与 Element 绑定
    // (Element 内 reflect 的链接 Box 指向 builder 内部容器)
    for (auto& b : builders) {
        out.attachments.push_back(std::move(b));
    }
    // 控件 Box 的生命周期与 Element 绑定:
    // 即使 decorHits_ / 中断命中区域在下一帧被清空, Reflect 所引用的 Box
    // 也由 Element 的 attachments 持有而不会被提前析构, 杜绝 UAF 悬空指针
    for (size_t i = decorHitsBefore; i < decorHits_.size(); ++i) {
        if (decorHits_[i].box) {
            out.attachments.push_back(decorHits_[i].box);
        }
    }
    for (size_t i = interruptHitsBefore; i < interruptView_.hitBoxes().size(); ++i) {
        if (interruptView_.hitBoxes()[i].box) {
            out.attachments.push_back(interruptView_.hitBoxes()[i].box);
        }
    }
    for (size_t i = attachmentHitsBefore; i < attachmentHits_.size(); ++i) {
        if (attachmentHits_[i].box) {
            out.attachments.push_back(attachmentHits_[i].box);
        }
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
        // 流式输出期间不显示耗时: think 耗时在输出完成时才由 agent 端结算
        // (空文本 ThinkToken 结算包回填到已提交消息), pendingTokenDurationMs
        // 仅在完成瞬间短暂非零, 平时恒为 0 → 不渲染耗时文本。
        // 不回退读取上一条同角色消息的时长 (那是旧数据, 与当前流无关)
        const int64_t durationMs = st.pendingTokenDurationMs;

        // 折叠态 (SingleLine 设置或用户点击折叠): 单行 header + 末尾截取预览;
        // 展开态: "- " + 角色标签 header + 全文多行渲染 (标签随界面语言切换)。
        // 两种形态均支持点击切换 (见 OnRender 流式区命中登记)
        // 头部前缀 = 1 列折叠标记/加载动画 + 角色标签 (标签值自带首尾空格)
        const std::string_view roleLabel = tr("msg.roleThink");
        if (streamThinkCollapsed()) {
            Elements header;
            // 流式输出中的 think 恒为运行态: 动画等级 >= High 时用加载动画替代 "+"
            header.push_back(runningHeaderMark(false) | color(theme.thinkingColor));
            header.push_back(text(roleLabel) | color(theme.thinkingColor));
            // 前缀显示列数 (按实际标签宽度计, 宽字符占 2 列): 供预览宽度预算使用
            int prefixCols = 1 + static_cast<int>(markdown::utf8_display_width(roleLabel));
            if (durationMs > 0) {
                const std::string durationText
                    = utilxx_base::formatDurationMilliseconds(durationMs);
                header.push_back(text(durationText) | color(theme.thinkingColor));
                header.push_back(text(" "));
                prefixCols += static_cast<int>(markdown::utf8_display_width(durationText)) + 1;
            }
            if (st.currentToken && !st.currentToken->empty()) {
                // 预览自适应宽度: 内容区剩余列数与用户设置截取长度取较小值
                const int budget
                    = collapsedPreviewBudget(std::max(1, scrollable_->contentWidth()), prefixCols);
                const size_t previewLen = budget > 0 ? static_cast<size_t>(budget) : prefixCols;
                header.push_back(
                    text(tailLinePreview(lastNonBlankLine(*st.currentToken), previewLen))
                    | color(theme.thinkingColor) | theme.dim() | xflex_shrink
                );
            } else if (st.pendingTokenThink && st.pendingTokenThink->reasoningTokens > 0) {
                header.push_back(
                    text(trf("think.encryptedTokens", st.pendingTokenThink->reasoningTokens))
                    | color(theme.thinkingColor) | theme.dim() | xflex_shrink
                );
            } else if (st.pendingTokenThink && st.pendingTokenThink->isEncrypted) {
                header.push_back(
                    text(tr("think.encrypted")) | color(theme.thinkingColor) | theme.dim()
                    | xflex_shrink
                );
            }
            block = hbox(std::move(header));
        } else {
            Elements lines;
            Elements header;
            // 流式输出中的 think 恒为运行态: 动画等级 >= High 时用加载动画替代 "-"
            header.push_back(runningHeaderMark(true) | color(theme.thinkingColor));
            header.push_back(text(roleLabel) | color(theme.thinkingColor));
            if (durationMs > 0) {
                header.push_back(
                    text(utilxx_base::formatDurationMilliseconds(durationMs))
                    | color(theme.thinkingColor)
                );
                header.push_back(text(" "));
            }
            lines.push_back(hbox(std::move(header)));
            lines.push_back(paragraph(*st.currentToken) | color(theme.thinkingColor));
            block = vbox(std::move(lines));
        }
    } else {
        block = paragraph(*st.currentToken) | color(theme.normalColor);
    }
    out.element = std::move(block);
    return out;
}

LazyBuiltItem MessageListComponent::buildStreamingHeader(const TUIRenderState& st) {
    // thinking 头部项: "[Think] <时长>" 单行 (角色标签随界面语言切换)
    const auto& theme = *ctx_.theme;
    Elements    header;
    // 流式输出中的 think 恒为运行态: 动画等级 >= High 时用加载动画替代 "-"。
    // 此时该项不可缓存 —— 缓存命中的旧 Element 是静止帧快照, 点阵不会转动;
    // 不可缓存项由 LazyScrollable 每帧重建, spinner 帧随动画推进刷新
    const bool animMark = runSpinner_->animationEnabled();
    header.push_back((animMark ? runningHeaderMark(true) : text("-")) | color(theme.thinkingColor));
    header.push_back(text(tr("msg.roleThink")) | color(theme.thinkingColor));
    // 流式输出期间不显示耗时 (同 buildStreamingItem): think 耗时在输出完成时
    // 才由 agent 端结算回填到已提交消息; pendingTokenDurationMs 平时恒为 0。
    // 不回退读取上一条同角色消息的时长 (旧数据与当前流无关)
    if (st.pendingTokenDurationMs > 0) {
        header.push_back(
            text(utilxx_base::formatDurationMilliseconds(st.pendingTokenDurationMs) + " ")
            | color(theme.thinkingColor)
        );
    }
    LazyBuiltItem out;
    out.element = hbox(std::move(header));
    // 使用加载动画时不可缓存 (每帧重建刷新点阵帧); 静态标识时保持可缓存
    out.cacheable   = !animMark;
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
        // 流结束: 清除流式折叠的用户点击覆盖态 (已提交的 Think 消息由
        // msg.collapsed 管理), 下一次思考回到设置模式的默认展示
        streamThinkOverride_ = -1;
        return;
    }

    // 末尾思考折叠展示时 (SingleLine 设置或用户点击折叠): 不启用增量多行渲染,
    // 走 buildStreamingItem 单行折叠子项; 点击展开后恢复增量路径 —— 折叠期间
    // 不 feed 渲染器、不更新 fedLen/epoch, 恢复后按 fedLen 补齐缺失增量即可
    // (与下方动画降级路径的恢复语义一致)
    if (st.currentTokenRole == TUIMessage::Role::Think && streamThinkCollapsed()) {
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
        // 新流开始: 清除上一流的用户点击折叠覆盖态,
        // 使新一轮思考回到设置模式的默认展示
        streamThinkOverride_ = -1;
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
// 消息块构建
// ---------------------------------------------------------------------------

Element MessageListComponent::buildMessageBlock(
    const TUIMessage&                                   msg,
    size_t                                              msgIndex,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    const auto& theme = *ctx_.theme;

    switch (msg.role) {
        case TUIMessage::Role::User: {
            // 内容超宽时 xflex_shrink 使段落吸收剩余宽度换行/裁剪,
            // 避免 hbox 按比例压缩前缀 "> " (见 ftxui box_helper::ComputeShrinkHard)
            Elements userElements;
            // 多模态附件卡片 (仅展示元信息, 不展示 Base64 数据;
            // 点击在文件管理器中定位显示对应文件)
            for (size_t ai = 0; ai < msg.attachments.size(); ++ai) {
                const auto&      att = msg.attachments[ai];
                AttachmentHitBox hit;
                hit.msgIndex = msgIndex;
                hit.attIndex = ai;
                hit.box      = std::make_shared<Box>();
                attachmentHits_.push_back(std::move(hit));
                auto       boxPtr = attachmentHits_.back().box;
                const auto iconKey
                    = att.type == agentxx::agent::MediaType::Image   ? "msg.attachImage"
                      : att.type == agentxx::agent::MediaType::Audio ? "msg.attachAudio"
                                                                     : "msg.attachVideo";
                auto cardRow = hbox({
                                   text(fmt::format(
                                       "|- {}: {} ",
                                       TuiI18n::instance().t(iconKey),
                                       att.displayName
                                   )) | color(theme.accentColor)
                                       | bold,
                                   text(attachmentSizeText(att.sizeBytes)) | theme.dim(),
                                   text(std::string(TuiI18n::instance().t("msg.attachOpen")))
                                       | color(theme.accentColor),
                               })
                               | reflect(*boxPtr);
                userElements.push_back(cardRow);
            }
            userElements.push_back(hbox({
                text("> ") | color(theme.userColor),
                paragraph(msg.text) | color(theme.userColor) | xflex_shrink,
            }));
            return vbox(std::move(userElements));
        }
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
            // 角色标签 "[System] " 随界面语言切换 (见 tui_i18n.h)
            const std::string_view roleLabel = tr("msg.roleSystem");
            const bool             expanded  = !msg.collapsed;
            Elements               lines;
            Elements               header;
            header.push_back(text(expanded ? "-" : "+") | color(tipColor));
            header.push_back(text(roleLabel) | color(tipColor));
            if (!expanded) {
                // 预览自适应宽度: 按内容区剩余列数截断 (宽字符按 2 列计),
                // 不再固定 60 字符; 超出部分仍由 xflex_shrink 右缘裁剪兜底
                const int prefixCols
                    = 1 + static_cast<int>(markdown::utf8_display_width(roleLabel));
                const int budget = collapsedPreviewBudget(maxWidth, prefixCols);
                header.push_back(
                    text(oneLinePreview(msg.text, static_cast<size_t>(budget))) | color(tipColor)
                    | theme.dim() | xflex_shrink
                );
            }
            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                lines.push_back(paragraph(msg.text) | color(tipColor));
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Tip: {
            // 按提示级别区分颜色与级别标签 (Info/Warning/Error; 标签随界面语言切换)
            ftxui::Color     tipColor = theme.hintColor;
            std::string_view levelKey = "";
            const auto       tipLevel = msg.tip ? msg.tip->tipLevel : TUIMessage::TipLevel::Info;
            switch (tipLevel) {
                case TUIMessage::TipLevel::Warning:
                    tipColor = theme.thinkingColor;
                    levelKey = "msg.tipLevelWarn";
                    break;
                case TUIMessage::TipLevel::Error:
                    tipColor = theme.errorColor;
                    levelKey = "msg.tipLevelError";
                    break;
                case TUIMessage::TipLevel::Info:
                    // 不增加提示
                    // levelKey = "msg.tipLevelInfo";
                    break;
            }
            // 头部前缀 "[Tip] # <级别>" (例: "+ [Tip] # Warn"), 级别文本随语言切换;
            // 折叠态在其后追加 " · " 与单行预览
            const std::string prefix = trf("msg.tipPrefix", tr(levelKey));
            // 可折叠: header 行带 +/- 折叠标记与单行预览 (折叠态), 展开态显示全文
            // (与 Think 消息同一折叠模式; 默认折叠见创建处 makeText / onDelta)
            const bool expanded = !msg.collapsed;
            Elements   lines;
            Elements   header;
            header.push_back(text(expanded ? "-" : "+") | color(tipColor));
            header.push_back(text(prefix) | color(tipColor));
            if (!expanded) {
                // 同 System: 预览自适应内容区剩余列宽, 超宽时右缘裁剪兜底
                header.push_back(text(" · ") | color(tipColor));
                const int prefixCols
                    = 1 + static_cast<int>(markdown::utf8_display_width(prefix)) + 3; // " · "
                const int budget = collapsedPreviewBudget(maxWidth, prefixCols);
                header.push_back(
                    text(oneLinePreview(msg.text, static_cast<size_t>(budget))) | color(tipColor)
                    | xflex_shrink
                );
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
            // 时长文本先计算, 供头部渲染与预览列宽预算共用
            std::string durationText;
            if (msg.durationMs > 0) {
                durationText = utilxx_base::formatDurationMilliseconds(msg.durationMs);
            }
            header.push_back(text(expanded ? "-" : "+") | color(theme.thinkingColor));
            header.push_back(text(tr("msg.roleThink")) | color(theme.thinkingColor));
            if (!durationText.empty()) {
                header.push_back(text(durationText) | color(theme.thinkingColor));
                header.push_back(text(" "));
            }
            if (!expanded) {
                // 预览自适应宽度: 前缀列数 = 折叠标记 (1 列) + 角色标签 + 时长 + 空格
                // (标签随语言切换, 按实际显示宽度计, 不按英文宽度写死)
                int prefixCols
                    = 1 + static_cast<int>(markdown::utf8_display_width(tr("msg.roleThink")));
                if (!durationText.empty()) {
                    prefixCols += static_cast<int>(markdown::utf8_display_width(durationText)) + 1;
                }
                const int   budget = collapsedPreviewBudget(maxWidth, prefixCols);
                std::string previewText;
                if (!msg.text.empty()) {
                    const auto& st = *ctx_.frameState;
                    const bool  isTailMsg
                        = (msgIndex + 1 == st.messages.size() && !hasStreamingToken(st));
                    if (isTailMsg
                        && TUISettings::instance().tailThinkingMode()
                               == TailThinkingMode::SingleLine) {
                        previewText = tailLinePreview(
                            lastNonBlankLine(msg.text),
                            static_cast<size_t>(budget)
                        );
                    } else {
                        previewText = oneLinePreview(msg.text, static_cast<size_t>(budget));
                    }
                } else if (msg.think && msg.think->reasoningTokens > 0) {
                    previewText = trf("think.encryptedTokens", msg.think->reasoningTokens);
                } else if (msg.think && msg.think->isEncrypted) {
                    previewText = std::string(tr("think.encrypted"));
                }
                if (!previewText.empty()) {
                    header.push_back(
                        text(std::move(previewText)) | color(theme.thinkingColor) | theme.dim()
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
                        infoText = trf("think.encryptedTokens", msg.think->reasoningTokens);
                    } else if (msg.think && msg.think->isEncrypted) {
                        infoText = std::string(tr("think.encrypted"));
                    }
                    if (!infoText.empty()) {
                        lines.push_back(
                            text(std::move(infoText)) | color(theme.thinkingColor) | theme.dim()
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
            const bool expanded = !msg.collapsed;
            const bool finished = msg.tool && msg.tool->toolFinished;
            const bool isError  = finished && isToolResultError(msg.tool->toolResult);

            // 统一工具特化渲染查询 (包含动态 per-call decor 与按 tool_name 注册的渲染器/模版)
            auto renderRes = queryToolRender(
                ctx_,
                *ctx_.frameState,
                msg.tool->toolCallId,
                msg.tool->toolName,
                msg.text,
                msg.tool->toolResult,
                finished,
                isError,
                maxWidth
            );

            Elements lines;
            Elements header;
            {
                // 头部折叠标记: 运行中且动画等级 >= High 时用加载动画 (braille
                // 点阵, 同输入框前缀) 替代静态 +/-; 已完成/等级不足保持原标识。
                // spinner 与 +/- 均为 1 列宽, 后续角色标签文本与预览列宽预算不变
                if (!finished) {
                    header.push_back(runningHeaderMark(expanded) | color(theme.toolColor));
                } else {
                    header.push_back(text(expanded ? "-" : "+") | color(theme.toolColor));
                }
                header.push_back(text(tr("msg.roleTool")) | color(theme.toolColor));
            }

            // 显示名: renderRes 提供的 displayName 优先, 回退原始 toolName
            std::string displayName = (renderRes.matched && !renderRes.displayName.empty())
                                          ? renderRes.displayName
                                          : msg.tool->toolName;

            if (!expanded) {
                // 折叠状态, 特化渲染 (摘要内部预览按内容区剩余列宽自适应截断)
                const int nameCols = static_cast<int>(markdown::utf8_display_width(displayName));
                // 头部前缀列数 (折叠标记 1 列 + 角色标签; 标签随语言切换, 不按英文宽度写死)
                const int prefixCols
                    = 1 + static_cast<int>(markdown::utf8_display_width(tr("msg.roleTool")));
                std::string resOrArgsSummary;

                if (isError) {
                    // 执行失败: 保持特化 toolName, 后续内容显示为异常结果 (红色)
                    const int budget
                        = collapsedPreviewBudget(maxWidth, prefixCols + nameCols + 1); // " "
                    auto resPreview
                        = oneLinePreview(msg.tool->toolResult, static_cast<size_t>(budget));
                    if (!resPreview.empty()) {
                        resOrArgsSummary = " · " + std::move(resPreview);
                    }
                } else if (renderRes.matched && !renderRes.summary.empty()) {
                    if (renderRes.summary.starts_with(" ·")) {
                        resOrArgsSummary = renderRes.summary;
                    } else {
                        resOrArgsSummary = " · " + renderRes.summary;
                    }
                } else if (!finished) {
                    // 运行中无特化摘要回退
                    resOrArgsSummary = " ·";
                    if (!msg.text.empty()) {
                        const int budget = collapsedPreviewBudget(
                            maxWidth,
                            prefixCols + nameCols + 3
                        ); // " · "
                        resOrArgsSummary
                            += " " + oneLinePreview(msg.text, static_cast<size_t>(budget));
                    }
                } else {
                    // 已完成无特化摘要回退: 展示结果预览
                    const int budget
                        = collapsedPreviewBudget(maxWidth, prefixCols + nameCols + 1); // " "
                    auto resPreview
                        = oneLinePreview(msg.tool->toolResult, static_cast<size_t>(budget));
                    if (!resPreview.empty()) {
                        resOrArgsSummary = " " + std::move(resPreview);
                    }
                }

                // toolName: 运行中高亮
                if (!finished) {
                    header.push_back(
                        text(std::move(displayName)) | color(theme.accentColor) | bold
                    );
                } else {
                    header.push_back(
                        text(std::move(displayName)) | color(theme.toolColor) | theme.dim()
                    );
                }

                if (!resOrArgsSummary.empty()) {
                    if (isError) {
                        header.push_back(
                            text(std::move(resOrArgsSummary)) | color(theme.errorColor)
                            | theme.dim() | xflex_shrink
                        );
                    } else {
                        header.push_back(
                            text(std::move(resOrArgsSummary)) | color(theme.toolColor) | theme.dim()
                            | xflex_shrink
                        );
                    }
                }
            } else {
                // 展开头: 动态装饰显示名优先, 否则回退原始 toolName
                const auto& shownName
                    = (renderRes.matched && renderRes.isDecor && !renderRes.displayName.empty())
                          ? renderRes.displayName
                          : msg.tool->toolName;
                if (!finished) {
                    header.push_back(text(shownName) | color(theme.accentColor) | bold);
                } else {
                    header.push_back(text(shownName) | color(theme.toolColor));
                    if (msg.durationMs > 0) {
                        header.push_back(text(" "));
                        header.push_back(
                            text(utilxx_base::formatDurationMilliseconds(msg.durationMs))
                            | color(theme.toolColor) | theme.dim()
                        );
                    }
                }
            }

            lines.push_back(hbox(std::move(header)));
            if (expanded) {
                if (renderRes.matched && !renderRes.items.empty() && !(finished && isError)) {
                    // 插件装饰/特化工具体 (items 渲染; 失败时回退通用错误展示)
                    // decor 按钮 owner=tool_call_id (renderRes 携带归因);
                    // 非 decor (toolRenderer) 按钮 owner 同样为 tool_call_id
                    // (TUI 侧组装, 无需插件操心)
                    agentxx::plugin::ClientToolDecor decorForBody;
                    if (renderRes.isDecor) {
                        decorForBody.plugin      = renderRes.decorPlugin;
                        decorForBody.toolCallId  = renderRes.decorToolCallId.empty()
                                                       ? msg.tool->toolCallId
                                                       : renderRes.decorToolCallId;
                        decorForBody.displayName = renderRes.displayName;
                        decorForBody.summary     = renderRes.summary;
                        decorForBody.items       = renderRes.items;
                    } else {
                        decorForBody.toolCallId = msg.tool->toolCallId;
                        decorForBody.items      = renderRes.items;
                    }
                    appendDecorToolBody(decorForBody, lines, maxWidth, mdBuilders);
                } else {
                    if (!msg.text.empty()) {
                        // 参数 JSON 缩进格式化 (2 空格) 便于阅读; 解析失败回退原文
                        lines.push_back(hbox({
                            text(tr("tool.args")) | color(theme.toolColor),
                            paragraph(formatToolArgs(msg.text)) | color(theme.toolColor)
                                | xflex_shrink,
                        }));
                    }
                    if (finished) {
                        const bool isErr = isToolResultError(msg.tool->toolResult);
                        lines.push_back(hbox({
                            text(tr("tool.result")) | color(theme.toolColor),
                            paragraph(msg.tool->toolResult)
                                | color(isErr ? theme.errorColor : theme.toolColor) | xflex_shrink,
                        }));
                    } else {
                        lines.push_back(text(tr("tool.running")) | color(theme.toolColor));
                    }
                }
            }
            return vbox(std::move(lines));
        }
        case TUIMessage::Role::Interrupt:
            // 中断输入消息: 渲染/交互完全由消息携带的 UI 描述数据决定
            // (InterruptView 通用实现: 头行 + 描述块 (内容块共享块渲染层,
            // 控件块/提交行由中断视图实现)), 本组件不感知任何具体询问类型 (含权限)
            return interruptView_.build(msg, msgIndex, maxWidth, mdBuilders);
    }
    return text("");
}

// ---------------------------------------------------------------------------
// 差异对比渲染 (diff)
// ---------------------------------------------------------------------------

void MessageListComponent::appendEditToolBody(const TUIMessage& msg, Elements& lines) {
    const auto& theme = *ctx_.theme;
    // 操作失败: 渲染错误信息, 不渲染基于请求参数的 diff (避免误导: 文件实际未被修改)
    if (msg.tool && msg.tool->toolFinished && isToolResultError(msg.tool->toolResult)) {
        lines.push_back(hbox({
            text(tr("tool.result")) | color(theme.toolColor),
            paragraph(msg.tool->toolResult) | color(theme.errorColor) | xflex_shrink,
        }));
        return;
    }
    std::string path, oldStr, newStr;
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto args = utilxx_base::Json::parse(msg.text);
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
            text(tr("tool.file")) | color(theme.hintColor),
            text(path) | color(theme.toolColor) | xflex_shrink,
        }));
    }
    lines.push_back(renderEditToolDiff(oldStr, newStr));
}

Element MessageListComponent::renderEditToolDiff(std::string_view oldStr, std::string_view newStr) {
    // 历史遗留入口: 转调共享 helper (与 DiffOverlay/decor diff 同实现),
    // 保留供旧调用点兼容; 新代码直接用 renderPluginDiff
    const auto& theme = *ctx_.theme;
    return agentxx::client::renderPluginDiff({}, oldStr, newStr, theme);
}

// ---------------------------------------------------------------------------
// 插件工具消息装饰渲染 (通用机制, 无任何具体工具特化):
// 装饰内容 (displayName/summary/items) 由插件经 update_tool_decor 推送,
// items schema 见
// [client_plugin_api.h](/agent/lib/include/agentxx/plugin/api/client_plugin_api.h)
// (text/button/diagram/separator/diff); button 走通用 action_id 派发
// (owner=tool_call_id), 解析/配色走 plugin_ui_items 共享 helper
// ---------------------------------------------------------------------------

void MessageListComponent::appendDecorToolBody(
    const agentxx::plugin::ClientToolDecor&             decor,
    Elements&                                           lines,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    appendDecorItems(decor.items, decor.plugin, decor.toolCallId, lines, maxWidth, mdBuilders);
}

void MessageListComponent::appendDecorItems(
    const utilxx_base::Json&                            items,
    const std::string&                                  plugin,
    const std::string&                                  ownerId,
    Elements&                                           lines,
    int                                                 maxWidth,
    std::vector<std::unique_ptr<markdown::DomBuilder>>& mdBuilders
) {
    auto reg = ctx_.frameState && ctx_.frameState->pluginRegistry
                   ? ctx_.frameState->pluginRegistry.get()
                   : nullptr;

    // 插件装饰 items 与中断内容块**共用同一渲染实现** (见 ui_items_render.h):
    // 逐项归一化 → 行模型 (元素 + 行数 + 命中信息), 再把行元素追加到 lines,
    // 可点按钮的行转写为 decorHits_ 命中区域。
    // 高度估算侧 (estimateHeight) 用同一模块的 measureUiItem, 两侧判定同源。
    UiRenderCtx rc;
    rc.theme    = ctx_.theme;
    rc.width    = maxWidth;
    rc.indent   = kDecorItemIndent;
    rc.plugin   = plugin;
    rc.ownerId  = ownerId;
    rc.registry = reg;

    UiRenderResult out;
    for (const auto& it : items) {
        if (auto item = uiItemFromPluginJson(it)) {
            renderUiItem(*item, rc, out);
        }
    }
    // markdown 渲染器生命周期交回调用方 (随 LazyBuiltItem.attachments 与 Element 同存活)
    for (auto& builder : out.builders) {
        mdBuilders.push_back(std::move(builder));
    }
    for (auto& row : out.rows) {
        // 可点按钮命中: owner=tool_call_id (以 toolCallId 作 owner_id, fallback 覆盖)
        if (row.box && !row.hitId.empty()) {
            DecorHitBox hb;
            hb.plugin     = plugin;
            hb.ownerId    = row.hitOwner.empty() ? ownerId : row.hitOwner;
            hb.actionId   = std::move(row.hitId);
            hb.argsJson   = std::move(row.hitArgs);
            hb.generation = reg ? reg->generationOf(plugin) : 0;
            hb.box        = std::move(row.box);
            decorHits_.push_back(std::move(hb));
        }
        lines.push_back(std::move(row.element));
    }
}

ftxui::Box MessageListComponent::retryButtonBox() const {
    // 测试辅助: banner 命中表仅登记 [重试] 按钮一项
    for (const auto& entry : bannerHits_.entries()) {
        return *entry.box;
    }
    return agentxx::client::kNoBox;
}

bool MessageListComponent::handleRetryClick(const Mouse& mouse) {
    if (bannerHits_.findClick(mouse) == nullptr) {
        return false;
    }
    ctx_.postRedraw();
    if (onRetryClick_) {
        onRetryClick_();
    }
    return true;
}

bool MessageListComponent::handleDecorButtonClick(const Mouse& mouse) {
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    for (const auto& h : decorHits_) {
        if (!h.box) {
            continue;
        }
        const auto& box = *h.box;
        if (mouse.y < box.y_min || mouse.y > box.y_max || mouse.x < box.x_min
            || mouse.x > box.x_max) {
            continue;
        }
        if (auto mgr = ctx_.pluginManager) {
            mgr->dispatchAction(h.plugin, h.ownerId, h.actionId, h.argsJson, h.generation);
        }
        return true;
    }
    return false;
}

/// 多模态附件落盘/文件管理器定位辅助 (与 MessageListComponent 解耦, 便于测试)
namespace {

std::string attachmentSizeText(uint64_t bytes);

/// 将附件解析为本地可打开路径:
/// - 本地已存在文件直接返回
/// - 远端 dataUrl 则解码落盘到临时目录后返回 (失败返回空)

std::string resolveAttachmentLocalPath(const agentxx::agent::MediaAttachment& att) {
    std::error_code ec;
    if (!att.pathOrUrl.empty() && std::filesystem::is_regular_file(att.pathOrUrl, ec) && !ec) {
        return att.pathOrUrl;
    }
    if (!att.dataUrl.empty() && att.dataUrl.rfind("data:", 0) == 0) {
        const auto comma = att.dataUrl.find(',');
        if (comma == std::string::npos) {
            return "";
        }
        auto raw = utilxx_base::base64Decode(std::string_view{att.dataUrl}.substr(comma + 1));
        if (!raw.has_value()) {
            return "";
        }
        auto dir = std::filesystem::temp_directory_path(ec) / "agentxx-media";
        if (ec) {
            return "";
        }
        std::filesystem::create_directories(dir, ec);
        // 防目录穿越: 仅取文件名部分
        std::string name = att.displayName.empty() ? "attachment.bin" : att.displayName;
        name             = std::filesystem::path(name).filename().string();
        if (name.empty()) {
            name = "attachment.bin";
        }
        auto          dst = dir / name;
        std::ofstream ofs(dst, std::ios::binary);
        if (!ofs) {
            return "";
        }
        ofs.write(raw->data(), static_cast<std::streamsize>(raw->size()));
        if (!ofs) {
            return "";
        }
        return dst.string();
    }
    return att.pathOrUrl;
}

/// 在文件管理器中定位显示文件 (选中对应文件; detached 线程, 不阻塞 UI )
/// - Windows: explorer /select,<path> (选中文件)
///
/// - macOS: open -R <path> (在 Finder 中显示)
///
/// - Linux: 优先 xdg-open 所在目录 (文件管理器中显示目录),
///   回退 xdg-open 直接打开 (按桌面默认程序)
void revealPathInFileManager(std::string path) {
    if (path.empty()) {
        return;
    }
    std::thread([p = std::move(path)] {
    // 命令参数中的路径加双引号包裹, 防空格/特殊字符截断
    // (路径本身含双引号属极端情况, 此处不做转义处理)
#if XX_IS_WIN_D
        std::string cmd = "explorer /select,\"" + p + "\"";
        (void)std::system(cmd.c_str());
#elif XX_IS_MACOS_D
        std::string cmd = "open -R \"" + p + "\" >/dev/null 2>&1 &";
        (void)std::system(cmd.c_str());
#else
        // Linux 各桌面文件管理器选中文件的参数不统一 (nautilus/dolphin 等),
        // 统一用文件所在目录调起文件管理器保证可显示对应位置;
        // 目录不存在 (如临时落盘失败的远端残留路径) 时回退直接打开原路径
        std::error_code       ec;
        std::filesystem::path fp(p);
        std::filesystem::path dir   = fp.parent_path();
        const bool         hasDir   = !dir.empty() && std::filesystem::is_directory(dir, ec) && !ec;
        const std::string& showPath = hasDir ? dir.string() : p;
        std::string        cmd      = "xdg-open \"" + showPath + "\" >/dev/null 2>&1 &";
        (void)std::system(cmd.c_str());
#endif
    }).detach();
}

std::string attachmentSizeText(uint64_t bytes) {
    if (bytes == 0) {
        return "";
    }
    if (bytes < 1024 * 1024) {
        return fmt::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    }
    return fmt::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace

bool MessageListComponent::handleAttachmentClick(const Mouse& mouse) {
    // 仅支持鼠标点击打开 (无 Enter 等键盘绑定: OnEvent 键盘分支仅处理
    // 中断输入, 此处不消费任何键盘事件)
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Released) {
        return false;
    }
    if (!ctx_.frameState) {
        return false;
    }
    for (const auto& h : attachmentHits_) {
        if (!h.box) {
            continue;
        }
        const auto& box = *h.box;
        if (mouse.y < box.y_min || mouse.y > box.y_max || mouse.x < box.x_min
            || mouse.x > box.x_max) {
            continue;
        }
        const auto& msgs = ctx_.frameState->messages;
        if (h.msgIndex >= msgs.size()) {
            continue;
        }
        const auto& atts = msgs[h.msgIndex]->attachments;
        if (h.attIndex >= atts.size()) {
            continue;
        }
        revealPathInFileManager(resolveAttachmentLocalPath(atts[h.attIndex]));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 中断输入消息 (渲染/交互由 InterruptView 通用实现, 见 interrupt_view.{h,cpp}):
// 本组件仅转发通道注入与命中区域/状态查询, 并以 interruptView_ 作为
// buildMessageBlock / estimateHeight / itemKey / OnEvent 的实现
// ---------------------------------------------------------------------------

void MessageListComponent::attachInterruptChannel(
    int64_t                                 wireId,
    std::shared_ptr<InterruptResultChannel> ch
) {
    interruptView_.attachChannel(wireId, std::move(ch));
}

void MessageListComponent::releaseInterruptChannel(int64_t wireId) {
    interruptView_.releaseChannel(wireId);
}

void MessageListComponent::clearInterruptUiState() {
    interruptView_.clear();
}

size_t MessageListComponent::interruptEstimate(size_t msgIndex, int width) const {
    // 取实时快照 (测试辅助路径: 不依赖上一帧 frameState 的刷新时机)
    auto st = ctx_.state ? ctx_.state->readSnapshot() : ctx_.frameState;
    if (!st || msgIndex >= st->messages.size()) {
        return 1;
    }
    return interruptView_.estimate(*st->messages[msgIndex], width);
}

} // namespace agentxx::client