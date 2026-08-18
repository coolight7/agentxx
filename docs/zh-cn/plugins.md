# 插件开发
- 内置插件示例参考[plugins](/agent/plugins/)

## C++ 插件
- 兼容性准寻: 准寻以上要求，主程序和插件动态库允许由不同编译器、不同C++或标准库版本编译后仍能兼容运行
    - 为了尽量保持兼容性，主程序和插件之间的接口只能使用 C Api，不能使用 c++；插件会被编译成动态库，然后按接口要求导出接口符号，由主程序运行时加载插件动态库后查找符号调用
    - 主程序和插件编译时默认动态链接 c++ 标准库，减少体积；插件也可以自己静态链接c++标准库、libgcc_s
    - 主程序和插件可以复用一些代码，比如一些工具函数，这部分复用代码需要静态链接进主程序和各自插件内，确保兼容不同版本的复用代码编译的主程序和插件可以加载运行
    - 在主程序和插件的接口中不能传递标准库结构体，也不能传递复用代码里的结构体，这些都只能各自内部使用，且插件编译时应当只导出接口符号，其他符号全部隐藏
- **导出符号控制** (2026-08 起): 插件动态库默认隐藏全部符号, 仅导出宿主查找的
  入口符号。插件源码定义入口函数时必须用 `AGENTXX_PLUGIN_EXPORT` 宏标记
  (定义在 `plugin_api.h`, 须位于 `extern "C"` 之后):
    ```c
    extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** plugin_ctx);
    ```
  - 入口符号 (dlsym/GetProcAddress 按名查找):
    `agentxx_plugin_get_info` / `agentxx_plugin_entry` / `agentxx_plugin_unload`
    (agent 侧) + `agentxx_client_get_info` / `agentxx_client_entry` /
    `agentxx_client_unload` (client 侧, 双端插件)
  - 构建侧已统一配置: ELF `-fvisibility=hidden` + version script 白名单
    (隐藏第三方静态库符号), MSVC 不自动导出 (仅 dllexport 生效);
    `plugins/CMakeLists.txt` 与各插件 CMakeLists 已设置, 新插件无需额外配置
  - 实测: 插件动态库导出符号数 = 入口符号数 (双端 6 / 单端 3), 内部 C++
    符号与第三方库符号全部隐藏
- **工具函数复用** (2026-08 起): 插件可复用主程序 `agent/lib/include/agentxx/util`
  的全部工具函数 (字符串/编码转换/路径/base64/http/sqlite/regex 等), 经独立
  静态库 `agentxx_util` 提供 (由 libagentxx 与插件各自静态链接一份, 符号隐藏互不冲突):
    ```cmake
    # 插件 CMakeLists.txt (动态编译分支)
    find_package(agentxx_util REQUIRED)
    target_link_libraries(${PLUGIN_NAME} PRIVATE agentxx_util)
    ```
  - 用法示例见 [example_plugin](/agent/plugins/example_plugin/):
    ```cpp
    #include "agentxx/util/string_util.h"
    auto b64 = agentxx::util::base64Encode(data);  // 静态链入本插件副本
    ```
  - 收录范围: **`src/util/` 全部源文件** (http_client/http_server/ws_client/
    string_util/util/sqlite/settings_db/log/regex/http_header) + 全部头文件
  - 定位: 面向 agentxx 内置插件 (与主程序同一 superbuild 构建、依赖齐全) 的
    便捷复用库; 第三方插件不需要它 (纯 C ABI 头即可, 甚至不用 C++)
  - 依赖全部 PUBLIC 传递: 轻量 (fmt/sqlite3/uchardet/iconv) + 重依赖
    (neograph 系/yyjson/OpenSSL/hyperscan/uring) 的链接与 include 随
    INTERFACE 自动获得, 插件链接本库后直接可用全部 util, 无需自行配置;
    未引用的模块按目标文件提取规则自动裁剪 (实测 9 个插件 DT_NEEDED 仅系统库)
  - 内置合并编译模式: 插件目标 `target_link_libraries(${_target} PRIVATE agentxx_util)`
    (仅传递编译宏/头路径)
- 命名建议:
    - `group`_`name`; 生成的动态库统一前缀添加 `lib`，比如`agentxx`自带的`codegraph`插件命名为 `agentxx_codegraph`，生成动态库文件名`libagentxx_codegraph`
- 插件目前支持两种类型，可以编译到同一插件，生成一份动态库加载执行:
    - agent插件:  在 agent 执行会话 agent-loop 的过程中执行
    - client插件: 在 TUI/GUI 等UI客户端执行
- 默认情况下插件将编译为动态库，主程序运行时由 `agentxx-config.yaml` 的 `plugins` 段指定动态加载插件，但也可以在编译 `agentxx` 时直接内嵌插件, 详见[AGENTXX_ENABLE_PLUGIN_BUILTIN](/agent/CMakeLists.txt)参数
- [C++插件示例](/agent/plugins/example_plugin/)

## Javascript 插件
- agentxx原生支持C++插件，JS由插件 [agentxx_javascript_engine](/agent/plugins/agentxx_javascript_engine/) (QuickJS) 提供支持，也就是说，JS插件可以声明依赖`agentxx_javascript_engine`，然后JS代码可以调用它进行执行
- JS 插件仍需要保持C++插件的代码结构，同样需要编译出独立动态库，整体跟普通的C++插件是一样的，只是多带了JS代码文件可以运行时加载执行。执行流程为:
```
主程序调用插件 -> JS 插件的 c++代码 调用`agentxx_javascript_engine` -> `agentxx_javascript_engine` 加载执行JS代码
```
- 当然也可以自行开发 `JS执行引擎插件` 替换
- [JS插件示例](/agent/plugins/example_js/)

## 其他编程语言插件
- 类似于 Javascript插件 的实现，可以先开发一个 `xxx_engine` 的编程语言执行插件，然后再依赖它扩展