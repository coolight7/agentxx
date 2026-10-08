# agentxx_wire

Pure Dart client library for the Agentxx Wire protocol.

Features:
- Pure Dart implementation with no FFI / native dependencies.
- Complete message models and JSON codecs for the Agentxx Wire protocol.
- Connection management: WebSocket transport, handshake, heartbeat (ping/pong), auto-reconnect.
- Reactive stream of incoming events and structured async request APIs.

## Usage

```dart
import 'package:agentxx_wire/agentxx_wire.dart';

void main() async {
  final client = AgentxxWireClient(
    url: 'ws://127.0.0.1:24658',
    sessionId: 'my_session',
  );

  client.events.listen((msg) {
    if (msg is WireDelta) {
      print('Delta: ${msg.text}');
    }
  });

  await client.connect();
  await client.sendInput('Hello, Agentxx!');
}
```
