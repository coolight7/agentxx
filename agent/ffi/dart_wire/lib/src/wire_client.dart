import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'messages.dart';

/// 纯 Dart 实现的 Agentxx Wire 协议客户端
class AgentxxWireClient {
  final Uri uri;
  final String sessionId;
  final String token;
  final String language;

  WebSocket? _ws;
  Timer? _heartbeatTimer;
  final _eventController = StreamController<WireMessage>.broadcast();
  bool _isConnected = false;

  /// 事件广播流
  Stream<WireMessage> get events => _eventController.stream;

  /// 连接状态
  bool get isConnected => _isConnected;

  AgentxxWireClient({
    required String url,
    this.sessionId = '',
    this.token = '',
    this.language = 'en',
  }) : uri = Uri.parse(url);

  /// 建立连接并完成握手
  Future<WireHelloAck> connect({Duration timeout = const Duration(seconds: 10)}) async {
    await disconnect();

    final ws = await WebSocket.connect(uri.toString()).timeout(timeout);
    _ws = ws;
    _isConnected = true;

    final ackCompleter = Completer<WireHelloAck>();

    ws.listen(
      (data) {
        try {
          final text = data is String ? data : utf8.decode(data as List<int>);
          final map = jsonDecode(text);
          if (map is Map<String, dynamic>) {
            final msg = parseWireMessage(map);
            if (msg != null) {
              if (msg is WireHelloAck && !ackCompleter.isCompleted) {
                ackCompleter.complete(msg);
              }
              _eventController.add(msg);
            }
          }
        } on Object catch (e, st) {
          _eventController.addError(e, st);
        }
      },
      onDone: () {
        _handleDisconnect();
      },
      onError: (Object err, StackTrace? st) {
        _eventController.addError(err, st);
        _handleDisconnect();
      },
      cancelOnError: false,
    );

    // 发送 Hello 握手
    final hello = WireHello(
      sessionId: sessionId,
      token: token,
      language: language,
      protocolVersion: 1,
      capabilities: ['host_tools'],
    );
    send(hello);

    final ack = await ackCompleter.future.timeout(
      timeout,
      onTimeout: () => throw TimeoutException('Waiting for hello_ack timed out'),
    );

    _startHeartbeat();
    return ack;
  }

  /// 断开连接
  Future<void> disconnect() async {
    _heartbeatTimer?.cancel();
    _heartbeatTimer = null;
    _isConnected = false;
    if (_ws != null) {
      await _ws!.close();
      _ws = null;
    }
  }

  void _handleDisconnect() {
    _heartbeatTimer?.cancel();
    _heartbeatTimer = null;
    _isConnected = false;
    _ws = null;
  }

  void _startHeartbeat() {
    _heartbeatTimer?.cancel();
    _heartbeatTimer = Timer.periodic(const Duration(seconds: 15), (_) {
      if (_isConnected && _ws != null) {
        sendRaw({'type': 'ping'});
      }
    });
  }

  /// 发送任意 Wire 消息
  void send(WireMessage msg) {
    sendRaw(msg.toJson());
  }

  /// 发送原始 Map
  void sendRaw(Map<String, dynamic> data) {
    if (_ws != null && _isConnected) {
      _ws!.add(jsonEncode(data));
    } else {
      throw StateError('AgentxxWireClient is not connected');
    }
  }

  /// 发送用户输入
  void sendInput(
    String text, {
    String? model,
    List<WireAttachment> attachments = const [],
    String delivery = '',
    int requestId = 0,
  }) {
    send(WireUserInput(
      sessionId: sessionId,
      text: text,
      model: model ?? '',
      attachments: attachments,
      delivery: delivery,
      requestId: requestId,
    ));
  }

  /// 请求取消当前轮次
  void cancel() {
    send(WireCancel(sessionId: sessionId));
  }

  /// 切换模型
  void selectModel(String model) {
    send(WireSelectModel(sessionId: sessionId, model: model));
  }

  /// 应答中断
  void respondInterrupt(int interruptId, dynamic values) {
    sendRaw({
      'type': 'interrupt_response',
      'id': interruptId,
      'result': values,
    });
  }

  /// 应答宿主工具 (Host Tool)
  void respondHostTool(
    int callId, {
    bool isError = false,
    String resultJson = '',
    String errorMessage = '',
  }) {
    send(WireHostToolResult(
      callId: callId,
      ok: !isError,
      resultJson: resultJson,
      errorMessage: errorMessage,
    ));
  }

  /// 注册宿主工具
  void registerHostTools(List<WireHostToolInfo> tools) {
    send(WireHostToolRegister(sessionId: sessionId, tools: tools));
  }

  /// 注销宿主工具
  void unregisterHostTools(List<String> names) {
    send(WireHostToolUnregister(sessionId: sessionId, names: names));
  }

  /// 关闭并释放资源
  Future<void> dispose() async {
    await disconnect();
    await _eventController.close();
  }
}
