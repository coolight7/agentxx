# cxx_utilxx

重依赖工具库 (网络 / 存储 / 正则 / 进程 / 散列)。

## 定位

- **用途**: HTTP 客户端与服务端 (Boost.Beast + OpenSSL)、WebSocket 客户端、
  正则 (HyperScan 或 std::regex)、Aho-Corasick、路由、文本差异、
  git worktree、MD5/设备标识
- **依赖**: `cxx_utilxx_base` (基础件与取消抽象)、Boost (beast/process/asio)、OpenSSL、
  fmt、html2md; 可选 HyperScan (正则加速, 导出接口以库名
  `PkgConfig::hyperscan` + `hs_runtime` 声明, 具体库由使用方解析)。
  文件异步 I/O 的 io_uring 依赖属 `cxx_utilxx_base` (其 `system.cpp` 使用), 经本库
  的链接接口间接传递
- **命名空间**: `utilxx` (与 `cxx_utilxx_base` 的 `utilxx/cancel.h`、`utilxx/async_offload.h` 同一命名空间)
- **禁止**依赖 neograph 与任何宿主头文件
- 数据库 (SQLite) 封装与全局设置库已迁回宿主 `<宿主>/util/` (宿主专用, 本库不提供)

## 目录结构

```
include/utilxx/
  http_client.h  http_header.h  http_error.h  http_server.h  ws_client.h
  router.h  regex.h  aho_corasick.h
  diff_util.h  worktree.h  crypto.h
src/                    实现 (crypto/http_client/http_header/http_server/regex/ws_client)
```

## 构建与使用

```cmake
# 顺序: 先查找依赖库的依赖 (条件依赖, 导出接口只声明目标名), 再导入依赖库本体
if (CXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED)  # cxx_utilxx_base 声明该依赖
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(uring REQUIRED IMPORTED_TARGET liburing)
endif ()
if (CXX_UTILXX_ENABLE_HYPERSCAN)               # 本库声明该依赖
  pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)
endif ()
find_package(cxx_utilxx REQUIRED)   # 内部 find_dependency(cxx_utilxx_base) 链解析其余依赖
target_link_libraries(your_target PRIVATE cxx_utilxx_static)  # 或 cxx_utilxx_shared
```

- 产物命名: Release `libcxx_utilxx.so` / `libcxx_utilxx_static.a`,
  Debug 追加 `d` → `libcxx_utilxxd.so` / `libcxx_utilxx_staticd.a`
- 可选特性由 CMake 开关控制: `CXX_UTILXX_ENABLE_HYPERSCAN` (正则实现)、
  `CXX_UTILXX_ENABLE_BOOST_PROCESS` (worktree 子进程调用)
- **条件依赖只声明库名, 使用方在本机 find (顺序: 先依赖库的依赖, 再依赖库本体)**:
  开启 HyperScan 后导出接口里出现的是 `PkgConfig::hyperscan` 与裸库名 `hs_runtime`
  (**名称**, 与 `fmt::fmt`/`OpenSSL::SSL` 同类), 不含库文件路径。静态库不携带依赖二进制,
  具体库由使用方在自己机器上解析: 须**先**在自身 CMakeLists 的依赖查找段写
  `pkg_check_modules(hyperscan REQUIRED IMPORTED_TARGET libhs)`
  (`hs_runtime` 经 `INTERFACE_LINK_DIRECTORIES` 里的安装目录以裸库名解析, 无需 find),
  **再** `find_package(cxx_utilxx)` —— 顺序颠倒会报 "target ... not found";
  本库 PUBLIC 依赖的 `cxx_utilxx_base` 声明的 `PkgConfig::uring` 同理须先查找

```c++
#include "utilxx/http_client.h"

auto resp = co_await utilxx::HttpClient::postAsync(url, body, "application/json", headers,
                                                   utilxx::HttpClient::RequestConfig{});
auto json = resp.bodyJson();          // std::optional<utilxx_base::Json>
```

## 子进程相关

`utilxx/worktree.h` 的 git 操作与 HTTP 客户端的 DNS 解析在阻塞路径上工作,
调用方应经 `utilxx::offloadAsync` / `offloadCancellableAsync` 卸载到线程池,
避免阻塞 io_context 线程; 取消语义统一经 `utilxx::CancelToken` (见 `cxx_utilxx_base`)。

## 平台

Linux / Windows / macOS / Android / iOS 均可编译; HyperScan 与 io_uring 仅在
Linux (HyperScan 另支持 Windows 受限支持) 生效, 未启用时自动回退 std::regex /
同步文件 I/O。
