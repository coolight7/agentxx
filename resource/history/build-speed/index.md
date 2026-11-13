# 编译速度优化

## 目标

找出 Daily build (`agent/script/windows_debug_build.bat` → 顶层 superbuild) 的时间都花在哪里,
在构建配置层面做几处改动, 并用同一台机器上的前后数据说明收益。

## 范围

- 只改构建配置 (`CMakeLists.txt` / `cmake/*.cmake` / 构建脚本), 不动业务代码
- 平台重点: Windows + MSVC (VS18 / MSVC 14.51, 12 核); Linux/macOS 走"不拆分"路径, 行为不变

## 状态

已完成。结论见 [work.md](work.md) 的"实测结果"段。

## 关键决策

- **关掉 C++20 模块扫描** (`CMAKE_CXX_SCAN_FOR_MODULES=OFF`): 项目不用模块, 但 VS 生成器
  每个工程都会跑 `ComputeStdModulesCompileInputs` / `GetLibraryModuleSources`, 每次构建白花约 10 s
- **MSVC 启用 PCH** (此前只在 GCC/Clang 生效): 每个 TU 都要重新解析 std/asio/fmt/boost
- **MSVC 静态库与动态库共用同一批 .obj**: 导出标记是 `ffi_api.h` 里无条件的
  `__declspec(dllexport)`, 与目标无关, 过去"各编一遍"是纯重复
- **源文件拆成多个 OBJECT 工程** (`agent/cmake/agentxx_split_sources.cmake`): VS 生成器只在
  工程之间并行, 同一个 `.vcxproj` 内的编译是串行的 —— 这是全量/头文件改动慢的主因。
  **不能改开 `UseMultiToolTask`** (它隐式带 `/FS`, 破坏 FileTracker 增量跟踪, 实测改 6 个
  文件重编 56 个)
- 不做: 换 Ninja 生成器 (脚本靠 VS 生成器自动发现工具链, 影响面太大); 动 ExternalProject 拓扑

## 结论 (数字见 work.md)

| 场景 | 改前 | 改后 |
| --- | --- | --- |
| lib 全量重建 (107 TU) | 861 s | 194 s |
| test 全量重建 (152 TU) | 1510 s | 317 s |
| lib 改 `context.h` 后重建 | ≥ 15 min (串行估算) | 156 s |
| 内层 lib / test no-op | 25.1 s / 18.6 s | 8.6 s / 3.8 s |
| 令顶 no-op | 60.5 s | 67 s (拆工程的固定开销) |
| 单文件增量 (整条链) | ~108 s | 113.7 s (持平, 由 ASan 下的 4 次完整链接主导) |

偏差: 令顶 no-op 比改前慢约 6 s (多出 39 个 `.vcxproj` 的过期判断), 与全量重建的
4~5 倍收益相比是可接受的; 需要单工程时可用 `-DAGENTXX_MSVC_SPLIT_OBJECTS=OFF` 回退。
