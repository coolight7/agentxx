> 状态: **设计 v3.2（重置历史 + 保留兼容机制）, 待实施**（进度记录写同目录 `work.md`）
> 迭代记录:
> - v1（2026-10 初稿）: "分级兼容" —— 支持新老跨代混用, 逐表考古历史布局、放宽基线。
> - v2（同日）: "重置方案" —— 放弃跨代混用, 把版本窗口/基线/逐成员判定整体删除。
> - **v3（用户拍板）**: **一次性重置为干净 v1（不背历史包袱）, 但机制全部保留**,
>   从这次改动之后开始"改动必须考虑兼容性"。
> - **v3.1（用户拍板）**: ① `hooks_ex` 折回 `hooks` **已定**（不再保留"两张表"这个选项）;
>   ② **每张接口表都必须带四件套: IID + `struct_size` + `version` + `MIN_VERSION`**。
> - **v3.2（用户拍板）**: ③ **跨边界结构体统一带 `struct_size`**（4 个 client spec 补上,
>   并顺带补齐另外 5 个同样缺的跨边界结构体, 共 9 个 —— 见 §3.5、§5.3）。
> - **v3.3（用户拍板）**: ④ **新增并列工作项: 术语统一与文档/注释整理**（见 §11）——
>   "宿主 / 应用侧" 这类含糊称呼统一修正（agentxx = 主程序, cxx_* = 插件框架,
>   插件分动态库插件 / JS 插件）, 并清理产品黑话。
> - **v3.4（用户拍板）**: ⑤ **直译词换成日常说法**（§11.6）：`领域` → `业务`、
>   `装载` → `加载`, 以及 `承载`/`载体`/`编排`/`接缝`/`链路`/`拓扑(非算法处)` 等同类词。
>
> 用户拍板原话:
> - "MIN_VERSION 要保留, 只是这一次重构抛弃历史版本兼容性, 以后修改要开始考虑兼容性的。"
> - "既然这一版重构不考虑以前版本的兼容，自然就没有 hooks_ex 的问题了，直接从 hooks 表
>   改就可以了，每个表都应当有 IID、struct_size、version、min_version。"
> - "5.3 是的需要补充 struct_size，保证统一。"
> - "进一步修改计划文档, 增加: 整理修正 agentxx、cxx_* 依赖库的文档和注释, 统一关系称呼,
>   agentxx 称为主程序, 它依靠 cxx_* 库实现的动态加载插件框架称为插件框架, 可加载动态库插件,
>   也可以通过 JS 运行时插件实现加载 JS 插件。不再使用 `宿主`、`应用侧` 等模糊的词语 …
>   需要替换移除产品黑话, 更换为通俗易懂、开发人员注释常用的词语和语句。"
> - "整理文档时忽略 `resource/history` 内的文档 … 对于 agentxx 项目的文档和注释, 可以说明
>   主程序就是指的 agentxx, 而 cxx_* 的库是多项目通用的, 称主程序而不能关联 agentxx。"
>
> 触发背景: 阶段 5（钩子清单与优先级）在 `AgentxxPluginHooksIface` 表尾追加成员时，
> 撞上"`struct_size < sizeof(Iface)` 即整表判不可用"的检查规则，被迫改用新接口表
> `agentxx.agent.hooks_ex` 规避。

---

## 0. 结论摘要

1. **旧账一次清掉**：全部 29 张接口表的 `version` / `MIN_VERSION` 重置为 1，
   `BASELINE_SIZE` = 当前（本次整理后的）表长度；插件级 `PLUGINXX_API_VERSION` /
   `AGENTXX_CLIENT_PLUGIN_API_VERSION` 也保持 1。**本次不再考古历史布局**，
   "老布局长什么样"从此不关心。
2. **每张表四件套（硬要求）**：IID 表名宏 + `struct_size` + `version` + `MIN_VERSION`,
   缺一不可（逐表清单与校验规则见 §3.9）。现状 30 张表已有前三项, 本方案补第四项。
3. **跨边界结构体统一带 `struct_size`**：凡跨边界传递的结构体（spec = 插件填宿主读、
   input = 宿主填插件读）都必须有一个 `uint32_t struct_size` 字段（§3.5）。
   现状 9 个缺（4 个 client spec + 3 个 agent spec + 2 个宿主填的输入结构体）,
   本方案一次补齐 —— 其中 6 个是"把已有的 `_reserved` 对齐占位改名为 `struct_size`"，
   **大小与偏移不变**，只有 3 个因无空洞而 +8 字节（§5.3）。
4. **机制全部保留**：IID（换契约）、版本对（`VERSION` + `MIN_VERSION`, 语义级）、
   `BASELINE_SIZE` + `struct_size`（能安全读多少字节）、能力名字符串（可选功能开关）。
   **从这次之后，任何表改动都必须按 §3.6 的规则做**，
   做不到就换 IID，而不是"改完拒载一片"。
5. **判定规则**：`version >= MIN_VERSION && struct_size >= BASELINE_SIZE`
   （整表级）+ 逐成员 `ifaceHas`（追加成员级）。修掉现状写死字面量 `1` 的潜伏 bug
   （一张表升版会连累所有表查询失败）。
6. **借重置窗口做结构整理**：`hooks_ex` **折回** `hooks`（agent 侧 21 张表 → 20 张,
   全部表 30 → 29 张）。折回是既定动作: 既然本次不背历史兼容, "独立表"的唯一理由
   （回避表尾追加导致的整表判死）就不存在了。旧二进制在新宿主上会因 `struct_size` 不足
   被明确拒绝（可诊断, 不会静默错调）。
7. **不做的事**：不重排成员顺序（结构体大小不变, 闸门挡不住 → 换 IID 才有意义）；
   不给"表内成员"再造第三套机制；不保留任何"为旧二进制留的后门"。
8. **未来改动的操作手册见附录 E**（这是本次改动最重要的产出物之一）。
9. **并列工作项: 术语统一**（§11）：统一称呼 —— **agentxx = 主程序**，
   **cxx_\* 库提供的插件加载机制 = 插件框架**，插件分**动态库插件**与 **JS 插件**
   （后者由 **JS 运行时插件** 承载）。停用 `宿主`、`应用侧`、`基座` 等含糊称呼
   （实测 `宿主` 1388 行/179 文件、`应用侧` 4 行、`宿主侧` 38 行），
   清理产品黑话（§11.5），并把**直译词换成日常说法**（§11.6）：
   `领域` → `业务`（106 行）、`装载` → `加载`（114 行，主流写法已是"加载" 658 行）、
   `承载`/`载体`/`编排`/`接缝`/`链路`/`拓扑(非算法)` 一并处理。
   `resource/history/` 内文档不动（§11.3）。

---

## 1. 背景与现状

### 1.1 当前机制

| 机制 | 位置 | 现状 |
|---|---|---|
| 核心 vtable | `pluginxx/api/abi.h` 的 `PluginxxHostVtable` | 冻结: `alloc` / `free` / `query_interface` |
| IID 查询 | `query_interface(host, &iid)` + `pluginxx/kit/kit.h` 的 `queryInterface<Iface>` | COM 风格, agent 21 张表 + client 9 张表 |
| 表头字段 | 每张表前 8 字节: `int32_t version; uint32_t struct_size;` | 表版本恒为 1（API v1 规范化时全部重置过） |
| SDK 校验 | `pluginxx::validateInterface<Iface>` | `iface->version != 1 \|\| iface->struct_size < sizeof(Iface)` → 整表返回 nullptr |

调用点: `queryInterface<Iface>` 只被两个聚合查询用到 ——
`agentxx::plugin::AgentIfaces::query()`（agent 侧 21 张）与
`agentxx::plugin::ClientIfaces::query()`（client 侧 9 张），由 `pluginxx/kit/kit.h` 提供模板，
`agent/lib/include/agentxx/plugin/api/plugin_kit.h` 里 `using pluginxx::validateInterface;`。

插件级版本（另一层，别与表级混淆）：

- `PLUGINXX_API_VERSION = 1` / `PLUGINXX_MIN_API_VERSION = 1`（`pluginxx/api/abi.h`）。
- client 侧 `AGENTXX_CLIENT_PLUGIN_API_VERSION = 1`，宿主**严格相等**检查
  （`client_plugin_manager.cpp`）；agent 侧宿主**完全不检查**（缺口, 见 D6）。

### 1.2 三类跨版本形态与现状行为

| 场景 | 具体例子 | 现状 | v3 期望 |
|---|---|---|---|
| **A. 表长增加（尾部追加成员）** | 新插件用 `ui->open_overlay`，跑在老宿主（表更短）上 | 整表拒绝 → 面板/状态栏/toast/命令**全丢** | 表可用；只有该成员按 `ifaceHas` 判为不可用（其余照常） |
| **B. 表版本提升（语义变化）** | 宿主把某张表升到 v2，插件用 v1 头文件 | `version != 1` → 整表拒绝（且**所有表**一起失败, 见 D2） | `version >= MIN_VERSION` 即接受；需要特定档位时 `ifaceHasVersion(t, n)` |
| **C. 表尾成员为 NULL（同代能力子集）** | CLI 宿主填不满 ui 表，某成员为 NULL | OK（插件判空降级） | 不变（这是"宿主能力子集"，与版本无关） |

**v3 与现状的差别只在判定规则**：现状是"长度不等就整表死"，v3 是"低于基线才整表死，
达到基线就把表交出去，越界的成员逐个判"。重置让"低于基线"这件事**只在真正跨代**时发生，
不再像今天这样"加个成员就踩"。

### 1.3 现状缺陷清单

- **D1**（方向 A）新插件 + 老宿主：整表拒绝，能力"越用越少"，且违反 `client_plugin_api.h`
  自己写的约定（"老宿主同位置为空指针 → 插件按该子能力不支持降级"）。
- **D2**（潜伏 bug）`validateInterface` 写死字面量 `1` 而非"该表自己的 `MIN_VERSION`"：
  任意一张表合法升到 2，**所有表**的查询一起失败。
- **D3** 表版本号当前无实际作用（全为 1），既不能表达"我支持到第几级"，也不能表达
  "我不再兼容谁"；语义变化（布局不变、含义变化）没有任何信号可用。
- **D4** 入参结构体（插件 → 宿主）的 `struct_size` 注释写的是"**传 0 时按当前布局解析**"，
  这是**前进兼容陷阱**（老调用方清零内存后，新宿主按最新布局读它没填的尾部字段）；
  且三处检查写法各不相同。应统一为"**0 = 基线布局**"。
- **D5** `AgentxxPluginFeaturePointSpec` / `AgentxxPluginFeatureImplSpec` 声明了
  `struct_size` 但宿主**完全没检查**（同一契约两种执行力度）。
- **D5b** 跨边界结构体**契约不齐**：现状 9 个结构体**没有 `struct_size` 字段**
  （其中有 6 个拿 `_reserved` 占位当对齐 —— 有位置却没当长度用）：

  | 方向 | 结构体 | 现状头 8 字节 | 位置 |
  |---|---|---|---|
  | 插件填 → 宿主读 | `AgentxxPluginToolSpec` | `name`, `description`…（无空洞） | `plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxPluginHookSpec` | `point`, `_reserved` | `plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxPluginGraphNodeTypeSpec` | `type`, `run_start`…（无空洞） | `plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxToolRenderSpec` | `version`, `_reserved` | `client_plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxOverlaySpec` | `version`, `type`（无空洞） | `client_plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxTimerSpec` | `version`, `_reserved` | `client_plugin_api.h` |
  | 插件填 → 宿主读 | `AgentxxKeybindSpec` | `version`, `_reserved` | `client_plugin_api.h` |
  | 宿主填 → 插件读 | `AgentxxToolRenderInput` | `version`, `_reserved` | `client_plugin_api.h` |
  | 宿主填 → 插件读 | `AgentxxUiActionContext` | `version`, `_reserved` | `client_plugin_api.h` |

  后果：这些结构体**无法尾部追加可选字段**（新插件/老宿主、或新插件读老宿主填的输入时
  没有长度可判），只能"整体换新结构体"。§3.5 统一补齐。
- **D6** agent 侧宿主不检查插件 `api_version`（client 侧检查），插件级闸门两边不一致。
- **D7** 追加策略缺失：没有任何规则说"新增成员必须追加在表尾"，历史上
  `PluginxxCapabilitiesIface::register_capability_ex` 就是插在中间。
- **D8** 客户端 UI 表的"v2/v3 段"注释、`plugin_api.h` 里 `hooks_ex` 的注释、
  `plugins.md` §9 的"版本约定"段，三者都在用"整表校验会失败，所以永远别追加"这套
  说法为现状辩护 —— 重置后它们全部要重写。
- **D9** 表头契约不齐：现状 30 张表都有 IID 宏、`version`、`struct_size`，
  **但都没有 `MIN_VERSION` / `BASELINE_SIZE` 常量**，判定只能写死字面量（D2 的根因）；
  且没有任何测试在保证"新表也配齐这四项"（§3.9 的四件套规则就是补这一条）。

### 1.4 本轮获得的自由度

因为**一次性重置**（所有宿主与插件一起重编，旧二进制明确不受支持），以下操作
**本次允许**；但**从本次之后**，除第一行外都要按 §3.6 的规则走（即"要兼容"）：

| 操作 | 本次（重置窗口） | 以后 |
|---|---|---|
| 表尾追加成员 | 允许（等同"纳入 v1 布局"） | 允许（不改基线 + `ifaceHas` 判定） |
| 合并/删除整张表（如 `hooks_ex` 折回 `hooks`） | **允许**（表对象消失 → IID 未命中，可诊断） | 允许但要走"废弃周期"：先停用、后删除 |
| 改某张表的大小（增删成员） | 允许 | 只在表尾追加；删除成员 = 破坏性（换 IID） |
| 重排成员顺序（大小不变） | 允许但不做（无收益, 且有静默错调风险） | **禁止**（闸门挡不住） |
| 改成员签名（大小可能不变） | 允许但不做（同上） | **禁止**（换 IID） |
| 改入参结构体布局 | 允许 | 只许尾部追加 + `struct_size` gate |

---

## 2. 目标与非目标

**目标**

- G1 抓住重置窗口，把当前（整理后的）布局**冻结为一组干净的 v1 基线**，
  不再背任何历史包袱。
- G2 **完整保留兼容机制**（版本对 + 基线 + 逐成员判定 + 能力名），并把使用规则
  写进规范与文档，让"以后的改动默认是兼容的"。
- G3 修掉现状机制里的缺陷：写死 `1`（D2）、整表判死（D1）、三种 spec 校验写法（D4）、
  缺失的 spec 检查（D5）、**9 个跨边界结构体缺 `struct_size`**（D5b）、
  缺失的插件级闸门（D6）。
- G4 借窗口做结构整理：`hooks_ex` 折回 `hooks`，删掉一切"为跨代兼容而做的绕路"。
- G5 机制可自检：追加成员破坏"尾部规则"、基线写错、布局被重排时，编译期或测试期报错。
- G6 **每张表四件套齐全**（IID + `struct_size` + `version` + `MIN_VERSION`），
  由 `boundaries` 逐表校验，新增表漏配任何一项即测试失败（§3.9）。
- G7 **称呼统一、去含糊**（§11）：同一件事在全项目只有一个称呼 ——
  主程序（agentxx）/ 插件框架（cxx_\*）/ 动态库插件 / JS 插件 / JS 运行时插件；
  不再出现 `宿主`、`应用侧`、`基座` 这类"看上下文才知道指谁"的词。
- G8 **黑话清零 + 直译词换日常说法**：注释与文档里不出现产品黑话
  （AGENTS.md 禁用词表 + 本项目自定的 `口径`/`落地`/`埋点`/`门禁` 等），
  也不用"要回译才能懂"的直译词（`领域`→`业务`、`装载`→`加载`、`承载`/`载体`/
  `编排`/`接缝`/`链路`/`拓扑(非算法处)`，§11.5、§11.6）。

**非目标**

- N1 不改核心 vtable（`alloc/free/query_interface` 永久冻结）。
- N2 不改 IID 字符串与结构体/宏名字（除 `hooks_ex` 折回这一项，见 §5.1）。
- N3 不引入"反射式能力探测"（不按符号名/哈希查成员）。
- N4 **不保留旧二进制的兼容性**：本次重置明确声明"重置前的宿主/插件不再支持"，
  由插件级版本闸门 + 表长度闸门让它们**明确失败**（而不是静默错调）。
- N5 不改 `PLUGINXX_API_VERSION` 数值（保持 1）。
- N6 **本轮只改文档与注释的措辞**：代码标识符（类名 / 目录名 / 函数名 / 参数名）
  的重命名单列（§11.4 第二档），不在本轮做；`cxx_utilxx` 里带项目名的**字符串常量**
  （`.agentxx/agent/worktrees`、`agentxx_default_device`、`kServerBanner`）属行为，
  也不在本轮（§11.7 记为待决项）。

---

## 3. 设计

### 3.1 四种机制的分工

| 机制 | 回答的问题 | 谁维护 | 变化时的动作 |
|---|---|---|---|
| **IID** | 是不是这张表 | 字符串常量（人） | 破坏性改动 / 语义级别不可共存 → 新 IID |
| **版本对**（`MIN_VERSION` + `VERSION`） | 契约是第几级、带哪些语义保证 | 人（测试锁定） | 追加/语义变化 → `VERSION` +1；不再兼容老版本 → `MIN_VERSION` +1 |
| **`BASELINE_SIZE` + `struct_size`** | 我能安全读多少字节 | 编译器（`offsetof`/`sizeof`） | 追加成员 → 追加在表尾（基线不变，成员偏移自动变大） |
| **能力名字符串** | 支持不支持某个**可选功能** | 人（清单/能力表） | 与 ABI 无关的功能开关，优先用它 |

**为什么四件都要（缺一不可）**

- 只有版本号、没有 size：忘记给新增成员加守卫就是**越界读**（读到别人的内存/UB）；
  size 由编译器算（`offsetof + sizeof`），写错会 `static_assert` 失败，不会静默。
- 只有 size、没有版本号："布局不变但语义变了"没有任何信号；也表达不了"我支持到第几级"。
- 只有 size + 版本号、没有能力名：每个可选功能都得占一个表成员（表越来越长、
  老宿主每次都被迫整表判死），而能力名是纯字符串、零 ABI 代价。
- 没有 IID：无法表达"这是一份新契约"，只能靠"改内容"，无从协商。

### 3.2 版本与基线的定义（每张表三个宏）

```c
/* 表版本 (当前/最新): 本头文件所描述的完整布局与语义级别
 * - 追加成员 / 语义变化 → +1 (用于日志排障与 ifaceHasVersion)
 * - 纯文案/注释变化 → 不动 */
#define AGENTXX_IFACE_CLIENT_UI_VERSION 1

/* 最低兼容版本: 本头文件编写的代码能与之协同工作的最低表版本
 * - 宿主表 `version < MIN_VERSION` → 整表不可用 (拒绝)
 * - 只在"同 IID 内不再兼容旧布局/旧语义"时才 +1 (会拒掉老宿主, 优先换 IID) */
#define AGENTXX_IFACE_CLIENT_UI_MIN_VERSION 1

/* 基线布局: 表头之后所有提供本表的宿主都应具备的字节数
 * - 重置时 = sizeof(表) (冻结的 v1 布局)
 * - 以后追加成员时保持不变 (数值上等于"第一个追加成员的偏移")
 * - 用 offsetof 表达, 不写死字节数 (32/64 位与编译器无关) */
#define AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE \
    sizeof(AgentxxClientUiIface)
```

规则：

- **宿主发布**：`version = X_VERSION`，`struct_size = sizeof(XIface)`
  （即"我能提供到这里"）。
- **插件判定整表可用**：IID 命中 + `version >= X_MIN_VERSION` +
  `struct_size >= X_BASELINE_SIZE`。
- **插件判定某成员可用**：`ifaceHas(ui, member)`（§3.3），与 `ifaceHasVersion(ui, n)`
  （语义档位）配合。
- **常量只增不减**（当前大版本内）：`BASELINE_SIZE` 冻结；`MIN_VERSION` 只升；
  `VERSION` 只升。
- 头文件保留"追加历史"表（记录每个追加成员属于哪一档与起始 `VERSION`），供排障。

### 3.3 表查询判定规则（替换现状）

```cpp
// pluginxx/api/iface_meta.h（新增, 机制层）
namespace pluginxx {

/// 每张表的元信息: 由各表头在本头之后提供特化; 未特化 = 严格 (等价于"必须完整")
/// - 各表头另需 `static_assert`, 保证基线是该表成员的合法偏移
template<typename Iface>
struct IfaceMeta {
    static constexpr int32_t  min_version   = 1;
    static constexpr uint32_t baseline_size = sizeof(Iface);
};

/// 成员可用性: 表长覆盖该成员 (追加成员必须用它判定, 不能只判空)
template<auto Member, typename Iface>
constexpr bool ifaceHas(const Iface* t) noexcept {
    return t != nullptr
        && t->struct_size >= uint32_t{offsetof(Iface, Member)}
                              + uint32_t{sizeof(((Iface*)nullptr)->Member)};
}

/// 版本档位判定 (语义变化用; 布局可能完全不变)
template<typename Iface>
constexpr bool ifaceHasVersion(const Iface* t, int32_t v) noexcept {
    return t != nullptr && t->version >= v;
}

} // namespace pluginxx
```

```cpp
// pluginxx/kit/kit.h（改判定, 修 D2）
/// 查询主程序接口表并转型 (IID → 接口表)
///
/// 判定规则:
/// - `version >= IfaceMeta<Iface>::min_version` (语义级别兼容)
/// - `struct_size >= IfaceMeta<Iface>::baseline_size` (基线字节数够)
///
/// 注意: 不再与字面量比较; 每张表用自己的常量 (修掉"一张表升版连累所有表"的 bug)。
/// 表可用 ≠ 全部成员可用: 追加成员必须用 ifaceHas 判定 (见 iface_meta.h)。
template<typename Iface>
const Iface* validateInterface(const void* raw) noexcept {
    if (!raw) {
        return nullptr;
    }
    const auto* iface = static_cast<const Iface*>(raw);
    if (iface->version < IfaceMeta<Iface>::min_version
        || iface->struct_size < IfaceMeta<Iface>::baseline_size) {
        return nullptr;
    }
    return iface;
}
```

特化与自检（放 `pluginxx/api/iface_meta.h` 尾部与 `agentxx/plugin/api/plugin_iface_meta.h`）：

```cpp
template<> struct pluginxx::IfaceMeta<AgentxxClientUiIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_UI_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE;
};
// 自检: 基线必须是合法偏移 (表头 8 字节之后, 且不超过完整尺寸)
static_assert(AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE >= 8);
static_assert(AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE <= sizeof(AgentxxClientUiIface));
```

**重置期的取值**：所有表 `min_version = 1`、`baseline_size = sizeof(表)`（严格）。
于是**行为与今天完全一致**（表要么全可用要么全不可用）—— 差别在"以后"：
追加成员时把 `baseline_size` 改成 `offsetof(Iface, 第一个追加成员)` 即可，无需动别处。

### 3.4 成员可用性判定的两条路（都要）

- **首选（零 ABI 改动）**：能力名字符串。`plugin.yaml` 的 `interfaces.require/optional`
  + `get_client_state().interfaces` 数组（client 侧）与能力表 `pluginxx.capabilities`
  的 `has_capability`（agent 侧）。适用于"功能有无"，
  且插件在老宿主上不会被跳过加载（用 optional）。
- **兜底（ABI 级）**：`ifaceHas(t, member)` / `ifaceHasVersion(t, n)`。适用于"成员是新增的，
  没有任何能力名"或"能力名无法表达的程度差异"。写法（**判空必须与 ifaceHas 同时满足**）：

```cpp
if (ifaceHas<&AgentxxClientUiIface::open_overlay>(ui) && ui->open_overlay) {
    return ui->open_overlay(host, &spec);
}
return -1;   // 宿主不支持: 插件降级
```

- **基线以内的成员不用逐成员判**（整表门槛已保证）；判断成本只落在追加段成员上。
- SDK 侧统一收口（避免每个调用点重复）：`plugin_kit.h` 的 `ClientPluginBase` 与自由函数
  内部用 `ifaceHas` 判定，插件调用点不变。
- **重置期没有追加段**（基线 = 完整表），所以本期**不产生** `ifaceHas` 调用点；
  机制先备好，第一次追加时按 §3.6 使用。

### 3.5 跨边界结构体（spec / input）规则

**统一规则（一句话）**：凡跨边界传递的结构体，**填充方必须填 `struct_size`（自己写入的字节数），
读取方必须按它 gate**。方向有两个，规则同一条：

| 方向 | 结构体 | 填充方 | 读取方 | 读取方什么时候需要 gate |
|---|---|---|---|---|
| 插件填 → 宿主读 | spec（如 `AgentxxOverlaySpec`） | 插件 | 宿主 | 老插件（短）+ 新宿主（长）：宿主读尾部字段前必须判长度 |
| 宿主填 → 插件读 | input（如 `AgentxxToolRenderInput`） | 宿主 | 插件 | 新插件（认识长布局）+ 老宿主（填得短）：插件读尾部字段前必须判长度 |

（反方向都天然安全：读取方只认识自己的前缀字段，偏移与填充方逐字节一致。）

**统一形态**：

```c
typedef struct AgentxxFooSpec {
    uint32_t struct_size;  /* 首字段: 0 = 基线布局; 其它 = 填充方写入的字节数 */
    int32_t  version;      /* 可选: 该结构体自身语义档位 (有语义档位需求时才设) */
    /* ... 基线字段 ... */
    /* ... 追加字段 (必须全部在尾部) ... */
} AgentxxFooSpec;

/* 基线 = 第一个追加字段的偏移; 全是基线字段时 = sizeof */
#define AGENTXX_FOO_SPEC_BASELINE_SIZE sizeof(AgentxxFooSpec)
```

读取方判定（一个 helper 收口，放 `agent/lib/include/agentxx/plugin/api/plugin_spec.h`）：

```cpp
/// 跨边界结构体长度校验 (读取方)
/// - `0` = 填充方按基线布局填写 (零填充是安全默认)
/// - `>= 基线` = 接受; 追加字段必须按对方给的长度 gate 后再读
/// - `< 基线` = 拒绝 (连基线字段都缺, 无法定义语义)
template<typename Spec>
constexpr bool specSizeOk(uint32_t struct_size) noexcept {
    return struct_size == 0 || struct_size >= SpecBaseline<Spec>::value;
}

/// 追加字段可读判定 (与 ifaceHas 同理, 只是长度由对方给而不是我方看表)
template<auto Member, typename Spec>
constexpr bool specHas(uint32_t struct_size) noexcept {
    return struct_size >= uint32_t{offsetof(Spec, Member)}
                          + uint32_t{sizeof(((Spec*)nullptr)->Member)};
}
```

**本次要补齐的 9 个结构体**（现状见 D5b；6 个零成本、3 个 +8 字节）：

| 结构体 | 现状首 8 字节 | 改法 | 大小变化 |
|---|---|---|---|
| `AgentxxPluginHookSpec` | `point(4) + _reserved(4)` | `struct_size(4) + point(4)`（`_reserved` 让出位置） | **不变** |
| `AgentxxToolRenderSpec` | `version(4) + _reserved(4)` | `struct_size(4) + version(4)` | **不变** |
| `AgentxxTimerSpec` | `version(4) + _reserved(4)` | 同上 | **不变** |
| `AgentxxKeybindSpec` | `version(4) + _reserved(4)` | 同上 | **不变** |
| `AgentxxToolRenderInput` | `version(4) + _reserved(4)` | 同上（宿主填 `sizeof`） | **不变** |
| `AgentxxUiActionContext` | `version(4) + _reserved(4)` | 同上（宿主填 `sizeof`） | **不变** |
| `AgentxxPluginToolSpec` | `name…`（无空洞） | 首字段插入 `struct_size` | +8 |
| `AgentxxPluginGraphNodeTypeSpec` | `type…`（无空洞） | 首字段插入 `struct_size` | +8 |
| `AgentxxOverlaySpec` | `version(4) + type(4)` | 插入 `struct_size`（`version` 之后） | +8 |

**已经有 `struct_size` 的 5 个**（不动布局，只统一注释说法为"`0` = 基线布局"）：
`AgentxxPluginHookSpecEx`（首字段）、`AgentxxPluginPermissionPathQuery`（首字段）、
`AgentxxPluginFeaturePointSpec`（首字段）、`AgentxxPluginFeatureImplSpec`（首字段）、
`AgentxxPluginToolPermissionSpec`（中部，已满足"必须有"）。

要点与变更：

- **`0` = 基线布局**（现状注释写"当前布局"，是前进兼容陷阱；改为基线后"零填充"是安全默认）。
- 三处宿主检查（工具权限声明 / 路径查询 / 钩子扩展）改为 `specSizeOk`；
  `plugin_manager_feature.cpp` 的两处**补齐**检查（D5）。
- 主程序侧新增填 `struct_size` 的位置：`AgentxxToolRenderInput`（构造渲染输入时）、
  `AgentxxUiActionContext`（派发动作时）。
- 插件侧读宿主填的 input 时若读追加字段要 `specHas`（本期没有追加字段 → 无调用点）。
- 规范（写进 `plugins.md` §9）：**结构体新增可选字段只能加在尾部，并保持
  `BASELINE_SIZE` 不变**；读取方读追加字段前必须 `specHas` 判定。

### 3.6 追加策略（本次之后必须遵守的规则）

1. **表的成员只能追加在表尾**；"插入到中间"视为破坏性改动（换 IID）。
2. 追加成员时：`BASELINE_SIZE` **不变**（值 = 第一个追加成员的 `offsetof` 表达式）；
   `VERSION` +1；受影响调用点改为 `ifaceHas` 判定；头文件"追加历史"表补一行。
3. 语义变化（不改布局）：`VERSION` +1，头文件写明"v2 起 xxx 语义为…"，
   调用点用 `ifaceHasVersion(t, 2)`；`MIN_VERSION` 不动。
4. **不再兼容老版本（破坏性）**：优先换 IID；确实要同 IID 硬切时提升 `MIN_VERSION`
   （会拒绝所有低于它的宿主/插件），并在 `CHANGELOG` / `plugins.md` 里点名。
5. **禁止**：重排成员、改成员签名、删除成员（三者都可能"大小不变"从而静默错调）。
   要改 → 换 IID。
6. 自检：新增成员若被插在中间，`static_assert(第一个追加成员的 offset == BASELINE_SIZE)`
   会失败；`boundaries` 测试增加"追加策略"规则与逐成员偏移断言（§7）。

### 3.7 重置窗口要完成的整理（一次性动作）

| 整理项 | 动作 | 原因 |
|---|---|---|
| `agentxx.agent.hooks_ex` | 折回 `agentxx.agent.hooks`（表尾追加三成员） | 单列一张表的唯一理由是"回避表尾追加的整表判死"；机制修好后该理由消失（§5.1） |
| 历史承诺注释 | 改写（"老宿主同位置为空指针"等） | 与机制说法统一（D8） |
| 历史段标记 `(v3)` | 去掉 | 分代标记已无意义（版本对取代了它） |
| `capabilities` 的中间插入 | **不重排**（保持现状） | 重排无收益且无闸门；当前布局直接冻结为 v1 基线（§5.2） |
| 跨边界结构体补 `struct_size` | 9 个（4 个 client spec + 3 个 agent spec + 2 个 input；6 个零成本、3 个 +8 字节） | 统一"填充方填长度、读取方 gate"规则（§3.5、§5.3） |
| 接口表数量 | 21 → 20（10 通用 + 10 领域） | 折回的连带更新（`kInterfaceTableCount`、`plugins.md` §8、根 `AGENTS.md`、`boundaries`） |

### 3.8 FAQ

**Q1：表有变更，`MIN_VERSION` 不就应该跟着涨吗？涨了直接拒载，那 `struct_size` 还有用？**

`MIN_VERSION` 只在**破坏性改动**（同 IID 内不再兼容旧布局/旧语义）时涨 —— 那是唯一该拒载的
场合，而且优先做法是换 IID。变更分三类，只有第三类拒载：

| 变更类型 | 布局 | `VERSION` | `MIN_VERSION` | 拒载? | 成员可用性靠什么判定 |
|---|---|---|---|---|---|
| ① 尾部追加成员 | 变长 | +1 | **不动** | 否 | `struct_size` 的 `ifaceHas`（唯一判据） |
| ② 语义变化（不改布局） | 不变 | +1 | **不动** | 否 | `version` 的 `ifaceHasVersion` |
| ③ 破坏性改动 | 任意 | +1 | 可 +1 | 是 | 不适用（优先换 IID） |

如果按"表一变就涨 `MIN_VERSION`"，等于"新增一个成员 = 淘汰所有老宿主/老插件" ——
那就是把现状的整表判死换个判据，D1 一点没修。**所以 `MIN_VERSION` 保留但平时恒为 1**，
它的价值是"将来真做破坏性改动时有个明确、可诊断的拒载开关"。

**Q2：表都是宿主构造的，那"新插件 + 老宿主"才需要判断；反向（新宿主 + 老插件）
老插件本来就用不到新成员，是不是完全不受影响？**

对，正是这条不对称：

| 组合 | 表 `struct_size` | 谁读 | 需要判断? | 结果 |
|---|---|---|---|---|
| 新插件 + 老宿主 | 短 | 插件 | 需要（`struct_size >= 基线` + 追加成员 `ifaceHas`） | 表可用，新成员判为不可用 |
| 老插件 + 新宿主 | 长 | 插件 | 不需要（只认识自己的前缀成员，偏移逐字节相同） | 完全不受影响 |

这个免费方向成立的**唯一前提是"只追加在尾部"**（同名成员偏移必须逐字节相同）。
往中间插一个成员，老插件会静默调到隔壁函数 —— 这是 §3.6 第 1/5 条与 §7 偏移断言的来由。

**Q3：为什么不用"判空指针"判断成员在不在？**

门槛放宽到基线后，越界成员**必须先看 size**：老宿主的表是它自己二进制里的一份静态对象
（例 `client_plugin_manager.cpp` 的 `static const AgentxxClientUiIface table = {...}`），
越过末尾去读 `ui->open_overlay` 读到的是**紧邻的另一个全局变量**的字节，
几乎不可能是 NULL → 判空放它过去 → 调进野地址。所以"判空"必须与 `ifaceHas` 同时满足。
基线**以内**的成员不受影响（整表门槛已保证），判断成本只在追加段。

**Q4：为什么要保留 `struct_size` 字段本身？**

四个用途，都不是版本号能替代的：① 追加成员的可用性判据（唯一）；② 基线门槛；
③ 入参结构体长度校验；④ 跨编译器/手写结构体的布局漂移自检（类型宽度、pack、对齐、
32/64 位差异会让"版本号相同而字节数不同"）；⑤ 排障时一眼看出两边表长不同。

**Q5：为什么这次重置还要保留机制，而不是将来需要时再加？**

因为机制的价值全在"**改的时候**"：等你需要跨代兼容时再加，就得面对一堆已经发布出去的
布局（就是本次要避免的考古）。重置窗口是把它**零成本**铺好的唯一时机：
铺好后所有表的基线 = 当前布局，一切从"干净 v1"开始。

---

### 3.9 每张表的四件套（硬要求）

**规则**：每一张接口表都必须同时具备四项，缺一不可；新增表同样。

| 项 | 形态 | 谁写 | 作用 |
|---|---|---|---|
| **IID** | `#define X_IFACE_<名> "命名空间.名字"` | 人（字符串常量） | `query_interface` 的键；换契约 = 换 IID |
| **`struct_size`** | 表头第 2 个字段 `uint32_t` | 宿主装配时填 `sizeof(表)` | 能安全读多少字节（基线门槛 + 追加成员 `ifaceHas` + 排障） |
| **`version`** | 表头第 1 个字段 `int32_t` | 宿主装配时填 `X_VERSION` | 语义档位（追加/语义变化 → +1） |
| **`MIN_VERSION`** | `#define X_MIN_VERSION` | 人（当前恒为 1） | 本头文件能协同工作的最低表版本；破坏性改动时才 +1 |

**四件套在源码里的位置（统一形态）**：

```c
/* ---- 1. IID 与版本对 (三个宏, 紧挨着表格结构体声明之前) ---- */
#define AGENTXX_IFACE_CLIENT_UI             "agentxx.client.ui"
#define AGENTXX_IFACE_CLIENT_UI_VERSION     1
#define AGENTXX_IFACE_CLIENT_UI_MIN_VERSION 1
#define AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE sizeof(AgentxxClientUiIface)

/* ---- 2. 表格结构体 (头 8 字节固定为 version + struct_size) ---- */
typedef struct AgentxxClientUiIface {
    int32_t  version;     /* == ..._VERSION, 由宿主填写 */
    uint32_t struct_size; /* == sizeof(...), 由宿主填写 */
    /* ... 成员 ... */
} AgentxxClientUiIface;

/* ---- 3. 元信息特化 (集中在 plugin_iface_meta.h) ---- */
template<> struct pluginxx::IfaceMeta<AgentxxClientUiIface> {
    static constexpr int32_t  min_version   = AGENTXX_IFACE_CLIENT_UI_MIN_VERSION;
    static constexpr uint32_t baseline_size = AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE;
};
```

**本方案覆盖的 29 张表（折回后）**：

| 分组 | 数量 | 表（IID 去掉命名空间前缀） |
|---|---|---|
| 通用表 `pluginxx.*`（`pluginxx/api/tables.h`） | 10 | `events` / `capabilities` / `scheduler` / `coroutine_runtime` / `plugins` / `config` / `cancel` / `json` / `log` / `tasks` |
| agent 领域表 `agentxx.agent.*`（`plugin_api.h`） | 10 | `tools` / `permission` / `hooks`（含折回的三成员） / `session` / `context` / `model` / `prompt` / `resources` / `graph` / `feature` |
| client 领域表 `agentxx.client.*`（`client_plugin_api.h`） | 9 | `ui` / `events` / `session` / `wire` / `self` / `json` / `log` / `timer` / `keybind` |

合计 **29 张**（现状 30 张 = 上表 + `agentxx.agent.hooks_ex`，折回后消失）。
计数方式（三处必须一致，`boundaries` 测试会校验）：agent 侧 10 + 10 = **20**，
client 侧 7 + 2 = **9**，通用表 10（两侧共用，不重复计入领域表）。

**校验（防漏防错）**：`boundaries` 新增一条规则，逐表检查四项齐全：

1. 从三份头文件里收集"表格结构体名字"（`typedef struct XxxIface {`）与对应的
   `X_IFACE_*` / `X_VERSION` / `X_MIN_VERSION` / `X_BASELINE_SIZE` 宏；
2. 断言：**每个表结构体都有 IID 宏**（用 `query_interface` 里出现的 IID 集合反查）、
   `VERSION`、`MIN_VERSION`、`BASELINE_SIZE`，且 `MIN_VERSION <= VERSION`；
3. 断言：`IfaceMeta` 特化数与表数相等（漏加特化 → 编译期/测试期报错）；
4. 断言：`static_assert(BASELINE_SIZE >= 8 && BASELINE_SIZE <= sizeof(表))` 对每张表成立。

**`IID` 的"每张表都有"怎么体现**：IID 是"每张表一个名字宏"，本方案不改命名体系；
`boundaries` 现有的"表名集合"规则（规则 10）已经把 29 个名字与 `plugins.md` §8、
`AGENTS.md` 对齐，本次只需把数量从 30/21 改成 29/20。

---

## 4. 变更清单（逐文件）

### 4.1 插件框架（`agent/third_party/cxx_pluginxx/`，自研、可改）

| 文件 | 变更 |
|---|---|
| `include/pluginxx/api/iface_meta.h` | **新增**：`IfaceMeta` 模板 + `ifaceHas` + `ifaceHasVersion`；10 张通用表的特化与 `static_assert` |
| `include/pluginxx/kit/kit.h` | `validateInterface` 改用 `IfaceMeta`（修 D2）；头注释重写（基线 + 逐成员 + 禁止重排）；`queryInterface` 语义不变（不加新形参） |
| `include/pluginxx/api/tables.h` | 10 张通用表：加 `X_MIN_VERSION` / `X_BASELINE_SIZE` 常量（配齐四件套, §3.9）；表头注释统一为"版本对 + 基线 + 只允许表尾追加" |
| 构建 | 改了 `third_party` 后**必须删掉 build 里对应目录**再编，否则不生效（AGENTS.md 有记） |

### 4.2 表头（`agent/lib/include/agentxx/plugin/api/`）

| 文件 | 变更 |
|---|---|
| **新增** `plugin_iface_meta.h` | agent + client 侧各表的 `IfaceMeta` 特化与自检（集中一处便于清点；特化数须等于表数） |
| **新增** `plugin_spec.h` | `specSizeOk<Spec>()` / `specHas<Member, Spec>()` + 各结构体基线常量（`SpecBaseline<Spec>` 特化） |
| `plugin_api.h` | 各表配齐四件套（补 `MIN_VERSION` / `BASELINE_SIZE`，§3.9）；3 个 spec 补 `struct_size`（`AgentxxPluginHookSpec` / `AgentxxPluginToolSpec` / `AgentxxPluginGraphNodeTypeSpec`）；已有 `struct_size` 的 5 个 spec 注释改为"0 = 基线布局"；文件头"版本策略"段重写 |
| `client_plugin_api.h` | 同上；ui 表注释重写（删"老宿主同位置"与 `(v3)`）；4 个 spec 补 `struct_size`（3 个零成本 + `AgentxxOverlaySpec` +8）、2 个 input 补 `struct_size`（`AgentxxToolRenderInput` / `AgentxxUiActionContext`） |
| `plugin_kit.h` | 钩子相关：`hookEx`/`unhookEx`/`listHooks` 内部改用 `ifaceHas`（折回后它们在同一张表）；表头文档补"表可用 ≠ 成员可用"；渲染输入的填充逻辑填 `struct_size` |

### 4.3 主程序侧（`agent/lib/src/plugins/`）

| 位置 | 变更 |
|---|---|
| `plugin_manager_adapters.cpp`（工具权限声明） | "小于我的就拒" → `specSizeOk`（接受 `0` 与 `>= 基线`） |
| `plugin_manager_vtable.cpp`（路径查询 / 表装配） | 同上；`hooks_ex` 表对象删除，三入口填入 `hooks` 表；`query_interface` 去掉 `hooks_ex` 分支 |
| `plugin_manager_hooks.cpp`（钩子扩展） | 长度检查改 `specSizeOk`；文件头注释表名更新 |
| `plugin_manager_feature.cpp` | **新增**两处 `specSizeOk` 检查 |
| 渲染输入 / 动作上下文填充处（`client_plugin_manager.cpp` 的渲染回调构造与 `AgentxxUiActionContext` 派发） | **新增**填 `struct_size = sizeof(...)` |
| `plugin_manager.cpp` / `plugin_manager_lifecycle.cpp`（加载入口） | **新增**插件级检查：`info->api_version < PLUGINXX_MIN_API_VERSION` → 拒载（对齐 client 侧的检查方式；`0`/过低都拒） |
| `client_plugin_manager.cpp` | 检查方式统一为 `< AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION`（当前等于 1，行为不变）；`hooks_ex` 相关装配删除 |
| `plugin_manager.h` | `kInterfaceTableCount` 10 + 11 → 10 + 10；`hookPriorities` 等注释里的表名更新 |
| `assembly_snapshot.cpp` | 装配快照的接口表清单与数量（20）；钩子段来源表名 |

### 4.4 SDK 与插件侧

| 位置 | 变更 |
|---|---|
| `plugin_interfaces.h` | `AgentHooksEx` 常量删除，"有序登记/优先级"说明并入 `AgentHooks` |
| `plugins/*/plugin.yaml` | 删 `agentxx.agent.hooks_ex` 声明（example_feature 等） |
| `plugins/example_feature` | `hookEx` 用法说明/接口名改为 `hooks`（代码调用点不变） |

### 4.5 文档

| 文件 | 变更 |
|---|---|
| `docs/zh-cn/design/plugins.md` §9 | 重写"版本约定"：四机制分工、三常量、判定规则、追加策略、四件套要求、跨边界结构体规则（填充方填长度 + 读取方 gate）、样例代码 |
| `docs/zh-cn/design/plugins.md` §8 | 接口表表格：`hooks_ex` 行并入 `hooks`；补"版本对/基线"列或说明 |
| 根 `AGENTS.md` | "接口表数量" agent 21 → 20（合计 30 → 29）；新增"接口表版本与基线"小节（§3.2/§3.6/§3.9 浓缩） |
| `docs/zh-cn/design/index.md` | 目录树注释补"表版本对 + 基线"一句 |
| `resource/history/plugin-share-feature/work.md` | 阶段 5 章节补一句"`hooks_ex` 已按 plugin-version 方案折回" |

### 4.6 测试

| 文件 | 变更 |
|---|---|
| `agent/test/plugin/test_plugin_runtime.cpp` | 判定规则用例（§7.1）；ABI 断言表更新为折回后的结构与数量 |
| `agent/test/core/test_boundaries.cpp` | 数量 agent 21 → 20（合计 30 → 29）；新增"四件套完整性"规则（§3.9）与"追加策略"规则（成员顺序台账 + 偏移断言） |
| `agent/test/plugin/test_plugin_hooks.cpp` | 表名 `hooks_ex` → `hooks`（断言主体复用） |
| `agent/test/plugin/test_plugin_sdk.cpp` / `test_client_plugins.cpp` | 接口名替换；插件级 `api_version` 闸门用例 |
| 新增结构体用例（可并入 `plugin_runtime`） | `specSizeOk` 三态 + `specHas` 两态；6 个零成本结构体的 `sizeof`/偏移断言；3 个 +8 结构体的新 `sizeof` 断言 |
| 新增 DSO fixture（可选） | `api_version = 0` 的插件 → 断言拒载 + 日志 |
| `boundaries`（术语规则，§11.8） | 新增"称呼白名单"规则：源码/文档里出现 `宿主`/`应用侧`/`基座` 时，必须命中白名单（标识符名、目录名、OS 内核用法）否则报错；另加黑话与直译词检查（§11.8 第 2/3 条） |

### 4.7 术语与文档统一（§11 的逐文件对应位置）

| 位置 | 变更 |
|---|---|
| `agent/third_party/cxx_pluginxx/{README.md, include/pluginxx/**}` | 主体叙述统一为"主程序/插件框架"；**去掉 agentxx 作为"本库服务的项目"的写法**（示例改中性或标"例如"）；`host/*.h` 已有的"主程序"这一说法推广到全库 |
| `agent/third_party/cxx_pluginxx_ui/{README.md, docs/integration.md}` | 同上（该库文档里 agentxx 出现较多，属"客户端接入示例"，须标注为示例） |
| `agent/third_party/cxx_utilxx*/**` | 仅注释措辞（`http_server.cpp` 的 `宿主` → `主程序`；`crypto.h` 的历史说明措辞）；**不动字符串常量** |
| `agent/lib/include/agentxx/plugin/**` | `宿主` → `主程序`（表头 / SDK / 管理器，实测 `plugin_kit.h` 78、`client_plugin_manager.h` 62、`client_plugin_api.h` 50、`plugin_api.h` 44、`plugin_manager.h` 42、`plugin_interfaces.h` 31 …）；`基座`/`内核` 按 §11.2 改写 |
| `agent/lib/src/plugins/**` | 同上（`client_plugin_manager.cpp` 38、`plugin_manager_vtable.cpp` 8、`plugin_manager_lifecycle.cpp` 10 …） |
| `agent/lib/{include,src}/agent/{agent_host,event_host}*`、`tools/host_tool.h` | 按 §11.3 各自含义改写注释（这几处 `宿主` **不是**插件语境：`AgentHost` = agent 节点容器、`EventHost` = 事件总线持有者、`HostTool` = 外部注册工具），须单独判定 |
| `agent/lib/**/ffi*` | FFI 场景的 `宿主` → **嵌入方程序**（§11.2 第 3 行；`HostTool` / `WireHostTool*` 属外部注册工具，注释写清注册方） |
| `agent/lib/include/agentxx/agent/io/{wire_protocol.h,agent_io_transport.h}` | `WireHostTool*` / `host_tool_*` 消息名的注释改为"外部注册工具"（**标识符与协议字符串不动**） |
| `agent/plugins/**` | `agentxx_javascript_engine.cpp`（68 处）为主线；其余插件按需（`宿主` 多指主程序） |
| `agent/test/**` | 注释与测试描述里的称呼（`test_plugins.cpp` 40、`test_client_plugins.cpp` 28、`test_plugin_bridge.cpp` 37 …） |
| `docs/zh-cn/design/{plugins.md,index.md,ffi.md,tui.md,security.md,roadmap.md,configuration.md,develop.md,benchmark.md}` | `plugins.md` 208 处、`index.md` 35 处、`ffi.md` 58 处为主；`tui.md` 里"宿主维护"= 主程序；`ui-layer.md` 1 处 |
| `docs/en/**` | 英文对应词：`宿主` → `main program`、`插件框架` → `plugin framework`、`动态库插件` → `dynamic library plugin`、`JS 插件` → `JS plugin`；`host` 只保留在标识符里（`PluginxxHost` 等） |
| 根 `AGENTS.md` | 全量替换（27 处）；并在"插件框架"小节写明三层关系（§11.1 的标准句） |
| 排除 | `resource/history/**`（用户明确要求不动） |

---

## 5. 结构整理的决策点

### 5.1 `agentxx.agent.hooks_ex` 折回 `agentxx.agent.hooks`（**已定: 折回**）

| 对比项 | 折回前（两张表） | 折回后（`hooks` 单表） |
|---|---|---|
| 表数量 | agent 侧 21（合计 30） | agent 侧 20（合计 29） |
| `hooks` 表布局 | 原两项 | 原两项 + 表尾三项（`register_hook_ex` / `unregister_hook_ex` / `list_hooks`） |
| 基线 | 两张表各自 `sizeof` | `hooks` 基线 = **新布局** `sizeof`（含三成员） |
| 与旧二进制混用 | 基础钩子可用，扩展登记不可用 | `hooks` 表整体不可用（`struct_size` 不足 → 可诊断） |
| SDK 实现 | 双路（表缺失回退基础钩子） | 单路 |
| 文档 | 多一条接口名 + 一段"为什么不追加" | 少一条，说明改在通用规则里 |
| 与重置方案一致性 | 独立表的理由是"跨代兼容"，与"机制修好后不需要绕路"矛盾 | 一致 |

**结论：折回（已定）**。改动点清单见附录 D；不再保留"两张表并存"的选项 ——
既然本次重置不背历史兼容，"独立表"的唯一理由（回避表尾追加导致的整表判死）就不存在了，
从 `hooks` 表直接改是最短路径。

### 5.2 `PluginxxCapabilitiesIface::register_capability_ex`（中间插入）

- 现状：位于 `register_capability` 之后、`unregister_capability` 之前。
- **决策：不重排**。重排不改变结构体大小 → `struct_size` 与 `version` 都挡不住旧调用方
  静默错调；而重排的收益（"让基线能划在中间"）在本方案里不存在 —— 本次重置后
  该成员**本来就是 v1 基线的一部分**，中间插入不再是问题。
- 注释里注明"本表成员顺序即契约（含历史中间插入项），禁止再动；改签名/顺序请换 IID"。

### 5.3 跨边界结构体是否补 `struct_size`（**已定: 全部补**）

用户在 §5.3 上拍板"需要补充 struct_size，保证统一"。为避免"统一了 4 个、还剩 5 个两套规则"，
本次按 §3.5 的统一规则**一次补齐 9 个**（4 个 client spec + 3 个 agent spec + 2 个 host→plugin 输入）：

| 结构体 | 动作 | 风险/代价 |
|---|---|---|
| `AgentxxToolRenderSpec` / `AgentxxTimerSpec` / `AgentxxKeybindSpec` | `_reserved` → `struct_size`（`version` 保留） | **零成本**（大小与偏移不变） |
| `AgentxxPluginHookSpec` | `_reserved` → `struct_size`（`point` 前移一位） | **零成本** |
| `AgentxxToolRenderInput` / `AgentxxUiActionContext` | `_reserved` → `struct_size`（宿主填 `sizeof`） | **零成本**（宿主填充处各加一行） |
| `AgentxxOverlaySpec` | 插入 `struct_size` | +8 字节 |
| `AgentxxPluginToolSpec` / `AgentxxPluginGraphNodeTypeSpec` | 首字段插入 `struct_size` | +8 字节 |
| 已有 `struct_size` 的 5 个 | 不动布局，注释统一为"`0` = 基线布局" | 无 |

> 注：spec / input 不是接口表，§3.9 的四件套（含 IID、`MIN_VERSION`）**不适用于它们**；
> 它们的规则见 §3.5 —— 一句话是"填充方填长度，读取方 gate"，两个方向同一条规则。
> 本期没产生任何"读追加字段"的调用点（无追加字段），机制先备好。

### 5.4 决策状态一览

| 项 | 状态 |
|---|---|
| 重置所有表为 v1（`VERSION` / `MIN_VERSION` = 1，基线 = 当前布局） | **已定** |
| 每张表四件套（IID / `struct_size` / `version` / `MIN_VERSION`） | **已定** |
| `hooks_ex` 折回 `hooks` | **已定**（§5.1） |
| `capabilities` 中间插入成员不重排 | **已定**（§5.2） |
| 跨边界结构体统一补 `struct_size`（9 个） | **已定**（§5.3） |

无未决项。

---

## 6. 实施步骤（建议 10 个提交: 6 个机制 + 4 个术语）

| 提交 | 内容 | 风险 |
|---|---|---|
| **1** | 机制层：新增 `iface_meta.h`（`IfaceMeta`/`ifaceHas`/`ifaceHasVersion`）+ `kit.h` 判定改用元信息（修 D2）+ 10 张通用表常量与特化 | 极低（重置期基线 = `sizeof`，行为与今天一致） |
| **2** | agent/client 表头：加 `MIN_VERSION` / `BASELINE_SIZE` 常量 + `plugin_iface_meta.h` 特化与自检（折回后 29 张表） | 低 |
| **3** | spec/input 统一：新增 `plugin_spec.h`；改 3 处宿主检查、补 2 处；9 个跨边界结构体补 `struct_size`（6 个零成本、3 个 +8）；宿主填充处填 `sizeof`；插件级 `api_version` 闸门检查统一 | 低-中（改 9 个结构体，牵动 client UI 与工具/图注册相关测试） |
| **4** | 结构整理：`hooks_ex` 折回 `hooks`（表头/宿主/SDK/示例/测试/清单数量 agent 21→20） | 中（牵动阶段 5 产物，全量回归） |
| **5** | 自检与测试：布局台账 + 逐成员偏移断言（对本期关心的表）、`specSizeOk`/`specHas` 单测、`boundaries` 四件套完整性规则与追加策略规则 | 低 |
| **6** | 文档与注释清理：`plugins.md` §8/§9、`AGENTS.md`、删历史承诺注释与 `(v3)` 标记 | 极低 |

每步编译 + 跑测试；完成后更新 `resource/history/plugin-version/work.md` 并提交。

### 6.1 术语统一与文档整理（§11 的工作项，建议 4 个提交）

> **顺序**：排在提交 1~6 **之后**（术语改动会碰到同一批文件：表头、SDK、plugins.md；
> 先做完 ABI/机制改动，再统一措辞，避免同一行改两遍）。

| 提交 | 内容 | 风险 |
|---|---|---|
| **7** | cxx_\* 三个库（`cxx_pluginxx` / `cxx_pluginxx_ui` / `cxx_utilxx*`）的文档与注释统一：主体叙述用"主程序 / 插件框架"，去掉把 agentxx 当"本库服务对象"的写法；本库内的 `装载`→`加载`、`承载`/`接缝`/`领域` 同批替换 | 极低（纯注释/文档；不留字符串常量） |
| **8** | agentxx 插件相关源码注释：`agent/lib/include/agentxx/plugin/**`、`agent/lib/src/plugins/**`、`agent/plugins/**`（含 JS 引擎 68 处）、`agent/test/**` 的称呼替换（含 `基座`/`内核` 归并）；本目录内的 `领域`→`业务`、`装载`→`加载`、`承载`/`载体`/`编排`/`接缝`/`链路` 同批替换 | 低（只改注释；`agent_host`/`event_host`/`ffi` 按各自含义单独判定） |
| **9** | agentxx 文档：`docs/zh-cn/design/*.md`（plugins.md / index.md / ffi.md / tui.md / security.md / roadmap.md / configuration.md / develop.md / benchmark.md）、`docs/en/**`、根 `AGENTS.md`；写入 §11.1 的标准三段关系句；直译词同批处理 | 低（纯文档） |
| **10** | 黑话清理（`口径`/`落地`/`埋点`/`门禁`/`颗粒度` 等，逐条看上下文）+ 直译词收尾（全仓扫 `领域`/`装载`/`载入`/`承载`/`载体`/`编排方`/`接缝`/`回灌`）+ `boundaries` 新增"称呼 / 黑话 / 直译词"规则 + 全量回归 | 低-中（新增测试规则要能稳定通过） |

---

## 7. 测试方案

1. **判定规则（机制层）**
   - `version = 0` → 拒绝；`version = MIN_VERSION` + `struct_size = 基线` → 通过；
   - `struct_size = 基线 - 8` → 拒绝（模拟老宿主）；
   - `version = 2` + 完整长度 → **通过**（证明不再写死 `1`）；
   - `version = 2` + 长度覆盖追加成员 → 通过且 `ifaceHas(追加成员) == true`；
   - `version = 0`（另一张表用 `version = 2`）→ 只影响该表（回归 D2）。
2. **成员判定**：伪造一张表对象（`struct_size` 分别取"基线"、"基线+8"、"基线+16"），
   断言 `ifaceHas` 在追加成员上的三态；`ifaceHasVersion` 两态。
3. **退化路径**：表指针 NULL、表 `struct_size = 0`、IID 不匹配 → 全部返回 NULL 且不崩。
4. **结构体长度（spec / input）**：`specSizeOk` 三态（`0` / 基线 / 基线以下拒绝）；
   `specHas` 两态；`plugin_manager_feature` 两处新增检查各有正反例；
   6 个"零成本"结构体断言 `sizeof` 与关键字段偏移未变（证明只改名不加字段）；
   3 个"+8"结构体断言新的 `sizeof`。
5. **插件级闸门**：`api_version` 过低/相等两种 DSO，断言拒载与否 + 日志；
   client 侧同。
6. **四件套完整性**（§3.9）：`boundaries` 逐表断言 IID 宏 / `VERSION` / `MIN_VERSION` /
   `BASELINE_SIZE` 四项齐全、`MIN_VERSION <= VERSION`、`IfaceMeta` 特化数与表数相等；
   表数量断言：agent 侧 20、client 侧 9、通用 10、合计 29。
7. **追加策略自检**：`static_assert`（基线合法）；`boundaries` 读各表头生成"成员顺序台账"，
   与期望顺序比对（防重排）；对本期关心的表（`agentxx.client.ui`、`hooks`、`capabilities`、
   `tools`、`session`、`graph`）逐成员断言 `offsetof`。
8. **回归**：`plugin_hooks`（156 项，表名替换后需全绿）、`plugin_runtime`、`plugin_sdk`、
   `client_plugins`、`plugins`、`boundaries`、`assembly_snapshot`、`plugin_feature`、
   `plugin_cleanup`、`observability`、`tui_ui_items`。
9. **称呼检查**（§11.8）：脚本/`boundaries` 规则扫描源码与文档，断言
   `宿主` / `应用侧` / `基座` 只出现在白名单行（标识符名、目录名、OS 内核用法、
   `resource/history/**`）；断言禁用词表（AGENTS.md 的 22 词 + 本项目补充词）与
   直译词（`装载`/`载入`/`承载`/`载体`/`编排方`/`接缝`/`回灌`，`领域` 限标识符行、
   `拓扑` 限"拓扑排序"）零命中。

---

## 8. 兼容性与迁移

- **本次声明**：重置前的宿主二进制与插件二进制**不再支持**（需一起重编）。
  可诊断，不会静默错调：插件级 `api_version` 闸门 + 表长度基线闸门 + IID 未命中。
- **迁移动作**（对使用方）：用同一版本 SDK 重编插件与宿主；`plugin.yaml` 里
  `agentxx.agent.hooks_ex` 改为 `agentxx.agent.hooks`（折回后该接口名不再存在）；
  插件代码若使用 `hookEx` / `listHooks`（SDK 名称不变，底层表改回 `hooks`）无需改动。
- **回滚**：每个提交独立可回滚。回滚提交 1~2 即回到"整表严格判定"（机制消失但行为等价），
  回滚提交 4 即恢复 `hooks_ex`。

---

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| 有人重排成员（大小可能不变 → 静默错调） | §3.6 第 5 条明令禁止 + §7 成员顺序台账与 `offsetof` 断言 |
| 追加成员时忘记改调用点（只判空不判 `ifaceHas`） | SDK 内部收口 + 文档样例 + 追加时同步加测试；`boundaries` 检查"追加段成员在源码里是否都出现在 `ifaceHas` 列表"（尽力而为的静态检查） |
| 追加成员时误改 `BASELINE_SIZE` | `static_assert(BASELINE_SIZE == offsetof(第一个追加成员))` + 追加历史表 |
| `MIN_VERSION` 被误用于普通变更 | §3.6 第 4 条 + FAQ Q1 + `boundaries` 检查"所有表 MIN_VERSION == 1"（当前阶段） |
| agent 侧新加 `api_version` 闸门拒掉某些测试 DSO | 测试 DSO 用 `PLUGINXX_API_VERSION` 宏填充；先跑回归验证 |
| 9 个跨边界结构体补 `struct_size` 漏改某个调用点 | 编译期即可发现（结构体初始化 / 字段名变化）；`plugin_runtime` 断言 6 个零成本结构体的 `sizeof` 与字段偏移未变；测试覆盖 renderer/overlay/timer/keybind/工具注册/节点注册 |
| 折回 `hooks_ex` 时漏改某处 | 附录 D 的出现点清单 + 全局 grep `hooks_ex` + 全量回归 |

---

## 10. 不做的事（本轮明确排除）

1. 不重排任何表的成员顺序（`capabilities` 的中间插入保持原样）。
2. 不删除任何现有表成员（要删 → 新 IID）。
3. 不给"表内成员"再造第三套机制（不引入 `reserved` 占位字段；占位会让"表可用 = 成员可用"
   的判定变复杂，且本次重置已把基线钉死）。
4. 不改核心 vtable。
5. 不改 `PLUGINXX_API_VERSION` 数值（保持 1）。
6. 不保留任何"为旧二进制开的后门"（不做"老布局识别兼容"）。
7. 不改 IID 字符串与结构体/宏名字（除 `hooks_ex` 折回）。
8. 术语统一只改**文档与注释的措辞**，不改代码标识符（类名 / 目录名 / 函数名 / 参数名），
   也不改 `cxx_utilxx` 里带项目名的字符串常量（§11.4、§11.7）。

---

## 11. 术语统一与文档/注释整理（并列工作项）

### 11.1 三层关系与标准说法（写进每一份首段）

```
agentxx (主程序)
    └── 使用 cxx_* 库提供的「插件框架」
            ├── 直接加载「动态库插件」(.so / .dll / .dylib)
            └── 经「JS 运行时插件」加载「JS 插件」(agentxx_javascript_engine + plugin.js)
```

标准句子（各文档首段可用同一句，避免各自造句）：

> agentxx 是主程序；它使用 cxx_\* 库实现的插件框架动态加载插件。插件有两种：
> 直接加载的动态库插件，以及由 JS 运行时插件（`agentxx_javascript_engine`）加载的 JS 插件。

写作用词规则：

| 概念 | 统一称呼 | 不再使用 |
|---|---|---|
| agentxx 这个程序（agent 侧 / client 侧都算） | **主程序** | 宿主、宿主程序、宿主进程、宿主应用、应用、应用侧 |
| agentxx 的两端 | **主程序的 agent 侧 / client 侧**（也可简写 "agent 侧 / client 侧"） | 宿主侧、server 侧宿主、client 侧宿主 |
| cxx_pluginxx / cxx_pluginxx_ui 提供的插件加载与运行机制 | **插件框架** | 宿主框架、框架层、基座 |
| 插件框架中与主程序业务无关的那部分 | **插件框架（通用部分）** | 内核（单用）、C ABI 基座、SDK 基座 |
| cxx_utilxx / cxx_utilxx_base | **工具库**（或按名 `cxx_utilxx`） | 基础设施、底层库 |
| `.so/.dll/.dylib` 插件 | **动态库插件** | 原生插件、C 插件、"C++ 插件"（见下条） |
| 用 JS 写的插件（+ C++ 壳 + `plugin.js`） | **JS 插件** | 脚本插件、JS 脚本插件（"JS 脚本"只用于指文件本身） |
| 提供 JS 运行时的那一个动态库插件 | **JS 运行时插件**（`agentxx_javascript_engine`） | 引擎插件、JS 引擎插件 |
| 用 FFI 把 libagentxx 嵌入自己进程的程序（Python/Rust/Go…） | **嵌入方程序** | 宿主、宿主程序 |
| 单进程内 agent 节点容器（类名 `AgentHost`） | **agent 节点容器**（写 `AgentHost` 时附中文） | 宿主 |
| 事件总线持有者（类名 `EventHost`） | **事件总线持有者** | 宿主 |
| C ABI 里传给插件函数的主程序句柄（`PluginxxHost`） | **主程序句柄**（保留类型名 `PluginxxHost`） | 宿主句柄 |
| 外部程序注册的工具桥（类 `HostTool`、协议 `WireHostTool*`、消息名 `"host_tool_*"`） | **外部注册工具**（注释里写清是谁注册的：嵌入方程序 / 客户端） | 宿主工具 |
| 操作系统内核 | **内核**（不变，如"内核对象/句柄"） | —— |

关于"C++ 插件"：现在 `plugins.md` 有一句"所有插件都是 C++ 插件；JS 插件表现为标准 C++
动态库外壳"，与"JS 插件"这一称呼不一致。改写为：**插件框架只认动态库入口；
JS 插件由一个动态库外壳（含 `plugin.js`）承载**，即 JS 插件也是一种"动态库外壳 + 脚本"。

### 11.2 替换判定：`宿主` 的 8 类出现形态

同一个词在仓库里有 8 种含义（实测 1388 行 / 179 文件）。**不做全局替换**，
按"看它指谁"逐条判定：

| # | 含义 | 出现形态（实测行数） | 替换为 |
|---|---|---|---|
| 1 | agentxx（加载插件、提供接口表的一方） | `宿主 io 线程` 63 行/27 文件、`宿主侧` 38/26、`宿主句柄` 23/6、`宿主进程` 5、`宿主进程内` 2 | **主程序**（`主程序 io 线程` / `主程序侧` / `主程序句柄` / `主程序进程`） |
| 2 | cxx_pluginxx（插件框架本身） | `框架内核`、`宿主运行时`（README/index.md 架构图） | **插件框架** / 插件框架的运行时 |
| 3 | 嵌入式会话运行时的外层程序（FFI） | `ffi.md`（58 行）、`ffi_api.h`（15） | **嵌入方程序** |
| 4 | agent 节点容器 `AgentHost` | `agent_host.h`（15）、`test_agent_host.cpp`（7） | **agent 节点容器**（保留类名） |
| 5 | 事件总线持有者 `EventHost` | `event_host.h`（9） | **事件总线持有者** |
| 6 | `PluginxxHost` 句柄类型 | 各表头 `const PluginxxHost* host`、`PluginxxHostVtable`（标识符，不改名） | 注释写 **主程序句柄 / 主程序 vtable** |
| 7 | 其它带 `host` 的标识符 | `PluginHostCore` / `PluginHostLifecycle`（插件框架）、`onHostReady`（SDK 挂钩）、`HostTool` / `WireHostTool*` / 消息名 `"host_tool_*"`（外部注册工具） | 不改名；注释按 §11.1 各行的中文称呼写 |
| 8 | 操作系统内核 | `writer_lease.h` 的"内核对象/句柄" | **保持"内核"**（不是本工作项对象） |

**顺手替换**：`应用侧`（4 行）→ **主程序**（`feature.h` 的"应用侧取值" = 主程序取值）；
`基座`（10 行）→ 按 §11.1 表归并（`C ABI 基座` → 插件框架的 C ABI 部分）。

### 11.3 边界与排除

- **排除目录**：`resource/history/**`（用户要求不动，历史记录保持原样）。
- **排除范围**：`agent/third_party/` 内非自研库（boost / curl / FTXUI / quickjs /
  hyperscan / yaml-cpp / fmt / sqlite3 / zlib / uchardet / simdjson / cmark-gfm /
  html2md / glob / iconv / liburing / neograph / markdown_ftxui / codegraph-cpp /
  mimalloc / OpenSSL）——属上游代码。
- **英文文档**（`docs/en/**`）：`宿主` → `main program`、`插件框架` → `plugin framework`、
  `动态库插件` → `dynamic library plugin`、`JS 插件` → `JS plugin`；
  `host` 只保留在标识符名里。
- **agentxx 文档可显式说明**："本仓库里 **主程序 即 agentxx** 本身"（只允许在 agentxx 的
  文档/注释里这样写）。
- **cxx_\* 库不得关联 agentxx**：库的文档/注释里，主体叙述一律用"主程序"这一通用词；
  确实需要举例时（如入口符号名前缀、接口表命名空间），写法改为中性或明确标"例如"，
  不写成"本库服务于 agentxx"。**实测 cxx_\* 库里的 agentxx 提及点**：

| 文件 | 处数 | 处置 |
|---|---|---|
| `cxx_pluginxx/README.md` | 8 | 表格与说明里的项目名改为"示例项目"或去掉；"agentxx 用 …"改为"(例如 `<项目>` 用 …)" |
| `cxx_pluginxx/include/pluginxx/api/abi.h` | 3 | 入口符号名例子改为中性（`<项目>_plugin_agent_*`），或保留两个示例并标"例如" |
| `cxx_pluginxx/include/pluginxx/api/entry.h` | 3 | 同上 |
| `cxx_pluginxx/include/pluginxx/kit/kit.h` | 10 | 说明"主程序业务部分在主程序仓库"即可，去掉指向 agentxx 路径的链接（或标为示例） |
| `cxx_pluginxx_ui/README.md` + `docs/integration.md` | 7 | 接入示例段落标注"示例：某主程序的接入" |
| `cxx_pluginxx` 其余头/源（`host/*.h`、`kit/guard.h`、`runtime/*`、`src/manifest.cpp`） | 约 21 | 逐条判定：属"示例写法"或"指向 agentxx 文件链接"的，改写为中性说法 |
| `cxx_utilxx/include/utilxx/worktree.h` | 10 | **不改**：10 处都是路径/分支名常量（`.agentxx/agent/worktrees` 等），属行为 |
| `cxx_utilxx/{crypto.h,crypto.cpp,http_server.h,http_server.cpp}` | 4 | `crypto.h` 的历史说明改写；`http_server.cpp` 注释 `宿主` → `主程序`；字符串常量见 §11.7 |

### 11.4 两档执行范围（本轮只做第一档）

| 档 | 内容 | 本轮 |
|---|---|---|
| **第一档（做）** | 文档与注释的措辞：`宿主`/`应用侧`/`基座`/`内核（单用）` → 统一称呼（§11.1~§11.3）；产品黑话清理（§11.5）；**直译词替换**（§11.6）；标准关系句 | ✔ |
| **第二档（不做，单列）** | 代码标识符重命名：目录 `pluginxx/host/`（25 个文件 include 它）、类型 `PluginxxHost` / `PluginxxHostVtable` / `PluginHostCore` / `PluginHostLifecycle` / `AgentHost` / `EventHost` / `HostTool` / `DomainHooks`、参数名 `host` | ✘（改动量大且牵动 ABI 名字与全部插件示例，收益是"名字更直白"，需单独评估） |

第二档若将来要做，建议只做"注释里附中文名"的轻量做法（本轮已包含），
不做目录/类型改名。

### 11.5 产品黑话清理（实测清单）

**结论：AGENTS.md 的 22 词禁用表基本已清干净** —— 逐词实测每个词只有 1~2 行命中，
且绝大多数命中就是"禁用词表本身"（`agent/lib/src/agent/prompt.cpp` 与 `AGENTS.md` 里
列出这些词的那两句），**不要去改那两句**。

**真正需要改的（本项目自有的产品味用词）**：

| 词 | 实测行数/文件 | 处置 |
|---|---|---|
| `口径` | 9 / 6 | 改"规则 / 定义 / 同一套取值方式"（如"同一口径" → "同一套规则"） |
| `落地` | 3 / 3 | 改"实现"（如"接口表的落地" → "接口表的实现"） |
| `埋点` | 1 / 1 | 看上下文（`feature.h` 的"埋点天生接受外部实现" → "扩展点天生接受外部实现"） |
| `门禁` | 2 / 2 | 改"校验 / 检查"（`benchmark.md` 的"协议门禁" → "协议校验"；另一处是禁用词表本身，不动） |
| `颗粒度` | 1 / 1 | 改"划分精细程度 / 粒度" |
| `通路` | 6 / 3 | 逐条看：指调用路径时改"调用路径 / 流程" |
| `收敛` | 123 / 68 | **逐条看**：指"合并到一处"时改"合并 / 集中到一处"；数学/循环收敛含义保留 |
| `对齐` | 126 / 68 | **不机械替换**：TUI 排版"对齐"、格式对齐是正常技术词；只有"和某某对齐一下"这类产品味用法才改 |
| `兜底` | 286 / 118 | **保留**（开发日常用词，语义明确：默认/保底处理）；仅当出现"兜底逻辑"这类含糊表述时改为具体动作 |

判定原则（写进 `plugins.md` 或 `develop.md` 的"注释写法"小节）：
**只改"看上下文才知道指什么"的词；含义明确的技术词不动。**

### 11.6 直译词替换（`领域` / `装载` 一类）

**判定标准**：词是英文词的字面翻译，中文读者心里要先"回译"成英文才知道指什么；
换成开发日常说法更直接。**逐条看上下文**，不机械全替换（下表已给出边界）。

| 词 | 直译来源 | 实测 | 处置 |
|---|---|---|---|
| **领域** | domain | **106 行 / 29 文件**（其中 `领域表` 30、`领域注册` 11、`领域 helper` 10、`领域部分` 10、`领域钩子` 6、`领域数据` 4、`领域动作` 3、`领域事件` 2、`领域扩展` 1） | **业务**（与 cxx_pluginxx README 已在用的"业务表"一致）：`领域表` → `业务表`；`领域 helper` → `业务 helper`；`领域注册` → 业务注册（或写清注册了哪些）；`领域部分` → 业务部分；`领域钩子` → **业务数据入口**（指 `DomainHooks`）；`领域数据` → 业务数据；`领域动作` → 写清动作（摘除注册 / 注入）；`领域事件` → 写具体事件（如"会话轮次开始"）；`与宿主领域无关` → `与主程序业务无关`（与现有 README 用词一致） |
| **装载** | load / mount | **114 行 / 36 文件** | **加载**（同义且已是主流：`加载` 658 行）：`插件装载顺序` → `插件加载顺序`；`动态库装载` → `动态库加载`；`装载失败` → `加载失败`；`装载耗时` → `加载耗时`；`装载参数` → `加载参数` |
| **载入** | load | 7 行 / 3 文件 | **加载**（统一到同一个词） |
| **承载** | carry | 33 行 / 21 文件 | **保存 / 包含 / 提供 / 负责**（按句意）：`图状态不再承载消息内容` → `不再保存`；`一个文件承载三件事` → `一个文件包含三件事`；`仅承载名称/描述等静态元数据` → `只保存名称/描述` |
| **载体** | carrier | 23 行 / 8 文件 | **数据 / 内容 / 块**：`加密思考载体` → `加密思考内容`（指匿名 thinking 块）；`唯一的有效载体` → `唯一的有效内容` |
| **编排** | orchestrate | 11 行 / 4 文件（`编排方` 7） | **调度 / 组织**：`编排方` → `调度方`（或直接写"压缩中间件"）；`这些是编排方的事` → `这些由调度方负责` |
| **接缝** | seam | 26 行 / 14 文件 | **扩展点 / 对接点**：`宿主接缝` → `主程序要覆写的扩展点`；`覆写本接缝` → `覆写本扩展点`；`内核只定义…的接缝` → `…的对接点` |
| **拓扑** | topology | 19 行 / 14 文件 | `拓扑排序` **保留**（算法名，勿改）；其余 → **结构 / 连接方式**：`线程拓扑` → `线程结构`；`agent_io.h` 的"拓扑" → "连接方式" |
| **链路** | chain / link | 18 行 / 5 文件 | **流程 / 调用路径**：`走完整链路` → `走完整流程`；`关闭链路是否走完` → `关闭流程是否走完`；`插件链路跟随` → `插件调用路径跟随`（测试里作路径字符串的"真实链路"不改） |
| **视角** | perspective | 9 行 / 5 文件 | **角度**：`从 X 的视角` → `从 X 的角度` |
| **维度** | dimension | 2 行 / 1 文件 | **方面** |
| **层面** | layer | 2 行 / 2 文件 | **方面** |
| **回灌** | pour back | 2 行 / 2 文件 | **写回 / 重新写入** |
| **骨架** | skeleton | 52 行 / 20 文件 | 建议 **公共实现 / 统一实现**（`生命周期骨架` → `生命周期公共实现`）；也可保留（开发日常用词）—— 实施时按"是否引起歧义"逐条判断，**不强制** |
| **落盘 / 落库** | write to disk / DB | 164 / 61 行 | **低优先、可选**：`落盘` → `写入磁盘` / `持久化`；`落库` → `写入数据库`。量较大，改到同一句时顺手替换即可，不为此单独扫全仓 |

**明确保留（标准技术词，不要动）**：`粒度`（14 行，计算机领域标准词）、`幂等`、
`全量 / 增量`、`抽象`、`映射`、`语义`、`契约`、`解耦`、`透传`、`锚点`（TUI 滚动锚点）、
`归属`（134 行，"属于谁"含义明确）、`兜底`（286 行，开发日常词）、
`对齐`（125 行，多为排版含义）、`收敛`（数学含义；"合并到一处"的用法按 §11.5 处理）。

**与标识符的边界**（不改名，只在注释里换措辞）：`DomainHooks`、
`detachDomainRegistrations` / `clearDomainRegistrations`、`plugin_manager_domain_hooks.{h,cpp}`、
`applyDeclaredResources` 里的 domain 字样 —— 属第二档（§11.4）。

**与 §11.1 的联动**：`领域` 换 `业务` 之后，`与宿主领域无关` → `与主程序业务无关`，
正好与 cxx_pluginxx 现有的"与主程序业务无关的插件框架"用词统一 —— 两处改动互相印证。

### 11.7 顺带发现的问题（本轮不改，记录待决）

`cxx_utilxx` 是"多项目通用库"，但里面有几处 agentxx 专有的**字符串常量与行为**：

| 位置 | 内容 | 为什么单列 |
|---|---|---|
| `include/utilxx/http_server.h:432` | `kServerBanner = "agentxx/" + kVersion` | 通用库的 HTTP 头里写死项目名；应改成由主程序传入的 banner 参数（接口变更，影响使用方） |
| `src/crypto.cpp:101` | `return "agentxx_default_device"` | 设备标识默认值写死项目名；改动可能影响已存的设备 id / 指纹一致性 |
| `include/utilxx/worktree.h`（10 处） | `.agentxx/agent/worktrees`、`agentxx/wt-`、`# agentxx worktrees` | 是磁盘上真实存在的路径与分支前缀；改名 = 迁移动作 |

处置建议：**单独立项**（接口/行为变更 + 迁移方案），不与本轮文档统一混在一起。

### 11.8 校验方式（防止回潮）

新增 `boundaries` 规则（或在 `agent/script/` 下加一个检查脚本，二者取一，倾向 `boundaries`，
因为它已在跑且能读文件树）：

1. **称呼检查**：扫描 `agent/lib`、`agent/client`、`agent/plugins`、`agent/test`、
   `agent/third_party/cxx_*`、`docs`（排除 `resource/history/**` 与上游第三方库），
   出现 `宿主` / `应用侧` / `基座` 时必须在**白名单**里，否则报错。白名单（实测确认存在的标识符）：
   - C ABI：`PluginxxHost`、`PluginxxHostVtable`、参数名 `host`；
   - 插件框架：`PluginHostCore`、`PluginHostLifecycle`、`pluginxx/host/`（含
     `#include "pluginxx/host/..."` 行）、`host_core.h`、`onHostReady`；
   - agentxx 内部：`AgentHost`、`EventHost`、`HostTool`、`WireHostTool*`、
     协议消息名 `"host_tool_*"`；
   - OS 内核用法（`writer_lease.h` 的"内核对象/句柄"行）；
   - `resource/history/**`（整目录跳过）。
2. **黑话检查**：AGENTS.md 的 22 词禁用表 + 本项目补充词（`口径`/`落地`/`埋点`/`门禁`/
   `颗粒度`/`组合拳`…），零命中（禁用词表自身那两句加行级豁免）。
3. **直译词检查**（§11.6）：`装载` / `载入` / `承载` / `载体` / `编排方` / `接缝` /
   `回灌` 目标为零；`领域` 仅允许出现在标识符与其所在行（`DomainHooks`、
   `plugin_manager_domain_hooks`、`detachDomainRegistrations` …）；
   `拓扑` 仅允许出现在 `拓扑排序`；`骨架` / `落盘` / `落库` 不检查（§11.6 列为可选）。
4. **关系句检查**（轻量）：`docs/zh-cn/design/plugins.md`、`AGENTS.md` 里存在
   §11.1 的标准关系句（防止将来文档改散）。

---

## 附录 A：重置后的表头写法（以 `agentxx.client.ui` 为例）

```c
#define AGENTXX_IFACE_CLIENT_UI             "agentxx.client.ui"
#define AGENTXX_IFACE_CLIENT_UI_VERSION     1   /* 追加成员 / 语义变化 → +1 */
#define AGENTXX_IFACE_CLIENT_UI_MIN_VERSION 1   /* 只在破坏性改动时 +1 (优先换 IID) */

/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为 offsetof(第一个追加成员)) */
#define AGENTXX_IFACE_CLIENT_UI_BASELINE_SIZE sizeof(AgentxxClientUiIface)

/*
 * 版本与布局规则 (全部接口表通用):
 * - `version` / `struct_size` 由宿主发布 (宿主自己编译期看到的值)
 * - 判定: IID 命中 + version >= MIN_VERSION + struct_size >= BASELINE_SIZE
 * - 成员只能追加在表尾; 追加段成员必须用 ifaceHas 判定 (判空不够)
 * - 重排 / 改签名 / 删成员 → 换新 IID (struct_size 与 version 都拦不住)
 * - 表内某成员为 NULL = 宿主不支持该子能力 (精简宿主), 与版本无关
 * - 追加历史: (无, 首版)
 */
typedef struct AgentxxClientUiIface {
    int32_t  version;     /* == AGENTXX_IFACE_CLIENT_UI_VERSION */
    uint32_t struct_size; /* == sizeof(AgentxxClientUiIface) */
    /* ... 成员（顺序即契约） ... */
} AgentxxClientUiIface;
```

## 附录 B：布局台账与偏移断言

```bash
# 1) 生成当前布局台账 (表名 / 成员顺序), 供评审与文档使用
python3 - <<'PY'
import re, pathlib
for p in pathlib.Path("agent").rglob("*.h"):
    if "plugin/api" not in str(p) and "pluginxx/api" not in str(p):
        continue
    txt = p.read_text(encoding="utf-8", errors="ignore")
    for m in re.finditer(r"typedef struct (\w+Iface) \{(.*?)\} \1;", txt, re.S):
        name, body = m.group(1), m.group(2)
        print("==", name)
        for fm in re.finditer(r"\(\s*PLUGINXX_CALL\s*\*\s*(\w+)\s*\)", body):
            print("   ", fm.group(1))
PY

# 2) 成员顺序台账进测试: boundaries 比对"期望顺序" (防重排/防删成员)
# 3) 偏移断言 (C++ 侧) 写在 test_plugin_runtime.cpp 的 ABI 表里, 例:
#    {11, offsetof(AgentxxPluginToolsIface, struct_size)},
#    {14, sizeof(AgentxxClientUiIface)},
#    本期关心的表: 逐成员加 offsetof 断言 (追加成员时只改"大小"那一条)
```

## 附录 C：与其它体系对照

| 体系 | 表/接口演进方式 | 与本方案的关系 |
|---|---|---|
| COM | 接口不可变；加能力 = 新接口（新 IID） | 本方案的"首选路"与之一致 |
| Vulkan / OpenGL | 结构体带 `sType`/`pNext` 链或版本号，成员按版本可用 | 与本方案的"版本对 + 成员 gate"同类（本项目用 `struct_size` 代替 `pNext`） |
| D3D12 `CheckFeatureSupport` | 入参结构体带 size，宿主按 size 决定填多少 | 对应本方案 §3.5 的 spec 规则 |
| GLib / GObject | 结构体尾部 `_reserved` 占位，需要时启用 | 本方案**不预留**占位（改用"追加 + `ifaceHas`"，避免占位带来的判定复杂度） |
| Windows `WINDOWS_VERSION` 式全局版本 | 单一全局版本号 | 本项目已用 `PLUGINXX_API_VERSION` 处理插件级；表级用每表版本对 |

## 附录 D：`hooks_ex` 折回改动点清单（已定动作; 出现点为实测 grep 结果）

| 文件 | 出现 | 改动 |
|---|---|---|
| `agent/lib/include/agentxx/plugin/api/plugin_api.h` | 6 | 删表定义与 `AGENTXX_IFACE_AGENT_HOOKS_EX*` 宏；三个成员移入 `AgentxxPluginHooksIface` 表尾；`AgentxxPluginHookSpecEx` 结构体**保留**（入参结构体，与表无关） |
| `agent/lib/include/agentxx/plugin/api/plugin_kit.h` | 5 | `queryInterface<AgentxxPluginHooksExIface>` → `AgentxxPluginHooksIface`；删"表缺失回退基础钩子"的双路分支；注释跟进 |
| `agent/lib/include/agentxx/plugin/plugin_interfaces.h` | 2 | 删 `AgentHooksEx` 常量与其注释 |
| `agent/lib/include/agentxx/plugin/plugin_manager.h` | 3 | `kInterfaceTableCount` 10+11 → 10+10；表清单注释去 `hooks_ex`；钩子段注释表名 |
| `agent/lib/src/plugins/plugin_manager_hooks.cpp` | 1 | 文件头注释表名（实现体不变） |
| `agent/lib/src/plugins/plugin_manager_vtable.cpp` | 3 | 段注释、三入口移入 `hooks` 段、`g_ifaceHooksEx` 删除并填入 `g_ifaceHooks`、`query_interface` 去分支 |
| `agent/plugins/example_feature/{example_feature.cpp,plugin.yaml}` | 3+1 | 接口声明与用法注释改为 `hooks` |
| `agent/test/include/agentxx-test/plugin/test_plugin_hooks.h` + `agent/test/plugin/test_plugin_hooks.cpp` | 1+2 | 表名/接口名替换（断言主体复用） |
| `docs/zh-cn/design/plugins.md` | 1 | §8 表格 `hooks_ex` 行并入 `hooks` 行 |
| `AGENTS.md` | 2 | "接口表数量" agent 21 → 20（合计 30 → 29）；同步 `boundaries` 规则 8/10 里的 "10 + 11 = 21" |

> 折回后 `hooks` 表的四件套（§3.9）一次性配齐：IID `agentxx.agent.hooks`、
> `AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_VERSION = 1`、
> `AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_MIN_VERSION = 1`、
> `AGENTXX_PLUGIN_IFACE_AGENT_HOOKS_BASELINE_SIZE = sizeof(AgentxxPluginHooksIface)`
> （含表尾三成员）—— 不保留"老布局"这一档，所以基线直接是完整表长。

## 附录 E：以后怎么改（操作手册）

**加一个可选能力（首选）**
1. 优先新增**接口表**（新 IID）或**能力名字符串**（`plugin.yaml` 的 `optional`）；
2. 两者都不合适（必须往已有表里加）→ 只能加在**表尾**：
   - `X_VERSION` +1，`X_BASELINE_SIZE` 改为 `offsetof(XIface, 新成员)`（或其他等值表达式），
     `X_MIN_VERSION` 不动；
   - 头文件"追加历史"表补一行；
   - 所有使用点写成 `ifaceHas<&XIface::新成员>(t) && t->新成员`；
   - 需要"宿主提供了更高语义档位"时用 `ifaceHasVersion(t, 2)`；
   - 跑 §7 的判定用例。

**改某个成员的语义（布局不变）**
1. `X_VERSION` +1；头文件写清"v2 起 xxx 的含义/返回值有什么变化"；
2. 调用点需要新版语义时 `ifaceHasVersion(t, 2)`，否则按 v1 语义兼容处理；
3. `X_MIN_VERSION` 不动；不换 IID（除非两套语义无法共存）。

**加入参/输入结构体的可选字段**
> 规则统一（§3.5）：**填充方填 `struct_size`，读取方按它 gate** —— 两个方向同一条规则。
1. 字段加在**结构体尾部**；`BASELINE_SIZE` 保持不变（= 第一个追加字段的 `offsetof`）；
2. 读取方读该字段前 `specHas<&Spec::字段>(对方给的 struct_size)`；
3. 填充方填新字段时把 `struct_size` 填成 `sizeof(Spec)`；不填的（老代码）保持 `0` 或旧长度；
4. spec 方向（插件填 → 宿主读）与 input 方向（宿主填 → 插件读）都照此办理。

**真的要破坏兼容**
1. 首选**换 IID**（新表名，插件按"表缺失"降级）；
2. 必须同 IID 硬切 → `X_MIN_VERSION` +1，并在 `CHANGELOG` 与
   `docs/zh-cn/design/plugins.md` 里点名"这一版开始不再支持 xx 之前的宿主/插件"。

**永远不要做**
- 重排成员、改成员签名、删除成员（结构体大小可能不变 → 闸门失效 → 静默错调）；
- 追加成员却忘了把 `X_BASELINE_SIZE` 留在旧值（老宿主会被误判整表不可用）或忘了加 `ifaceHas`（越界读）；
- 用 `MIN_VERSION` 表达"我加了新成员"（那等于把所有老宿主判死）。

## 附录 F：术语速查表（实施时照着替换）

| 中文（统一） | 英文（docs/en） | 说明与边界 |
|---|---|---|
| 主程序 | `main program` | agentxx 的文档里可注明"主程序 = agentxx"；cxx_\* 库的文档里**只**用通用含义，不关联 agentxx |
| 插件框架 | `plugin framework` | = `cxx_pluginxx`（+ 描述层库 `cxx_pluginxx_ui`）；不再单用"内核/基座"指它 |
| 动态库插件 | `dynamic library plugin` | `.so/.dll/.dylib`；不用"原生插件/C 插件" |
| JS 插件 | `JS plugin` | 由"动态库外壳 + `plugin.js`"构成；不用"脚本插件" |
| JS 运行时插件 | `JS runtime plugin` | = `agentxx_javascript_engine`；不用"引擎插件" |
| 嵌入方程序 | `embedding program` | FFI 场景（Python/Rust/Go… 把 libagentxx 嵌进自己进程）；不用"宿主" |
| 主程序句柄 | `main program handle` | 类型名仍是 `PluginxxHost`（不改名），注释用中文称呼 |
| agent 节点容器 | `agent node container` | 类名仍是 `AgentHost` |
| 事件总线持有者 | `event bus holder` | 类名仍是 `EventHost` |
| 内核 | `kernel` | 只用于操作系统内核含义（如"内核对象/句柄"） |

**写作规则三条**（供实施与将来 review 用）：

1. 一句话里出现"它/他"时，先确认前一句的称呼是"主程序"还是"插件框架"——
   这两个词是本次统一的核心，不允许再用"宿主"顶替任何一个。
2. 说"哪一端的代码"时用"主程序的 agent 侧 / client 侧"，不用"宿主侧"。
3. 代码标识符（`PluginxxHost`、`PluginxxHostVtable`、`PluginHostCore`、`PluginHostLifecycle`、
   `onHostReady`、`AgentHost`、`EventHost`、`HostTool`、`WireHostTool*`、`host_tool_*` 消息名、
   `pluginxx/host/` 目录）**不改名**；注释里首次出现时补一句中文解释即可。

## 附录 G：术语扫描脚本（实施与复查用）

本文 §11 的行数统计由下面这段脚本得出（PowerShell，Windows 可用；Linux 用等价的 grep）：

```powershell
$base = '<repo>'           # 例如 D:\0Acoolight\Program\cpp\agentxx
$roots = @('agent\lib','agent\client','agent\plugins','agent\test',
           'agent\third_party\cxx_pluginxx','agent\third_party\cxx_pluginxx_ui',
           'agent\third_party\cxx_utilxx','agent\third_party\cxx_utilxx_base','docs')
$files = @()
foreach ($r in $roots) {
    $p = Join-Path $base $r
    if (Test-Path $p) {
        $files += Get-ChildItem -Path $p -Recurse -File -Include *.h,*.hpp,*.cpp,*.md,*.yaml,*.json |
            Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\out\\' }
    }
}
$files += Get-Item (Join-Path $base 'AGENTS.md')

function CountTerm($term) {
    $hits = $files | Select-String -SimpleMatch $term
    "$term : lines=$(($hits | Measure-Object).Count) files=$(($hits |
        Select-Object -ExpandProperty Path -Unique | Measure-Object).Count)"
}

# 称呼: 目标为零（除白名单）
'宿主','应用侧','基座','内核','宿主侧','宿主进程' | ForEach-Object { CountTerm $_ }
# 黑话: 目标为零（禁用词表自身两处加行级豁免）
'口径','落地','埋点','门禁','颗粒度','收敛','兜底','对齐' | ForEach-Object { CountTerm $_ }
# 直译词: 目标为零（领域 仅标识符行, 拓扑 仅"拓扑排序", 骨架/落盘/落库 可选）
'领域','装载','载入','承载','载体','编排方','接缝','回灌','拓扑','链路','视角','维度','层面' | ForEach-Object { CountTerm $_ }
```

复查标准（实施完成时的验收）：

- `宿主` / `应用侧` / `基座`：命中数 == 白名单行数（§11.8 列出的标识符与 `resource/history/**`）；
- `内核`：命中仅剩 OS 内核用法与"插件框架（通用部分）"的新写法；
- 黑话：上表所列词（除 `兜底`/`对齐` 判定后保留的用法）为零；
- 直译词：`装载` / `载入` / `承载` / `载体` / `编排方` / `接缝` / `回灌` / `链路` /
  `视角` / `维度` / `层面` 为零；`领域` 仅剩标识符行；`拓扑` 仅剩 `拓扑排序`；
  `骨架` / `落盘` / `落库` 不检查（§11.6 列为可选）。

**实施顺序提示**：`领域`（106 行）与 `装载`（114 行）分布最广，建议在提交 7~9 里
按目录归属顺带完成，最后在提交 10 里扫一遍残余 + 加规则。

**实施进度记录写同目录 `work.md`**（与 §6 的 10 个提交对应）。

## 附录 H：直译词对照速查（实施时照着替换）

| 现状用词 | 直译来源 | 换成 | 一句话说明为什么 |
|---|---|---|---|
| 领域（表/注册/部分/钩子/数据/事件） | domain | **业务** | "业务表""业务数据"是中文开发日常说法；"领域"要先想 domain 才懂，且和 DDD 的"领域"概念混用 |
| 装载 | load / mount | **加载** | 仓库里 `加载` 已经是 658 行、`装载` 114 行；"加载插件"人人都懂，"装载"更像硬件/挂载语境 |
| 载入 | load | **加载** | 同上，统一到一个词 |
| 承载 | carry | **保存 / 包含 / 提供 / 负责** | 按句意写动作，读者不用猜"承载"具体是什么行为 |
| 载体 | carrier | **数据 / 内容 / 块** | 说清是什么东西，而不是说它是"什么的东西的载体" |
| 编排（方） | orchestrate | **调度（方）** | "调度"是中文里原本就有的词，"编排"是 orchestrate 的直译 |
| 接缝 | seam | **扩展点 / 对接点** | "扩展点"直接说清"这里可以接自己的实现" |
| 拓扑（非算法处） | topology | **结构 / 连接方式** | `拓扑排序` 是算法名要保留；"线程拓扑""连接拓扑"改成"结构"更直白 |
| 链路 | chain / link | **流程 / 调用路径** | 说"走完整流程"比"走完整链路"更常见 |
| 视角 | perspective | **角度** | "从 X 的角度"是中文原有说法 |
| 维度 / 层面 | dimension / layer | **方面** | 两个词在多处混用，统一成"方面" |
| 回灌 | pour back | **写回 / 重新写入** | …… |
| 骨架（可选） | skeleton | **公共实现 / 统一实现** | 可选：不引起歧义时可保留 |
| 落盘 / 落库（可选，量大） | write to disk / DB | **写入磁盘 / 持久化、写入数据库** | 可选：改到同一句时顺手替换 |

**保留不动**（标准技术词）：`粒度`、`幂等`、`全量`/`增量`、`抽象`、`映射`、`语义`、
`契约`、`解耦`、`透传`、`锚点`、`归属`、`兜底`、`对齐`（排版含义）、`收敛`（数学含义）。
