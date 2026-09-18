# cxx_utilxx_base

无重依赖基础工具库 (与宿主无关的可复用基础设施)。

## 定位

- **用途**: 日志 / JSON / 字符串与编码转换 / 容器辅助 / 环境变量 / 系统探测 /
  取消令牌 / 异步卸载 —— 供 `cxx_utilxx`、`cxx_pluginxx` 与各宿主复用
- **不含**: 网络 / 数据库 / 正则 / 图引擎 / 会话语义; 需要这些请用 `cxx_utilxx`
- **依赖**: fmt、simdjson、Boost (仅头文件; asio/beast)、iconv + uchardet (可选, 字符编码)、
  liburing (可选, Linux/Android 文件异步 I/O —— `CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED`;
  导出接口以库名 `PkgConfig::uring` 声明, 具体库由使用方解析)
  —— **禁止**依赖 neograph / OpenSSL / SQLite 等重依赖

## 目录结构

```
include/utilxx_base/    基础件 (命名空间 utilxx_base)
  log.h  json.h  json_view.h  string_util.h  env.h  system.h
  container_util.h  hash.h  lru_cache.h  path_sanitize.h  stream.h
  async_mutex.h  asio_error.h  exception.h  version.h
include/utilxx/         跨库共享契约 (命名空间 utilxx)
  cancel.h              CancelToken / CancelTokenPtr / SignalCancelToken / CancelledException
  async_offload.h       offloadAsync / offloadCancellableAsync / asyncWithTimeout
src/                    实现 (env/json/json_view/log/string_util/system)
```

> `utilxx` 命名空间在两处扩展: 本库提供取消与卸载契约, `cxx_utilxx` 提供网络/存储/
> 正则等工具; 二者 include 目录同名 (`utilxx/`) 合并安装, 文件名不重复。

## 构建与使用

```cmake
# 顺序: 先按开关查找依赖库的依赖 (条件依赖, 导出接口只声明目标名), 再导入依赖库本体
if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)  # 与宿主构建的开关保持一致
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()
find_package(cxx_utilxx_base REQUIRED)
target_link_libraries(your_target PRIVATE cxx_utilxx_base_static)  # 或 cxx_utilxx_base_shared
```

- 产物命名: Release `libcxx_utilxx_base.so` / `libcxx_utilxx_base_static.a`,
  Debug 追加 `d` → `libcxx_utilxx_based.so` / `libcxx_utilxx_base_staticd.a`
- 可选特性由 CMake 开关控制: `CXX_UTILXX_BASE_ENABLE_CHARSET` (iconv/uchardet 字符编码)、
  `CXX_UTILXX_BASE_USE_BOOST_ASIO`、`CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED`
  (Linux/Android 文件异步 I/O)
- **条件依赖只声明库名, 使用方在本机 find (顺序: 先依赖库的依赖, 再依赖库本体)**:
  开启 io_uring 后导出接口里出现的是 `PkgConfig::uring` 这个**名称**(与
  `fmt::fmt`/`OpenSSL::SSL` 同类), 不含任何库文件路径 —— 静态库不携带依赖二进制,
  具体库由使用方在自己机器上解析: 导入方须**先**在自身 CMakeLists 的依赖查找段写
  ```cmake
  if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)   # 与宿主构建的开关同源
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
  endif ()
  ```
  **再** `find_package(cxx_utilxx_base)` (导出目标会校验 INTERFACE 引用的目标是否
  已存在, 顺序颠倒会报 "target PkgConfig::uring not found");
  漏查找时要么 configure 报目标不存在, 要么运行期加载报
  `undefined symbol: io_uring_queue_init`
- **独立构建提醒**: Boost.Asio 在 Linux 上检测到 `<liburing.h>` 时会自动定义
  `ASIO_HAS_IO_URING` (system.cpp 随之引用 io_uring), 故独立构建 (非宿主
  superbuild 的子项目) 时应把 `CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED` 设为 ON
- **静态 / 动态变体选择**: 同一进程内需要单份实现 (如插件宿主与插件共享状态) 时用动态变体;
  否则用静态变体 (默认, 便于裁剪与分发)
- 依赖经 `find_dependency` 链自动解析 (fmt/simdjson/Boost, 启用字符集时另有 iconv/uchardet;
  io_uring 为条件依赖, 按上述库名由使用方在 find_package 之前查找)

```c++
#include "utilxx_base/json.h"        // utilxx_base::Json
#include "utilxx_base/log.h"         // XX_LOGI / XX_LOGW ...
#include "utilxx/cancel.h"           // utilxx::CancelToken
#include "utilxx/async_offload.h"    // utilxx::offloadCancellableAsync

auto v = utilxx_base::Json::parse(R"({"a":1})");
auto token = std::make_shared<utilxx::SignalCancelToken>();
auto r = co_await utilxx::offloadCancellableAsync<int>(pool, token, [](std::atomic<bool>& flag) -> asio::awaitable<int> {
    // 阻塞工作: 周期性检查 flag
    co_return 1;
});
```

## asio 来源

本库统一使用 **Boost.Asio**: 源码写 `asio/xxx.hpp`, 由构建侧加入的两个 include 根
(`<Boost>/include` 与 `<Boost>/include/boost/`) 映射到 `boost/asio/xxx.hpp`;
`UTILXX_USE_BOOST_ASIO` 宏 (PUBLIC 传递) 提供全局 `namespace asio = ::boost::asio`。
这**两个 include 根缺一不可**, 否则 boost asio 内部头会回退到系统 Boost, 与宿主 asio 混用崩溃。

## 平台

Linux / Windows / macOS / Android / iOS 均可编译。文件异步 I/O 支持情况见
`utilxx_base::isAsyncFileIoSupported()` (Linux 由 io_uring 提供, 需运行时确认可用;
liburing 依赖经 `CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED` 启用, 导出接口以
`PkgConfig::uring` 名称声明, 使用方自行 find)。
