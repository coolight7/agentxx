import '../lib/agentxx_wire.dart';

void main() async {
  final client = AgentxxWireClient(
    url: 'ws://127.0.0.1:24658',
    sessionId: 'example_session',
  );

  client.events.listen((WireMessage msg) {
    if (msg is WireDelta) {
      print('[Delta] ${msg.text}');
    } else if (msg is WireTurnResult) {
      print('[TurnEnd] hasError=${msg.hasError}, duration=${msg.durationMs}ms');
    } else if (msg is WireHostToolCall) {
      print('[HostToolCall] ${msg.name}(${msg.argsJson})');
      client.respondHostTool(msg.callId, resultJson: '{"status":"ok"}');
    }
  });

  print('Agentxx Wire Client initialized for session: ${client.sessionId}');
}
