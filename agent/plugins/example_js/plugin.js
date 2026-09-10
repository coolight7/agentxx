// example_js —— JS 插件示例
// 通过全局 agentxx 桥注册工具/钩子/事件; 引擎 (agentxx_javascript_engine) 承载执行

// ---- 工具 1: 纯 JS 计算 ----
agentxx.registerTool({
  name: "js_hello",
  description: "JS plugin demo: greet a person and echo input.",
  parameters: {
    name: { type: "string", description: "who to greet" },
  },
  execute: (args, ctx) => {
    agentxx.log(2, "js_hello called, session=" + ctx.sessionId);
    const who = args.name || "world";
    return { greeting: "Hello, " + who + "!", from: "js plugin" };
  },
});

// ---- 工具 2: async 支持 (Promise) ----
agentxx.registerTool({
  name: "js_async_wait",
  description: "JS plugin demo: async function with Promise resolution.",
  execute: async (args, ctx) => {
    await new Promise((resolve) => setTimeout(resolve, 10));
    return { waited: true, ms: 10, session: ctx.sessionId };
  },
});

// ---- 工具 3: JS 内互调 (callTool 始终返回 Promise; 命中本引擎工具 → 内联执行) ----
agentxx.registerTool({
  name: "js_call_js",
  description: "Call another JS tool (js_hello) via agentxx.callTool (in-engine).",
  execute: async (args) => {
    const resp = await agentxx.callTool("js_hello", { name: args.name || "inner" }, "");
    return { inner: resp };
  },
});

// ---- 工具 4: 调用宿主插件工具 (经 C 桥) ----
agentxx.registerTool({
  name: "js_call_host",
  description: "Call host plugin tool example_echo via agentxx.callTool.",
  execute: async (args) => {
    const resp = await agentxx.callTool("example_echo", args, "");
    return { host: resp };
  },
});

// ---- 工具 5: Promise 拒绝 (rejection → 工具失败, 不再当成成功文本) ----
agentxx.registerTool({
  name: "js_reject_demo",
  description: "JS plugin demo: a rejected Promise maps to a failed tool call.",
  parameters: {
    reason: { type: "string", description: "rejection reason" },
  },
  execute: (args) => Promise.reject(new Error(args.reason || "demo rejection")),
});

// ---- 工具 6: callTool 返回值形态 (F17: 始终为 Promise) ----
agentxx.registerTool({
  name: "js_calltool_kind",
  description: "JS plugin demo: typeof agentxx.callTool(...) must be 'object' (Promise).",
  execute: () => ({ kind: typeof agentxx.callTool("js_hello", {}, "") }),
});

// ---- 钩子: agent_start (point 0) ----
agentxx.onHook(0, (info) => {
  agentxx.log(2, "js hook agent_start fired: " + JSON.stringify(info));
});

// ---- 事件: 订阅 demo.topic (宿主自动加 plugin. 前缀) ----
agentxx.subscribe("demo.topic", (payload) => {
  agentxx.log(2, "js event received: " + JSON.stringify(payload));
});

// ---- 插件互查 API 演示: 判断依赖是否安装 + 自适应 ----
// - agentxx.getPlugin("agentxx_javascript_engine") → 信息对象或 null
// - agentxx.listPlugins() → 全部已安装插件数组
(function checkDeps() {
  const engine = agentxx.getPlugin("agentxx_javascript_engine");
  if (engine) {
    agentxx.log(2, "js engine installed: v" + engine.version + " (type " + engine.type + ")");
  } else {
    agentxx.log(4, "js engine missing");
  }
  const all = agentxx.listPlugins();
  agentxx.log(2, "installed plugins: " + all.map((p) => p.name + "@" + p.version).join(", "));
})();

// ---- 顶层异步初始化 (Promise 驱动) ----
(async () => {
  await new Promise((resolve) => setTimeout(resolve, 5));
  agentxx.log(2, "example_js async init done");
})();
