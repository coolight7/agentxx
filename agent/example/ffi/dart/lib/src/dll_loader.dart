/// 动态库解析 —— 定位并加载 libagentxx 共享库。
///
/// 查找顺序:
/// 1. 显式传入的 [override] 路径 / 环境变量 `AGENTXX_DLL`
/// 2. 本仓库约定布局下的候选目录 (debug 优先, 兼容 linux/android/mac 命名)
/// 3. 系统库搜索路径 (裸名加载, 供安装后的部署形态使用)
library;

import 'dart:ffi';
import 'dart:io';

/// 当前平台的动态库文件名 (优先级从高到低; MSVC debug 构建带 'd' 后缀,
/// 见 lib/CMakeLists.txt)
List<String> _platformLibraryNames() {
  if (Platform.isWindows) {
    return const ['libagentxxd.dll', 'libagentxx.dll'];
  }
  if (Platform.isMacOS) {
    return const ['libagentxx_shared.dylib'];
  }
  // linux / android
  return const ['libagentxx_shared.so', 'libagentxx.so'];
}

/// 相对当前工作目录 / 可执行文件目录的候选目录 (仓库内开发布局)
List<String> _candidateDirectories() {
  final exeDir = File(Platform.resolvedExecutable).parent.path;
  final dirs = <String>[
    // 以 cwd 为基准 (在 dart/ 目录内运行 `dart run ...` 的典型场景)
    '../agent/build/windows-debug/exec',
    '../agent/build/windows-release/exec',
    '../agent/build/linux-debug/exec',
    '../agent/build/linux-release/exec',
    '../agent/build/android-release/exec',
    // 以可执行文件为基准 (编译为 exe 后运行的场景; `dart run` 时指向
    // dart sdk 目录, 候选不存在会被安全跳过)
    '$exeDir/../agent/build/windows-debug/exec',
    '$exeDir/../agent/build/windows-release/exec',
    '$exeDir',
    '.',
  ];
  return dirs;
}

/// 打开 libagentxx 共享库; 找不到时抛出异常并列出全部已尝试路径。
DynamicLibrary openAgentxxLibrary({String? override}) {
  final candidates = <String>[];

  final explicit = override ?? Platform.environment['AGENTXX_DLL'];
  if (explicit != null && explicit.isNotEmpty) {
    candidates.add(explicit);
  }

  final names = _platformLibraryNames();
  for (final dir in _candidateDirectories()) {
    for (final name in names) {
      candidates.add(dir.isEmpty ? name : '$dir/$name');
    }
  }
  // 最后回退系统搜索路径
  candidates.addAll(names);

  final tried = <String>[];
  for (final path in candidates) {
    try {
      return DynamicLibrary.open(path);
    } catch (_) {
      tried.add(path);
    }
  }
  throw StateError(
    '无法加载 libagentxx 动态库。\n'
    '已尝试:\n  ${tried.join('\n  ')}\n'
    '请先编译动态库 (agent/script/*_debug_build.bat|.sh), 或经环境变量\n'
    'AGENTXX_DLL / --dll 参数显式指定动态库路径。',
  );
}
