#pragma once

/// 命中区域登记表 (TUI 鼠标点击命中检测的统一机制)
///
/// 背景: 组件内的可点击控件此前各自持有一个 `ftxui::Box` 成员, 并在 OnEvent 里
/// 逐个 `box.Contain(...)` 判定。这种写法有两个问题:
/// - 样板代码多: 每加一个按钮就要加一个成员变量 + 一段命中分支 + 一处 reflect
/// - 残留命中区: `reflect` 只在元素参与布局 (SetBox) 时写回坐标, 未渲染的元素
///   命中盒会保留上一帧的值 —— 于是"当前没显示的按钮"仍可能吃掉那块区域的点击
///   (更糟的是默认构造的 `ftxui::Box{}` **不是空区域**: 它的 4 个分量都是 0,
///   `Contain(0, 0)` 为真, 因此清空命中区必须用 [kNoBox])
///
/// 本表用一条规则消除这两个问题: **每帧开头清空, 只有真正渲染出来的元素才登记**。
/// 未渲染 = 未登记 = 不参与命中检测。
///
/// 用法 (组件内):
/// ```c++
/// ftxui::Element OnRender() override {
///     hits_.beginFrame();                                 // 帧首清空 (必需)
///     return ftxui::hbox({
///         hits_.add(ftxui::text("设置"), "settings"),      // 登记并把 Box 交给 reflect
///         hits_.add(ftxui::text("退出"), "quit"),
///     });
/// }
///
/// bool OnEvent(ftxui::Event event) override {
///     if (const auto* hit = hits_.findClick(event.mouse())) {
///         openBy(hit->payload);                           // payload 语义由调用方定义
///         return true;
///     }
///     return false;
/// }
/// ```
///
/// 相关:
/// - 渲染风格 helper 见 [surface.h](/agent/client/include/agentxx-client/io/tui/surface.h)
/// - 声明式条目列表 (菜单/设置项) 见
///   [ui_action_list.h](/agent/client/include/agentxx-client/io/tui/framework/ui_action_list.h)
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentxx {
namespace client {

/// 空命中区域 (x_min > x_max 且 y_min > y_max, `Contain` 恒为 false)
///
/// 注意: 不要用默认构造的 `ftxui::Box{}` 表示"无命中区域" —— 它的 4 个分量
/// 都是 0, 会把屏幕左上角 (0, 0) 当成命中。
inline constexpr ftxui::Box kNoBox{0, -1, 0, -1};

/// 通用命中载荷 (标识 + 附加参数)
struct UiHitInfo {
    /// 命中标识 (同帧内应唯一; 命中后按它判断"点了哪个")
    std::string id;
    /// 附加参数 (调用方自定义语义, 如 JSON 文本 / 行下标)
    std::string arg;
};

/// 命中登记表: 帧首清空 + 渲染时登记 + 事件时查询
///
/// - Payload 为命中时取回的上下文 (如 [UiHitInfo] 或插件按钮归属信息),
///   按值存放, 命中后可直接读取
/// - `add()` 返回的元素已带 `reflect`, 布局完成后 Box 即为该元素的屏幕区域;
///   元素若被父级裁剪/滚出视口, Box 会被收敛为空 (FTXUI 的 reflect 与
///   screen stencil 求交), 因此视口外子项自然不会命中
/// - Box 由 shared_ptr 持有, vector 扩容不会使已登记项的 Box 失效
template <class Payload>
class UiHitRegistry {
public:

    struct Entry {
        Payload                     payload;
        std::shared_ptr<ftxui::Box> box;
    };

    /// 帧首清空 (每帧渲染入口调用一次): 未在本帧重新登记的项不再参与命中检测
    void beginFrame() {
        entries_.clear();
    }

    /// 清空 (等价于 beginFrame; 供事件处理等场景显式清理)
    void clear() {
        entries_.clear();
    }

    /// 登记可命中元素: 返回的元素已附加 reflect, 布局后写入该元素的屏幕区域
    /// - `args...` 用于构造 Payload (聚合初始化)
    template <class... Args>
    ftxui::Element add(ftxui::Element element, Args&&... args) {
        entries_.push_back(Entry{
            Payload{std::forward<Args>(args)...},
            std::make_shared<ftxui::Box>(kNoBox),
        });
        return std::move(element) | ftxui::reflect(*entries_.back().box);
    }

    /// 坐标命中查询 (未布局或已被裁剪的项返回空 Box, 自然不命中)
    const Entry* find(int x, int y) const {
        for (const auto& entry : entries_) {
            if (entry.box->IsEmpty()) {
                continue;
            }
            if (entry.box->Contain(x, y)) {
                return &entry;
            }
        }
        return nullptr;
    }

    /// 左键释放 (一次点击完成) 且命中时返回命中项, 否则 nullptr
    const Entry* findClick(const ftxui::Mouse& mouse) const {
        if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Released) {
            return nullptr;
        }
        return find(mouse.x, mouse.y);
    }

    const std::vector<Entry>& entries() const {
        return entries_;
    }

    size_t size() const {
        return entries_.size();
    }

    bool empty() const {
        return entries_.empty();
    }

protected:

    std::vector<Entry> entries_;
};

/// 通用命中表 (载荷 = 标识 + 参数)
using UiHitMap = UiHitRegistry<UiHitInfo>;

} // namespace client
} // namespace agentxx
