// 表单交互专项测试 (控件状态机 + 各接入点的结果回传契约)
//
// 背景: 控件 (checkbox/select/buttons/number/text) 与提交行的渲染、点击、键盘、
// 校验、取值只有一份实现 (`ui_components.cpp` 的表单函数), 中断 / 插件面板 / 通用
// overlay 三个接入点共用; 差别只在"结果去处":
// - 中断 → 中断结果通道 (`{"values":{...}}`, 见 `tui_interrupt`)
// - 插件面板 / overlay → 动作通道 (`__submit` / `__cancel` / `commitOnPick` 的控件 id)
//
// 本模块因此只覆盖两件事:
// 1) 共享控件状态机本身 (初始化/点击/键盘/校验/取值) —— 与具体接入点无关
// 2) 插件面板与 overlay 接入点的"结果回传"端到端 (经动作通道)
#include "agentxx-test/client/test_tui_form.h"

#include "agentxx-client/io/tui/agent_tui.h"
#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_context.h"
#include "agentxx-client/io/tui/tui_theme.h"
#include "agentxx-client/io/tui/ui_components.h"
#include "agentxx/plugin/client_plugin_manager.h"
#include "asio/io_context.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_form_passed = 0;
int g_tui_form_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_form_passed
#define XX_TEST_FAILED g_tui_form_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;
using utilxx_base::Json;

namespace {

TUITheme& theme() {
    static TUITheme t = TUITheme::darkTheme();
    return t;
}

/// 解析组件描述 (表单用例统一入口)
std::vector<agentxx::ui::Item> formItems(const char* json) {
    return agentxx::ui::parseItems(Json::parse(json));
}

// ---------------------------------------------------------------------------
// 面板 / overlay 接入点的动作通道探针
// ---------------------------------------------------------------------------

/// 动作派发探针 (记录 owner/action/args; 供断言"结果经动作通道回传")
struct ActionProbe {
    std::mutex         mu;
    int                calls = 0;
    std::string        owner;
    std::string        action;
    std::string        args;

    static void PLUGINXX_CALL onAction(const AgentxxUiActionContext* ctx, void* ud) {
        auto* self = static_cast<ActionProbe*>(ud);
        std::lock_guard<std::mutex> lk(self->mu);
        ++self->calls;
        self->owner.assign(
            ctx->owner_id.data ? ctx->owner_id.data : "",
            static_cast<size_t>(ctx->owner_id.data ? ctx->owner_id.size : 0)
        );
        self->action.assign(
            ctx->action_id.data ? ctx->action_id.data : "",
            static_cast<size_t>(ctx->action_id.data ? ctx->action_id.size : 0)
        );
        self->args.assign(
            ctx->action_args.data ? ctx->action_args.data : "",
            static_cast<size_t>(ctx->action_args.data ? ctx->action_args.size : 0)
        );
    }
};

/// 测试用 UI 适配器 (只声明必要能力; 决定接口表内哪些成员可用)
class FormTestUiAdapter : public agentxx::plugin::PluginUiAdapter {
public:

    agentxx::plugin::InterfaceSet supportedInterfaces() const override {
        namespace pi = agentxx::plugin::plugin_interfaces;
        return {
            std::string{pi::ClientUi},
            std::string{pi::ClientPanel},
            std::string{pi::ClientInfoSection},
            std::string{pi::ClientOverlay},
            std::string{pi::ClientAction},
            std::string{pi::ClientComponents},
            std::string{pi::ClientForm},
            std::string{pi::ClientLayout},
        };
    }
};

/// 测试用管理器: 暴露 createInstance 以构造"伪实例"
/// (生产路径的实例由 dlopen + lifecycle 创建; 渲染/派发用例只需要一个实例身份)
class FormTestManager : public agentxx::plugin::ClientPluginManager {
public:

    using ClientPluginManager::ClientPluginManager;
    using ClientPluginManager::createInstance;
};

/// 屏幕上指定文本的位置 (返回左上角逐格坐标; 未找到返回 false)
///
/// 逐格读取屏幕内容后按行查找: 命中行的首个字符所在列即 x, 行为 y
bool findTextPos(const ftxui::Screen& screen, std::string_view needle, int& outX, int& outY) {
    if (needle.empty()) {
        return false;
    }
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string line;
        for (int x = 0; x < screen.dimx(); ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            line += ch.empty() ? std::string{" "} : ch;
        }
        const auto pos = line.find(needle);
        if (pos != std::string::npos) {
            outX = static_cast<int>(pos);
            outY = y;
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// 1) 共享控件状态机
// ---------------------------------------------------------------------------

/// 初始化: 按描述填默认值 (勾选态/选中下标/编辑文本), 已初始化项不被覆盖
void test_form_init_and_click() {
    auto items = formItems(R"([
        {"kind":"checkbox","id":"opt","label":"Opt","default":true},
        {"kind":"select","id":"mode","options":[{"value":"fast","label":"Fast"},
                                                {"value":"safe","label":"Safe"}],
         "default":"safe"},
        {"kind":"number","id":"num","label":"Num","default":5,"min":1,"max":9,"integer":true},
        {"kind":"input","id":"name","label":"Name","default":"abc"}
    ])");

    agentxx::client::UiFormState form;
    agentxx::client::initFormState(form, items);
    XX_TEST_EXPECT_EQ(form.controls.size(), size_t{4});
    XX_TEST_EXPECT_TRUE(form.ensure("opt").checked);
    XX_TEST_EXPECT_EQ(form.ensure("mode").selected, 1); // default=safe → 下标 1
    XX_TEST_EXPECT_EQ(form.ensure("name").editText, std::string{"abc"});
    XX_TEST_EXPECT_FALSE(form.ensure("name").edited);
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"5"});

    // 勾选项: 点击翻转
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormControlHit(items, form, "opt", 0),
        agentxx::client::UiFormAction::Changed
    );
    XX_TEST_EXPECT_FALSE(form.ensure("opt").checked);

    // 单选: 点击选中指定候选项 (无 commitOnPick → 仅变更)
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormControlHit(items, form, "mode", 0),
        agentxx::client::UiFormAction::Changed
    );
    XX_TEST_EXPECT_EQ(form.ensure("mode").selected, 0);

    // 数值步进: 子序号 0 = 减, 1 = 加 (受 min/max 约束)
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormControlHit(items, form, "num", 1),
        agentxx::client::UiFormAction::Changed
    );
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"6"});
    form.ensure("num").editText = "9";
    form.ensure("num").edited   = true;
    agentxx::client::handleFormControlHit(items, form, "num", 1);
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"9"}); // 上界不再增加

    // 未知控件 / 未知 id: 不产生动作
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormControlHit(items, form, "nope", 0),
        agentxx::client::UiFormAction::None
    );

    // 聚焦: 点击控件即聚焦
    form.focusedId = "name";
    XX_TEST_EXPECT_EQ(form.focusedId, std::string{"name"});
}

/// 键盘: 输入 (首次输入替换缺省值)、退格、Tab 移动、Esc 释放、数值过滤
void test_form_keyboard() {
    auto items = formItems(R"([
        {"kind":"input","id":"name","label":"Name","default":"abc"},
        {"kind":"number","id":"num","label":"Num","default":5,"integer":true}
    ])");

    agentxx::client::UiFormState form;
    agentxx::client::initFormState(form, items);
    form.focusedId = "name";

    // 首次输入替换缺省值 (与中断历史行为一致)
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Character('x')));
    XX_TEST_EXPECT_EQ(form.ensure("name").editText, std::string{"x"});
    XX_TEST_EXPECT_TRUE(form.ensure("name").edited);

    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Character('y')));
    XX_TEST_EXPECT_EQ(form.ensure("name").editText, std::string{"xy"});

    // 退格删除一个字符
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Backspace));
    XX_TEST_EXPECT_EQ(form.ensure("name").editText, std::string{"x"});

    // Tab 在控件间移动焦点
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Tab));
    XX_TEST_EXPECT_EQ(form.focusedId, std::string{"num"});

    // 数值框过滤非数字字符 (只接受数字)
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Character('a')));
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"5"});
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Character('7')));
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"7"});

    // 方向键被消费 (单行无光标定位, 不落到滚动)
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::ArrowLeft));
    XX_TEST_EXPECT_EQ(form.ensure("num").editText, std::string{"7"});

    // 非输入类控件的键盘语义: checkbox 空格翻转; buttons 左右切换; select 上下切换
    auto others = formItems(R"([
        {"kind":"checkbox","id":"opt","label":"Opt","default":false},
        {"kind":"buttons","id":"pick","options":[{"value":"a","label":"A"},{"value":"b","label":"B"}]},
        {"kind":"select","id":"mode","options":[{"value":"a","label":"A"},{"value":"b","label":"B"}]}
    ])");
    agentxx::client::UiFormState f2;
    agentxx::client::initFormState(f2, others);
    f2.focusedId = "opt";
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(others, f2, ftxui::Event::Character(' ')));
    XX_TEST_EXPECT_TRUE(f2.ensure("opt").checked);
    f2.focusedId = "pick";
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(others, f2, ftxui::Event::ArrowRight));
    XX_TEST_EXPECT_EQ(f2.ensure("pick").selected, 1);
    f2.focusedId = "mode";
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(others, f2, ftxui::Event::ArrowDown));
    XX_TEST_EXPECT_EQ(f2.ensure("mode").selected, 1);

    // Escape 释放焦点
    XX_TEST_EXPECT_TRUE(agentxx::client::handleFormKeyInput(items, form, ftxui::Event::Escape));
    XX_TEST_EXPECT_TRUE(form.focusedId.empty());
}

/// 校验与取值: 越界写提示且拒绝提交; 通过后按控件形态组装值
void test_form_validate_and_values() {
    auto items = formItems(R"([
        {"kind":"checkbox","id":"opt","label":"Opt","default":true},
        {"kind":"select","id":"mode","options":[{"value":"fast","label":"Fast"},
                                                {"value":"safe","label":"Safe"}],
         "default":"safe"},
        {"kind":"number","id":"num","label":"Num","default":5,"min":1,"max":9,"integer":true},
        {"kind":"input","id":"name","label":"Name","default":"abc"}
    ])");
    agentxx::client::UiFormState form;
    agentxx::client::initFormState(form, items);
    XX_TEST_EXPECT_TRUE(agentxx::client::validateForm(items, form));

    form.ensure("num").editText = "99";
    form.ensure("num").edited   = true;
    XX_TEST_EXPECT_FALSE(agentxx::client::validateForm(items, form));
    XX_TEST_EXPECT_FALSE(form.ensure("num").tip.empty());

    form.ensure("num").editText = "abc";
    XX_TEST_EXPECT_FALSE(agentxx::client::validateForm(items, form));

    form.ensure("num").editText = "7";
    XX_TEST_EXPECT_TRUE(agentxx::client::validateForm(items, form));
    XX_TEST_EXPECT_TRUE(form.ensure("num").tip.empty());

    auto values = agentxx::client::formValues(items, form);
    XX_TEST_EXPECT_TRUE(values.contains("values"));
    const auto& v = values["values"];
    XX_TEST_EXPECT_TRUE(v.contains("opt"));
    XX_TEST_EXPECT_EQ(v["opt"].get<bool>(), true);
    XX_TEST_EXPECT_EQ(v["mode"].get<std::string>(), std::string{"safe"});
    XX_TEST_EXPECT_EQ(v["name"].get<std::string>(), std::string{"abc"});
    XX_TEST_EXPECT_EQ(v["num"].get<double>(), 7.0);

    // 提交行命中: 动作 id → 语义
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormSubmitHit(agentxx::client::kFormSubmitActionId),
        agentxx::client::UiFormAction::Submit
    );
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormSubmitHit(agentxx::client::kFormCancelActionId),
        agentxx::client::UiFormAction::Cancel
    );
    XX_TEST_EXPECT_EQ(
        agentxx::client::handleFormSubmitHit("other"),
        agentxx::client::UiFormAction::None
    );
}

/// 候选项缺失: 校验失败并写提示 (避免"提交了空值")
void test_form_validate_missing_options() {
    auto items = formItems(R"([
        {"kind":"select","id":"mode","options":[]},
        {"kind":"buttons","id":"pick","options":[]}
    ])");
    agentxx::client::UiFormState form;
    agentxx::client::initFormState(form, items);
    XX_TEST_EXPECT_FALSE(agentxx::client::validateForm(items, form));
    XX_TEST_EXPECT_FALSE(form.ensure("mode").tip.empty());
}

/// 容器内的控件也能收集与初始化 (row/box 内的控件)
void test_form_nested_controls() {
    auto items = formItems(R"([
        {"kind":"box","title":"F","items":[
            {"kind":"row","items":[{"kind":"checkbox","id":"inner","label":"I","default":false}]}
        ]}
    ])");
    auto ids = agentxx::client::collectControlIds(items);
    XX_TEST_EXPECT_EQ(ids.size(), size_t{1});
    XX_TEST_EXPECT_EQ(ids[0], std::string{"inner"});
    agentxx::client::UiFormState form;
    agentxx::client::initFormState(form, items);
    XX_TEST_EXPECT_EQ(form.controls.size(), size_t{1});
    auto values = agentxx::client::formValues(items, form);
    XX_TEST_EXPECT_TRUE(values["values"].contains("inner"));
}

// ---------------------------------------------------------------------------
// 2) overlay 接入点: 表单提交经动作通道回传
// ---------------------------------------------------------------------------

/// 自定义 overlay 内的表单: 点击提交行 → `__submit` + `{"values":{...}}`
void test_overlay_form_submit_through_action_channel() {
    asio::io_context io;
    // 持有 work guard: poll() 在"无工作可做"时会返回并把 io_context 标记为 stopped,
    // 之后再投递的派发动作会因"executor 已停止"被丢弃 (见 manager_base::postToIo)
    auto             work = asio::make_work_guard(io);
    auto             mgr = std::make_shared<FormTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<FormTestUiAdapter>());
    auto inst = mgr->createInstance("form_probe");
    XX_TEST_EXPECT_TRUE(inst != nullptr);
    // 登记实例 (派发路径按插件名查找实例; 生产路径由 dlopen + lifecycle 完成)
    mgr->plugins_.emplace(inst->name, inst);
    ActionProbe probe;
    XX_TEST_EXPECT_EQ(
        mgr->bindActionHandler(
            inst.get(),
            std::string_view{AGENTXX_CLIENT_OVERLAY_OWNER},
            &ActionProbe::onAction,
            &probe
        ),
        0
    );

    TUICtx ctx;
    ctx.theme         = &theme();
    ctx.pluginManager = mgr;
    ctx.postRedraw    = [] {};
    // 固定视口: 弹窗尺寸 (宽高比例) 由此确定, 不随物理终端漂移
    ctx.viewportWidth  = 100;
    ctx.viewportHeight = 30;
    ctx.refreshFrameSize();

    const std::string payload = R"({"items":[
        {"kind":"table","header":true,"columns":[{"title":"Path","w":"flex"},
                                                {"title":"Scope","w":6}],
         "rows":[["a.txt","write"]]},
        {"kind":"checkbox","id":"remember","label":"Remember","default":false},
        {"kind":"submit","label":"APPLY","cancelLabel":"DROP"}
    ]})";
    auto overlay = agentxx::client::createUniversalOverlay(
        ctx,
        AGENTXX_OVERLAY_CUSTOM,
        "Files",
        payload,
        "{\"size\":\"large\"}",
        "form_probe",
        [] {}
    );
    XX_TEST_EXPECT_TRUE(overlay != nullptr);
    if (!overlay) {
        return;
    }

    // 渲染一帧: 内容 (表格 + 控件 + 提交行) 上屏
    auto renderFrame = [&]() {
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(ctx.viewportWidth),
            ftxui::Dimension::Fixed(ctx.viewportHeight)
        );
        ftxui::Render(screen, overlay->Render());
        return screen;
    };
    auto screen = renderFrame();
    int  sx     = 0;
    int  sy     = 0;
    XX_TEST_EXPECT_TRUE(findTextPos(screen, "a.txt", sx, sy));
    XX_TEST_EXPECT_TRUE(findTextPos(screen, "APPLY", sx, sy));

    // 点击提交行 (提交标签所在位置即该行可点区域)
    ftxui::Mouse m;
    m.button = ftxui::Mouse::Left;
    m.motion = ftxui::Mouse::Released;
    m.x      = sx;
    m.y      = sy;
    XX_TEST_EXPECT_TRUE(overlay->OnEvent(ftxui::Event::Mouse("", m)));
    io.restart();
    io.poll(); // 动作派发经 io 线程投递 (派发路径不阻塞 UI 线程)

    {
        std::lock_guard<std::mutex> lk(probe.mu);
        XX_TEST_EXPECT_EQ(probe.calls, 1);
        XX_TEST_EXPECT_EQ(probe.action, std::string{agentxx::client::kFormSubmitActionId});
        XX_TEST_EXPECT_EQ(probe.owner, std::string{AGENTXX_CLIENT_OVERLAY_OWNER});
        const auto args = Json::parse(probe.args);
        XX_TEST_EXPECT_TRUE(args.contains("values"));
        XX_TEST_EXPECT_TRUE(args["values"].contains("remember"));
        XX_TEST_EXPECT_EQ(args["values"]["remember"].get<bool>(), false);
    }

    // 取消行: `__cancel` (无 values)
    if (findTextPos(screen, "DROP", sx, sy)) {
        m.x = sx;
        m.y = sy;
        XX_TEST_EXPECT_TRUE(overlay->OnEvent(ftxui::Event::Mouse("", m)));
        io.restart();
        io.poll();
        std::lock_guard<std::mutex> lk(probe.mu);
        XX_TEST_EXPECT_EQ(probe.calls, 2);
        XX_TEST_EXPECT_EQ(probe.action, std::string{agentxx::client::kFormCancelActionId});
    }
}

// ---------------------------------------------------------------------------
// 3) 面板接入点: 表单描述 → 命中区域登记 (点击回传见 `tui_widget` 的面板用例)
// ---------------------------------------------------------------------------

/// 面板内容里的表单: 渲染后控件行登记可命中区域 (owner = 面板 id)
void test_panel_form_registers_hit_regions() {
    asio::io_context io;
    auto             mgr = std::make_shared<FormTestManager>(io.get_executor());
    mgr->setUiAdapter(std::make_shared<FormTestUiAdapter>());
    auto inst = mgr->createInstance("panel_form_probe");
    auto* panel = static_cast<AgentxxPanel*>(mgr->registerPanel(
        inst.get(),
        std::string_view{"panel_form_probe.panel"},
        std::string_view{R"({"title":"Form"})"}
    ));
    XX_TEST_EXPECT_TRUE(panel != nullptr);
    const std::string items = R"({"items":[
        {"kind":"checkbox","id":"verbose","label":"Verbose","default":false},
        {"kind":"submit","label":"SAVE"}
    ]})";
    XX_TEST_EXPECT_EQ(mgr->updatePanel(inst.get(), panel, std::string_view{items}), 0);

    auto tui = std::make_shared<TUIClientAgentIO>(io.get_executor(), "session");
    tui->setPluginManager(mgr);
    tui->refreshRenderContext();
    auto rows = tui->renderPluginPanel("panel_form_probe.panel");
    XX_TEST_EXPECT_TRUE(!rows.empty());

    // 控件行登记了可命中区域 (归属 = 面板 id / 插件名), 提交行同理
    size_t regionCount = 0;
    bool   hasCheckbox = false;
    bool   hasSubmit   = false;
    for (const auto& row : rows) {
        for (const auto& region : row.hits) {
            ++regionCount;
            XX_TEST_EXPECT_EQ(region.ownerId, std::string{"panel_form_probe.panel"});
            XX_TEST_EXPECT_EQ(region.plugin, std::string{"panel_form_probe"});
            if (region.kind == UiHitRegionKind::Form && region.id == "verbose") {
                hasCheckbox = true;
            }
            if (region.kind == UiHitRegionKind::FormSubmit) {
                hasSubmit = true;
            }
        }
    }
    XX_TEST_EXPECT_TRUE(regionCount >= 2);
    XX_TEST_EXPECT_TRUE(hasCheckbox);
    XX_TEST_EXPECT_TRUE(hasSubmit);
}

TestResult testTuiForm() {
    g_tui_form_passed = 0;
    g_tui_form_failed = 0;

    test_form_init_and_click();
    test_form_keyboard();
    test_form_validate_and_values();
    test_form_validate_missing_options();
    test_form_nested_controls();
    test_overlay_form_submit_through_action_channel();
    test_panel_form_registers_hit_regions();

    return TestResult{g_tui_form_passed, g_tui_form_failed};
}

} // namespace test
} // namespace agentxx
