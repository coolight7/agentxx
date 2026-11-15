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

第二步（同日完成，client 表全部上桥）:
- 展示项：状态栏项 / 侧边栏面板 / Info 段落（三个方法族，句柄是整数 token）
- 交互：斜杠命令（`execute` 同步返回动作 JSON）、按钮动作绑定、全局快捷键、
  客户端定时器、区域可见性查询
- 回调登记：时刻活到引擎析构的 `JsClientCallback` + 实例侧 token 表 `clientHandles`
- 示例与测试：`example_js` client 分支扩到七类面；`client_plugins` 第 34 段再增
  12 条断言

不做:
- 客户端侧脚本之间的隔离策略变化（仍是一份脚本一个 JSContext）
- “回调里 await”的异步桥（当前 150 ms 同步桥已覆盖渲染与命令这类毫秒级需求；
  耗时活走订阅事件 + `updateToolDecor`）

## 状态

已完成（2026-11-13）:
- 引擎 `Side` 分流 + client 入口 + client 桥子集 + 渲染同步桥
- 第二批桥：展示项（状态栏 / 面板 / Info 段落）、命令、动作绑定、定时器、快捷键
  + 句柄 token + 通用回调登记与同步/投递两条调用路径
- `example_js` client 入口与脚本分支、清单声明；脚本演示了工具行渲染与其余七类面
- `client_plugins` 第 34 段（真实插件加载 + 渲染回调 + 展示项/命令/快捷键/动作/定时器
  断言 + 注销/查询入口，模块从 774 增长到 817）、`plugins` 模块回归无关
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
5. **client 表的句柄换成整数 token**：C 表的句柄是主程序侧不透明指针，JS 的 number
   存不下。实例里维护 `clientHandles`（token → {类别, 指针, id}），`0` 表示不支持或注册
   失败；更新/注销先校类别，不符返回 false（不抛异常，脚本不必写 try）。
6. **两类回调、两种语义**：命令 `execute` 与渲染回调走同步桥（150 ms 上限）；
   动作/定时器/快捷键回调只投递（主程序不等结果，回调里抛异常只记日志）。
   回调登记（`JsClientCallback`）不含 `JSValue`，与路由键（`cmd:` / `act:` / `timer:` /
   `keybind:`）一起放在引擎与实例上，摘注册不就地释放。
7. **不做 agent 侧任何改动**：agent 侧桥、工具、挂载、会话运行时全部保持原样，
   client 分支代码独立（宁可少量重复也不动已验证的 agent 路径）。

## 结论

JS 脚本现在可以作为 client 端插件运行，client 六张表（ui / session / wire / self / timer /
keybind）的能力面都已在桥上：工具行特化渲染、状态栏项、面板、Info 段落、斜杠命令、
按钮动作、定时器、快捷键都在脚本里注册并回调，agent 侧不参与。

剩下的可选项是“异步回调桥”（让回调里能 await）与客户端脚本之间的隔离策略变化，
两个都不是当前需求，已记录在上面“不做”里。
