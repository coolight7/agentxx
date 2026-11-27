// example_js —— JS 插件示例
// 通过全局 agentxx 桥注册工具/通知/事件; 引擎 (agentxx_javascript_engine) 负责执行
//
// 同一份脚本在两侧都会执行:
// - `agentxx.side === "agent"`: 注册工具 / 通知点 / 事件 (agent 侧)
// - `agentxx.side === "client"`: 注册工具行内容 / 展示面 / 命令 / 动作 (client 侧)
//
// **脚本按 ES module 执行**: 表调用 (注册/查询/发布/...) 一律返回 Promise,
// 需要生效或取值时 `await`; 顶层 `await` 可用。同步返回的只有纯本地函数
// (`log` / `jsonEscape` / `jsonGetString` / `scope` / `setTimeout` / `showToast`
// 这类提交即返回的动作) 与"必须当场回答"的回调 (内容生成函数 / 命令 execute)。
const SIDE =
  typeof agentxx !== "undefined" && typeof agentxx.side === "string"
    ? agentxx.side
    : "agent";

if (SIDE === "client") {

  // ---- client 侧: 工具行内容 (声明项 + 内容) ----
  // 工具行 = `tool` 槽位上的一份声明项 + 一份内容:
  // - 声明项的 data 带工具名与显示名: {tool, displayName}
  // - 内容生成函数拿到 `agentxx.client.tool.*` 变量 (deps 里声明的那些);
  //   part === "summary" 时只要一行文字 (主程序取文档里的纯文本),
  //   否则给整块内容 (blocks / items)
  // - 生成函数可以当场返回, 也可以返回 Promise (结果到了再交回)
  const itemOk = await agentxx.registerUiItem({
    slot: "tool",
    id: "js_hello",
    data: { tool: "js_hello", displayName: "JS Hello" },
  });
  const contentOk = await agentxx.registerUiContent({
    slot: "tool",
    id: "js_hello",
    deps: [
      "agentxx.client.tool.args",
      "agentxx.client.tool.part",
      "agentxx.client.tool.state",
    ],
    generate: (ctx) => {
      let args = {};
      try {
        args = JSON.parse(ctx["agentxx.client.tool.args"] || "{}");
      } catch (e) {
        args = {};
      }
      const who = args.name || "world";
      const state = ctx["agentxx.client.tool.state"];
      if (ctx["agentxx.client.tool.part"] === "summary") {
        return state === "running" ? "greeting " + who + "..." : "greeted " + who;
      }
      return {
        blocks: [
          { kind: "Text", text: "JS Hello (client side)", type: "title" },
          { kind: "Text", text: "target: " + who, color: "hint" },
          { kind: "Text", text: "state: " + state, color: "hint" },
        ],
      };
    },
  });
  agentxx.log(
    2,
    "example_js: client side ready (side=" +
      SIDE +
      ", tool row=" +
      itemOk +
      "/" +
      contentOk +
      ")",
  );

  // ---- client 侧: 展示面 (状态栏 / Info 栏 / 侧边栏面板) ----
  // 三处都是"槽位上的内容": 一份声明项给位置与标题, 一份内容给生成函数。
  // 生成函数回最近一次算好的文档 (文档放在闭包变量里), 主程序来问就交这一份;
  // 自己算好新的一份时用 pushUiContent 推过去。
  let statusDoc = { blocks: [{ kind: "Text", text: "js: ready", dim: true }] };
  const statusItem = await agentxx.registerUiItem({
    slot: "status",
    id: "js.demo",
    data: { align: 0, order: 50 },
  });
  const status =
    statusItem &&
    (await agentxx.registerUiContent({
      slot: "status",
      id: "js.demo",
      generate: () => statusDoc,
    }));
  const pushStatus = (text) => {
    statusDoc = { blocks: [{ kind: "Text", text, dim: true }] };
    if (status) {
      agentxx.pushUiContent("js.demo", statusDoc);
    }
  };

  let infoDoc = {
    blocks: [
      { kind: "Text", text: "side: client", color: "hint" },
      { kind: "Text", text: "工具行: js_hello", color: "hint" },
    ],
  };
  const infoItem = await agentxx.registerUiItem({
    slot: "info",
    id: "js.demo.info",
    data: { title: "JS Demo" },
  });
  const info =
    infoItem &&
    (await agentxx.registerUiContent({
      slot: "info",
      id: "js.demo.info",
      title: "JS Demo",
      generate: () => infoDoc,
    }));
  const pushInfo = (lines) => {
    infoDoc = { blocks: lines };
    if (info) {
      agentxx.pushUiContent("js.demo.info", infoDoc);
    }
  };

  let panelDoc = {
    blocks: [
      { kind: "Text", text: "example_js (client side)", type: "title" },
      {
        kind: "Button",
        label: "打个招呼",
        action: { kind: "call", name: "js.demo.hello" },
      },
    ],
  };
  const panelItem = await agentxx.registerUiItem({
    slot: "panel",
    id: "js.demo.panel",
    data: { title: "JS Demo" },
  });
  const panel =
    panelItem &&
    (await agentxx.registerUiContent({
      slot: "panel",
      id: "js.demo.panel",
      title: "JS Demo",
      generate: () => panelDoc,
    }));
  if (panel) {
    agentxx.pushUiContent("js.demo.panel", panelDoc);
  }

  // ---- client 侧: 斜杠命令 (execute 同步返回动作 JSON) ----
  // 动作格式与 C++ client 插件一致: {"action":"toast","text":...} /
  // {"action":"send","text":...} / {"action":"none"}
  await agentxx.registerCommand({
    name: "jsdemo",
    description: "example_js: 打个招呼 (client 侧命令)",
    execute: () => ({ action: "toast", text: "hello from JS", level: 0 }),
  });

  // ---- client 侧: 按钮动作绑定 (面板里的 Button 点击后派发到这里) ----
  // 派发式回调可以写成 async: 主程序不等它的返回值
  await agentxx.bindAction("js.demo.panel", async (ev) => {
    agentxx.showToast("JS action: " + (ev.actionId || "?"), 0);
  });

  // ---- client 侧: 全局快捷键 ----
  let keyTicks = 0;
  const tickBind = await agentxx.registerKeybind(
    { keys: "ctrl+alt+j", description: "example_js: 状态栏计数 +1" },
    async () => {
      keyTicks += 1;
      pushStatus("js: key " + keyTicks);
    },
  );

  // ---- client 侧: 定时器 (主程序驱动; 不可见时顺延) ----
  let timerTicks = 0;
  const demoTimer = await agentxx.setClientTimer(
    { intervalMs: 60, repeat: 2, pauseWhenHidden: true, ownerId: "js.demo.info" },
    async () => {
      timerTicks += 1;
      pushInfo([
        { kind: "Text", text: "side: client", color: "hint" },
        { kind: "Text", text: "timer tick " + timerTicks, color: "hint" },
      ]);
    },
  );

  // ---- client 侧: 主动推内容 / 按变量重新生成 ----
  // - pushUiContent 直接给一份已算好的内容 (不等主程序来问; 内容要已注册)
  // - updateUiSlot 让主程序按当前变量把槽位上的内容重新生成一遍
  //   (第二个参数不给 = 该槽位上本插件的全部内容)
  const pushOk = await agentxx.pushUiContent("js.demo.info", {
    blocks: [{ kind: "Text", text: "pushed from JS", color: "hint" }],
  });
  const slotOk = await agentxx.updateUiSlot("tool", ["js_hello"]);
  agentxx.log(2, "example_js: push=" + pushOk + " slot=" + slotOk);

  // ---- client 侧: 注销与查询入口 ----
  await agentxx.registerKeybind(
    { keys: "ctrl+alt+u", description: "example_js: 注销展示项" },
    async () => {
      const binds = await agentxx.listKeybinds();
      const visible = await agentxx.isRegionVisible("js.demo.info");
      // 注销要成对来: 内容与声明项各一份
      if (info) {
        await agentxx.unregisterUiContent("js.demo.info");
        await agentxx.unregisterUiItem("js.demo.info");
      }
      if (panel) {
        await agentxx.unregisterUiContent("js.demo.panel");
        await agentxx.unregisterUiItem("js.demo.panel");
      }
      if (status) {
        await agentxx.unregisterUiContent("js.demo");
        await agentxx.unregisterUiItem("js.demo");
      }
      if (demoTimer) {
        await agentxx.cancelClientTimer(demoTimer);
      }
      if (tickBind) {
        await agentxx.unregisterKeybind(tickBind);
      }
      agentxx.showToast(
        "cleanup: binds=" + binds.length + " visible=" + visible,
        0,
      );
    },
  );

  // ---- client 侧: 网络 (`agentxx.net`, 与 agent 插件同一张表) ----
  // 要结果的动作返回 Promise (结果晚些到, 仍在 JS 线程)。
  await agentxx.registerCommand({
    name: "jsnet",
    description: "example_js: 网络表自检 (net.info)",
    execute: () => {
      // 命令回调必须同步返回内容, 所以结果经 toast 补上
      agentxx.netInfo().then((info) => {
        const servers = info && info.servers ? info.servers.length : -1;
        agentxx.netCall("net.no_such_op", {}).then((bogus) => {
          agentxx.showToast(
            "net info ok=" + (info && info.ok) +
              " servers=" + servers +
              " bogus ok=" + (bogus && bogus.ok),
            0,
          );
        });
      });
      return { action: "toast", text: "net: probing", level: 0 };
    },
  });

  // 回环往返: 脚本自己起一个 HTTP 服务 + 静态路由, 再用 netFetch 取回
  // (Promise 完成时回到 JS 线程, 界面线程全程不等)。
  await agentxx.bindAction("js.demo.net", () => {
    agentxx.netCall("http.serve", { host: "127.0.0.1", port: 0 }).then((served) => {
      if (!served || !served.ok || !served.port) {
        agentxx.showToast("net: serve failed", 2);
        return;
      }
      agentxx.netCall("http.route", {
        serverId: served.id,
        path: "/hello",
        bytes: "hello-from-js-client",
      }).then(() => {
        agentxx.netFetch("http://127.0.0.1:" + served.port + "/hello").then((r) => {
          const body = (r && r.body) || "";
          agentxx.showToast(
            "net fetch: ok=" + (r && r.ok) +
              " status=" + (r && r.status) +
              " body=" + body,
            0,
          );
          agentxx.netCall("http.stop", { serverId: served.id });
        });
      });
    });
  });

  // ---- client 侧: 广播订阅 (与 agent 侧同一套 subscribe / onEnd) ----
  // 订阅自己声明的频道: 发布后回调在 JS 线程执行。回调只记数 (不弹 toast),
  // 由 `js.demo.sdk` 把上一轮收到的次数写进 toast, 端到端可断言。
  const subChannel = "plugin.example_js.client.echo";
  let subEcho = 0;
  let subEnds = 0;
  await agentxx.declareBroadcast({ id: subChannel, cache: "none" });
  await agentxx.subscribe(
    subChannel,
    () => {
      subEcho++;
    },
    { onEnd: () => { subEnds++; } },
  );

  // ---- client 侧: 通用表 (配置 / 插件清单 / 广播声明-发布-读-结束 / 能力行) ----
  await agentxx.bindAction("js.demo.sdk", async () => {
    // 订阅计数先取快照: 快照在本次动作的第一次 await 之前, 读到的只有
    // "上一轮及更早"的投递 (回调在 JS 线程排队, 本次的发布还没跑)
    const echoSeen = subEcho;
    const endsSeen = subEnds;
    const plugins = await agentxx.listPlugins();
    const self = await agentxx.getPlugin("example_js");
    const cfg = await agentxx.getConfig();
    const channel = "plugin.example_js.demo.channel";
    await agentxx.declareBroadcast({ id: channel, cache: "latest" });
    await agentxx.publish(channel, { tick: 1 });
    const current = await agentxx.readBroadcast(channel);
    const ended = await agentxx.endBroadcast(channel, "done");
    const afterEnd = await agentxx.readBroadcast(channel);
    // 订阅自检: 本频道的发布与结束在上一轮已经到过回调 (处理异步, 这里读到的是上一轮)
    await agentxx.publish(subChannel, { n: 1 });
    await agentxx.endBroadcast(subChannel, "round done");

    // 能力行 (client 侧登记的是进程级行): 登记 → 调用 (Promise) → 注销
    const capId = "client.selftest";
    const capProvided = await agentxx.provideCapability({
      id: capId,
      handler: (args) => ({ echo: args && args.ping, side: "client" }),
    });
    const capResult = await agentxx.invokeCapability(
      "plugin.example_js." + capId,
      { ping: "pong" },
    );
    const value = (capResult && capResult.value) || {};
    await agentxx.unprovideCapability(capId);
    agentxx.showToast(
      "sdk: plugins=" + (plugins ? plugins.length : -1) +
        " self=" + (self && self.name ? self.name : "?") +
        " config=" + (cfg === null ? "null" : "object") +
        " read=" + (current && current.tick === 1) +
        " ended=" + ended +
        " afterEnd=" + (afterEnd === null) +
        " echo=" + echoSeen +
        " ends=" + endsSeen +
        " cap=" + capProvided +
        "/" + (capResult && capResult.ok ? value.echo : "fail") +
        "/" + (value.side || "-") +
        " json=" + agentxx.jsonGetString('{"k":"v"}', "k"),
      0,
    );
  });

} else {

  // ---- 工具 1: 纯 JS 计算 ----
  await agentxx.registerTool({
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
  await agentxx.registerTool({
    name: "js_async_wait",
    description: "JS plugin demo: async function with Promise resolution.",
    execute: async (args, ctx) => {
      await new Promise((resolve) => setTimeout(resolve, 10));
      return { waited: true, ms: 10, session: ctx.sessionId };
    },
  });

  // ---- 工具 3: JS 内互调 (callTool 始终返回 Promise; 命中本引擎工具 → 内联执行) ----
  await agentxx.registerTool({
    name: "js_call_js",
    description: "Call another JS tool (js_hello) via agentxx.callTool (in-engine).",
    execute: async (args) => {
      const resp = await agentxx.callTool("js_hello", { name: args.name || "inner" }, "");
      return { inner: resp };
    },
  });

  // ---- 工具 4: 调用主程序插件工具 (经 C 桥) ----
  await agentxx.registerTool({
    name: "js_call_host",
    description: "Call host plugin tool example_echo via agentxx.callTool.",
    execute: async (args) => {
      const resp = await agentxx.callTool("example_echo", args, "");
      return { host: resp };
    },
  });

  // ---- 工具 5: Promise 拒绝 (rejection → 工具失败, 不再当成成功文本) ----
  await agentxx.registerTool({
    name: "js_reject_demo",
    description: "JS plugin demo: a rejected Promise maps to a failed tool call.",
    parameters: {
      reason: { type: "string", description: "rejection reason" },
    },
    execute: (args) => Promise.reject(new Error(args.reason || "demo rejection")),
  });

  // ---- 工具 6: callTool 返回值形态 (始终为 Promise) ----
  await agentxx.registerTool({
    name: "js_calltool_kind",
    description: "JS plugin demo: typeof agentxx.callTool(...) must be 'object' (Promise).",
    execute: () => ({ kind: typeof agentxx.callTool("js_hello", {}, "") }),
  });

  // ---- 工具 6b: 多模态附件 (返回对象里的 attachments 作为第二段元数据上报) ----
  // `text` 是给模型读的正文, `attachments` 不经过正文 (否则图片信息会和正文混在一起)
  await agentxx.registerTool({
    name: "js_media_demo",
    description: "JS plugin demo: return text plus a media attachment.",
    parameters: {
      path: { type: "string", description: "place the attachment should point at" },
    },
    execute: (args) => ({
      status: "ok",
      text: `attached ${args.path || "chart.png"}`,
      attachments: [
        {
          type: "image",
          display_name: "chart.png",
          mime_type: "image/png",
          path_or_url: args.path || "/tmp/chart.png",
          size_bytes: 2048,
        },
      ],
    }),
  });

  // ---- 工具 7: SDK 查询面自查 ----
  // 与 C++ 插件 SDK 同名的查询接口: 提示词 / 会话 / 上下文 / 执行图 / 挂载 / 网络。
  // 返回值只描述"形状" (类型/是否数组), 测试用它确认这些桥方法都能调通。
  await agentxx.registerTool({
    name: "js_sdk_probe",
    description: "JS plugin demo: report the shapes returned by the SDK query helpers.",
    execute: async () => {
      const out = {};
      const kindOf = (v) => (v === null ? "null" : (Array.isArray(v) ? "array" : typeof v));
      const scope = agentxx.scope();
      out.scope = scope === null ? "null" : typeof scope.agentId;
      out.language = kindOf(await agentxx.getLanguage());
      out.setLanguage = typeof (await agentxx.setLanguage("zh-cn"));
      out.pluginArgs = kindOf(await agentxx.getPluginArgs());
      out.workDir = kindOf(await agentxx.getSessionWorkDir(""));
      out.canceled = typeof agentxx.isCanceled("probe-thread");
      out.graphName = kindOf(await agentxx.getGraphName());
      out.graph = kindOf(await agentxx.getGraphJson());
      out.mountsIsArray = Array.isArray(await agentxx.listMounts());
      const mount = await agentxx.getMount();
      out.mount = mount === null ? "null" : typeof mount.agentId;
      out.messagesIsArray = Array.isArray(await agentxx.getMessages("probe-session"));
      out.messagesCount = typeof (await agentxx.messagesCount("probe-session"));
      // 会话消息 (接口表 v5): 展示历史与 LLM 上下文各一套 get/count/set/append/update。
      // 不存在的会话走拒绝分支 (读给空数组/-1, 写给 null), 这里只验形状。
      out.viewMessagesIsArray = Array.isArray(await agentxx.getViewMessages("probe-session"));
      out.viewMessagesCount = typeof (await agentxx.countViewMessages("probe-session"));
      out.setViewMessages = await agentxx.setViewMessages("probe-session", [
        { role: "user", text: "x" },
      ]);
      out.appendViewMessages = await agentxx.appendViewMessages("probe-session", [
        { role: "user", text: "x" },
      ]);
      out.updateViewMessages = await agentxx.updateViewMessages("probe-session", [
        { id: "msg_x", role: "user", text: "y" },
      ]);
      out.llmMessagesIsArray = Array.isArray(await agentxx.getLlmMessages("probe-session"));
      out.llmMessagesCount = typeof (await agentxx.countLlmMessages("probe-session"));
      out.setLlmMessages = await agentxx.setLlmMessages("probe-session", []);
      out.appendLlmMessages = await agentxx.appendLlmMessages("probe-session", [
        { role: "user", content: "x" },
      ]);
      out.updateLlmMessages = await agentxx.updateLlmMessages("probe-session", [
        { id: "llm_x", role: "user", content: "y" },
      ]);
      out.prompt = kindOf(await agentxx.getPrompt());
      out.toolPrompt = kindOf(await agentxx.getToolPrompt("no_such_tool_for_probe"));
      out.shareStore = kindOf(await agentxx.getShareStore("probe-session", 0));
      // 网络: 要结果的动作给 Promise (结果晚些到); `netCancel` 是同步成员, 结果对象里 `ok` 是 boolean
      out.netInfoIsPromise = agentxx.netInfo() instanceof Promise;
      out.netCallIsPromise = agentxx.netCall("net.info", {}) instanceof Promise;
      out.netCancelOkType = typeof agentxx.netCancel(999999).ok;
      // 后台任务: 空参数会被拒 (空 id), 查询/取消都是纯读取
      out.submitAsyncWork = typeof (await agentxx.submitAsyncWork({}));
      out.listAsyncWorkIsArray = Array.isArray(await agentxx.listAsyncWork());
      out.getAsyncWork = kindOf(await agentxx.getAsyncWork("async-does-not-exist"));
      out.cancelAsyncWork = await agentxx.cancelAsyncWork("async-does-not-exist");
      // 会话运行时: 只打无效句柄的路径 (真建根/开通道会真的起 agent, 留给真实用例)
      out.listRuntimeKind = kindOf(await agentxx.listRuntime());
      out.openChannelInvalid = typeof (await agentxx.openChannel({ rootId: "no-such-root" }));
      out.getChannelStateInvalid = kindOf(await agentxx.getChannelState(0));
      out.getChannelMessagesIsArray = Array.isArray(await agentxx.getChannelMessages(0));
      out.closeChannelInvalid = typeof (await agentxx.closeChannel(0));
      out.cancelChannelInvalid = typeof (await agentxx.cancelChannel(0));
      out.selectModelInvalid = typeof (await agentxx.selectModel(0, "no-such-model"));
      out.respondInterruptInvalid = typeof (await agentxx.respondInterrupt(0, 0, { values: {} }));
      out.unmountRootInvalid = typeof (await agentxx.unmountRoot("no-such-root"));
      out.registerTriggerInvalid = typeof (await agentxx.registerTrigger({
        channelId: 0,
        topic: "plugin.example_js.probe",
      }));
      out.unregisterTriggerInvalid = typeof (await agentxx.unregisterTrigger(0));
      out.registerTimerInvalid = typeof (await agentxx.registerTimer({
        channelId: 0,
        firstDelayMs: 1,
      }));
      out.unregisterTimerInvalid = typeof (await agentxx.unregisterTimer(0));
      out.emitEvent = typeof (await agentxx.emitEvent("plugin.example_js.probe", { probe: true }));
      // 广播写入方: 声明自己的频道 → 发布 → 读当前值 → 结束流 (可重复执行)
      const stream = "plugin.example_js.probe_stream";
      out.declareBroadcast = await agentxx.declareBroadcast({
        id: stream,
        cache: "latest",
        publish: "owner",
      });
      await agentxx.publish(stream, { probe: 1 });
      out.readBroadcast = kindOf(await agentxx.readBroadcast(stream));
      out.endBroadcast = typeof (await agentxx.endBroadcast(stream, { reason: "probe done" }));
      out.readAfterEnd = kindOf(await agentxx.readBroadcast(stream));
      // share_store 写入 / 资源 / 模型配置
      out.addShareStore = typeof (await agentxx.addShareStore("no-such-session", "probe"));
      out.ownResources = kindOf(await agentxx.getOwnResources());
      out.modelConfig = kindOf(await agentxx.getModelConfig());
      return out;
    },
  });

  // ---- 工具 8: SDK 对齐自查 (自身信息 / 配置路径 / 路径权限 / 能力行 / 流结束) ----
  // 逐项调一遍与 C++ 插件 SDK 同名的新接口, 返回结果形状与关键值供测试断言。
  await agentxx.registerTool({
    name: "js_parity_probe",
    description: "JS plugin demo: self info, path permissions, capability rows and stream end.",
    parameters: {},
    execute: async () => {
      const out = {};
      const kindOf = (v) => (v === null ? "null" : Array.isArray(v) ? "array" : typeof v);

      // 1) 自身信息 / 配置文件路径 (未配置时是空串, 不是 undefined)
      const info = await agentxx.getOwnInfo();
      out.ownInfo = info && typeof info === "object" ? info : null;
      out.ownId = out.ownInfo && typeof out.ownInfo.name === "string" ? out.ownInfo.name : "";
      out.configPathKind = kindOf(await agentxx.getPluginConfigPath());

      // 2) 路径权限三态查询: checkPaths 给三个状态, filterPaths 只留允许项
      const dir = await agentxx.getSessionWorkDir("");
      const probePaths = [typeof dir === "string" && dir.length > 0 ? dir : "."];
      const decisions = await agentxx.checkPaths(probePaths, "read");
      out.checkPathsIsArray = Array.isArray(decisions);
      out.checkPathDecision = Array.isArray(decisions) && decisions.length === 1 ? decisions[0] : null;
      const allowed = await agentxx.filterPaths(probePaths, "read");
      out.filterPathsIsArray = Array.isArray(allowed);
      out.filterPathsCount = Array.isArray(allowed) ? allowed.length : -1;
      const writeDecisions = await agentxx.checkPaths(["/js-parity-probe/not-allowed"], "write");
      out.writeDecision = Array.isArray(writeDecisions) && writeDecisions.length === 1 ? writeDecisions[0] : null;

      // 3) 能力行: 登记 → 调用 → 自省 → 注销 → 再调用 (应当失败)
      const capName = "js.selftest";
      out.provided = await agentxx.provideCapability({
        id: capName,
        handler: (args) => ({ echo: args && args.ping, from: "js" }),
      });
      const full = "plugin." + (out.ownId || "example_js") + "." + capName;
      out.invoke = await agentxx.invokeCapability(full, { ping: "pong" });
      const capInfo = await agentxx.capabilityInfo(full);
      out.capInfoIsObject = typeof capInfo === "object" && capInfo !== null;
      out.capProviders = capInfo && Array.isArray(capInfo.providers) ? capInfo.providers.length : -1;
      out.listCapabilitiesKind = kindOf(await agentxx.listCapabilities());
      out.unprovided = await agentxx.unprovideCapability(capName);
      out.invokeAfterUnprovide = await agentxx.invokeCapability(full, { ping: "after" });

      // 4) 功能埋点实现行: 用核心点验证登记形状 (登记 / 自省 / 撤销)
      const pointId = "agentxx.agent.point.context.countTokens";
      out.pointProvided = await agentxx.providePoint(pointId, () => ({ tokens: 7 }));
      const pointInfo = await agentxx.capabilityInfo(pointId);
      out.pointProviders =
        pointInfo && Array.isArray(pointInfo.providers) ? pointInfo.providers.length : -1;
      await agentxx.unprovideCapability(pointId);

      // 5) 订阅的流结束回调 (onEnd 与主程序的 subscribe_broadcast_ex 对应)
      const stream = "plugin." + (out.ownId || "example_js") + ".js.selftest.stream";
      await agentxx.declareBroadcast({ id: stream, cache: "latest", publish: "owner" });
      const seen = [];
      let endReason = null;
      const token = await agentxx.subscribe(
        stream,
        (v) => seen.push(v),
        { onEnd: (reason) => { endReason = reason; } },
      );
      // 对照组: 老签名 (两个参数) 的订阅, 用来区分"事件没送到"与"新参数路径有问题"
      let plainSeen = 0;
      await agentxx.subscribe(stream, () => { plainSeen++; });
      out.subscribeTokenKind = kindOf(token);
      out.published = await agentxx.publish(stream, { n: 1 });
      out.readAfterPublish = kindOf(await agentxx.readBroadcast(stream));
      await new Promise((r) => setTimeout(r, 20));
      out.seenAfterPublish = seen.length;
      out.plainSeenAfterPublish = plainSeen;
      await agentxx.endBroadcast(stream, { why: "probe done" });
      await new Promise((r) => setTimeout(r, 20));
      await agentxx.unsubscribe(token);
      out.streamSeen = seen.length;
      out.streamEndReason = endReason;

      // 6) 执行图节点类型: 登记 → 同名再登记 (覆盖) → 注销 → 再注销
      // - 同名再登记不抛异常 (本实例的旧登记先撤销, 后登记者生效)
      // - 注销不存在的类型也只是什么也不做
      const nodeType = "js_parity_probe_node";
      try {
        out.registerNodeType = await agentxx.registerNodeType({
          type: nodeType,
          run: (input) => ({
            writes: [{ channel: "js.probe", value: { node: input.nodeName }, mode: "overwrite" }],
          }),
        });
        out.registerNodeTypeAgain = await agentxx.registerNodeType({
          type: nodeType,
          run: () => ({ writes: [] }),
        });
        out.unregisterNodeType = agentxx.unregisterNodeType(nodeType);
        out.unregisterNodeTypeAgain = agentxx.unregisterNodeType(nodeType);
      } catch (e) {
        out.registerNodeTypeError = String(e && e.message ? e.message : e);
      }

      // 7) JSON 辅助 (`agentxx.json`): 转义一段文本 / 取顶层字符串字段
      out.jsonEscape = agentxx.jsonEscape('a"b\\c');
      out.jsonGetString = agentxx.jsonGetString('{"k":"v","n":1}', "k");
      return out;
    },
  });

  // ---- 工具 9: 网络 (async HTTP) ----
  // 拒绝连接 / 超时都按业务失败返回 (`ok:false` + `error`), 不会抛异常
  await agentxx.registerTool({
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

  // ---- 工具 10: 返回带状态的结果 {status, text} ----
  // - 与返回普通值的区别: 状态显式给了, 主程序不再把结果当成功
  // - text 是给模型读的正文; 失败时只写错误内容 (不要自己加 "[Error: ...]")
  await agentxx.registerTool({
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

  // ---- 执行图节点类型: 脚本自定义节点 (`agentxx.agent.graph`) ----
  // - 必须在实例初始化 (脚本顶层) 登记: 主程序只在建图之前认新类型
  // - run 收到 {nodeName, type, config, state, threadId}, 返回节点输出:
  //   {writes: [{channel, value, mode}], command: {goto_node, updates}, sends: [...]}
  // - 节点执行是异步操作契约, run 也可以返回 Promise
  try {
    await agentxx.registerNodeType({
      type: "js_echo_node",
      configSchema: {
        type: "object",
        properties: { echo: { type: "string" } },
      },
      run: (input) => ({
        writes: [
          {
            channel: "js.out",
            value: { node: input.nodeName, echo: input.config && input.config.echo },
            mode: "overwrite",
          },
        ],
      }),
    });
  } catch (e) {
    // 主程序没有图注册表 (例如只验证脚本能力的环境): 不影响其余注册
    agentxx.log(3, "example_js registerNodeType skipped: " + e);
  }

  // ---- 工具 11: 工具标志与默认超时 (与 C++ kit 的 flags / timeoutMs 对齐) ----
  await agentxx.registerTool({
    name: "js_flag_probe",
    description: "JS plugin demo: tool with autoSummary / parallelSafe / repeatCallCheck.",
    parameters: {},
    autoSummary: true,
    parallelSafe: true,
    repeatCallCheck: true,
    timeoutMs: 5000,
    execute: () => ({ ok: true }),
  });

  // ---- 工具 12: 协作式取消 (ctx.isCanceled() 轮询, 与 C++ SDK 同语义) ----
  // - 主程序取消 (会话取消 / timeoutMs 到点) 只置标记, 脚本不查就继续跑完
  let cancelProbeObservation = -1;

  await agentxx.registerTool({
    name: "js_cancel_probe",
    description: "JS plugin demo: poll ctx.isCanceled() while working (default timeout 120ms).",
    parameters: {},
    timeoutMs: 120,
    execute: async (args, ctx) => {
      let seen = -1;
      for (let i = 0; i < 100; i++) {
        if (typeof ctx.isCanceled === "function" && ctx.isCanceled()) {
          seen = i;
          break;
        }
        await new Promise((r) => setTimeout(r, 10));
      }
      cancelProbeObservation = seen;
      return { canceledAt: seen };
    },
  });

  await agentxx.registerTool({
    name: "js_cancel_report",
    description: "JS plugin demo: report what the cancel probe last observed.",
    parameters: {},
    execute: () => ({ canceledAt: cancelProbeObservation }),
  });

  // ---- 通知点: agent_start (point 0) ----
  await agentxx.onHook(0, (info) => {
    agentxx.log(2, "js hook agent_start fired: " + JSON.stringify(info));
  });

  // ---- 事件: 订阅 `plugin.<插件名>.<事件>` ----
  // - 名字写全, 框架只校验不补前缀; 读侧可跨族 (也能订阅 `agentxx.agent.*`)
  await agentxx.subscribe("plugin.example_js.demo", (payload) => {
    agentxx.log(2, "js event received: " + JSON.stringify(payload));
  });

  // ---- 插件互查 API 演示: 判断依赖是否安装 + 自适应 ----
  // - agentxx.getPlugin("agentxx_javascript_engine") → 信息对象或 null
  // - agentxx.listPlugins() → 全部已安装插件数组
  const engineInfo = await agentxx.getPlugin("agentxx_javascript_engine");
  if (engineInfo) {
    agentxx.log(
      2,
      "js engine installed: v" + engineInfo.version + " (type " + engineInfo.type + ")",
    );
  } else {
    agentxx.log(4, "js engine missing");
  }
  const allPlugins = await agentxx.listPlugins();
  agentxx.log(2, "installed plugins: " + allPlugins.map((p) => p.name + "@" + p.version).join(", "));

  // ---- 顶层异步初始化 (module 顶层 await 直接可用) ----
  await new Promise((resolve) => setTimeout(resolve, 5));
  agentxx.log(2, "example_js async init done");

}
