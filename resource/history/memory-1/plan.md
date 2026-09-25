# 内存优化方案: 降低每轮上下文拷贝 (请求体 JSON / SSE 解析 / 图状态消息数组)

- 难度: C (跨 lib + neograph, 需保持 checkpoint/协议语义不变)
- 类型: 性能与内存优化 (本文仅为设计方案, **未实施**)
- 时间: 2026-09-25 (2026-09-26 / 88fee6e0 按更新后的 neograph 重新实测基线并核对调用点)
- 状态: 设计完成, 基线已按新依赖重测 (旧数据作废), 待实施
- 相关文档: [benchmark.md 第 10 / 11 节](../../../docs/zh-cn/design/benchmark.md) (mimalloc 开关对照与重测记录)
- 原始数据: [resource/benchmark/](../../benchmark/) (`2026-09-26_88fee6e0_windows-longctx/` 采样与分配统计、
  `2026-09-26_88fee6e0_linux-resource/` 基准报告、`harness/` 驱动脚本)

## 0. 背景与实测数据

### 0.1 依赖更新与本次重新实测

neograph 子模块已更新为上游重写后的 master + 重写后的 fork 补丁
(`1522761`, 主仓提交 `88fee6e0`, 2026-09-25)。重写后图状态的读写实现、
内置 reducer 与各调用点行号均有变化, 因此本文的基线数据整体重测了一遍,
**下面所有数字都来自 2026-09-26 的这次重测**, 早期版本的数字已作废。

测量环境与负载 (与 benchmark.md 第 11 节同一套台账):

- 构建: Windows release (VS18 / MSVC), 同一构建目录内只切换
  `AGENTXX_ENABLE_MIMALLOC` (ON + `MIMALLOC_LINK=SHARED`, 或 OFF 即系统分配器);
- 负载: `agentxx_cli cli` + 本地 OpenAI 兼容 mock 服务 (只回 SSE 且立即返回),
  配置无插件、单个 mock 模型; 经 stdin 逐条送入固定大小的用户消息 (每条一轮),
  全部轮次完成后空转 4 秒再取稳态值;
- 对照组: 上一发布版本 `agentxx-0.3.0-windows-x64` (2026-09-17, 系统分配器),
  用同一套负载驱动;
- 采样口径与复现步骤见 §4;
- 标注: 日期 `2026-09-26`, 主仓 commit `88fee6e0`, neograph `1522761`; 原始数据与
  驱动脚本见 `resource/benchmark/2026-09-26_88fee6e0_windows-longctx/` 与
  `resource/benchmark/harness/`。

结论先说: **重写后的每轮分配次数比旧版本高约 2~4 倍**
(50 × 8KB 组: `malloc req~` 1.3 GiB → **3.1 GiB**; 小上下文组每轮固定开销
5.8 MB → **26 MB**), 常驻内存也相应升高 (系统分配器口径 100K 上下文
专用工作集 7.95 MB → **8.2~8.5 MB**, 提交 10.36 MB → **21~24 MB**)。

### 0.2 每轮分配次数 (mimalloc `MIMALLOC_SHOW_STATS=1`, 退出统计)

| 组 | `malloc req~` (累计) | 每轮 | 对照: 旧基线 |
|---|---|---|---|
| 50 轮 × 8KB (≈100K 上下文, 请求体 ≈410 KB) | **3.1 GiB** | 62 MB | 1.3 GiB / 26 MB |
| 100 轮 × 8KB (≈200K 上下文, 请求体 ≈813 KB) | **9.9 GiB** | 99 MB | – |
| 5 轮 × 80KB (≈100K 上下文) | **364.8 MiB** | 73 MB | – |
| 50 轮 × 100B (小上下文, 请求体 ≈15 KB) | **1.3 GiB** | 26 MB | 290 MiB / 5.8 MB |

50 × 8KB 组的分配器自身明细: 活跃数据仅 632 KiB (`binned current`),
`binned` 累计 2.0 GiB + `huge` (≥512 KiB) 累计 1.1 GiB, 线程峰值 26,
用时 5.38 s (user 0.343 s / sys 0.562 s), 进程峰值 RSS 63.1 MiB /
峰值提交 224.9 MiB。

分 bin 归因 (50 × 8KB 组, 取累计量与块数较大的若干 bin):

| 块大小 (bin) | 累计量 | 块数 | 每轮块数 | 推测对应 |
|---|---|---|---|---|
| 10 KiB (`bin S 37`) | 419.1 MiB | 42.9K | ≈858 | 待确认 (协议/流式/日志缓冲) |
| 128.5 KiB (`bin L 52`) | 393.8 MiB | 3.1K | ≈62 | 整段上下文级缓冲 (请求体/状态) |
| 257.0 KiB (`bin L 56`) | 486.5 MiB | 1.9K | ≈38 | 整段上下文级缓冲 (请求体/状态) |
| 64.2 KiB (`bin M 48`) | 243.3 MiB | 3.8K | ≈76 | 中间缓冲 |
| 32.1 KiB (`bin M 44`) | 132.5 MiB | 4.2K | ≈84 | 中间缓冲 |
| 16.0 KiB (`bin M 40`) | 67.5 MiB | 4.3K | ≈86 | 中间缓冲 |
| 384 B (`bin S 18`) | 59.4 MiB | 162.3K | ≈3.2K | 每条消息/每字段级小对象 |
| 6 KiB (`bin S 34`) | 47.7 MiB | 8.1K | ≈162 | 单条消息级 |
| 128 B (`bin S 12`) | 19.8 MiB | 162.8K | ≈3.3K | 小对象 |
| 32 B (`bin S 4`) | 7.2 MiB | 237.7K | ≈4.8K | 小对象 |
| ≥512 KiB (`huge`) | 1.1 GiB | 570 | ≈11 (平均 ≈2 MB/块) | 整段图状态序列化产物 |

与旧基线相比有两点变化, 方案的重点因此调整:

1. **8 KiB 块不再是主要项**: 旧基线里 "每条消息文本被拷贝约 13 次/轮"
   (8 KiB bin 264 MiB / 33.8K 块) 的推理在重写后不成立 —— 现在
   `bin S 36` (8 KiB) 只有 2.8 MiB / 365 块;
2. **占比最大的是整段状态序列化**: `huge` 块 1.1 GiB / 570 块 (每轮 ≈11 块,
   平均 ≈2 MB), 加上 128/257 KiB 两档 L 块 (每轮 ≈100 块)。这与 neograph
   重写后的调用路径吻合 —— 运行期每一步都在整段序列化图状态:
   `graph_engine.cpp:1370 / 1404 / 1460 / 1505 / 1541 / 1627 / 1637`
   (`state.serialize()`), `graph_executor.cpp:222`
   (`hash_state_for_cache(state.serialize())` —— 只为算缓存键就整段序列化),
   `graph_executor.cpp:772` (`state_snapshot`)。

### 0.3 常驻内存 (Windows release, 稳态值, MB)

系统分配器 (release 默认, `AGENTXX_ENABLE_MIMALLOC=OFF`):

| 组 | 专用工作集 | 工作集 | 提交 | 旧基线 (同口径) |
|---|---|---|---|---|
| 50 轮 × 8KB | 8.2~8.5 (首次运行 11.8) | 25.5~25.9 | 21.0~24.1 | 7.95 / 21.95 / 10.36 |
| 100 轮 × 8KB | 19.9 | 34.0 | 30.8 | 13.75 / 28.29 / 17.79 |
| 5 轮 × 80KB | 9.2 | 21.3 | 16.9 | 6.09 / 20.09 / – |
| 50 轮 × 100B | 4.6 | 19.3 | 14.9 | – |

mimalloc 打开时 (`AGENTXX_ENABLE_MIMALLOC=ON`, 同一构建目录):

| 组 | 专用工作集 | 工作集 | 提交 | 调参后提交 |
|---|---|---|---|---|
| 50 轮 × 8KB | 31.8~47.6 | 62.0~64.6 | 224.4~238.9 | 43.8 |
| 100 轮 × 8KB | 83.9 | 117.5 | 288.6 | – |
| 5 轮 × 80KB | 31.2 | 45.6 | 176.1 | – |
| 50 轮 × 100B | 9.1 | 25.1 | 112.6 | – |

调参 = 运行期 `MIMALLOC_PURGE_DELAY=0` + `MIMALLOC_PAGE_COMMIT_ON_DEMAND=1`
(分配次数不变, 但释放的页及时归还, 提交量回落)。

对照: 上一发布版本 (2026-09-17, 系统分配器) 同负载 50 轮 × 8KB 为
专用工作集 6.1~10.5 / 工作集 24.6~24.9 / 提交 12.7~13.5 MB —— 即当前代码的
每轮分配量与提交量都高于上一版本, 而工作集相近, 说明**多出来的部分主要是
分配器保留/提交的临时缓冲, 不是稳态数据**。

因此本轮优化无论用哪个分配器都受益: 把 churn 降下来 (尤其是整段状态序列化),
提交量与 peak 才能跟着降。

## 1. 目标与非目标

目标 (同一 mock 负载):

1. 50 轮 × 8KB 组 `malloc req~` 从 **3.1 GiB 降到 1.2 GiB 以内** (每轮 ≤24 MB);
2. 其中的整段状态序列化类结果 (`huge` ≥512 KiB) 从 **1.1 GiB / 570 块
   降到 200 MiB / 100 块以内** (每轮 ≤2 块);
3. 小上下文组 (50 × 100B) 的**每轮固定开销**从 **26 MB 降到 10 MB 以内**;
4. 系统分配器口径: 100K 上下文的专用工作集/提交再降 3~5 MB (重测后基线
   8.2~8.5 / 21~24 MB);
5. **行为不变**: LLM 请求内容、SSE 语义、checkpoint/恢复、消息顺序与 reducer
   语义一致。

非目标:

- 不更换/不恢复默认分配器 (mimalloc 仍默认关闭, 需要时用选项打开);
- 不改 SSE 协议与请求体字段;
- 不改 `viewMessages` / `llmMessages` 双份存储 (那是稳态内存, 不在本轮范围);
- 不重构图引擎整体状态模型, 只加零拷贝访问 API 并收敛调用点。

## 2. 现状链路 (每轮发生的上下文级拷贝, 行号已按更新后的依赖核对)

| # | 步骤 | 位置 (2026-09-26 核对) | 粒度 | 说明 |
|---|---|---|---|---|
| 1 | `state.get_messages()` | `lib/src/nodes/modelcall.cpp:153` (`build_params`) | 整段 + 每条 | 内部 `get("messages")` 整数组深拷贝 + 逐条 `from_json` |
| 2 | `messages_to_json(params.messages)` | `lib/src/protocol/openai_provider.cpp:504` | 整段 + 每条 | typed → neograph json |
| 3 | `fromNeographJson(...)` | 同上 / `lib/include/agentxx/util/neograph_json_bridge.h:54` | 整段 + 每条 | neograph json → `utilxx_base::Json` 逐节点深拷贝 |
| 4 | `body.dump()` | `openai_provider.cpp:764 / 889 / 1069 / 1202` | 整段 | 序列化成请求体文本 (含 Responses / 非流式路径) |
| 5 | `req.body() = body;` | `third_party/cxx_utilxx/src/http_client.cpp:1358` (重定向路径 `:1201`) | 整段 | `string_view` → beast body 再拷一份 (请求期间两份并存) |
| 6 | `in.state.get("messages")` + 重建 + `overwrite` | `lib/src/nodes/modelcall.cpp:647` 读, `:677` 写 (同文件 `:633` 为修复路径) | 整段 | 每轮都把 system 消息替换/插入: 读一份深拷贝, 再整份覆盖写回 |
| 7 | 每次写 messages 通道 | `lib/.../modelcall.cpp:230 / 267 / 701`、`toolcall.cpp:546 / 1112 / 1131`, reducer 见 `neograph/src/core/graph_loader.cpp:30` (`reducer_append`), 落地见 `graph_state.cpp:96 / 149` (`write` / `apply_writes`) | 整段 | `reducer_append` 每次 `json result = current;` 深拷贝整个数组, 每轮 2~4 次 |
| 8 | 会话/运行起始播种 | `lib/src/agent/agent_runner.cpp:31`、`base_agent.cpp:983 / 1090` (`state.overwrite("messages", toNeographJson(...))`) | 整段 | 每次进入运行把会话侧消息整体转 JSON 再写进图状态 |
| 9 | `get_messages()` 其它调用点 | `modelcall.cpp:277`、`toolcall.cpp:527 / 846 / 1140` (另 `:1125` 走 `state.get("messages")`)、`summarization.cpp:721 / 1074`、`code_agent.cpp:141`(默认关) | 整段 + 每条 | 每轮 5~8 次 |
| 10 | neograph 内部 `get_messages()` | `graph_node.cpp:81 / 179 / 240`、`graph_loader.cpp:119` (`has_tool_calls` 条件)、`plan_execute_graph.cpp:104`、`graph_engine.cpp:1648` (运行结束取 final_response) | 整段 + 每条 | LLM/ToolDispatch/Intent 等节点各自再读一次 |
| 11 | 每步整段状态序列化 | `neograph/src/core/graph_engine.cpp:1370 / 1404 / 1460 / 1505 / 1541 / 1627 / 1637`, `graph_executor.cpp:222 / 772` | 整段 (含全部消息) | 重写后新增占比最大的一项: 只为缓存键/事件/结果就整段 `serialize()` |
| 12 | 异常路径整段读取 | `lib/include/agentxx/nodes/wrap_handle.h:262` (`state.get("messages")`) | 整段 | 仅在节点异常重抛时发生, 低频 |
| 13 | SSE 逐行解析 | `openai_provider.cpp:1330` (`processSseBuffer`) / `:1355` (`processSseLine`) | 单事件 | 每事件 3 次堆分配 + `buf.erase(0,n)` 逐行搬移 (字节量小, 分配次数多) |

已经做过优化、本轮保持的部分: SSE 用 `utilxx_base::JsonView` 零拷贝路由
(`openai_provider.cpp:1389` 起)、HTTP 层 `respBody.clear()` 复用容量
(`http_client.cpp` `flushBody`)、工具结果裁剪、`viewMessages`/`llmMessages`
合并前的现状。

## 3. 方案

### P0. 收敛"每步整段状态序列化" (neograph 侧, 重测后占比最大)

重测显示这一项已经超过请求体链路, 建议列为第一优先:

- `graph_executor.cpp:222` 的 `hash_state_for_cache(state.serialize())`:
  只为给节点缓存算一个键就把整段状态 (含全部消息) 序列化一次。可改为
  只对参与缓存的通道取内容 (例如新增 `GraphState::hash_channels(names)`),
  或对消息通道用"长度 + 尾部若干条摘要 + 版本号"的廉价指纹;
- `graph_engine.cpp` 内多处 `result.output = state.serialize()` /
  事件里的 `state.serialize()`: 按需产出 (只在回调真的消费时, 或只带 `messages`
  的尾窗/增量), 并把"运行结束的最终结果"与"事件载荷"分开;
- `graph_executor.cpp:772` 的 `state_snapshot`: 明确它是"每轮一次"还是
  "每节点一次", 只在真正需要一致快照的地方保留。

验收信号: `huge` 块从 570 块 / 1.1 GiB 降到 ≤100 块 / ≤200 MiB,
`malloc req~` 至少下降 1.5 GiB (每轮 −30 MB 量级)。

### P1. 请求体链去重 (agentxx 侧, 低风险)

**P1-1 `requestSseAsync` 支持移动 body**

- 现状: `HttpClient::requestSseAsync(..., std::string_view body, ...)` → `req.body() = body;`
- 改法: 增加 `std::string&&` 重载 (或 `ByValue`/`TakeBody` 显式参数), 内部
  `req.body() = std::move(body);`, 之后 `prepare_payload()`; 调用方
  (`OpenAIProvider::doStream`) 传 `std::move(bodyStr)`
- 收益: 省 1 份整段拷贝 + 降峰值 1 份 (100K ≈ 0.4 MB/轮, 200K ≈ 0.8 MB/轮)
- 注意: 重定向/重试路径 (`requestAsync` 的同名逻辑, 见 `http_client.cpp:1201`)
  需要 body 存活 → 仅在"不跟随重定向/一次性发送"的 SSE 路径移动, 其它路径保持拷贝

**P1-2 删掉 `fromNeographJson` 这一整份深拷贝**

- 现状: `body["messages"] = fromNeographJson(messages_to_json(params.messages));`
  (`openai_provider.cpp:504`) —— neograph json 先建一棵树, 再逐节点深拷贝到
  `utilxx_base::Json`, 最后 `dump()` 成文本
- 改法: 让 provider 直接持有 neograph json 作为 body (`body["messages"] =
  messages_to_json(...)`), 用 `neograph::json::dump()` 产出请求体文本;
  `utilxx_base::Json` 只在需要写日志/埋点的裁剪路径上按需构造
- 收益: 省 1 份整段 + 每条的深拷贝 (≈0.4~0.8 MB/轮)
- 注意: 目前 provider 的 body 类型是 `utilxx_base::Json`, 需同时改
  `buildBody` / `buildResponsesBody` / `completeAsync` / `doStream` 的签名与
  `applyHeaders` 无关部分; `params.extra_fields` 等字段构造要逐一核对

**P1-3 (依赖 P2) `build_params` 不再做 typed ↔ JSON 往返**

- 现状: `state.get_messages()` (JSON→typed) → 插 system 消息 → `messages_to_json` (typed→JSON)
- 改法: provider 直接接收 state 的 messages JSON 引用 (P2 的零拷贝 API),
  system 消息替换在 JSON 层完成 (或由调用方一次性给出 `messages_override`)
- 收益: 省 2 份整段 (含 P1-2 合计每轮省 3~4 份上下文拷贝)

### P2. 图状态零拷贝访问 + 就地 append (neograph 侧)

注: 重写后的 neograph 已经有 `ChannelWrite::Mode::Overwrite`
(`graph_state.cpp:129` `apply_writes`)、`GraphState::overwrite(json&&)`
(`:105`) 与 `GraphState::remove()` (`:121`), 但仍**没有**本节要加的
"回调式只读访问"和"就地 append" —— 这两项加进去与现有 API 并列即可。

**P2-1 零拷贝读取 API**

`GraphState::get()` (`graph_state.cpp:64`) 按值返回 `json`, `get_messages()`
(`:71`) 再叠加一次逐条反序列化。新增 (保留旧接口不删, 老调用点可逐步迁移):

```cpp
// 读取: 回调形式, 锁在回调期间持有 (不能返回裸指针/引用, 否则解锁后可能被写者失效)
template <class Fn>
decltype(auto) with_json(std::string_view channel, Fn&& fn) const;

// 消息专用: 逐条给出 ChatMessage (不整体拷贝消息数组)
template <class Fn>
void for_each_message(Fn&& fn) const;          // fn(const ChatMessage&) 或 fn(ChatMessage&&)

// 需要整体视图时 (只读、不跨写操作): 快照句柄, 内部持有 json 的所有权
std::shared_ptr<const json> messages_snapshot() const;
```

- `get_messages()` 内部改走 `for_each_message` 以省掉"整数组 json 拷贝"这一步
  (逐条 `from_json` 仍然需要, 但不再整体复制文档);
- 注意锁语义: `get()` 目前用 `shared_lock` 后返回副本; 新 API 必须把锁的持有期
  限定在回调内, 文档里明确"回调中不得再写同名通道"。

**P2-2 `reducer_append` 就地追加**

- 现状 (每轮 2~4 次整段深拷贝, `graph_loader.cpp:30`):

```cpp
static json reducer_append(const json& current, const json& incoming) {
    json result = current.is_array() ? current : json::array();   // 整份深拷贝
    for (const auto& item : incoming) result.push_back(item);     // 逐条再拷贝
    return result;
}
```

- 改法 (二选一, 建议先做 (a)):
  - (a) 新增 `ChannelWrite::Mode::Append`: 在 `apply_writes`
    (`graph_state.cpp:129`) 里对 append 模式直接在通道值上
    `append_in_place(w.value)` (yyjson 可变文档 `yyjson_mut_arr_add_val`,
    必要时先 `reserve`); 结果值与现有 `reducer_append` 完全一致, 写日志/checkpoint
    重放语义不变;
  - (b) 新增 reducer 名 `"append_inplace"` (base_agent 的图定义里 messages 通道改用),
    语义与 (a) 等价, 改动更局部;
- 注意: 就地修改会让此前通过 `with_json`/`messages_snapshot` 获得的视图失效 →
  文档要求"视图生命周期内不得写入", 并且在 type/结构检查失败时回退到深拷贝路径;
- 注意: yyjson 文档扩容会搬移内部缓冲, 因此不要长期持有 `json` 内部指针 (现有封装
  `operator[]` 句柄已隐含这一点, 新 API 要写清楚)。

**P2-3 调用点收敛 (agentxx 侧)**

| 位置 | 现状 | 改法 |
|---|---|---|
| `modelcall.cpp:153` `build_params` | 读整份 messages | 交给 provider 直接读 (P1-3), 或只读一次后复用 |
| `modelcall.cpp:647`→`:677` (system 消息替换) | 每轮读一份深拷贝 + 整份覆写 | 用回调式 API 只判断首条是否是 system; 覆写改为"只改首条"的写入 (新增 `update_first` 或 `Mode::Overwrite` 的局部版本) |
| `modelcall.cpp:277` `repairMessages` | 再读一份, 可能整体覆写回 | 先判定"是否需要修复"(轻量检查: 遍历 + 早退), 需要时才取可写视图 |
| `toolcall.cpp:527 / 846 / 1140` | 3 次整份读取 | 抽一次读取结果在同一次 tool 执行内复用; 只读部分 (取最后一条 tool 消息等) 改 `for_each_message` 反向查找 |
| `summarization.cpp:721 / 1074` | 读整份 + `countTokens` | 用 `for_each_message` 累加 token; 只在真正触发压缩时才取整份 |
| `agent_runner.cpp:31` / `base_agent.cpp:983 / 1090` | 运行起始整段播种 | 与 P2-2 的就地 append 结合, 或改成"只在确实变了"时才覆写 |
| neograph `graph_node.cpp:81 / 179 / 240`、`graph_loader.cpp:119`、`plan_execute_graph.cpp:104`、`graph_engine.cpp:1648` | 每个节点各读整份 | 改 `for_each_message` 或加"节点内一次性读取"缓存 (节点运行期 state 不会被别的节点写) |

- 目标: 每轮整份读取次数从 10+ 次降到 2~3 次 (进入 LLM 前一次、写回一次)。

### P3. SSE 行解析 (低风险, 主要降 CPU / 分配次数)

- `processSseBuffer` (`openai_provider.cpp:1330`): 用 `std::string_view` 就地在
  缓冲上找 `\n` 并解析, 记录"已消费偏移"; 缓冲只在 `offset > 阈值` (如 8KB) 或
  整段处理完时 `erase(0, offset)` 一次, 去掉"每行搬移剩余缓冲";
- `processSseLine` (`:1355`): 去掉 `std::string line{line_in}` 与
  `payload = line.substr(5)` 两次分配, 改 `string_view` 就地裁剪 (`\r`、前导空格、尾部空白);
- 快速路径: 仅当行长含 `"usage"` / `"tool_calls"` / `finish_reason` 时才走
  `JsonView`; 纯 `delta.content` 行用字符串扫描取引号内文本 (注意转义 `\"`、`\\`、`\uXXXX`);
  保留现有 `JsonView` 分支作为慢路径 (出现意外字段时回退);
- 收益: 每轮约 2000 个事件 → 少约 6000 次堆分配; 字节量小, 主要体感在 CPU 与分配器压力;
- 测试: 覆盖 `\r\n`、跨块切断、`data:` 后无空格、`[DONE]` 无尾随换行、
  超长单事件、`\uXXXX` 转义等边界 (现有 `openai` 相关测试模块 + 新增用例)。

### P4. 次要项 / 待确认

- 会话持久化是逐条写 (`lib/src/agent/session_store.cpp:138` `updateViewMessage`),
  不是每轮整份快照 → 暂不处理;
- 工具 schema 每轮重建 (`build_params` 里 `tool_defs` + `appendDefinitions`) →
  若工具数多可缓存, 待量化后再定;
- `bin S 37` (10 KiB, 每轮 ≈858 块) 与 `bin S 18` (384 B, 每轮 ≈3.2K 块) 的
  来源尚未定位 —— 实施前先用一次带调用栈的采样确认是协议帧、日志缓冲还是
  流式渲染缓冲, 再决定是否纳入本轮;
- wire 协议侧是否存在整份上下文推送 (TUI 的 LLMContext 视图) → 未发现按轮推送,
  实施前再确认一次;
- `LogPrint` 中间件默认关闭 (`include/agentxx/agent/config.h:356`), 不在本轮范围。

## 4. 实施顺序与验收

建议顺序 (每步独立可验证、可回退):

| 步骤 | 内容 | 验收信号 |
|---|---|---|
| 1 | P0 收敛 `state.serialize()` (缓存键 + 事件载荷) | `huge` 块 ≤100 块 / ≤200 MiB; `malloc req~` −1.5 GiB |
| 2 | P1-1 body move | `malloc req~` 再降 ~20 MiB (50 轮); 无行为差异 |
| 3 | P1-2 去掉 `fromNeographJson` | 再降 ~20~40 MiB; 请求体与改造前逐字节一致 (mock 记录 body 做 diff) |
| 4 | P3 SSE 行解析 | 事件数不变; 每轮小分配明显减少; 边界用例全过 |
| 5 | P2-1 零拷贝读取 API + 调用点收敛 | 整段读取次数 ≤3 次/轮 |
| 6 | P2-2 就地 append | 8 KiB/384 B 档小对象块数下降; checkpoint 恢复测试通过 |

测量方法 (2026-09-26 / 88fee6e0 实际使用的台账, 可复现; 驱动脚本见
`resource/benchmark/harness/`):

1. 起本地 OpenAI 兼容 mock 服务 (Python, 只回 SSE, 立即返回), 配置
   `base_url: http://127.0.0.1:<port>/v1`、`plugin.list: []`、
   `model_context_max_token` 取 1M (避免压缩介入), data_dir 指向临时目录;
2. `agentxx_cli cli --config <cfg>` 经 stdin 逐条送入固定大小的用户消息
   (每条一行, 每条一轮), 全部轮次被 mock 服务收到后空转 4 秒;
3. 采样 (300 ms 一次): 工作集 (`WorkingSet64`)、专用工作集
   (`Win32_PerfFormattedData_PerfProc_Process.WorkingSetPrivate`, 即任务管理器
   "内存"列)、提交 (`PrivateMemorySize64`)、峰值提交
   (`PeakPagedMemorySize64`)、CPU (`UserProcessorTime` / `PrivilegedProcessorTime`);
4. 关闭 stdin 让进程正常退出, 取 `MIMALLOC_SHOW_STATS=1` 的退出统计
   (`malloc req~` 与分 bin 明细);
5. 每个配置至少跑 3 次, 取范围 (本次基线: 50 轮 × 8KB 组 mimalloc
   `malloc req~` 3.1 GiB; 系统分配器专用工作集 8.2~8.5 MB, 提交 21~24 MB)。

回归测试 (必须覆盖):

- 图 state: messages 通道 append 后的值/顺序与旧 reducer 一致; checkpoint 写出→恢复→
  继续 append 的结果一致; 并发读写 (不同 thread_id 的会话) 不串;
- provider: 请求体字段与旧实现逐字节 diff 一致 (含 tools/extra_fields/stream_options);
- SSE: 上述边界用例 + 截断检测 (`missing [DONE]`) 行为不变;
- 既有测试模块: `openai` 相关、`session_persistence`、`memgrowth`、`interrupt_ui`。

## 5. 风险与回退

| 风险 | 说明 | 回退 |
|---|---|---|
| 就地 append 破坏 checkpoint 语义 | 结果值等价, 但写日志/重放依赖 reducer 契约 | 用新增 `Mode::Append`/新 reducer 名, 旧路径保留; 出问题切回 `"append"` |
| 零拷贝视图被写失效 | 回调后返回引用会悬空 | 只提供回调式 API + 快照句柄; 文档明确锁与生命周期 |
| 请求体改造引入字段差异 | body 类型从 `utilxx_base::Json` 换到 `neograph::json` | 用 mock 服务记录 body 逐字节 diff, 差异即回退信号 |
| SSE 快速路径漏转义 | `\uXXXX`/转义处理不完整会导致文本错误 | 只在"纯 content 且无转义序列"时走快速路径, 其余回退 JsonView |
| 减少序列化影响上游语义 | 缓存键/事件载荷是上游重写后新增的机制, 直接删可能影响节点缓存命中与可观测性 | 先按"键的等价替换"(只换成轻量指纹) 做, 保留开关; 事件载荷按需产出 |
| 依赖库继续演进导致行号漂移 | 本次已按 `1522761` 核对全部调用点 | 方案按函数名/符号定位; 每次更新依赖后重跑 §4 台账 |

## 6. 待确认问题

1. neograph 侧是否接受 P0 (轻量缓存键 + 按需事件载荷) 与 P2 的新 API
   (neograph 由本项目维护, fork 补丁已可自主添加 —— 本次更新后
   `ChannelWrite::Mode::Overwrite` / `GraphState::overwrite(json&&)` /
   `GraphState::remove()` / `GraphEngine::update_state` 都是这么加进去的);
2. `messages` 通道除 `append` 外是否有其它写入路径 (checkpoint 恢复、UI 注入、
   运行起始播种 —— 见 §2 第 7/8 行) 需要一并适配 `Mode::Append`;
3. `bin S 37` (10 KiB) / `bin S 18` (384 B) 的调用来源需要一次带调用栈的采样确认;
4. 本轮是否顺带处理 `viewMessages` / `llmMessages` 双份存储 (状态在实施前确认);
5. 是否需要在 `agentxx_benchmark` 里新增"逐轮长上下文"场景, 把上述指标纳入常驻基准
   (现有 `resource_cli` 是注入上下文, 序列化次数比真实逐轮对话少)。
