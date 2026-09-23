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
#include "agentxx-client/io/tui/framework/owned_reflect.h"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/box.hpp"
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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

/// 可命中区域的类型 (调用方按类型决定命中后做什么)
enum class UiHitRegionKind : uint8_t {
    /// 派发动作: `id` = 动作 id, `arg` = 参数 JSON
    Action = 0,
    /// 折叠标题: `id` = 组件 id (调用方翻转宿主维护的展开状态后重绘)
    Collapse = 1,
    /// 表单控件: `id` = 控件 id, `sub` = 子序号 (候选值下标 / 步进按钮 / 输入框)
    Form = 2,
    /// 表单提交或取消: `id` = "__submit" / "__cancel"
    FormSubmit = 3,
};

/// 元素内的可命中子区域 (局部坐标: 相对所属元素的左上角)
///
/// 用途: 一个元素内部可能有多个可点位置 (表格单元格、树节点、折叠标题、
/// 控件里的某个按钮)。整块登记只能判断"点到这个元素了", 无法区分点在哪一处;
/// 本结构把"元素内矩形 + 载荷"一起登记, 命中时返回具体子区域与局部坐标。
///
/// 坐标口径: `x/y` 相对所属元素左上角; `w <= 0` 表示"从 x 起直到元素右边界"
/// (适合整行可点的场景, 不必预先知道元素宽度)。
struct UiHitRegion {
    int             x = 0;
    int             y = 0;
    int             w = 0;
    int             h = 1;
    /// 区域类型 (决定命中后的处理方式)
    UiHitRegionKind kind = UiHitRegionKind::Action;
    /// 区域标识 (动作 id / 控件 id / 单元格键)
    std::string     id;
    /// 附加参数 (JSON 文本等, 原样回传)
    std::string     arg;
    /// 子序号 (控件内部下标: 候选值下标 / 加减按钮等; 非控件场景为 0)
    int sub = 0;
    /// 归属插件名 (面板/段落/overlay 渲染时填入; 空 = 由调用方按上下文补齐)
    std::string plugin;
    /// 归属 id (面板 id / 段落 id / tool_call_id / "__overlay")
    std::string ownerId;

    /// 局部坐标是否落在本区域 (w <= 0 视为延伸到右边界)
    bool contains(int localX, int localY) const {
        if (localY < y || localY > y + ((h > 0) ? (h - 1) : 0)) {
            return false;
        }
        if (localX < x) {
            return false;
        }
        return (w <= 0) || (localX <= x + w - 1);
    }

    /// 按偏移平移 (容器组合子项时使用)
    UiHitRegion offsetBy(int dx, int dy) const {
        UiHitRegion out = *this;
        out.x += dx;
        out.y += dy;
        return out;
    }
};

/// 命中登记表: 帧首清空 + 渲染时登记 + 事件时查询
///
/// - Payload 为命中时取回的上下文 (如 [UiHitInfo] 或插件按钮归属信息),
///   按值存放, 命中后可直接读取
/// - `add()` 返回的元素已带 `reflect`, 布局完成后 Box 即为该元素的屏幕区域;
///   元素若被父级裁剪/滚出视口, Box 会被收敛为空 (FTXUI 的 reflect 与
///   screen stencil 求交), 因此视口外子项自然不会命中
/// - Box 由 shared_ptr 持有, vector 扩容不会使已登记项的 Box 失效
template<class Payload>
class UiHitRegistry {
public:

    struct Entry {
        Payload                     payload;
        std::shared_ptr<ftxui::Box> box;
        /// 元素内的可命中子区域 (局部坐标; 空 = 整块可点, 命中返回区域为空)
        std::vector<UiHitRegion> regions;
    };

    /// 命中查询结果 (命中项 + 命中的子区域与局部坐标)
    struct FindResult {
        const Entry*        entry  = nullptr;
        /// 命中的子区域 (整块登记时为 nullptr)
        const UiHitRegion*  region = nullptr;
        /// 命中点相对元素左上角的局部坐标
        int                 localX = 0;
        int                 localY = 0;
        /// 命中的动作标识 (有子区域时取区域 id, 否则为空)
        std::string_view id() const {
            return (region != nullptr) ? std::string_view{region->id} : std::string_view{};
        }
        /// 命中的附加参数 (有子区域时取区域参数, 否则为空)
        std::string_view arg() const {
            return (region != nullptr) ? std::string_view{region->arg} : std::string_view{};
        }
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
    ///
    /// 返回的元素同时**自持**该 Box ([OwnedReflect]): 元素被搬进滚动容器/缓存跨帧
    /// 存活时, 即使登记表已在本帧开头清空 (Box 从表中移除), 元素后续布局也不会写
    /// 已释放内存; 命中检测仍读同一个 Box (见 [find] / [findRegion])。
    ///
    /// - `args...` 用于构造 Payload (聚合初始化)
    template<class... Args>
    ftxui::Element add(ftxui::Element element, Args&&... args) {
        entries_.push_back(Entry{
            Payload{std::forward<Args>(args)...},
            std::make_shared<ftxui::Box>(kNoBox),
            {},
        });
        return std::make_shared<OwnedReflect>(std::move(element), entries_.back().box);
    }

    /// 登记"元素 + 元素内若干可命中子区域": 返回的元素已附加 reflect
    /// - 子区域坐标相对该元素左上角 (见 [UiHitRegion]); 命中时返回 (payload, 区域, 局部坐标)
    /// - Box 归属说明同 [add]
    ftxui::Element addRegions(ftxui::Element element, Payload payload, std::vector<UiHitRegion> regions) {
        entries_.push_back(Entry{
            std::move(payload),
            std::make_shared<ftxui::Box>(kNoBox),
            std::move(regions),
        });
        return std::make_shared<OwnedReflect>(std::move(element), entries_.back().box);
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

    /// 坐标命中查询 (含元素内子区域与局部坐标)
    /// - 未布局/被裁剪的元素不参与; 元素命中但无子区域或未命中任何子区域时
    ///   `region` 为空 (调用方按"整块命中"处理), 命中时 `localX/localY` 仍有效
    FindResult findRegion(int x, int y) const {
        FindResult result;
        for (const auto& entry : entries_) {
            if (entry.box->IsEmpty()) {
                continue;
            }
            if (!entry.box->Contain(x, y)) {
                continue;
            }
            result.entry  = &entry;
            result.localX = x - entry.box->x_min;
            result.localY = y - entry.box->y_min;
            for (const auto& region : entry.regions) {
                if (region.contains(result.localX, result.localY)) {
                    result.region = &region;
                    break;
                }
            }
            return result;
        }
        return result;
    }

    /// 在登记项内查询命中的子区域 (不关心是哪个元素; 供调用方已经自行定位了
    /// 命中元素 (如滚动容器经 [Scrollable::hitTestItem] 定位) 的场景)
    static const UiHitRegion* matchRegion(
        const std::vector<UiHitRegion>& regions,
        int                             localX,
        int                             localY
    ) {
        for (const auto& region : regions) {
            if (region.contains(localX, localY)) {
                return &region;
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

    /// 左键释放且命中元素内子区域时返回命中结果 (未命中返回 `entry == nullptr`)
    FindResult findRegionClick(const ftxui::Mouse& mouse) const {
        if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Released) {
            return {};
        }
        return findRegion(mouse.x, mouse.y);
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

/// 在区域列表内按局部坐标查询命中项 (调用方已自行定位到元素时使用, 例如
/// 滚动容器经 [Scrollable::hitTestItem] 得到子项下标与局部坐标后)
/// - 未命中返回 nullptr
inline const UiHitRegion* matchUiHitRegion(
    const std::vector<UiHitRegion>& regions,
    int                             localX,
    int                             localY
) {
    for (const auto& region : regions) {
        if (region.contains(localX, localY)) {
            return &region;
        }
    }
    return nullptr;
}

} // namespace client
} // namespace agentxx
