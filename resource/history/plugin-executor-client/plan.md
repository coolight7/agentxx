# client 插件独立线程（同步桥修复 + 删除开关）

## 目标

client 插件代码也不再跑在 client io 线程上：`ClientPluginManager` **默认**起插件执行线程，
并删掉 `startPluginThread()` 这个可选开关。

## 现状与卡点

阶段 D/E 已把 client 侧六个派发点迁到 `dispatchToPlugin`，但线程默认不启动 —— 原因是
内置 client 插件的 `start`/`stop`（以及回调里的面板/Info/提示更新）用的是 kit 的**同步表助手**
（`clientTableOp` → `tableOpSyncWait`）。表操作在主程序 io 线程执行、完成回调**投回插件执行器**，
而同步等待正把那条线程占住 → 自己等自己，`tableOpSyncWait` 直接拒绝。

## 方案：同步桥回调就地调用（不再投递）

同步等待的调用方线程**阻塞在等这次回调**，所以它的完成回调不能投到别的线程再回来。做法：

1. kit 的 `tableOpSyncWait` 不再直接把业务 callback 交给表成员，而是包成
   **同步桥回调**（`pluginxx/api/sync_bridge.h`：固定标记 + `fn` + `ud`）；
2. 主程序 `OpCore::commit` 识别标记：**在完成线程（io）上就地调用**，不投递到插件执行器；
3. 就地调用只跑 kit 的唤醒体（往共享结构里写状态 + 通知条件变量），不碰插件业务状态；
   插件业务代码仍在插件执行线程上继续。

这样"插件执行线程同步等 io"成为合法方向（plan §4 本来就允许），io 仍然从不阻塞等插件线程。

### 插件执行线程上的准入规则

`tableOpSyncWait` 默认仍在插件执行线程上**拒绝**（agent 侧 `call_tool` / 能力调用可能把工作
体派发到同一条插件线程，同步等会互等）。client 表 op 的工作体只碰 client 注册表、不会派发到
插件执行器，因此 client 的 kit 助手显式放行（`SyncWaitThreadPolicy::allowOnPluginThread`）。

## 分步

| 步 | 内容 |
|---|---|
| 1 | 框架：`api/sync_bridge.h` + `OpCore::commit` 就地调用 + `tableOpSyncWait` 策略参数 |
| 2 | 主程序：`clientTableOp` 放行插件线程；client 文档更新 |
| 3 | `ClientPluginManager` 构造函数/首次加载时起线程，删除 `startPluginThread()` |
| 4 | 测试：`plugin_executor` 增同步桥用例（插件线程上同步等 → 回调在 io 就地调用、不互等）；`client_plugins` 全绿即证明真实 client 插件在插件线程上注册成功 |
| 5 | 文档（AGENTS.md / plugins.md §15.3、§9）+ 全量回归 |

## 边界

- 不动 client 插件的源码：同步助手继续可用（只是变成了"阻塞自己的线程"）。
- 不动 ABI：标记是 kit ↔ 主程序内部约定（同一份 cxx_pluginxx），不跨动态库边界。
- 不做"每插件一条线程"、不把 client 注册表搬到插件线程。
