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

## Regenerating Bindings

To regenerate bindings after updating `ffi_api.h`:

```bash
dart run ffigen --config ffigen.yaml
```
