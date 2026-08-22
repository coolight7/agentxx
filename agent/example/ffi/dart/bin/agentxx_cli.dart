/// agentxx_dart_cli —— 经 libagentxx FFI 运行内置 agent 会话的 Dart 命令行客户端。
///
/// 用法示例 (在 dart/ 目录):
/// ```sh
/// dart run bin/agentxx_cli.dart \
///     --base-url https://api.example.com/v1 \
///     --api-key sk-xxx \
///     --model gpt-4o-mini
/// ```
/// 也可用 --model-json/--config-json 直接传 ffi_api.h 契约的 JSON 文件,
/// 或经环境变量 AGENTXX_BASE_URL / AGENTXX_API_KEY / AGENTXX_MODEL_NAME 提供。
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:agentxx_dart_cli/src/agent_client.dart';
import 'package:agentxx_dart_cli/src/console_setup.dart';
import 'package:agentxx_dart_cli/src/repl.dart';

Future<void> main(List<String> args) async {
  setupConsole();

  final opts = _parseArgs(args);
  if (opts == null) {
    return; // --help 已打印 / 参数错误已报告
  }

  // ---- 装配模型配置 (ffi_api.h ModelConfig 契约) ----
  final modelJson = await _loadModelJson(opts);
  if (modelJson == null) {
    stdout.writeln('缺少模型配置。请任选其一:');
    stdout.writeln('  1) --base-url/--api-key/--model (或同名 AGENTXX_* 环境变量)');
    stdout.writeln('  2) --model-json <ModelConfig.json 文件路径>');
    exit(64);
  }
  final configJson = await _loadConfigJson(opts);

  // ---- 启动 ----
  final AgentClient client;
  try {
    client = AgentClient(dllPath: opts.dll);
  } catch (e) {
    stdout.writeln(e.toString());
    exit(65);
  }

  final repl = CliRepl(client);
  ProcessSignal.sigint.watch().listen((_) => repl.requestExit());

  // 先订阅事件再启动 (广播流无订阅者时会丢弃事件)
  repl.start();
  try {
    await client.start(configJson: configJson, modelJson: modelJson);
  } on Object catch (e) {
    stdout.writeln('✗ agent 启动失败: $e');
    final logs = client.drainLogs();
    if (logs != null && logs.isNotEmpty) {
      stdout.writeln('最近日志:');
      for (final entry in logs.take(10)) {
        if (entry is Map<String, dynamic>) {
          stdout.writeln('  [${entry['level']}] ${entry['message']}');
        }
      }
    }
    // 失败也要走完整清理 (停止原生 io 线程/释放队列), 避免 TLS/线程泄漏
    await _disposeQuietly(repl, client);
    exit(66);
  }

  // 等待就绪 (EVT_READY); 失败 (EVT_ERROR) 则退出
  const readyTimeout = Duration(seconds: 90);
  try {
    await client.ready.timeout(readyTimeout);
  } on TimeoutException {
    stdout.writeln('✗ 等待 agent 就绪超时 (${readyTimeout.inSeconds}s)');
    await _disposeQuietly(repl, client);
    exit(67);
  } on AgentxxException catch (e) {
    stdout.writeln('✗ agent 初始化失败: $e');
    await _disposeQuietly(repl, client);
    exit(67);
  }

  // ---- REPL 主循环直到退出 ----
  final lines = stdin.transform(utf8.decoder).transform(const LineSplitter());
  unawaited(lines.listen(
    (line) => unawaited(repl.handleLine(line)),
    onDone: repl.requestExit, // stdin 关闭 (Ctrl+Z/D) 视同退出
  ).asFuture<void>());

  await repl.done;

  stdout.writeln('停止中…');
  await _disposeQuietly(repl, client);
  exit(0);
}

Future<void> _disposeQuietly(CliRepl repl, AgentClient client) async {
  try {
    await repl.dispose();
  } catch (_) {}
  try {
    await client.dispose();
  } catch (_) {}
}

// ---------------------------------------------------------------------------
// 参数与配置装配
// ---------------------------------------------------------------------------

class _Options {
  String? dll;
  String? modelJsonPath;
  String? configJsonPath;
  String? baseUrl;
  String? apiKey;
  String? modelName;
  String? apiType;
  String? permissionMode;
  String? dataDir;
  bool enableSessionStore = false;
}

_Options? _parseArgs(List<String> args) {
  final opts = _Options();
  for (var i = 0; i < args.length; i++) {
    final a = args[i];
    String next() {
      if (i + 1 >= args.length) {
        throw FormatException('$a 需要一个参数');
      }
      return args[++i];
    }

    switch (a) {
      case '-h':
      case '--help':
        _printUsage();
        return null;
      case '--dll':
        opts.dll = next();
      case '--model-json':
        opts.modelJsonPath = next();
      case '--config-json':
        opts.configJsonPath = next();
      case '--base-url':
        opts.baseUrl = next();
      case '--api-key':
        opts.apiKey = next();
      case '--model':
        opts.modelName = next();
      case '--api-type':
        opts.apiType = next();
      case '--permission-mode':
        opts.permissionMode = next();
      case '--data-dir':
        opts.dataDir = next();
      case '--enable-session-store':
        opts.enableSessionStore = true;
      default:
        throw FormatException('未知参数: $a (见 --help)');
    }
  }
  return opts;
}

void _printUsage() {
  stdout.write('''
agentxx_dart_cli —— libagentxx 的 Dart FFI 命令行 agent 客户端

用法: dart run bin/agentxx_cli.dart [选项]

选项:
  -h, --help                 显示本帮助
      --dll <path>           显式指定 libagentxx 动态库路径
                             (默认按仓库布局查找, 或设环境变量 AGENTXX_DLL)
      --model-json <path>    主模型 ModelConfig JSON 文件 (契约见 agent/lib/include/agentxx/ffi_api.h)
      --config-json <path>   可选 AgentConfig 覆盖 JSON 文件

快捷模型配置 (等价于手写 ModelConfig JSON):
      --base-url <url>       LLM API 地址 (env: AGENTXX_BASE_URL)
      --api-key <key>        API Key          (env: AGENTXX_API_KEY)
      --model <name>         模型名            (env: AGENTXX_MODEL_NAME)
      --api-type <t>         openai|anthropic|openai-responses (默认 openai)

可选 AgentConfig 覆盖:
      --permission-mode <m>  ask|all_ask|pass|deny (默认 ask: 工具访问询问)
      --data-dir <dir>       数据目录 (会话持久化等)
      --enable-session-store 开启会话持久化 (配合 /sessions 与 /switch)

交互命令: 对话直接输入; /help 查看; Ctrl+C 退出。
''');
}

/// 优先 --model-json 文件; 否则由快捷参数/环境变量拼装
Future<String?> _loadModelJson(_Options o) async {
  if (o.modelJsonPath != null) {
    return File(o.modelJsonPath!).readAsString();
  }

  final baseUrl = o.baseUrl ?? Platform.environment['AGENTXX_BASE_URL'];
  final apiKey = o.apiKey ?? Platform.environment['AGENTXX_API_KEY'];
  final modelName = o.modelName ?? Platform.environment['AGENTXX_MODEL_NAME'];
  if (baseUrl == null ||
      baseUrl.isEmpty ||
      modelName == null ||
      modelName.isEmpty) {
    return null;
  }

  return const JsonEncoder().convert(<String, dynamic>{
    'type': o.apiType ??
        Platform.environment['AGENTXX_API_TYPE'] ??
        'openai',
    'baseUrl': baseUrl,
    'apiKey': apiKey ?? 'EMPTY',
    'modelName': modelName,
  });
}

/// 读取 --config-json 并合并命令行覆盖项
Future<String?> _loadConfigJson(_Options o) async {
  Map<String, dynamic> config = <String, dynamic>{};

  final path = o.configJsonPath;
  if (path != null) {
    final text = await File(path).readAsString();
    final decoded = jsonDecode(text);
    if (decoded is! Map<String, dynamic>) {
      throw StateError('--config-json 文件内容必须是 JSON 对象');
    }
    config = decoded;
  }

  if (o.permissionMode != null) {
    config['permissionMode'] = o.permissionMode;
  }
  if (o.dataDir != null) {
    config['dataDir'] = o.dataDir;
  }
  if (o.enableSessionStore) {
    config['enableSessionStore'] = true;
  }

  if (config.isEmpty) {
    return null;
  }
  return const JsonEncoder().convert(config);
}
