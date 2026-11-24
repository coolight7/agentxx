/// 基本功能检查 —— 进程内 mock OpenAI 兼容 SSE 服务器 + 真实 libagentxx 会话。
///
/// 覆盖 (与 C++ 测试 test_ffi_c_api.cpp 同构, 但只走 Dart SDK 的公开面):
///   场景 A: 订阅事件 → start → ready → sendInput → 流式增量 → TURN_END
///   场景 B: HIL 权限中断 (all_ask 模式) → respondInterrupt(decision) → 轮次恢复
///   场景 C: 调用顺序 (model.add → model.select → model.list 依次生效)
///   场景 D: 取消 (AgentxxCancelToken 让在飞的调用以 canceled 收尾)
///   场景 E: 生命周期 (stop → EVT_STOPPED → dispose)
///
/// 运行 (在 dart/ 目录):
///   dart run example/smoke_check.dart
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:agentxx_dart_cli/agentxx_dart_cli.dart';

Future<void> main() async {
  final server = await _startMockLLM();
  var failed = false;
  try {
    await _scenarioA(server);
    stdout.writeln('✓ 场景 A 通过 (基础对话流)');
    await _scenarioB(server);
    stdout.writeln('✓ 场景 B 通过 (HIL 权限中断)');
    await _scenarioC(server);
    stdout.writeln('✓ 场景 C 通过 (调用顺序)');
    await _scenarioD(server);
    stdout.writeln('✓ 场景 D 通过 (取消)');
    await _scenarioE(server);
    stdout.writeln('✓ 场景 E 通过 (生命周期)');
  } on Object catch (e, st) {
    failed = true;
    stdout.writeln('✗ 基本功能检查失败: $e');
    stdout.writeln(st.toString());
  } finally {
    await server.server.close(force: true);
  }
  exit(failed ? 1 : 0);
}

// ---------------------------------------------------------------------------
// mock OpenAI 兼容流式服务器
// ---------------------------------------------------------------------------

class _MockServer {
  _MockServer(this.server, this.requests);

  final HttpServer server;
  final List<String> requests;

  /// 下一次请求返回工具调用 (场景 B 用)
  bool nextIsToolCall = false;

  /// 每次响应前的等待 (场景 D 用: 让压缩这类要调模型的操作真的"在飞")
  int responseDelayMs = 0;

  int get port => server.port;
}

String _sse(Map<String, dynamic> chunk) => 'data: ${jsonEncode(chunk)}\n\n';

String _textSse(String content) {
  const id = 'chatcmpl-dart-mock';
  return _sse({
    'id': id,
    'object': 'chat.completion.chunk',
    'model': 'dart-mock',
    'choices': [
      {
        'index': 0,
        'delta': {'role': 'assistant', 'content': ''},
        'finish_reason': null,
      }
    ],
  }) +
      _sse({
        'id': id,
        'object': 'chat.completion.chunk',
        'model': 'dart-mock',
        'choices': [
          {
            'index': 0,
            'delta': {'content': content},
            'finish_reason': null,
          }
        ],
      }) +
      _sse({
        'id': id,
        'object': 'chat.completion.chunk',
        'model': 'dart-mock',
        'choices': [
          {'index': 0, 'delta': <String, dynamic>{}, 'finish_reason': 'stop'}
        ],
      }) +
      'data: [DONE]\n\n';
}

String _toolCallSse(String toolName, String argsJson) {
  const id = 'chatcmpl-dart-tool';
  return _sse({
    'id': id,
    'object': 'chat.completion.chunk',
    'model': 'dart-mock',
    'choices': [
      {
        'index': 0,
        'delta': {
          'role': 'assistant',
          'content': null,
          'tool_calls': [
            {
              'index': 0,
              'id': 'call_dart_1',
              'type': 'function',
              'function': {'name': toolName, 'arguments': ''},
            }
          ],
        },
        'finish_reason': null,
      }
    ],
  }) +
      _sse({
        'id': id,
        'object': 'chat.completion.chunk',
        'model': 'dart-mock',
        'choices': [
          {
            'index': 0,
            'delta': {
              'tool_calls': [
                {
                  'index': 0,
                  'function': {'arguments': argsJson},
                }
              ],
            },
            'finish_reason': null,
          }
        ],
      }) +
      _sse({
        'id': id,
        'object': 'chat.completion.chunk',
        'model': 'dart-mock',
        'choices': [
          {
            'index': 0,
            'delta': <String, dynamic>{},
            'finish_reason': 'tool_calls',
          }
        ],
      }) +
      'data: [DONE]\n\n';
}

Future<_MockServer> _startMockLLM() async {
  final requests = <String>[];
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  final mock = _MockServer(server, requests);

  unawaited(() async {
    await for (final req in server) {
      try {
        final body = await utf8.decoder.bind(req).join();
        requests.add(body);
        if (mock.responseDelayMs > 0) {
          await Future<void>.delayed(Duration(milliseconds: mock.responseDelayMs));
        }
        req.response.headers
          ..contentType = ContentType('text', 'event-stream', charset: 'utf-8')
          ..set('cache-control', 'no-cache');
        if (mock.nextIsToolCall && requests.length == 1) {
          // 首次请求返回文件读取工具调用 (路径真实存在, 便于权限放行后成功执行)
          mock.nextIsToolCall = false;
          req.response.write(_toolCallSse(
            'agentxx_filesystem_list',
            jsonEncode({'path': Directory.systemTemp.path}),
          ));
        } else {
          req.response.write(_textSse('hello from dart ffi mock'));
        }
        await req.response.close();
      } catch (_) {
        try {
          await req.response.close();
        } catch (_) {}
      }
    }
  }());

  return mock;
}

// ---------------------------------------------------------------------------
// 场景
// ---------------------------------------------------------------------------

String _modelJson(int port) => jsonEncode({
      'type': 'openai',
      'baseUrl': 'http://127.0.0.1:$port/v1',
      'apiKey': 'sk-smoke',
      'modelName': 'dart-mock-model',
    });

Future<AgentClient> _createClient(String? configJson, int port) async {
  final client = AgentClient();
  client.events.listen((_) {}, cancelOnError: false); // 占位订阅 (防丢事件)
  await client.start(configJson: configJson, modelJson: _modelJson(port));
  return client;
}

/// 按仓库布局找插件目录 (dart 目录相对 ../../.. 是 agent/)
String? _findPluginDir(String name) {
  final candidates = <String>[
    '../../../build/windows-debug/exec/plugins/$name',
    '../../../build/windows-release/exec/plugins/$name',
    '../../../build/linux-debug/exec/plugins/$name',
    '../../../build/linux-release/exec/plugins/$name',
    '../../../build/macos-debug/exec/plugins/$name',
    '../../../build/macos-release/exec/plugins/$name',
    Platform.environment['AGENTXX_PLUGIN_DIR'] ?? '',
  ];
  for (final dir in candidates) {
    if (dir.isEmpty) {
      continue;
    }
    if (Directory(dir).existsSync()) {
      return Directory(dir).absolute.path;
    }
  }
  return null;
}

Future<void> _scenarioA(_MockServer mock) async {
  final client = await _createClient(null, mock.port);
  try {
    final text = StringBuffer();
    final turnEnd = Completer<TurnEndEvent>();
    final sub = client.events.listen((e) {
      if (e is DeltaEvent && e.kind == 'text_token') {
        text.write(e.text);
      } else if (e is TurnEndEvent && !turnEnd.isCompleted) {
        turnEnd.complete(e);
      } else if (e is ErrorEvent && !turnEnd.isCompleted) {
        turnEnd.completeError(AgentxxException(e.code, e.name, e.message));
      }
    }, cancelOnError: false);

    await client.ready.timeout(const Duration(seconds: 60));

    // 提交 + 回执: 输入被受理 (status 由服务端给)
    final ack = await client.sendInput('hello');
    if (ack['status'] == 'rejected') {
      throw StateError('输入被拒: ${ack['reason']}');
    }

    final end = await turnEnd.future.timeout(const Duration(seconds: 60));
    sub.cancel();

    if (!text.toString().contains('hello from dart ffi mock')) {
      throw StateError(
          '未收到预期流式文本, 实际收到: "${text.toString()}"; '
          'turnEnd(hasError=${end.hasError}, err=${end.errorMessage})');
    }
  } finally {
    await client.dispose();
  }
}

Future<void> _scenarioB(_MockServer mock) async {
  mock.nextIsToolCall = true;
  mock.requests.clear();

  // 文件系统工具在插件里 (agentxx_filesystem): 找到就用它触发权限询问
  final pluginDir = _findPluginDir('agentxx_filesystem');
  final config = <String, Object?>{'permissionMode': 'all_ask'};
  if (pluginDir != null) {
    config['plugins'] = [
      {'path': pluginDir, 'enabled': true, 'sides': 'auto'}
    ];
  } else {
    stdout.writeln('  (未找到 agentxx_filesystem 插件目录, 场景 B 跳过)');
    return;
  }

  final client = await _createClient(jsonEncode(config), mock.port);
  try {
    final interrupt = Completer<InterruptReqEvent>();
    final turnEnd = Completer<TurnEndEvent>();
    final sub = client.events.listen((e) {
      if (e is InterruptReqEvent && !interrupt.isCompleted) {
        interrupt.complete(e);
      } else if (e is TurnEndEvent && !turnEnd.isCompleted) {
        turnEnd.complete(e);
      } else if (e is ErrorEvent && !turnEnd.isCompleted) {
        turnEnd.completeError(AgentxxException(e.code, e.name, e.message));
      }
    }, cancelOnError: false);

    await client.ready.timeout(const Duration(seconds: 60));
    await client.sendInput('please list the temp dir');

    // 触发权限询问
    final irq = await interrupt.future.timeout(const Duration(seconds: 60));
    if (!irq.isPermissionAsk) {
      throw StateError('期望权限中断, 实际: name=${irq.interruptName}');
    }

    // 权限卡片的表单值是 decision / remember / fullAuth (见 agent_io.cpp)
    await client.respondInterrupt(irq.interruptId, <String, Object?>{
      'decision': true,
    });

    final end = await turnEnd.future.timeout(const Duration(seconds: 60));
    sub.cancel();
    if (end.hasError) {
      throw StateError('权限放行后轮次出错: ${end.errorMessage}');
    }
  } finally {
    await client.dispose();
  }
}

Future<void> _scenarioC(_MockServer mock) async {
  // 模型清单会落盘到 `{dataDir}/agentxx-config.yaml`: 用一个临时数据目录, 让重复跑
  // 不相互影响 (否则第二次 `model.add` 会报"已有同名模型")
  final dataDir = Directory.systemTemp.createTempSync('agentxx_smoke_');
  final client = await _createClient(
    jsonEncode({'dataDir': dataDir.path}),
    mock.port,
  );
  try {
    await client.ready.timeout(const Duration(seconds: 60));

    // 1) 添加模型 → 2) 切换 → 3) 列表: 每步都等回执, 因此顺序有保证
    await client.addModel(<String, Object?>{
      'name': 'dart-ordered-model',
      'type': 'openai',
      'baseUrl': 'http://127.0.0.1:${mock.port}/v1',
      'apiKey': 'EMPTY',
      'modelName': 'dart-ordered-model',
    });
    final selected = await client.selectModel('dart-ordered-model');
    if (selected['model'] != 'dart-ordered-model') {
      throw StateError('切换回执里的模型名不对: ${selected['model']}');
    }
    final list = await client.listModels();
    if (list['currentModel'] != 'dart-ordered-model') {
      throw StateError('model.list 的当前模型不对: ${list['currentModel']}');
    }
    final models = (list['models'] as List<dynamic>? ?? const []);
    if (!models.contains('dart-ordered-model')) {
      throw StateError('model.list 里没有新模型: $models');
    }

    // 4) 选一个不存在的模型 → ok:false + model_not_found (以前是静默忽略)
    try {
      await client.selectModel('no-such-model');
      throw StateError('选择不存在的模型应该失败');
    } on AgentxxException catch (e) {
      if (e.name != 'model_not_found') {
        throw StateError('失败原因应为 model_not_found, 实际: ${e.name}');
      }
    }

    // 5) 上下文查询与分页 (v3 的同步查询改成 op)
    final ctx = await client.getContext();
    if (!ctx.containsKey('messages')) {
      throw StateError('context.get 缺 messages: $ctx');
    }
    final page = await client.viewPage(count: 5);
    if (!page.containsKey('messages')) {
      throw StateError('context.view_page 缺 messages: $page');
    }

    // 6) 本地 op (不进 wire, 立即回执)
    final batch = await client.enableDeltaBatch(maxDelayMs: 16);
    if (batch['maxDelayMs'] != 16) {
      throw StateError('delta.set_batch 回执不对: $batch');
    }
    await client.disableDeltaBatch();
  } finally {
    await client.dispose();
    try {
      dataDir.deleteSync(recursive: true);
    } catch (_) {}
  }
}

Future<void> _scenarioD(_MockServer mock) async {
  final client = await _createClient(null, mock.port);
  try {
    await client.ready.timeout(const Duration(seconds: 60));

    // 先跑一轮: 压缩要有内容才会去调模型, 也才能被"卡住"
    final turnEnd = Completer<TurnEndEvent>();
    final sub = client.events.listen((e) {
      if (e is TurnEndEvent && !turnEnd.isCompleted) {
        turnEnd.complete(e);
      }
    }, cancelOnError: false);
    await client.sendInput('取消用例的种子消息');
    await turnEnd.future.timeout(const Duration(seconds: 60));
    sub.cancel();

    // 让模型慢下来: 压缩要调模型, 于是"提交 → 取消"必然落在在飞窗口内
    // (空会话的压缩会立刻失败, 那条错误回执会跑在取消之前, 测不到取消)
    mock.responseDelayMs = 3000;
    final token = AgentxxCancelToken();
    final pending = client.call('session.compact', cancel: token);
    token.cancel();
    try {
      await pending.timeout(const Duration(seconds: 30));
      throw StateError('被取消的调用应该抛 AgentxxCanceledException');
    } on AgentxxCanceledException {
      // 正常: 取消得到稳定错误
    } finally {
      mock.responseDelayMs = 0;
    }

    // 取消已完成的 reqId 是幂等的 (静默返回)
    final reqId = client.callNoWait('model.list');
    if (reqId <= 0) {
      throw StateError('callNoWait 未返回 reqId');
    }
    client.cancelCall(reqId);
    client.cancelCall(reqId); // 重复取消不应抛
  } finally {
    await client.dispose();
  }
}

Future<void> _scenarioE(_MockServer mock) async {
  final client = await _createClient(null, mock.port);
  final seen = <String>[];
  final sub = client.events.listen((e) {
    seen.add(e.runtimeType.toString());
  });

  await client.ready.timeout(const Duration(seconds: 60));
  await client.sendInput('hello');
  await client.events
      .firstWhere((e) => e is TurnEndEvent)
      .timeout(const Duration(seconds: 60));

  // stop 受理即返回; 收尾完成后 EVT_STOPPED 是最后一条事件
  await client.stop().timeout(const Duration(seconds: 90));

  // 事件流是异步投递的: 先等消费者真的收到 StoppedEvent, 再确认它后面没有新事件
  final deadline = DateTime.now().add(const Duration(seconds: 10));
  while (!seen.contains('StoppedEvent') && DateTime.now().isBefore(deadline)) {
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
  final atStop = seen.length;
  await Future<void>.delayed(const Duration(milliseconds: 300));
  sub.cancel();
  if (atStop != seen.length || seen.isEmpty || seen.last != 'StoppedEvent') {
    throw StateError('EVT_STOPPED 不是最后一条事件: ${seen.skip(atStop)}');
  }

  // destroy 在 Stopped 之后才成功 (dispose 内部先 stop 再 destroy)
  await client.dispose();
}
