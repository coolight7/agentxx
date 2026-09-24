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
            [this](size_t, int) {
                ++quickCalls;
                return quickRows;
            },
            [this](size_t i) {
                ++buildCalls;
                maxBuilt = std::max(maxBuilt, i);
                // 内容由**身份 id** 决定 (与索引无关): 前插时 id 随条目一起后移,
                // 因此"视口画面不变"可以直接逐行比较 (与真实消息按内容对齐一致)
                const std::string label = (i < ids.size()) ? std::to_string(ids[i]) : "?";
                ftxui::Elements    rows;
                for (size_t r = 0; r < std::max<size_t>(1, itemHeight); ++r) {
                    rows.push_back(ftxui::text(
                        r == 0 ? ("item " + label + " mark=" + label) : std::string{}
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
        auto screen
            = ftxui::Screen::Create(ftxui::Dimension::Fixed(kWidth), ftxui::Dimension::Fixed(kHeight));
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

    return TestResult{g_tui_lazy_view_passed, g_tui_lazy_view_failed};
}

} // namespace test
} // namespace agentxx
