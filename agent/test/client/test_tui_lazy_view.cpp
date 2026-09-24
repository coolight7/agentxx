// 懒构建列表视口测试 (LazyScrollable)
//
// 覆盖的契约 (渲染重构阶段 A/C):
// - [每帧成本与列表长度无关] 2000 条列表下, 单帧只构建/测量窗口内条目:
//   buildItem / itemKey 调用次数有界, 不随条目数增长
//   (高度和增量维护、key 按帧标记惰性比较、上一帧记录按列表复位)
// - [前插稳定] notifyPrepended 后视口内容保持不动: scrollOffset 与 totalHeight
//   同步等量增长, 屏幕逐行内容与滚动条比例均不跳变
// - [实测精确] 全部条目进入过视口后, totalHeight 等于各条目实测高度之和
// - [滚动边界] 上滚到顶 scrollOffset==0; 下滚到底恢复吸附底部
// - [估算偏差不影响视口内容] 给出离谱的粗略高度时, 视口内条目仍被渲染
//   (估算只影响滚动条长度与未实测区域的定位, 实测后自愈)
#include "agentxx-test/client/test_tui_lazy_view.h"

#include "agentxx-client/io/tui/lazy_scrollable.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_tui_lazy_view_passed = 0;
int g_tui_lazy_view_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_tui_lazy_view_passed
#define XX_TEST_FAILED g_tui_lazy_view_failed

namespace agentxx {
namespace test {

using namespace agentxx::client;

namespace {

constexpr int kWidth  = 60;
constexpr int kHeight = 20;

/// 视口夹具: 直接构造 LazyScrollable (不依赖消息列表), 可控条目高度与 key
struct ViewFixture {
    size_t   count      = 0;              // 条目数
    size_t   itemHeight = 1;              // 每条实测高度 (行)
    size_t   quickRows  = 1;              // 粗略估算高度 (行)
    uint64_t keySalt    = 0;              // 影响所有 key (模拟内容变化)
    /// 按条目身份 id 给出粗略高度 (未设置时用 quickRows);
    /// 用于构造"估算与实测严重不符"的场景 (估算只应影响滚动条长度)
    std::function<size_t(size_t id)> quickOf;

    /// 视口尺寸 (渲染一帧时使用; 变更即模拟终端 resize)
    int width  = 60;
    int height = 20;

    std::vector<size_t> ids;      // 条目身份 (与内容绑定; 前插时同步头插)
    size_t              nextId = 0;

    int buildCalls  = 0; // buildItem 调用次数
    int keyCalls    = 0; // itemKey 调用次数
    int quickCalls  = 0; // quickHeight 调用次数
    size_t maxBuilt = 0; // 构建过的最大索引 (下游是否被扫过)

    std::shared_ptr<LazyScrollable> scrollable;

    ViewFixture(size_t itemCount, size_t height = 1, size_t quick = 1) :
        count(itemCount),
        itemHeight(height),
        quickRows(quick) {
        ids.resize(count);
        for (size_t i = 0; i < count; ++i) {
            ids[i] = i;
        }
        nextId = count;
        scrollable = std::make_shared<LazyScrollable>(
            [this] {
                return count;
            },
            [this](size_t i) {
                ++keyCalls;
                return 0x1000ULL + i + keySalt;
            },
            [this](size_t i, int) {
                ++quickCalls;
                const size_t id = (i < ids.size()) ? ids[i] : 0;
                return quickOf ? quickOf(id) : quickRows;
            },
            [this](size_t i) {
                ++buildCalls;
                maxBuilt = std::max(maxBuilt, i);
                // 内容由**身份 id** 决定 (与索引无关): 前插时 id 随条目一起后移,
                // 因此"视口画面不变"可以直接逐行比较 (与真实消息按内容对齐一致)
                const std::string label = (i < ids.size()) ? std::to_string(ids[i]) : "?";
                ftxui::Elements    rows;
                for (size_t r = 0; r < std::max<size_t>(1, itemHeight); ++r) {
                    // 逐行带行号: 视口顶行/底行能被解析成 (条目 id, 条目内行号),
                    // 用于断言"定位只用实测高度"
                    rows.push_back(ftxui::text(
                        r == 0 ? ("item " + label + " mark=" + label)
                               : ("item " + label + " mark=" + label + " row " + std::to_string(r))
                    ));
                }
                LazyBuiltItem out;
                out.element     = ftxui::vbox(std::move(rows));
                out.sourceBytes = 64;
                return out;
            },
            LazyScrollable::CacheBudget{256, 4 * 1024 * 1024, 1024},
            nullptr
        );
    }

    /// 追加 n 条 (尾部新增, 吸附状态下视口跟随到底)
    void appendItems(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            ids.push_back(nextId++);
        }
        count += n;
    }

    /// 模拟历史分页: 头部插入 n 条 (状态已前插 + 组件并行数组同步平移)
    void prepend(size_t n) {
        std::vector<size_t> newIds(n);
        for (size_t i = 0; i < n; ++i) {
            newIds[i] = nextId++;
        }
        ids.insert(ids.begin(), newIds.begin(), newIds.end());
        count += n;
        scrollable->notifyPrepended(n);
    }

    /// 渲染一帧并返回屏幕文本
    std::string render() {
        auto el = scrollable->Render() | ftxui::flex;
        auto screen = ftxui::Screen::Create(
            ftxui::Dimension::Fixed(width),
            ftxui::Dimension::Fixed(height)
        );
        ftxui::Render(screen, el);
        return screen.ToString();
    }

    /// 在视口内滚一次滚轮
    void wheel(ftxui::Mouse::Button button) {
        ftxui::Mouse m;
        m.button = button;
        m.motion = ftxui::Mouse::Pressed;
        m.x      = 2;
        m.y      = 2;
        (void)scrollable->OnEvent(ftxui::Event::Mouse("", m));
    }

    /// 滚动 n 行 (上滚 n 次 / 下滚 n 次)
    void wheelRows(int rows) {
        for (int i = 0; i < std::abs(rows); ++i) {
            wheel(rows < 0 ? ftxui::Mouse::WheelUp : ftxui::Mouse::WheelDown);
        }
    }

    /// 取渲染帧内容 (去掉最右一列滚动条: 总高度变化会让 thumb 形态变化, 属预期)
    static std::string contentOf(const std::string& screenText) {
        std::string out;
        size_t      start = 0;
        while (start <= screenText.size()) {
            size_t end = screenText.find("\r\n", start);
            if (end == std::string::npos) {
                end = screenText.size();
            }
            std::string row = screenText.substr(start, end - start);
            // 去掉最后一列 (滚动条 gutter): 按 UTF-8 码点整段删除 —— 滚动条字符
            // (┃/╹/╻) 是多字节, 直接 pop_back 只删掉一个字节会留下残缺字节
            size_t cut = row.size();
            while (cut > 0 && (static_cast<unsigned char>(row[cut - 1]) & 0xC0) == 0x80) {
                --cut;
            }
            if (cut > 0) {
                --cut;
            }
            row.resize(cut);
            out += row;
            out += '\n';
            if (end >= screenText.size()) {
                break;
            }
            start = end + 2;
        }
        return out;
    }

    /// 渲染一帧并返回屏幕内容 (不含滚动条列)
    std::string lastFrame() {
        return contentOf(render());
    }

    /// 屏幕上一行的 (条目 id, 条目内行号)
    struct Pos {
        size_t id  = static_cast<size_t>(-1);
        int    row = -1;
    };

    /// 解析夹具渲染行 ("item <id> mark=<id>[ row <r>]"); 非夹具行返回 {npos, -1}
    static Pos parseLine(const std::string& line) {
        const size_t p = line.find("item ");
        if (p == std::string::npos) {
            return Pos{};
        }
        const size_t b  = p + 5;
        const size_t e  = line.find(' ', b);
        if (e == std::string::npos) {
            return Pos{};
        }
        Pos pos;
        pos.id         = static_cast<size_t>(std::stoul(line.substr(b, e - b)));
        const size_t r = line.find(" row ", e);
        pos.row        = (r == std::string::npos) ? 0 : std::stoi(line.substr(r + 5));
        return pos;
    }

    /// 视口首行 (顶行) 的 (条目 id, 行号)
    Pos topPos() {
        return parseLine(firstLineOf(lastFrame()));
    }

    /// 视口末行 (底行) 的 (条目 id, 行号)
    Pos bottomPos() {
        return parseLine(lastLineOf(lastFrame()));
    }

    /// 取多行文本的首行 (去掉末尾换行)
    static std::string firstLineOf(const std::string& s) {
        const size_t nl = s.find('\n');
        return (nl == std::string::npos) ? s : s.substr(0, nl);
    }

    /// 取多行文本的末行 (去掉末尾换行; 空行时返回空前一行)
    static std::string lastLineOf(const std::string& s) {
        std::string t = s;
        while (!t.empty() && t.back() == '\n') {
            t.pop_back();
        }
        const size_t nl = t.rfind('\n');
        return (nl == std::string::npos) ? t : t.substr(nl + 1);
    }
};

} // namespace

TestResult testTuiLazyView() {
    // ---------------- 每帧成本与列表长度无关 ----------------
    {
        ViewFixture f(2000, 1);
        f.render();
        // 吸附底部: 首帧只构建/测量尾部一屏内的条目 (不从头扫到列表尾部之前)
        XX_TEST_EXPECT_TRUE(f.buildCalls <= static_cast<int>(kHeight) + 16);
        XX_TEST_EXPECT_TRUE(f.maxBuilt >= f.count - 16);
        XX_TEST_EXPECT_TRUE(f.lastFrame().find("mark=1999") != std::string::npos);

        // 后续帧 (无滚动): 基本不再新增构建; key 校验也只覆盖窗口 ± 预取带
        const int buildsAfterFirst = f.buildCalls;
        f.keyCalls = 0;
        f.quickCalls = 0;
        f.render();
        XX_TEST_EXPECT_TRUE(f.buildCalls <= buildsAfterFirst + 2);
        XX_TEST_EXPECT_TRUE(f.keyCalls <= static_cast<int>(kHeight) + 32);
        XX_TEST_EXPECT_EQ(f.quickCalls, 0); // 已填充的高度会缓存, 不重复估算

        // 每帧成本与列表长度无关: 2000 条与 5000 条的首帧构建次数同量级
        ViewFixture huge(5000, 1);
        huge.render();
        XX_TEST_EXPECT_TRUE(huge.buildCalls <= static_cast<int>(kHeight) + 16);
        XX_TEST_EXPECT_TRUE(
            std::abs(huge.buildCalls - f.buildCalls) <= static_cast<int>(kHeight)
        );

        // 上滚若干行: 只构建新进入视口的条目
        ViewFixture g(2000, 1);
        g.render();
        g.scrollable->setStickToBottom(false);
        g.buildCalls = 0;
        g.wheelRows(-5);
        g.render();
        XX_TEST_EXPECT_TRUE(g.buildCalls <= 16);
        XX_TEST_EXPECT_TRUE(g.scrollable->scrollOffset() > 0);
    }

    // ---------------- 前插稳定 ----------------
    {
        // 粗略高度与实测一致 (估算无偏差): 本场景只考察前插锚定本身
        ViewFixture f(300, 2, 2);
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-7); // 上滚 7 行 (解除吸附)
        f.render();
        const int         offsetBefore = f.scrollable->scrollOffset();
        const int         totalBefore  = f.scrollable->totalHeight();
        const std::string before       = f.lastFrame();

        // 模拟历史分页: 头部插入 20 条
        f.prepend(20);
        f.render();
        const int offsetAfter = f.scrollable->scrollOffset();
        const int totalAfter  = f.scrollable->totalHeight();
        // 偏移与总高度等量增长 (视口内容不动)
        XX_TEST_EXPECT_EQ(offsetAfter - offsetBefore, totalAfter - totalBefore);
        XX_TEST_EXPECT_TRUE(offsetAfter > offsetBefore);

        // 视口画面 (逐行, 不含滚动条列) 保持不变
        const std::string after = f.lastFrame();
        XX_TEST_EXPECT_EQ(before, after);
    }

    // ---------------- 实测高度精确 ----------------
    {
        ViewFixture f(40, 3);
        f.scrollable->setStickToBottom(false);
        // 逐行滚到底, 让所有条目都进入过视口
        for (int i = 0; i < 400; ++i) {
            f.render();
            if (f.scrollable->scrollOffset()
                >= std::max(0, f.scrollable->totalHeight() - f.scrollable->viewportHeight())) {
                break;
            }
            f.wheelRows(1);
        }
        f.render();
        // 每条实测 itemHeight 行: 总高度 = 各条目实测高度之和
        XX_TEST_EXPECT_EQ(
            f.scrollable->totalHeight(),
            static_cast<int>(f.itemHeight) * static_cast<int>(f.count)
        );
        XX_TEST_EXPECT_EQ(
            f.scrollable->scrollOffset(),
            std::max(0, f.scrollable->totalHeight() - f.scrollable->viewportHeight())
        );
    }

    // ---------------- 滚动边界 ----------------
    {
        ViewFixture f(60, 2);
        f.render();
        // 上滚到顶: 偏移归零, 第一条可见
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-400);
        f.render();
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), 0);
        XX_TEST_EXPECT_TRUE(f.lastFrame().find("mark=0") != std::string::npos);
        // 下滚到底: 恢复吸附, 最后一条可见
        f.wheelRows(800);
        f.render();
        XX_TEST_EXPECT_TRUE(f.scrollable->isStickToBottom());
        XX_TEST_EXPECT_TRUE(f.lastFrame().find("mark=59") != std::string::npos);
        // 吸附状态下内容追加: 视口继续跟随底部
        f.appendItems(5);
        f.render();
        XX_TEST_EXPECT_TRUE(f.lastFrame().find("mark=64") != std::string::npos);
    }

    // ---------------- 估算偏差不影响视口内容 ----------------
    {
        // 粗略高度少报 10 倍 (每条 1 行, 实测 10 行): 吸附底部时尾部必须可见
        ViewFixture f(60, 10, 1);
        f.render();
        const std::string frame = f.render();
        XX_TEST_EXPECT_TRUE(frame.find("mark=59") != std::string::npos);
        // 视口无空白: 至少渲染了若干条目标记
        size_t visible = 0;
        for (size_t i = 0; i < 60; ++i) {
            if (frame.find("mark=" + std::to_string(i) + " ") != std::string::npos) {
                ++visible;
            }
        }
        XX_TEST_EXPECT_GE(visible, size_t{2});

        // 粗略高度多报 10 倍 (每条 100 行, 实测 2 行): 尾部同样必须可见
        ViewFixture g(60, 2, 100);
        g.render();
        const std::string gframe = g.render();
        XX_TEST_EXPECT_TRUE(gframe.find("mark=59") != std::string::npos);
        XX_TEST_EXPECT_TRUE(gframe.find("mark=55") != std::string::npos);

        // 估算只影响滚动条: 同一内容在两种离谱估算下 (少报 1/多报 100), 视口画面
        // 逐行一致, 而总高度 (滚动条长度) 明显不同
        ViewFixture a(60, 4, 1);
        ViewFixture b(60, 4, 100);
        a.render();
        b.render();
        XX_TEST_EXPECT_EQ(a.lastFrame(), b.lastFrame());
        XX_TEST_EXPECT_TRUE(a.scrollable->totalHeight() != b.scrollable->totalHeight());
    }

    // ---------------- key 变化触发重建 (窗口内) ----------------
    {
        ViewFixture f(30, 2);
        f.render();
        const int buildsBefore = f.buildCalls;
        f.keySalt = 0x5000; // 全部条目 key 变化
        f.render();
        XX_TEST_EXPECT_TRUE(f.buildCalls > buildsBefore); // 窗口内条目已重建
        XX_TEST_EXPECT_TRUE(f.lastFrame().find("mark=29") != std::string::npos);
    }

    // ==================== 锚点即主状态 (方案 §3.4) ====================
    //
    // 三个不变量:
    // ① 视口内位置只由锚点 (条目索引 + 行偏移) 与实测高度决定;
    //    视口上方未实测条目的粗略估算只影响 scrollOffset()/滚动条长度
    // ② 测量即渲染: 条目高度来自它自己的布局结果 (本夹具为固定 itemHeight 行)
    // ③ 每帧成本与列表长度无关: 只处理锚点到视口底部这一段

    // ---- 顶行严格由锚点决定 (逐行滚动不跳行/不重复) ----
    {
        ViewFixture f(200, 4, 4); // 每条 4 行, 估算==实测
        f.width  = 60;
        f.height = 20;
        f.render();
        const int bottom = std::max(0, f.scrollable->totalHeight() - f.scrollable->viewportHeight());
        // 吸附底部: 派生偏移 = 底部边界, 且视口末行就是最后一条的最后一行
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), bottom);
        XX_TEST_EXPECT_EQ(f.bottomPos().id, f.count - 1);
        XX_TEST_EXPECT_EQ(f.bottomPos().row, static_cast<int>(f.itemHeight) - 1);

        // 上滚 6 行 (跨条目): 顶行严格上移 6 行, 派生偏移同步减 6
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-6);
        f.render();
        const int s1 = bottom - 6;
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), s1);
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), static_cast<size_t>(s1 / 4));
        XX_TEST_EXPECT_EQ(f.scrollable->anchorRow(), s1 % 4);
        XX_TEST_EXPECT_EQ(f.topPos().id, static_cast<size_t>(s1 / 4));
        XX_TEST_EXPECT_EQ(f.topPos().row, s1 % 4);

        // 回到顶部后逐行下滚: 每一帧顶行都必须严格等于"初始行 + 滚动行数"
        f.wheelRows(-1000);
        f.render();
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), 0);
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{0});
        XX_TEST_EXPECT_EQ(f.topPos().row, 0);

        int expect = 0;
        for (int step = 0; step < 1000; ++step) {
            f.render();
            const auto p = f.topPos();
            XX_TEST_EXPECT_EQ(p.id, static_cast<size_t>(expect / 4));
            XX_TEST_EXPECT_EQ(p.row, expect % 4);
            XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), expect);
            if (f.scrollable->scrollOffset() + f.scrollable->viewportHeight()
                >= f.scrollable->totalHeight()) {
                break; // 已到底 (再下滚只会恢复吸附)
            }
            f.wheelRows(1);
            ++expect;
        }
        XX_TEST_EXPECT_EQ(
            expect,
            std::max(0, f.scrollable->totalHeight() - f.scrollable->viewportHeight())
        );
    }

    // ---- 定位只用实测高度: 估算与实测差 4 倍时位置仍严格正确 ----
    {
        // 每条实测 4 行, 粗略估算全部报 1 行 (少报 4 倍): 视口内的位置不得受估算影响
        ViewFixture f(120, 4, 1);
        f.render();
        // 尾部窗口 (20 行): 实测 5 条 -> 条目 115..119, 顶行 = 115 的第 0 行
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), size_t{115});
        XX_TEST_EXPECT_EQ(f.scrollable->anchorRow(), 0);
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{115});
        XX_TEST_EXPECT_EQ(f.topPos().row, 0);
        XX_TEST_EXPECT_EQ(f.bottomPos().id, size_t{119});
        XX_TEST_EXPECT_EQ(f.bottomPos().row, 3);
        // 总高度仍按估算 (115 条 x 1 + 实测 5 条 x 4 = 135) —— 只影响滚动条
        XX_TEST_EXPECT_EQ(f.scrollable->totalHeight(), 115 * 1 + 5 * 4);

        // 上滚 10 行: 顶行 = 全局行 460 - 10 = 450 -> 条目 112 的第 2 行
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-10);
        f.render();
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{112});
        XX_TEST_EXPECT_EQ(f.topPos().row, 2);
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), size_t{112});
        XX_TEST_EXPECT_EQ(f.scrollable->anchorRow(), 2);
        // 视口末行 = 450 + 19 = 469 -> 条目 117 的第 1 行
        XX_TEST_EXPECT_EQ(f.bottomPos().id, size_t{117});
        XX_TEST_EXPECT_EQ(f.bottomPos().row, 1);
    }

    // ---- 每帧成本与列表长度/视口上方条数无关 ----
    {
        ViewFixture f(5000, 1, 1);
        f.buildCalls = 0;
        f.render();
        XX_TEST_EXPECT_TRUE(f.buildCalls <= static_cast<int>(f.height) + 8);

        // 一次落实 100 行的上滚: 只构建 100 条 (与 5000 条列表长度无关)
        f.scrollable->setStickToBottom(false);
        f.buildCalls = 0;
        f.keyCalls   = 0;
        f.wheelRows(-100);
        f.render();
        XX_TEST_EXPECT_TRUE(f.buildCalls <= 105);
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), 5000 - 20 - 100);

        // 稳态帧 (无滚动): 不构建、不估算; key 校验只覆盖窗口 ± 预取带
        const int builds = f.buildCalls;
        f.quickCalls     = 0;
        f.keyCalls       = 0;
        f.render();
        XX_TEST_EXPECT_EQ(f.buildCalls, builds);
        XX_TEST_EXPECT_EQ(f.quickCalls, 0); // 视口上方条目本帧零成本 (不估算/不构建)
        XX_TEST_EXPECT_TRUE(f.keyCalls <= static_cast<int>(f.height) + 32);

        // 上滚 1 行: 只新增跨入视口上方那一条的构建
        f.buildCalls = 0;
        f.wheelRows(-1);
        f.render();
        XX_TEST_EXPECT_TRUE(f.buildCalls <= 1);
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset(), 5000 - 20 - 101);
    }

    // ---- 前插零校正: 新增区估算严重偏离实测时画面仍逐行不变 ----
    {
        ViewFixture f(50, 3, 3);
        // 前插区 (id >= 50) 估算 40 行, 实测 3 行: 估算只影响滚动条长度
        f.quickOf = [](size_t id) {
            return id < 50 ? 3 : 40;
        };
        f.render(); // 首帧: 吸附底部 (锚点 = 尾部窗口起点)
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-11);
        f.render();
        const std::string before       = f.lastFrame();
        const int         offsetBefore = f.scrollable->scrollOffset();
        const size_t      anchorBefore = f.scrollable->anchorIndex();
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{39});
        XX_TEST_EXPECT_EQ(f.topPos().row, 2);
        XX_TEST_EXPECT_EQ(anchorBefore, size_t{39});

        // 头部前插 10 条 (ids 50..59): 锚点索引平移 10, 顶行内容不变
        f.prepend(10);
        f.render();
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), anchorBefore + 10);
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{39});
        XX_TEST_EXPECT_EQ(f.topPos().row, 2);
        // 视口逐行画面完全不变 (前插发生在锚点上方, 且没有任何偏移校正)
        XX_TEST_EXPECT_EQ(before, f.lastFrame());
        // 派生偏移只按新增区估算总行数增加 (滚动条), 与实测无关
        XX_TEST_EXPECT_EQ(f.scrollable->scrollOffset() - offsetBefore, static_cast<int>(10 * 40));
        // 继续上滚 3 行: 跨入上一条目 (id 38 的最后一行), 位置仍严格
        f.wheelRows(-3);
        f.render();
        XX_TEST_EXPECT_EQ(f.topPos().id, size_t{38});
        XX_TEST_EXPECT_EQ(f.topPos().row, 2);
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), size_t{48});
        XX_TEST_EXPECT_EQ(f.scrollable->anchorRow(), 2);
    }

    // ---- 终端宽度变化: 锚点条目不变 (行偏移按新高度夹取) ----
    {
        ViewFixture f(100, 3, 3);
        f.render();
        f.scrollable->setStickToBottom(false);
        f.wheelRows(-30);
        f.render();
        const size_t idxBefore = f.scrollable->anchorIndex();
        const int    rowBefore = f.scrollable->anchorRow();
        const auto   posBefore = f.topPos();
        XX_TEST_EXPECT_EQ(posBefore.id, idxBefore);
        XX_TEST_EXPECT_EQ(posBefore.row, rowBefore);

        // 宽度变化: 高度全部回到粗略估算 (实测失效), 但锚点条目保持不变
        f.width      = 40;
        f.quickCalls = 0;
        f.render();
        XX_TEST_EXPECT_EQ(f.scrollable->anchorIndex(), idxBefore);
        XX_TEST_EXPECT_EQ(f.topPos().id, idxBefore);
        XX_TEST_EXPECT_TRUE(f.quickCalls >= static_cast<int>(f.count)); // 整体重估
    }

    return TestResult{g_tui_lazy_view_passed, g_tui_lazy_view_failed};
}

} // namespace test
} // namespace agentxx
