#pragma once

#include "ftxui/component/component_base.hpp"
#include "ftxui/dom/elements.hpp"
#include <memory>

/// 带局部重绘缓存的组件基类 (参考 Flutter 的 markNeedsBuild / dirty 标记机制)
///
/// 设计:
/// - 子类实现 build() 构建 Element 树 (类似 Flutter 的 Widget.build)
/// - OnRender() 仅在 dirty 或宽度变化时调用 build(), 否则复用缓存
/// - 外部数据变更时调用 markDirty() 标记需重建
///
/// 相比原实现中分散的 messageCache_ / logLineCache_ / signature 等多套缓存,
/// 本基类提供统一的缓存失效机制, 各组件只需关注自身 build 逻辑。
///
/// 用法:
///   class MyWidget : public DirtyComponent {
///       ftxui::Element build() override {
///           return ftxui::text("hello") | ftxui::bold;
///       }
///   };
class DirtyComponent : public ftxui::ComponentBase {
public:

    /// 标记本组件需要重建 (下帧 OnRender 时调用 build())
    void markDirty() {
        dirty_ = true;
    }

    bool isDirty() const {
        return dirty_;
    }

    ftxui::Element OnRender() override {
        const int w = box_.x_max - box_.x_min + 1;
        if (!dirty_ && cachedElement_ && w == cachedWidth_) {
            return cachedElement_;
        }
        cachedElement_ = build();
        cachedWidth_   = w;
        dirty_         = false;
        return cachedElement_;
    }

    /// 获取本组件上一帧的渲染区域 (由 reflect 填充)
    const ftxui::Box& box() const {
        return box_;
    }

protected:

    /// 子类实现: 构建本组件的 Element 树 (仅在 dirty 时调用)
    virtual ftxui::Element build() = 0;

    /// 本组件渲染区域 (子类在 build() 中用 reflect(box_) 填充, 供 OnEvent 命中检测)
    ftxui::Box box_;

private:

    bool           dirty_         = true;
    ftxui::Element cachedElement_ = nullptr;
    int            cachedWidth_   = -1;
};
