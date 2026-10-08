import 'dart:io';
import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart';

void main() {
  final libPath = Platform.isWindows
      ? 'libagentxx_shared.dll'
      : (Platform.isMacOS ? 'libagentxx.dylib' : 'libagentxx.so');

  print('Agentxx FFI Bindings example target: $libPath');
  print('API Version constant: $AGENTXX_FFI_API_VERSION');
}
