# 插件系统开发指南

- 内置插件示例参考 [plugins](/agent/plugins/)
- 详细设计与内部实现架构参考 [plugins.md](/docs/agent/plugins.md)

---

## 核心架构与兼容性准则

Agentxx 插件系统采用 **纯 C ABI + COM 风格接口表查询**（API v1 架构）：
- **跨编译器与标准库兼容**：主程序与插件动态库允许由不同的编译器、不同的 C++ 标准库版本（`libstdc++` / `libc++` / MSVC STL）或不同语言独立编译，运行时均能稳定兼容。
- **纯 C 边界**：宿主与插件动态库跨边界仅传递纯 C 基本类型、函数指针、不透明句柄和 `AgentxxPluginStringView`（只读借用），严禁直接跨边界传递 C++ 标准库对象（如 `std::string`、`std::vector`、`std::function`）或 C++ 异常。
- **内存所有权与生命周期**：所有跨边界返回的堆内存统一通过宿主核心 vtable 的 `host->alloc` 分配，接收方用完后负责通过 `host->vtable->free` 释放。
- **无锁单线程会话契约**：宿主会话可变状态仅在主 IO 线程访问；插件注册/注销及状态访问均串行于该线程。
- **锚定协程并发模型 (SDK)**：通过 `plugin_kit.h` 提供的 `Task<T>`，插件协程物理执行于宿主 IO 线程，挂起让出、完成通过 IO 线程回调唤醒，与内置工具原生交错，零轮询、零私有事件循环开销。

---

## 多实例三铁律

同一插件动态库在单进程内可能被多个独立的 Agent 宿主分别加载并创建多个并存实例：
1. **禁止可变全局/函数级 static 变量**：插件的所有可变状态必须封装在随实例创建的上下文堆对象中（`*plugin_ctx`，通常继承 `agentxx::kit::PluginBase`）。
2. **状态经上下文闭包恢复**：所有工具、钩子、事件回调必须通过 `spec.user_data` 恢复当前实例上下文。
3. **接口表缓存存入实例上下文**：接口表查询结果（如 `AgentIfaces`）保存在实例上下文成员中，各实例互不干扰。

---

## 导出符号控制

插件动态库默认隐藏全部符号，仅导出宿主按名查找的入口符号。插件源码定义入口函数时必须使用 `AGENTXX_PLUGIN_EXPORT` 宏标记（定义于 `plugin_api.h`，位于 `extern "C"` 内）：

```c
#include "agentxx/plugin/plugin_api.h"

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void);
extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx);
extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx);
```

- **入口符号集**：
  - Agent 侧插件：`agentxx_plugin_get_info` / `agentxx_plugin_create` / `agentxx_plugin_destroy`
  - Client 侧插件（双端或纯 UI 插件）：`agentxx_client_get_info` / `agentxx_client_create` / `agentxx_client_destroy`
- **构建侧自动化配置**：`plugins/CMakeLists.txt` 已统一配置 ELF `-fvisibility=hidden` + version script 白名单（macOS `-exported_symbols_list`，MSVC `dllexport`），第三方静态库符号与内部 C++ 符号会被自动隐藏。

---

## 工具函数复用 (`agentxx_util`)

面向项目内置插件，可通过独立静态库 `agentxx_util` 复用主程序的全部基础工具函数（字符串/字符编码检测/UTF-8转换/路径规范化/Base64/HTTP客户端/SQLite/正则等）。libagentxx 与各插件各自静态链接一份副本，符号经导出控制严格隐藏，互不冲突：

```cmake
# 插件 CMakeLists.txt
find_package(agentxx_util REQUIRED)
target_link_libraries(${PLUGIN_NAME} PRIVATE agentxx_util)
```

使用示例：
```cpp
#include "agentxx/util/string_util.h"
auto b64 = agentxx::util::base64Encode(data);
```

> **注意**：`agentxx_util` 仅供内置插件便捷开发；第三方插件直接包含纯 C 头文件 `plugin_api.h`（或 SDK `plugin_kit.h`）即可开发，无需链接任何宿主库。

---

## C++ 插件开发方式 (使用 SDK `plugin_kit.h`)

推荐使用官方 C++ 开发套件 `plugin_kit.h`（header-only，仅依赖 `plugin_api.h`）：

```cpp
#include "agentxx/plugin/plugin_kit.h"

struct MyPluginCtx : public agentxx::kit::PluginBase {
    // 实例级状态存放于此
};

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    auto ctx = std::make_unique<MyPluginCtx>();
    ctx->init(host);

    // 1. 注册 Task 锚定协程工具 (支持精确 sleep / yield / call_tool 等)
    agentxx::kit::tool(
        *ctx,
        "my_async_tool",
        "Tool description",
        R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<std::string> {
            co_await agentxx::kit::sleep(c, 100);
            ctl.throw_if_cancelled();
            co_return R"({"status":"ok"})";
        }
    );

    // 2. 注册快同步内联工具 (<~1ms，直接在 IO 线程计算并返回)
    agentxx::kit::fast_tool(
        *ctx,
        "my_fast_tool",
        "Fast tool description",
        R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json, std::string_view tid) -> std::string {
            return R"({"result": 42})";
        }
    );

    // 3. 注册阻塞工具 (自动卸载到宿主阻塞线程池执行，不占 IO 线程)
    agentxx::kit::blocking_tool(
        *ctx,
        "my_blocking_tool",
        "Blocking tool description",
        R"({"type":"object","properties":{}})",
        [](MyPluginCtx& c, std::string_view args_json) -> std::string {
            // 在工作线程池中执行重型计算或同步文件/网络操作
            return R"({"done": true})";
        }
    );

    // 4. 注册后台协作任务 (替代周期定时器)
    agentxx::kit::spawn(*ctx, [](MyPluginCtx& c, agentxx::kit::OpCtl ctl) -> agentxx::kit::Task<void> {
        while (!ctl.cancelled()) {
            co_await agentxx::kit::sleep(c, 5000);
            if (ctl.cancelled()) break;
            // 定期执行采集并发布事件
        }
    });

    *plugin_ctx = ctx.release();
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    delete static_cast<MyPluginCtx*>(plugin_ctx);
}
```

---

## 插件分类与编译模式

1. **按功能划分**：
   - **Agent 插件**：扩展 Agent 会话执行流（工具 Tool / 中间件钩子 Hook / 事件 Event / 能力 Capability / 资源声明 Resources）。
   - **Client 插件**：扩展 TUI / CLI 等客户端界面（状态栏 StatusItem / 弹窗面板 Panel / 侧边栏 InfoSection / 命令 Command）。
   - **双端插件**：同时导出 Agent 侧与 Client 侧入口，一份动态库同时服务两端。
2. **编译与分发模式**：
   - **独立动态库模式（默认）**：插件编译为独立动态库，通过 `agentxx-config.yaml` 的 `plugins` 字段按目录路径动态加载。
   - **内置合并编译模式**：开启 `AGENTXX_ENABLE_PLUGIN_BUILTIN=ON` 时，指定插件源码直接编译并合并进 `libagentxx`，运行期无需外部动态库即可零开销直接调用。

---

## Javascript 插件 (基于 QuickJS 引擎插件)

Agentxx 原生只维护单一的 C++ 插件基础设施。JS 脚本插件通过内置的 **[agentxx_javascript_engine](/agent/plugins/agentxx_javascript_engine/)** 引擎插件承载：
- 采用 **统一插件模型**：所有插件都是 C++ 插件。JS 脚本插件表现为一个标准的 C++ 动态库外壳（如 [example_js](/agent/plugins/example_js/)）附带 `plugin.js` 脚本。
- **执行流程**：
  ```
  宿主加载 JS 插件壳 (libexample_js.dll/.so)
      └── 插件壳在 create 阶段调用 "interpreter.js" 能力将 plugin.js 交给 QuickJS 引擎
          └── QuickJS 引擎在专用线程中解析并执行脚本，将脚本中声明的工具/钩子反向注册到宿主
  ```
- 开发者亦可开发自研的脚本引擎插件（如 Python、Lua 等）替换或扩充脚本能力。

---

## 插件示例索引

- [example_plugin](/agent/plugins/example_plugin/)：原生 C++ 插件综合示例（包含 fast_tool、Task 协程工具、插件互调 call_tool、精确 sleep、中间件钩子、事件订阅与能力注册）。
- [example_js](/agent/plugins/example_js/)：JS 脚本插件示例。
- [example_resources](/agent/plugins/example_resources/)：会话资源贡献插件示例（声明式与编程式 MCP / Skill / 规则 / 会话环境资源注入）。
