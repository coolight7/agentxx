/// 冒烟检查 —— 进程内 mock OpenAI 兼容 SSE 服务器 + 真实 libagentxx 会话。
///
/// 覆盖两条链路 (与 C++ 测试 test_ffi_c_api.cpp 同构):
///   场景 A: create→start→EVT_READY→send_input→流式 DELTA→TURN_END
///   场景 B: HIL 权限中断 (all_ask 模式工具调用触发 EVT_INTERRUPT_REQ
///           → interruptRespond(["true"]) → 轮次恢复)
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
    await _scenarioB(server);    stdout.writeln('✓ 场景 B 通过 (HIL 权限中断)');
  } on Object catch (e, st) {
    failed = true;
    stdout.writeln('✗ 冒烟检查失败: $e');
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
        req.response.headers
          ..contentType = ContentType('text', 'event-stream', charset: 'utf-8')
          ..set('cache-control', 'no-cache');
        if (mock.nextIsToolCall && requests.length == 1) {
          // 首次请求返回文件读取工具调用 (路径真实存在, 便于权限放行后成功执行)
          mock.nextIsToolCall = false;
          req.response.write(_toolCallSse(
            'agentxx_filesystem_read',
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

Future<AgentClient> _createClient(String? configJson, int port) async {
  final client = AgentClient();
  client.events.listen((_) {}, cancelOnError: false); // 占位订阅 (防丢事件)
  await client.start(configJson: configJson, modelJson: _modelJson(port));
  return client;
}

String _modelJson(int port) => jsonEncode({
      'type': 'openai',
      'baseUrl': 'http://127.0.0.1:$port/v1',
      'apiKey': 'sk-smoke',
      'modelName': 'dart-mock-model',
    });

Future<void> _scenarioA(_MockServer mock) async {
  final client = await _createClient(null, mock.port);
  try {
    final deltas = <DeltaEvent>[];
    final allEvents = <String>[];
    final turnEnd = Completer<TurnEndEvent>();
    final sub = client.events.listen((e) {
      allEvents.add('${e.runtimeType}: ${const JsonEncoder().convert(e.raw)}');
      if (e is DeltaEvent && e.kind == 'text_token') {
        deltas.add(e);
      } else if (e is TurnEndEvent && !turnEnd.isCompleted) {
        turnEnd.complete(e);
      } else if (e is ErrorEvent && !turnEnd.isCompleted) {
        turnEnd.completeError(AgentxxException(e.code, e.message));
      }
    }, cancelOnError: false);

    await client.ready.timeout(const Duration(seconds: 60));
    client.sendInput('hello');

    final end = await turnEnd.future.timeout(const Duration(seconds: 60));
    sub.cancel();

    final text = deltas.fold<String>('', (acc, d) => acc + d.text);
    if (!text.contains('hello from dart ffi mock')) {
      stdout.writeln('--- 收到的事件 (${allEvents.length}) ---');
      for (final l in allEvents.take(30)) {
        stdout.writeln(l.length > 300 ? '${l.substring(0, 300)}…' : l);
      }
      stdout.writeln('--- mock 请求数: ${mock.requests.length} ---');
      throw StateError(
          '未收到预期流式文本, 实际收到: "$text"; turnEnd(hasError=${end.hasError}, err=${end.errorMessage})');
    }
  } finally {
    await client.dispose();
  }
}

Future<void> _scenarioB(_MockServer mock) async {
  mock.nextIsToolCall = true;
  mock.requests.clear();

  final client = await _createClient(jsonEncode({'permissionMode': 'all_ask'}), mock.port);
  try {
    final interrupt = Completer<InterruptReqEvent>();
    final turnEnd = Completer<TurnEndEvent>();
    final sub = client.events.listen((e) {
      if (e is InterruptReqEvent && !interrupt.isCompleted) {
        interrupt.complete(e);
      } else if (e is TurnEndEvent && !turnEnd.isCompleted) {
        turnEnd.complete(e);
      } else if (e is ErrorEvent && !turnEnd.isCompleted) {
        turnEnd.completeError(AgentxxException(e.code, e.message));
      }
    }, cancelOnError: false);

    await client.ready.timeout(const Duration(seconds: 60));
    client.sendInput('please read the temp dir');

    // 触发权限询问
    final irq = await interrupt.future.timeout(const Duration(seconds: 60));
    if (!irq.isPermissionAsk) {
      throw StateError('期望权限中断, 实际: name=${irq.interruptName}');
    }

    // 允许并应答
    client.interruptRespond(irq.interruptId, jsonEncode(['true']));

    final end = await turnEnd.future.timeout(const Duration(seconds: 60));
    sub.cancel();
    if (end.hasError) {
      throw StateError('权限放行后轮次出错: ${end.errorMessage}');
    }
  } finally {
    await client.dispose();
  }
}
