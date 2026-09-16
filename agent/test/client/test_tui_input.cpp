#include "test_tui_input.h"

#include "agentxx-client/io/tui/components/input_bar.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/framework/tui_state.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "ftxui/component/animation.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/screen/screen.hpp"
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_input_passed = 0;
int g_tui_input_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_input_passed
#define XX_TEST_FAILED g_tui_input_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

// ---------------------------------------------------------------------------
// 测试夹具: 构建最小 TUICtx + InputComponent, 直接驱动 OnEvent
// ---------------------------------------------------------------------------

namespace {

struct InputFixture {
    TUISharedState                               sharedState;
    TUITheme                                     theme       = TUITheme::darkTheme();
    int                                          redrawCount = 0;
    std::string                                  sentText;
    std::vector<agentxx::agent::MediaAttachment> sentAttachments;
    bool                                         sent = false;

    TUICtx ctx;

    InputFixture() {
        ctx.state      = &sharedState;
        ctx.frameState = sharedState.readSnapshot();
        ctx.postRedraw = [this] {
            ++redrawCount;
        };
        ctx.theme     = &theme;
        ctx.sessionId = "session";
        ctx.remoteUrl = "";
    }

    /// 创建组件; ComponentBase 不可移动, 使用 shared_ptr 持有
    std::shared_ptr<InputComponent> makeComponent() {
        InputComponent::Config cfg;
        cfg.onSend
            = [this](std::string text, std::vector<agentxx::agent::MediaAttachment> atts) -> bool {
            sentText        = std::move(text);
            sentAttachments = std::move(atts);
            sent            = true;
            return true;
        };
        return std::make_shared<InputComponent>(ctx, std::move(cfg));
    }

    /// 依次输入字符串的每个字符
    static void type(InputComponent& comp, const std::string& s) {
        for (char c : s) {
            comp.OnEvent(ftxui::Event::Character(c));
        }
    }

    /// 模拟一次括号粘贴: 终端启用 \x1B[?2004h 后发送 \x1B[200~ ... \x1B[201~,
    /// 内容中的换行以 \r 传输 (解析后为 Event::Return)
    static void paste(InputComponent& comp, const std::string& text) {
        comp.OnEvent(ftxui::Event::Special("\x1B[200~"));
        for (char c : text) {
            if (c == '\n') {
                comp.OnEvent(ftxui::Event::Return);
            } else {
                comp.OnEvent(ftxui::Event::Character(c));
            }
        }
        comp.OnEvent(ftxui::Event::Special("\x1B[201~"));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 测试用例
// ---------------------------------------------------------------------------

void test_multiline_paste_inserted_not_sent() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 括号粘贴 "line1\rline2" (终端以 \r 表示换行)
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "line1");
    comp->OnEvent(ftxui::Event::Return); // 粘贴的换行
    InputFixture::type(*comp, "line2");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    // 多行内容被整体插入, 换行不会触发发送
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("line1\nline2"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_crlf_dedup() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴内容中的空行 (\n\n) 必须原样保留。
    // 背景: FTXUI 解析层已把 \r 归一化为 \n, 事件层面无法区分
    // "CRLF 的第二个 \n" 与 "粘贴内容中真实的空行" —— 去重会吞掉空行
    // (如代码块中的空行), 代价大于个别 Windows 终端 (CRLF 粘贴) 多出的空行,
    // 故不做去重, 全部换行原样保留。
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Character('a'));
    comp->OnEvent(ftxui::Event::Return); // 行尾换行
    comp->OnEvent(ftxui::Event::Return); // 空行 (原 CRLF 去重场景, 现保留)
    comp->OnEvent(ftxui::Event::Character('b'));
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("a\n\nb"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_preserves_tab() {
    InputFixture f;
    auto         comp = f.makeComponent();

    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Tab); // 粘贴中的 Tab 应原样保留
    InputFixture::type(*comp, "foo");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("\tfoo"));
}

void test_paste_inserts_at_cursor() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "ab", 光标左移一位, 粘贴 "XY" → 插入到光标处 → "aXYb"
    InputFixture::type(*comp, "ab");
    comp->OnEvent(ftxui::Event::ArrowLeft);
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "XY");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("aXYb"));
    // 粘贴后光标位于粘贴内容末尾 (即 'b' 之前), 后续输入接在粘贴内容之后
    comp->OnEvent(ftxui::Event::Character('Z'));
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("aXYZb"));
}

void test_paste_trailing_newline_not_sent() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴以换行结尾: 该换行属于粘贴内容, 不应触发发送
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "hello");
    comp->OnEvent(ftxui::Event::Return);
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("hello\n"));
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_paste_empty_content() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 空粘贴 (仅标记): 不崩溃, 内容为空
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));

    XX_TEST_EXPECT_TRUE(comp->inputText().empty());
    XX_TEST_EXPECT_FALSE(f.sent);
}

void test_real_enter_sends() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 正常输入 + 回车: 发送并清空输入框
    InputFixture::type(*comp, "hello");
    comp->OnEvent(ftxui::Event::Return);

    XX_TEST_EXPECT_TRUE(f.sent);
    XX_TEST_EXPECT_EQ(f.sentText, std::string("hello"));
    XX_TEST_EXPECT_TRUE(comp->inputText().empty());
}

void test_send_rejected_retains_input() {
    InputFixture           f;
    InputComponent::Config cfg;
    // 模拟 server-io 未初始化就绪, 拒绝发送
    cfg.onSend = [](std::string, std::vector<agentxx::agent::MediaAttachment>) -> bool {
        return false;
    };
    auto comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));

    InputFixture::type(*comp, "pending message");
    comp->OnEvent(ftxui::Event::Return);

    // 未就绪拒绝发送时, 输入框内容保留不被清空
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("pending message"));
}

void test_enter_after_paste_sends_all() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴多行后回车: 发送完整多行内容 (去除首尾换行)
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "line1");
    comp->OnEvent(ftxui::Event::Return); // 粘贴换行
    InputFixture::type(*comp, "line2");
    comp->OnEvent(ftxui::Event::Special("\x1B[201~"));
    comp->OnEvent(ftxui::Event::Return); // 真实回车

    XX_TEST_EXPECT_TRUE(f.sent);
    XX_TEST_EXPECT_EQ(f.sentText, std::string("line1\nline2"));
}

void test_alt_enter_newline_cursor_at_end() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "abc" + Alt+Enter + "def": 换行后光标应位于换行之后 (新行开头),
    // 后续输入接在换行之后 → "abc\ndef"
    InputFixture::type(*comp, "abc");
    comp->OnEvent(ftxui::Event::Special("\x1B\n")); // Alt+Enter
    InputFixture::type(*comp, "def");

    // 旧实现 (inputText_ += '\n' 不更新光标) 会得到 "abcdef\n"
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("abc\ndef"));
}

void test_alt_enter_newline_mid_text() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // "abc", 光标移到 'c' 前, Alt+Enter → "ab\nc", 光标在换行后
    InputFixture::type(*comp, "abc");
    comp->OnEvent(ftxui::Event::ArrowLeft);         // 光标在 'c' 前
    comp->OnEvent(ftxui::Event::Special("\x1B\r")); // Alt+Enter (部分终端发送 \r)
    comp->OnEvent(ftxui::Event::Character('X'));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("ab\nXc"));
}

void test_paste_timeout_recovery() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴开始后结束标记丢失: 超过超时 (2s) 后自动退出粘贴模式,
    // 后续输入不再被吞入缓冲区
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    InputFixture::type(*comp, "ab");

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    comp->OnEvent(ftxui::Event::Character('x'));
    // 未完成的粘贴缓冲区被丢弃, 仅 'x' 被正常插入
    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("x"));
}

void test_clear_resets_paste_state() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 粘贴进行中调用 clear(): 重置粘贴状态, 后续输入正常插入
    comp->OnEvent(ftxui::Event::Special("\x1B[200~"));
    comp->OnEvent(ftxui::Event::Character('a'));
    comp->clear();
    comp->OnEvent(ftxui::Event::Character('b'));

    XX_TEST_EXPECT_EQ(comp->inputText(), std::string("b"));
}

// ---------------------------------------------------------------------------
// 多模态附件托盘: 挂载后随 Enter 一并发送, 发送成功后清空托盘
// ---------------------------------------------------------------------------

void test_input_attachment_tray_send() {
    InputFixture f;
    auto         comp = f.makeComponent();

    agentxx::agent::MediaAttachment att;
    att.type        = agentxx::agent::MediaType::Image;
    att.displayName = "chart.png";
    att.mimeType    = "image/png";
    att.pathOrUrl   = "/tmp/chart.png";
    att.dataUrl     = "data:image/png;base64,AAA";
    att.sizeBytes   = 1234;
    comp->addAttachment(att);

    XX_TEST_EXPECT_EQ(comp->attachments().size(), size_t{1});

    InputFixture::type(*comp, "hi");
    comp->OnEvent(ftxui::Event::Return);

    XX_TEST_EXPECT_TRUE(f.sent);
    XX_TEST_EXPECT_EQ(f.sentText, std::string("hi"));
    XX_TEST_EXPECT_EQ(f.sentAttachments.size(), size_t{1});
    if (!f.sentAttachments.empty()) {
        XX_TEST_EXPECT_EQ(f.sentAttachments[0].displayName, std::string("chart.png"));
        XX_TEST_EXPECT_EQ(f.sentAttachments[0].dataUrl, std::string("data:image/png;base64,AAA"));
    }
    // 发送成功后托盘清空
    XX_TEST_EXPECT_TRUE(comp->attachments().empty());
    XX_TEST_EXPECT_TRUE(comp->inputText().empty());
}

// ---------------------------------------------------------------------------
// [+ 附件] 按钮显隐: canAttach==false 时不展示, true 时展示
// ---------------------------------------------------------------------------

void test_input_attach_button_visibility() {
    // 不支持多模态: 按钮隐藏
    {
        InputFixture           f;
        InputComponent::Config cfg;
        cfg.onSend = [](std::string, std::vector<agentxx::agent::MediaAttachment>) {
            return true;
        };
        cfg.canAttach = [] {
            return false;
        };
        auto          comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));
        ftxui::Screen screen(80, 6);
        ftxui::Render(screen, comp->OnRender());
        XX_TEST_EXPECT_TRUE(screen.ToString().find("📎︎︎") == std::string::npos);
    }
    // 支持多模态: 按钮展示
    {
        InputFixture           f;
        InputComponent::Config cfg;
        cfg.onSend = [](std::string, std::vector<agentxx::agent::MediaAttachment>) {
            return true;
        };
        cfg.canAttach = [] {
            return true;
        };
        auto          comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));
        ftxui::Screen screen(80, 6);
        ftxui::Render(screen, comp->OnRender());
        const auto out = screen.ToString();
        // 按钮文本为 "[📎︎︎]"（中英同形，仅图标）
        XX_TEST_EXPECT_TRUE(out.find("📎") != std::string::npos);
    }
}

void test_tui_state_message_queue_sync() {
    TUISharedState sharedState;

    // 模拟收到 WireMessageQueueUpdate 更新 pendingInputs
    agentxx::agent::WireMessageQueueUpdate update;
    update.sessionId = "test-session";
    agentxx::agent::MessageQueueItem it1;
    it1.id          = "q-1";
    it1.text        = "msg 1";
    it1.model       = "m1";
    it1.createdAtMs = 1000;

    agentxx::agent::MessageQueueItem it2;
    it2.id          = "q-2";
    it2.text        = "msg 2";
    it2.model       = "";
    it2.createdAtMs = 2000;

    update.items.push_back(it1);
    update.items.push_back(it2);

    sharedState.mutate([&](TUIRenderState& st) {
        st.pendingInputs.clear();
        for (const auto& item : update.items) {
            TUIPendingInput pi;
            pi.id          = item.id;
            pi.text        = item.text;
            pi.model       = item.model;
            pi.createdAtMs = item.createdAtMs;
            st.pendingInputs.push_back(std::move(pi));
        }
    });

    auto snap = sharedState.readSnapshot();
    XX_TEST_EXPECT_EQ(snap->pendingInputs.size(), size_t{2});
    if (snap->pendingInputs.size() == 2) {
        XX_TEST_EXPECT_EQ(snap->pendingInputs[0].id, std::string("q-1"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[0].text, std::string("msg 1"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[1].id, std::string("q-2"));
        XX_TEST_EXPECT_EQ(snap->pendingInputs[1].text, std::string("msg 2"));
    }
}

// ---------------------------------------------------------------------------
// 流式指示 spinner: 动画帧经组件树转发 OnAnimation 推进
// (回归: SpinnerComponent 若未 Add() 入组件树, 收不到动画回调, 恒显示首帧 ⠋)
// ---------------------------------------------------------------------------
void test_spinner_frame_advances_via_tree() {
    InputFixture           f;
    bool                   streaming = false;
    InputComponent::Config cfg;
    cfg.onSend = [](std::string, std::vector<agentxx::agent::MediaAttachment>) -> bool {
        return true;
    };
    cfg.isStreaming = [&streaming] {
        return streaming;
    };
    auto comp = std::make_shared<InputComponent>(f.ctx, std::move(cfg));

    // 渲染到字符串便于检查指示器字符 (braille 字符在输入栏中唯一)
    auto renderToString = [](InputComponent& c) {
        ftxui::Screen screen(80, 5);
        ftxui::Render(screen, c.OnRender());
        return screen.ToString();
    };

    // 非流式: 静态 ">" 提示符, 无 braille 动画字符
    const std::string idle = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(idle.find(">") != std::string::npos);
    XX_TEST_EXPECT_TRUE(idle.find("⠋") == std::string::npos);

    // 流式: 首帧渲染为第 0 帧 ⠋
    streaming = true;
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠋") != std::string::npos);

    // 经组件树分发动画步进 (根组件 OnAnimation 默认逐级转发给已注册子组件):
    // 单次 90ms > 帧间隔 80ms → 推进到第 1 帧 ⠙
    ftxui::animation::Params params(std::chrono::milliseconds(90));
    comp->OnAnimation(params);
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠙") != std::string::npos);

    // 多次小步长累计跨帧: 再累计 10×20ms=200ms ≥ 2×80ms → 第 3 帧 ⠸
    for (int i = 0; i < 10; ++i) {
        ftxui::animation::Params small(std::chrono::milliseconds(20));
        comp->OnAnimation(small);
    }
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find("⠸") != std::string::npos);

    // 停止流式: 指示器回到静态 ">"
    streaming = false;
    XX_TEST_EXPECT_TRUE(renderToString(*comp).find(">") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 待发送消息队列渲染:
// 1. pendingInputs 为空时不展示
// 2. pendingInputs 非空时渲染在输入框内
// 3. 同时存在 pendingInputs 与附件时, 待发送队列严格渲染在附件行之上
// ---------------------------------------------------------------------------

void test_input_pending_queue_visibility() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 1. pendingInputs 为空时: 屏幕中不含队列关键字, Box 为空
    {
        ftxui::Screen screen(80, 10);
        ftxui::Render(screen, comp->OnRender());
        std::string out = screen.ToString();
        XX_TEST_EXPECT_TRUE(
            out.find("Message Queue") == std::string::npos
            && out.find("待发送消息队列") == std::string::npos
        );
        // 未渲染的按钮不登记命中: 命中框为空区域 (空区域用 IsEmpty 判定,
        // 默认构造的 ftxui::Box 四分量皆为 0 并不是空区域)
        XX_TEST_EXPECT_TRUE(comp->pendingCounterBox().IsEmpty());
        XX_TEST_EXPECT_TRUE(comp->pendingInsertButtonBox().IsEmpty());
    }

    // 2. pendingInputs 非空时: 屏幕中出现队列标题与立即发送按钮, Box 被 reflect 填充有效尺寸
    f.sharedState.mutate([](TUIRenderState& st) {
        TUIPendingInput pi;
        pi.id   = "q-1";
        pi.text = "queued task";
        st.pendingInputs.push_back(std::move(pi));
    });
    f.ctx.frameState = f.sharedState.readSnapshot();

    {
        ftxui::Screen screen(80, 10);
        ftxui::Render(screen, comp->OnRender());
        std::string out = screen.ToString();
        XX_TEST_EXPECT_TRUE(
            out.find("Message Queue") != std::string::npos
            || out.find("待发送消息队列") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(
            out.find("Insert") != std::string::npos || out.find("立即发送") != std::string::npos
        );
        XX_TEST_EXPECT_TRUE(comp->pendingCounterBox().x_max > comp->pendingCounterBox().x_min);
        XX_TEST_EXPECT_TRUE(
            comp->pendingInsertButtonBox().x_max > comp->pendingInsertButtonBox().x_min
        );
        XX_TEST_EXPECT_TRUE(!comp->pendingCounterBox().IsEmpty());
        XX_TEST_EXPECT_TRUE(!comp->pendingInsertButtonBox().IsEmpty());
    }

    // 3. pendingInputs 清空后: 重新渲染, 队列消失, Box 重置
    f.sharedState.mutate([](TUIRenderState& st) {
        st.pendingInputs.clear();
    });
    f.ctx.frameState = f.sharedState.readSnapshot();

    {
        ftxui::Screen screen(80, 10);
        ftxui::Render(screen, comp->OnRender());
        std::string out = screen.ToString();
        XX_TEST_EXPECT_TRUE(
            out.find("Message Queue") == std::string::npos
            && out.find("待发送消息队列") == std::string::npos
        );
        // 未渲染的按钮不登记命中: 命中框为空区域 (空区域用 IsEmpty 判定,
        // 默认构造的 ftxui::Box 四分量皆为 0 并不是空区域)
        XX_TEST_EXPECT_TRUE(comp->pendingCounterBox().IsEmpty());
        XX_TEST_EXPECT_TRUE(comp->pendingInsertButtonBox().IsEmpty());
    }
}

void test_input_pending_queue_above_attachments() {
    InputFixture f;
    auto         comp = f.makeComponent();

    // 注入待发送队列
    f.sharedState.mutate([](TUIRenderState& st) {
        TUIPendingInput pi;
        pi.id   = "q-1";
        pi.text = "task queued";
        st.pendingInputs.push_back(std::move(pi));
    });
    f.ctx.frameState = f.sharedState.readSnapshot();

    // 挂载附件
    agentxx::agent::MediaAttachment att;
    att.type        = agentxx::agent::MediaType::Image;
    att.displayName = "photo.jpg";
    att.sizeBytes   = 5678;
    comp->addAttachment(std::move(att));

    ftxui::Screen screen(100, 15);
    ftxui::Render(screen, comp->OnRender());
    std::string out = screen.ToString();

    // 查找待发送队列、附件、输入提示符的位置
    size_t queuePos = out.find("Message Queue");
    if (queuePos == std::string::npos) {
        queuePos = out.find("待发送消息队列");
    }
    size_t attachPos = out.find("photo.jpg");
    size_t inputPos  = out.find(">");

    XX_TEST_EXPECT_TRUE(queuePos != std::string::npos);
    XX_TEST_EXPECT_TRUE(attachPos != std::string::npos);
    XX_TEST_EXPECT_TRUE(inputPos != std::string::npos);

    // 验证严格顺序: 待发送队列在附件之上, 附件在输入文本行之上
    XX_TEST_EXPECT_TRUE(queuePos < attachPos);
    XX_TEST_EXPECT_TRUE(attachPos < inputPos);
}

// ---------------------------------------------------------------------------
// 附件选择弹窗 (FilePickerOverlay): 纯导航列表 (已移除过滤输入框)
// - 内容区只有路径行 + 文件列表, 不再渲染过滤行
// - ↑/↓ 移动 + Enter 进入目录/确认文件仍然工作
// - 输入字符不再过滤列表 (列表内容与选中项不变)
// - 不支持当前模型的媒体文件 (如仅图像输入时的音频) 不可选中
// ---------------------------------------------------------------------------

void test_file_picker_navigation_without_filter() {
    namespace fs = std::filesystem;

    // 临时目录: 子目录 (内含一个 png) + 两个 png + 一个文本 + 一个当前模型不支持的 mp3
    const auto root = fs::temp_directory_path()
                      / fmt::format(
                          "agentxx_tui_picker_test_{}",
                          std::chrono::steady_clock::now().time_since_epoch().count()
                      );
    std::error_code ec;
    fs::create_directories(root / "sub", ec);
    XX_TEST_EXPECT_FALSE(ec);
    for (const char* name : {"a.png", "b.png", "note.txt", "song.mp3", "sub/inner.png"}) {
        std::ofstream ofs((root / name).string(), std::ios::binary);
        ofs << "x";
    }

    InputFixture                        f;
    agentxx::agent::ModelCapabilityInfo caps;
    caps.name       = "vision-model";
    caps.imageInput = true; // 仅图像: .mp3 属于媒体文件但当前模型不支持
    auto comp       = std::make_shared<FilePickerOverlay>(f.ctx, caps, root.string());

    std::string selectedPath;
    bool        closed = false;
    comp->onSelectFile([&](std::string p) {
        selectedPath = std::move(p);
    });
    comp->onClose([&] {
        closed = true;
    });

    auto renderToString = [](FilePickerOverlay& c) {
        ftxui::Screen screen(100, 30);
        ftxui::Render(screen, c.OnRender());
        return screen.ToString();
    };

    // 1. 列表内容: 子目录 + 两个 png 展示; 非媒体文件 (note.txt) 不展示;
    //    不支持的媒体文件 (song.mp3) 展示但标记为不支持
    std::string out = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(out.find("sub") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("a.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("b.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("note.txt") == std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("song.mp3") != std::string::npos);
    // 过滤输入框已移除: 弹窗内不再出现过滤标签 (中英文界面都不得出现)
    XX_TEST_EXPECT_TRUE(out.find("过滤") == std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("Filter") == std::string::npos);

    // 2. 输入字符不再过滤列表 (旧实现会按子串过滤, 使列表只剩匹配项):
    //    输入 "zzz" 后列表内容不变, 选中项仍为第 0 项
    for (char c : std::string("zzz")) {
        comp->OnEvent(ftxui::Event::Character(c));
    }
    out = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(out.find("a.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("b.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("sub") != std::string::npos);

    // 3. 目录在前: 第 0 项 = 上级目录, 第 1 项 = sub/; Enter 进入 sub/
    //    (用子目录内的文件确认已切换目录: 长路径在路径行会被折行, 不宜按整串匹配)
    comp->OnEvent(ftxui::Event::ArrowDown);
    comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_FALSE(closed); // 进入目录不关闭弹窗
    out = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(out.find("inner.png") != std::string::npos); // 子目录内容
    XX_TEST_EXPECT_TRUE(out.find("a.png") == std::string::npos);     // 父目录内容不再显示

    // 4. Enter 进入上级目录回到原目录 (原目录内容重新出现)
    comp->OnEvent(ftxui::Event::Return);
    out = renderToString(*comp);
    XX_TEST_EXPECT_TRUE(out.find("a.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("b.png") != std::string::npos);
    XX_TEST_EXPECT_TRUE(out.find("inner.png") == std::string::npos);

    // 5. 选择支持的媒体文件: 列表 = [上级目录, sub/, a.png, b.png, song.mp3],
    //    下移到 b.png 并确认 -> 回调文件路径并关闭弹窗
    comp->OnEvent(ftxui::Event::ArrowDown); // -> sub/
    comp->OnEvent(ftxui::Event::ArrowDown); // -> a.png
    comp->OnEvent(ftxui::Event::ArrowDown); // -> b.png
    comp->OnEvent(ftxui::Event::Return);
    XX_TEST_EXPECT_TRUE(closed);
    XX_TEST_EXPECT_EQ(fs::path(selectedPath).filename().string(), std::string("b.png"));

    // 6. 不支持的媒体文件不可选中 (另起弹窗, 下移到 song.mp3 并确认)
    {
        auto comp2     = std::make_shared<FilePickerOverlay>(f.ctx, caps, root.string());
        bool selected2 = false;
        comp2->onSelectFile([&](std::string) {
            selected2 = true;
        });
        for (int i = 0; i < 4; ++i) { // 上级目录 -> sub/ -> a.png -> b.png -> song.mp3
            comp2->OnEvent(ftxui::Event::ArrowDown);
        }
        comp2->OnEvent(ftxui::Event::Return);
        XX_TEST_EXPECT_FALSE(selected2);
    }

    // 7. Esc 关闭弹窗
    {
        auto comp3   = std::make_shared<FilePickerOverlay>(f.ctx, caps, root.string());
        bool closed3 = false;
        comp3->onClose([&] {
            closed3 = true;
        });
        comp3->OnEvent(ftxui::Event::Escape);
        XX_TEST_EXPECT_TRUE(closed3);
    }

    fs::remove_all(root, ec);
}

TestResult testTuiInput() {
    g_tui_input_passed = 0;
    g_tui_input_failed = 0;

    test_multiline_paste_inserted_not_sent();
    test_paste_crlf_dedup();
    test_paste_preserves_tab();
    test_paste_inserts_at_cursor();
    test_paste_trailing_newline_not_sent();
    test_paste_empty_content();
    test_real_enter_sends();
    test_send_rejected_retains_input();
    test_enter_after_paste_sends_all();
    test_alt_enter_newline_cursor_at_end();
    test_alt_enter_newline_mid_text();
    test_paste_timeout_recovery();
    test_clear_resets_paste_state();
    test_spinner_frame_advances_via_tree();
    test_tui_state_message_queue_sync();
    test_input_attachment_tray_send();
    test_input_attach_button_visibility();
    test_input_pending_queue_visibility();
    test_input_pending_queue_above_attachments();
    test_file_picker_navigation_without_filter();

    return TestResult{g_tui_input_passed, g_tui_input_failed};
}

} // namespace test
} // namespace agentxx
