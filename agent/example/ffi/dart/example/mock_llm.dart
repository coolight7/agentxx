/// 独立 mock OpenAI 兼容流式服务器 —— 供端到端测试 CLI 子进程使用。
///
/// 用法: dart run example/mock_llm.dart [--port <固定端口>]
/// 启动后把实际监听端口打印到 stdout (行: MOCK_PORT=<port>) 并常驻。
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

Future<void> main(List<String> args) async {
  var port = 0;
  for (var i = 0; i < args.length - 1; i++) {
    if (args[i] == '--port') {
      port = int.tryParse(args[i + 1]) ?? 0;
    }
  }

  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, port);
  stdout.writeln('MOCK_PORT=${server.port}');
  await stdout.flush();

  var n = 0;
  await for (final req in server) {
    try {
      await utf8.decoder.bind(req).join(); // 丢弃请求体
      n++;
      req.response.headers
        ..contentType = ContentType('text', 'event-stream', charset: 'utf-8')
        ..set('cache-control', 'no-cache');

      Map<String, dynamic> chunk(String content) => {
            'id': 'chatcmpl-mock',
            'object': 'chat.completion.chunk',
            'model': 'mock-model',
            'choices': [
              {
                'index': 0,
                'delta': content.isEmpty
                    ? <String, dynamic>{'role': 'assistant'}
                    : {'content': content},
                'finish_reason': null,
              }
            ],
          };

      final done = {
        'id': 'chatcmpl-mock',
        'object': 'chat.completion.chunk',
        'model': 'mock-model',
        'choices': [
          {'index': 0, 'delta': <String, dynamic>{}, 'finish_reason': 'stop'}
        ],
      };
      req.response.write([
        chunk(''),
        chunk('你好, 我是来自 Dart FFI CLI 的 mock 回复 (#$n)。'),
        done,
      ].map((c) => 'data: ${jsonEncode(c)}\n\n').join() + 'data: [DONE]\n\n');
      await req.response.close();
    } catch (_) {
      try {
        await req.response.close();
      } catch (_) {}
    }
  }
}
