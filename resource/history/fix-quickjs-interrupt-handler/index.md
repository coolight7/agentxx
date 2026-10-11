# 修复 quickjs 更新后 JS 引擎插件编译失败 (中断处理回调签名变化)
- 难度: C
- 类型: bug修复
- 基于commit: 2f0368d0dc0907fda6e3ad2155380eb480f41cf0
- 时间: 2026-10-11 22:30
- 需求:
```md
请修复编译失败 agentxx_javascript_engine.cpp
error C2664: "void JS_SetInterruptHandler(JSRuntime *,JSInterruptHandler (__cdecl *),void *)":
无法将参数 2 从 "int (__cdecl *)(JSRuntime *,void *)" 转换为 "JSInterruptHandler (__cdecl *)"
```

## 现象与根因

windows-release 编译内置插件 `agentxx_javascript_engine` 时 C2664 报错: 传给
`JS_SetInterruptHandler` 的中断处理回调类型不匹配。

根因是 `agent/third_party/quickjs` (quickjs-ng, submodule, 0.17 开发线) 当天更新到新
提交后, 公开 API 变了: 回调首参数由 `JSRuntime*` 改为 `JSContext*` (正在执行 JS 的上下文)。

- 新头文件: `typedef int JSInterruptHandler(JSContext *ctx, void *opaque);`
- 新实现 (`quickjs.c` 的 `__js_poll_interrupts`): `rt->interrupt_handler(ctx, rt->interrupt_opaque)`
  —— 传的确实是 `JSContext*`, 头文件与实现一致
- 插件里的 `JsEngine::interruptHandler` 仍是旧签名 `(JSRuntime*, void*)`, 于是报错

全项目只有本插件文件直接 `#include "quickjs.h"`, 因此只有本文件受这次 API 变更影响。

## 修改

- `agent/plugins/agentxx_javascript_engine/agentxx_javascript_engine.cpp`
  - `JsEngine::interruptHandler(JSRuntime* rt, void* opaque)` 改为
    `(JSContext* ctx, void* opaque)`, 未使用参数抑制 `(void)rt` 改为 `(void)ctx`,
    并补注释说明首参数含义 (本处只用 `opaque` 里的引擎指针)
  - 超时判定逻辑不变: 距 `taskStart_` 超过 `kTaskTimeoutMs` 返回 1, 解释器抛 "interrupted"

## 验证

- 静态核对: 新 `quickjs.h` 声明 `JS_SetInterruptHandler(JSRuntime*, JSInterruptHandler*, void*)`,
  回调取址类型 `int (*)(JSContext*, void*)` 与之完全一致
- 重新编译 windows-release / windows-debug 确认
  (quickjs_repo 从 submodule 源码构建, submodule 更新后会重编并重装头文件;
  若某个构建目录仍用旧头文件 (首参数仍是 `JSRuntime*`), 删除该目录下的
  `quickjs_repo-prefix/` 再编译即可)
