#pragma once

/// UI 快照夹具 (计划 UI-2 / TST-5)
///
/// 目的: 渲染回归能被自动发现, 而不是靠人眼看输出。做法是把"固定尺寸下的纯文本画面"
/// 与"命中区清单"存成仓库内的基线文件, 测试时逐字符比较; 不一致时打印首个不同的
/// 行/列, 让人一眼看出改了什么。
///
/// - 基线目录: `agent/test/snapshots/ui/` (构建时经 `AGENTXX_UI_SNAPSHOT_DIR` 传入)
/// - 一键更新: 设 `AGENTXX_UPDATE_UI_SNAPSHOTS=1` 运行测试, 基线被重写 (之后人工 review diff)
/// - 覆盖范围由 UI-2 定义: 表格 / 树 / 差异 / Markdown / 表单 / 窄终端 / CJK /
///   超长内容 / 未知组件降级
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {
struct UiRenderResult;
struct UiHitRegion;
} // namespace client
} // namespace agentxx

namespace agentxx {
namespace test {

/// 渲染行模型到固定尺寸字符网格 (宽 w 高 h; 未写入的格子补空格, 每行以 '\n' 结尾)
/// - 每格一个字符: 宽字符只在首格出现, 续格为空格, 因此网格文本与显示宽度一一对应
std::string renderRowsToGrid(const agentxx::client::UiRenderResult& res, int w, int h);

/// 命中区清单序列化 (一行一条, 顺序稳定; 含所属行下标, 坐标为行内局部坐标)
std::string serializeHitRegions(const agentxx::client::UiRenderResult& res);

/// 快照比较结果
struct UiSnapshotCheck {
    /// 与基线一致 (或在更新模式下已写回)
    bool        ok = false;
    /// 本次写回了基线 (更新模式)
    bool        updated = false;
    /// 不一致时的差异说明 (首个不同的行/列 + 两侧内容)
    std::string diff;
};

/// 与基线比较 (基线名 = `name`; 文件为 `{snapshotDir}/{name}.snap`)
/// - 基线缺失: 更新模式下写入并视为通过; 非更新模式下视为失败 (提示如何生成)
UiSnapshotCheck checkUiSnapshot(std::string_view name, const std::string& actual);

/// 基线目录 (未定义 `AGENTXX_UI_SNAPSHOT_DIR` 时为空串)
std::string uiSnapshotDir();

/// 是否处于基线更新模式 (`AGENTXX_UPDATE_UI_SNAPSHOTS=1`)
bool uiSnapshotUpdateMode();

} // namespace test
} // namespace agentxx
