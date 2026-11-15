# client 侧 JS 引擎（JS 脚本可作为 client 端插件运行）

## 目标

让 JS 脚本能作为 **client 侧插件**运行：脚本顶层在客户端执行，可直接使用
`agentxx.client.*` 系列接口（第一步是工具特化渲染 `register_tool_renderer`），
不需要在 agent 侧登记、也不需要跨端 wire 往返。

这是 `resource/history/subagent-tool-parity` 第五步（agent 侧工具渲染表 + 三条 wire
消息）被整体回退后的**正解方向**：渲染器注册面只在 client 侧一处，脚本跑到 client
侧就自然能注册。

## 范围

做（第一步）:
- `agentxx_javascript_engine` 增加 **client 侧入口**（同一动态库双入口），引擎按
  `Side` 区分 agent / client
- client 侧实例模型：无 agent 挂载作用域，一份脚本 = 一个实例
- client 侧桥子集：日志 / 自身信息 / 客户端状态 / toast / 工具装饰 / overlay /
  **工具特化渲染注册** / 会话输入与取消 / 跨端数据 / 广播订阅发布
- io 线程 → JS 线程的**同步回调桥**（渲染回调必须同步返回内容），带超时与
  "JS 线程正在等主程序"时的快速失败
- `example_js` 增加 client 入口与脚本 `agentxx.side` 分支，作为可用示例
- 测试：`client_plugins` 新增一段，加载真实插件产物、断言渲染器已注册并能回调出内容

不做（后续）:
- 状态栏项 / 侧边栏面板 / Info 段落 / 斜杠命令 / 客户端定时器 / 全局快捷键 /
  动作回调绑定（都需要句柄或回调管理，等这一层稳定后按同一套桥补齐）
- 客户端侧脚本之间的隔离策略变化（仍是一份脚本一个 JSContext）

## 状态

已完成（2026-11-13）:
- 引擎 `Side` 分流 + client 入口 + client 桥子集 + 渲染同步桥
- `example_js` client 入口与脚本分支、清单声明
- `client_plugins` 第 30 段（真实插件加载 + 渲染器回调断言）、`plugins` 模块回归
- 文档：`docs/zh-cn/design/plugins.md` §12、`AGENTS.md`

## 关键决策

1. **一个动态库两个入口**：`agentxx_plugin_agent_*` 与 `agentxx_plugin_client_*` 各自
   `new JsEngine(Side)`；引擎内部只有"侧"这一处分支，其余生命周期（线程/运行时/
   实例/定时器）完全共用。
2. **client 侧无挂载作用域**：client 表成员签名里没有 `PluginxxScopeRef`，实例键用
   固定的 `client` 伪作用域（`<脚本名>@client`），`pctx->scope()` 返回 `nullptr`。
3. **渲染回调走同步桥**（`kRenderSyncTimeoutMs = 150`）：
   - 回调在 client io 线程，必须同步给出内容；JS 只在 JS 线程执行；
   - 桥每次请求在堆上建一个结果槽（`shared_ptr`），io 线程超时返回后槽仍由任务持有，
     不会写已销毁的栈对象；
   - JS 线程正处在主程序调用里（`jsHostCallDepth_ > 0`，即它在等 io 线程）时，
     io 线程**立刻失败**（回退默认渲染），避免两边互等；
   - 超时/失败都按"没有自定义内容"处理，界面回退默认渲染，不报错、不阻塞。
4. **渲染注册项的 JS 函数按 token 存在实例里**（`JsPluginCtx::renderCallbacks`），
   注册项本身（`JsRenderReg`）不含 `JSValue` —— 这样它可以被 io 线程以
   `shared_ptr` 持有，释放时机不受"必须在 JS 线程"约束。
5. **不做 agent 侧任何改动**：agent 侧桥、工具、挂载、会话运行时全部保持原样，
   client 分支代码独立（宁可少量重复也不动已验证的 agent 路径）。

## 结论

JS 脚本现在可以作为 client 端插件运行，工具特化渲染由脚本自己注册并回调，
agent 侧不参与。后续把面板/Info/命令/定时器/快捷键按同一套桥补齐即可。
