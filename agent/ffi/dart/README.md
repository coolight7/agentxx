# agentxx_ffi_bindings

Dart FFI bindings for `libagentxx` C API (`agent/lib/include/agentxx/ffi_api.h`).

## Usage

```dart
import 'dart:ffi';
import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart';

void main() {
  final dylib = DynamicLibrary.open('libagentxx_shared.dll');
  final bindings = AgentxxFfiBindings(dylib);

  print('API Version: ${bindings.agentxx_ffi_api_version()}');
}
```

## 调用模型 (API v4)

所有业务操作都走一个入口: 提交 `{"op":..,"args":..}` 拿回 `reqId`, 结果经
`on_event` 的 `AGENTXX_FFI_EVT_RESULT` (`{reqId, op, ok, result?|error?}`) 回来。
事件 payload 归回调所有: 结构体与正文是库分配的**一块**堆内存, 取完正文后用
`agentxx_ffi_free(payload)` 释放整块 (`agentxx_ffi_string_free` 只用于出参式接口的字符串内容)。
因为结构体也在那块内存上, 用 `NativeCallable.listener` 把回调延后到宿主 isolate 处理也不会悬垂。

高层封装见 `agent/example/ffi/dart`（提交 + `Future` 回执、取消令牌、事件流）。

## Regenerating Bindings

To regenerate bindings after updating `ffi_api.h` (在本目录):

```bash
dart pub get
dart run ffigen --config ffigen.yaml
```

注: ffigen 19 从 `--config` 指定的文件读配置 (旧版从 `pubspec.yaml` 的 `ffigen:`
段读, 不再使用)。
