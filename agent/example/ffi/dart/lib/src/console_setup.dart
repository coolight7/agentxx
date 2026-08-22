/// 终端环境适配 —— Windows 控制台启用 UTF-8 代码页与 ANSI 转义序列。
///
/// Dart 的 stdout 在 Windows 上直接写 UTF-8 字节; 传统 conhost 需要切换到
/// 65001 代码页并开启 ENABLE_VIRTUAL_TERMINAL_PROCESSING 才能正确显示中文
/// 与彩色输出。Windows Terminal 天然支持, 此处设置幂等无害。其他平台 no-op。
library;

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

const int _cpUtf8 = 65001;
const int _stdOutputHandle = -11; // STD_OUTPUT_HANDLE
const int _enableVirtualTerminalProcessing = 0x0004;

void setupConsole() {
  if (!Platform.isWindows) {
    return;
  }
  try {
    final kernel32 = DynamicLibrary.open('kernel32.dll');

    // 切换控制台代码页为 UTF-8 (输入+输出), 使 CJK 与管道行为一致
    final setCp = kernel32.lookupFunction<Int32 Function(Uint32), int Function(int)>(
        'SetConsoleOutputCP');
    final setInputCp =
        kernel32.lookupFunction<Int32 Function(Uint32), int Function(int)>('SetConsoleCP');
    setCp(_cpUtf8);
    setInputCp(_cpUtf8);

    // 启用 VT 处理 (ANSI 转义着色); 失败静默降级为无色纯文本
    final getStdHandle = kernel32.lookupFunction<
        Pointer<Void> Function(Int32),
        Pointer<Void> Function(int)>('GetStdHandle');
    final getMode = kernel32.lookupFunction<
        Int32 Function(Pointer<Void>, Pointer<Uint32>),
        int Function(Pointer<Void>, Pointer<Uint32>)>('GetConsoleMode');
    final setMode = kernel32.lookupFunction<
        Int32 Function(Pointer<Void>, Uint32),
        int Function(Pointer<Void>, int)>('SetConsoleMode');

    final hOut = getStdHandle(_stdOutputHandle);
    if (hOut == nullptr) {
      return;
    }
    final mode = calloc<Uint32>();
    try {
      if (getMode(hOut, mode) != 0) {
        setMode(hOut, mode.value | _enableVirtualTerminalProcessing);
      }
    } finally {
      calloc.free(mode);
    }
  } catch (_) {
    // 尽力而为: 控制台能力探测失败不影响主流程
  }
}

/// ANSI 转义辅助 (检测不支持时调用方自行降级)
abstract final class Ansi {
  static const String reset = '\x1B[0m';
  static const String bold = '\x1B[1m';
  static const String dim = '\x1B[2m';
  static const String red = '\x1B[31m';
  static const String green = '\x1B[32m';
  static const String yellow = '\x1B[33m';
  static const String magenta = '\x1B[35m';
  static const String cyan = '\x1B[36m';
  static const String gray = '\x1B[90m';

  /// 用指定颜色包裹文本 (流式场景请勿逐 token 包裹, 见 [CliRenderer])
  static String paint(String s, String code) => '$code$s$reset';
}
