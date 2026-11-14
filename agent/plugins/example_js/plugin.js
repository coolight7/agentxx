// example_js —— JS 插件示例
// 通过全局 agentxx 桥注册工具/通知/事件; 引擎 (agentxx_javascript_engine) 负责执行

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

// ---- 工具 4: 调用主程序插件工具 (经 C 桥) ----
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

// ---- 工具 6: callTool 返回值形态 (始终为 Promise) ----
agentxx.registerTool({
  name: "js_calltool_kind",
  description: "JS plugin demo: typeof agentxx.callTool(...) must be 'object' (Promise).",
  execute: () => ({ kind: typeof agentxx.callTool("js_hello", {}, "") }),
});

// ---- 工具 7: SDK 查询面自查 ----
// 与 C++ 插件 SDK 同名的查询接口: 提示词 / 会话 / 上下文 / 执行图 / 挂载 / 网络。
// 返回值只描述"形状" (类型/是否数组), 测试用它确认这些桥方法都能调通。
agentxx.registerTool({
  name: "js_sdk_probe",
  description: "JS plugin demo: report the shapes returned by the SDK query helpers.",
  execute: () => {
    const out = {};
    const kindOf = (v) => (v === null ? "null" : (Array.isArray(v) ? "array" : typeof v));
    const scope = agentxx.scope();
    out.scope = scope === null ? "null" : typeof scope.agentId;
    out.language = kindOf(agentxx.getLanguage());
    out.setLanguage = typeof agentxx.setLanguage("zh-cn");
    out.pluginArgs = kindOf(agentxx.getPluginArgs());
    out.workDir = kindOf(agentxx.getSessionWorkDir(""));
    out.canceled = typeof agentxx.isCanceled("probe-thread");
    out.graphName = kindOf(agentxx.getGraphName());
    out.graph = kindOf(agentxx.getGraphJson());
    out.mountsIsArray = Array.isArray(agentxx.listMounts());
    const mount = agentxx.getMount();
    out.mount = mount === null ? "null" : typeof mount.agentId;
    out.messagesIsArray = Array.isArray(agentxx.getMessages("probe-session"));
    out.messagesCount = typeof agentxx.messagesCount("probe-session");
    out.prompt = kindOf(agentxx.getPrompt());
    out.toolPrompt = kindOf(agentxx.getToolPrompt("no_such_tool_for_probe"));
    out.shareStore = kindOf(agentxx.getShareStore("probe-session", 0));
    // 网络: 入口总是给出结果对象, 业务结论在 ok 里
    const netInfo = agentxx.netInfo();
    out.netInfoIsObject = typeof netInfo === "object" && netInfo !== null;
    out.netInfoOkType = typeof netInfo.ok;
    out.netCallOkType = typeof agentxx.netCall("net.info", {}).ok;
    // 后台任务: 空参数会被拒 (空 id), 查询/取消都是纯读取
    out.submitAsyncWork = typeof agentxx.submitAsyncWork({});
    out.listAsyncWorkIsArray = Array.isArray(agentxx.listAsyncWork());
    out.getAsyncWork = kindOf(agentxx.getAsyncWork("async-does-not-exist"));
    out.cancelAsyncWork = agentxx.cancelAsyncWork("async-does-not-exist");
    // 会话运行时: 只打无效句柄的路径 (真建根/开通道会真的起 agent, 留给真实用例)
    out.listRuntimeKind = kindOf(agentxx.listRuntime());
    out.openChannelInvalid = typeof agentxx.openChannel({ rootId: "no-such-root" });
    out.getChannelStateInvalid = kindOf(agentxx.getChannelState(0));
    out.getChannelMessagesIsArray = Array.isArray(agentxx.getChannelMessages(0));
    out.closeChannelInvalid = typeof agentxx.closeChannel(0);
    out.cancelChannelInvalid = typeof agentxx.cancelChannel(0);
    out.selectModelInvalid = typeof agentxx.selectModel(0, "no-such-model");
    out.respondInterruptInvalid = typeof agentxx.respondInterrupt(0, 0, { values: {} });
    out.unmountRootInvalid = typeof agentxx.unmountRoot("no-such-root");
    out.registerTriggerInvalid = typeof agentxx.registerTrigger({
      channelId: 0,
      topic: "plugin.example_js.probe",
    });
    out.unregisterTriggerInvalid = typeof agentxx.unregisterTrigger(0);
    out.registerTimerInvalid = typeof agentxx.registerTimer({ channelId: 0, firstDelayMs: 1 });
    out.unregisterTimerInvalid = typeof agentxx.unregisterTimer(0);
    out.emitEvent = typeof agentxx.emitEvent("plugin.example_js.probe", { probe: true });
    // 广播写入方: 声明自己的频道 → 发布 → 读当前值 → 结束流 (可重复执行)
    const stream = "plugin.example_js.probe_stream";
    try {
      agentxx.declareBroadcast({ id: stream, cache: "latest", publish: "owner" });
      out.declareBroadcast = true;
    } catch (e) {
      out.declareBroadcast = false;
    }
    agentxx.publish(stream, { probe: 1 });
    out.readBroadcast = kindOf(agentxx.readBroadcast(stream));
    out.endBroadcast = typeof agentxx.endBroadcast(stream, { reason: "probe done" });
    out.readAfterEnd = kindOf(agentxx.readBroadcast(stream));
    // share_store 写入 / 资源 / 模型配置
    out.addShareStore = typeof agentxx.addShareStore("no-such-session", "probe");
    out.ownResources = kindOf(agentxx.getOwnResources());
    out.modelConfig = kindOf(agentxx.getModelConfig());
    return out;
  },
});

// ---- 工具 8: 网络 (async HTTP) ----
// 拒绝连接 / 超时都按业务失败返回 (`ok:false` + `error`), 不会抛异常
agentxx.registerTool({
  name: "js_net_fetch",
  description: "JS plugin demo: HTTP via agentxx.netFetch; failures come back as ok:false.",
  parameters: {
    url: { type: "string", description: "absolute http(s) url" },
  },
  execute: async (args) => {
    const res = await agentxx.netFetch({
      url: args.url || "http://127.0.0.1:1/",
      timeoutMs: 2000,
    });
    return {
      ok: res.ok === true,
      error: typeof res.error === "string" ? res.error : null,
      resultIsObject: typeof res === "object",
    };
  },
});

// ---- 工具 9: 返回带状态的结果 {status, text} ----
// - 与返回普通值的区别: 状态显式给了, 主程序不再把结果当成功
// - text 是给模型读的正文; 失败时只写错误内容 (不要自己加 "[Error: ...]")
agentxx.registerTool({
  name: "js_fail_demo",
  description: "JS plugin demo: return {status, text} to report failure without throwing.",
  parameters: {
    reason: { type: "string", description: "why it failed" },
  },
  execute: (args) => ({
    status: "failed",
    text: "deliberate failure: " + (args.reason || "no reason given"),
  }),
});

// ---- 通知点: agent_start (point 0) ----
agentxx.onHook(0, (info) => {
  agentxx.log(2, "js hook agent_start fired: " + JSON.stringify(info));
});

// ---- 事件: 订阅 `plugin.<插件名>.<事件>` ----
// - 名字写全, 框架只校验不补前缀; 读侧可跨族 (也能订阅 `agentxx.agent.*`)
agentxx.subscribe("plugin.example_js.demo", (payload) => {
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
