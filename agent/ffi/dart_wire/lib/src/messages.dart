// ---------------------------------------------------------------------------
// Wire Protocol Message Models & Codecs for Agentxx
// ---------------------------------------------------------------------------

abstract class WireMessage {
  String get type;
  Map<String, dynamic> toJson();
}

class WireHello implements WireMessage {
  @override
  String get type => 'hello';

  final String sessionId;
  final String token;
  final int lastSeq;
  final String tailHash;
  final String language;
  final int afterViewSeq;
  final int protocolVersion;
  final List<String> capabilities;

  WireHello({
    this.sessionId = '',
    this.token = '',
    this.lastSeq = 0,
    this.tailHash = '',
    this.language = '',
    this.afterViewSeq = 0,
    this.protocolVersion = 1,
    this.capabilities = const [],
  });

  factory WireHello.fromJson(Map<String, dynamic> j) {
    return WireHello(
      sessionId: j['sessionId'] as String? ?? '',
      token: j['token'] as String? ?? '',
      lastSeq: j['lastSeq'] as int? ?? 0,
      tailHash: j['tailHash'] as String? ?? '',
      language: j['language'] as String? ?? '',
      afterViewSeq: j['afterViewSeq'] as int? ?? 0,
      protocolVersion: j['protocolVersion'] as int? ?? 1,
      capabilities: (j['capabilities'] as List?)?.map((e) => e.toString()).toList() ?? const [],
    );
  }

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'token': token,
    'lastSeq': lastSeq,
    'tailHash': tailHash,
    'language': language,
    'afterViewSeq': afterViewSeq,
    'protocolVersion': protocolVersion,
    'capabilities': capabilities,
  };
}

class WireHelloAck implements WireMessage {
  @override
  String get type => 'hello_ack';

  final bool ok;
  final String sessionId;
  final String tailHash;
  final List<String> models;
  final List<Map<String, dynamic>> plugins;
  final String deviceId;
  final String workDir;
  final String error;
  final int protocolVersion;
  final List<String> capabilities;

  WireHelloAck({
    this.ok = false,
    this.sessionId = '',
    this.tailHash = '',
    this.models = const [],
    this.plugins = const [],
    this.deviceId = '',
    this.workDir = '',
    this.error = '',
    this.protocolVersion = 1,
    this.capabilities = const [],
  });

  factory WireHelloAck.fromJson(Map<String, dynamic> j) {
    return WireHelloAck(
      ok: j['ok'] as bool? ?? false,
      sessionId: j['sessionId'] as String? ?? '',
      tailHash: j['tailHash'] as String? ?? '',
      models: (j['models'] as List?)?.map((e) => e.toString()).toList() ?? const [],
      plugins: (j['plugins'] as List?)?.whereType<Map<String, dynamic>>().toList() ?? const [],
      deviceId: j['deviceId'] as String? ?? '',
      workDir: j['workDir'] as String? ?? '',
      error: j['error'] as String? ?? '',
      protocolVersion: j['protocolVersion'] as int? ?? 1,
      capabilities: (j['capabilities'] as List?)?.map((e) => e.toString()).toList() ?? const [],
    );
  }

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'ok': ok,
    'sessionId': sessionId,
    'tailHash': tailHash,
    'models': models,
    'plugins': plugins,
    'deviceId': deviceId,
    'workDir': workDir,
    'error': error,
    'protocolVersion': protocolVersion,
    'capabilities': capabilities,
  };
}

class WireAttachment {
  final String kind;
  final String name;
  final String mimeType;
  final int size;
  final String dataUrl;
  final String path;

  WireAttachment({
    this.kind = 'image',
    this.name = '',
    this.mimeType = '',
    this.size = 0,
    this.dataUrl = '',
    this.path = '',
  });

  factory WireAttachment.fromJson(Map<String, dynamic> j) => WireAttachment(
    kind: j['kind'] as String? ?? 'image',
    name: j['name'] as String? ?? '',
    mimeType: j['mimeType'] as String? ?? '',
    size: j['size'] as int? ?? 0,
    dataUrl: j['dataUrl'] as String? ?? '',
    path: j['path'] as String? ?? '',
  );

  Map<String, dynamic> toJson() => {
    'kind': kind,
    'name': name,
    'mimeType': mimeType,
    'size': size,
    'dataUrl': dataUrl,
    'path': path,
  };
}

class WireUserInput implements WireMessage {
  @override
  String get type => 'user_input';

  final String sessionId;
  final String text;
  final String model;
  final List<WireAttachment> attachments;
  final String delivery;
  final int requestId;

  WireUserInput({
    this.sessionId = '',
    this.text = '',
    this.model = '',
    this.attachments = const [],
    this.delivery = '',
    this.requestId = 0,
  });

  factory WireUserInput.fromJson(Map<String, dynamic> j) => WireUserInput(
    sessionId: j['sessionId'] as String? ?? '',
    text: j['text'] as String? ?? '',
    model: j['model'] as String? ?? '',
    attachments: (j['attachments'] as List?)
        ?.whereType<Map<String, dynamic>>()
        .map(WireAttachment.fromJson)
        .toList() ?? const [],
    delivery: j['delivery'] as String? ?? '',
    requestId: j['requestId'] as int? ?? 0,
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'text': text,
    if (model.isNotEmpty) 'model': model,
    if (attachments.isNotEmpty) 'attachments': attachments.map((e) => e.toJson()).toList(),
    if (delivery.isNotEmpty) 'delivery': delivery,
    if (requestId > 0) 'requestId': requestId,
  };
}

class WireInputAck implements WireMessage {
  @override
  String get type => 'input_ack';

  final int requestId;
  final String sessionId;
  final String delivery;
  final String status;
  final String reason;
  final String detail;
  final String itemId;

  WireInputAck({
    this.requestId = 0,
    this.sessionId = '',
    this.delivery = '',
    this.status = '',
    this.reason = '',
    this.detail = '',
    this.itemId = '',
  });

  factory WireInputAck.fromJson(Map<String, dynamic> j) => WireInputAck(
    requestId: j['requestId'] as int? ?? 0,
    sessionId: j['sessionId'] as String? ?? '',
    delivery: j['delivery'] as String? ?? '',
    status: j['status'] as String? ?? '',
    reason: j['reason'] as String? ?? '',
    detail: j['detail'] as String? ?? '',
    itemId: j['itemId'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'requestId': requestId,
    'sessionId': sessionId,
    'delivery': delivery,
    'status': status,
    'reason': reason,
    'detail': detail,
    'itemId': itemId,
  };
}

class WireCancel implements WireMessage {
  @override
  String get type => 'cancel';

  final String sessionId;

  WireCancel({this.sessionId = ''});

  factory WireCancel.fromJson(Map<String, dynamic> j) => WireCancel(
    sessionId: j['sessionId'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
  };
}

class WireSelectModel implements WireMessage {
  @override
  String get type => 'select_model';

  final String sessionId;
  final String model;

  WireSelectModel({this.sessionId = '', this.model = ''});

  factory WireSelectModel.fromJson(Map<String, dynamic> j) => WireSelectModel(
    sessionId: j['sessionId'] as String? ?? '',
    model: j['model'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'model': model,
  };
}

class WireDelta implements WireMessage {
  @override
  String get type => 'delta';

  final String deltaType;
  final int seq;
  final String text;
  final String toolName;
  final String toolCallId;
  final String arguments;
  final String result;
  final bool hasError;

  WireDelta({
    this.deltaType = 'text_token',
    this.seq = 0,
    this.text = '',
    this.toolName = '',
    this.toolCallId = '',
    this.arguments = '',
    this.result = '',
    this.hasError = false,
  });

  factory WireDelta.fromJson(Map<String, dynamic> j) => WireDelta(
    deltaType: j['deltaType'] as String? ?? j['type'] as String? ?? 'text_token',
    seq: j['seq'] as int? ?? 0,
    text: j['text'] as String? ?? '',
    toolName: j['tool_name'] as String? ?? j['toolName'] as String? ?? '',
    toolCallId: j['tool_call_id'] as String? ?? j['toolCallId'] as String? ?? '',
    arguments: j['arguments'] as String? ?? '',
    result: j['result'] as String? ?? '',
    hasError: j['hasError'] as bool? ?? false,
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'deltaType': deltaType,
    'seq': seq,
    if (text.isNotEmpty) 'text': text,
    if (toolName.isNotEmpty) 'tool_name': toolName,
    if (toolCallId.isNotEmpty) 'tool_call_id': toolCallId,
    if (arguments.isNotEmpty) 'arguments': arguments,
    if (result.isNotEmpty) 'result': result,
    if (hasError) 'hasError': hasError,
  };
}

class WireSync implements WireMessage {
  @override
  String get type => 'sync';

  final String sessionId;
  final List<dynamic> messages;
  final int deltaSeq;
  final String tailHash;

  WireSync({
    this.sessionId = '',
    this.messages = const [],
    this.deltaSeq = 0,
    this.tailHash = '',
  });

  factory WireSync.fromJson(Map<String, dynamic> j) => WireSync(
    sessionId: j['sessionId'] as String? ?? '',
    messages: j['messages'] as List? ?? const [],
    deltaSeq: j['deltaSeq'] as int? ?? 0,
    tailHash: j['tailHash'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'messages': messages,
    'deltaSeq': deltaSeq,
    'tailHash': tailHash,
  };
}

class WireTurnResult implements WireMessage {
  @override
  String get type => 'turn_result';

  final String sessionId;
  final bool hasError;
  final String errorMessage;
  final bool interrupted;
  final int startTimeMs;
  final int durationMs;

  WireTurnResult({
    this.sessionId = '',
    this.hasError = false,
    this.errorMessage = '',
    this.interrupted = false,
    this.startTimeMs = 0,
    this.durationMs = 0,
  });

  factory WireTurnResult.fromJson(Map<String, dynamic> j) => WireTurnResult(
    sessionId: j['sessionId'] as String? ?? '',
    hasError: j['hasError'] as bool? ?? false,
    errorMessage: j['errorMessage'] as String? ?? '',
    interrupted: j['interrupted'] as bool? ?? false,
    startTimeMs: j['startTimeMs'] as int? ?? 0,
    durationMs: j['durationMs'] as int? ?? 0,
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'hasError': hasError,
    'errorMessage': errorMessage,
    'interrupted': interrupted,
    'startTimeMs': startTimeMs,
    'durationMs': durationMs,
  };
}

class WireContextStats implements WireMessage {
  @override
  String get type => 'context_stats';

  final int contextTokens;
  final int maxContextTokens;
  final double tps;

  WireContextStats({
    this.contextTokens = 0,
    this.maxContextTokens = 0,
    this.tps = 0.0,
  });

  factory WireContextStats.fromJson(Map<String, dynamic> j) => WireContextStats(
    contextTokens: j['contextTokens'] as int? ?? 0,
    maxContextTokens: j['maxContextTokens'] as int? ?? 0,
    tps: (j['tps'] as num?)?.toDouble() ?? 0.0,
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'contextTokens': contextTokens,
    'maxContextTokens': maxContextTokens,
    'tps': tps,
  };
}

class WireError implements WireMessage {
  @override
  String get type => 'error';

  final int code;
  final String message;

  WireError({this.code = 0, this.message = ''});

  factory WireError.fromJson(Map<String, dynamic> j) => WireError(
    code: j['code'] as int? ?? 0,
    message: j['message'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'code': code,
    'message': message,
  };
}

class WireModelInfo implements WireMessage {
  @override
  String get type => 'model_info';

  final String sessionId;
  final String currentModel;
  final List<String> models;
  final List<dynamic> capabilities;

  WireModelInfo({
    this.sessionId = '',
    this.currentModel = '',
    this.models = const [],
    this.capabilities = const [],
  });

  factory WireModelInfo.fromJson(Map<String, dynamic> j) => WireModelInfo(
    sessionId: j['sessionId'] as String? ?? '',
    currentModel: j['currentModel'] as String? ?? '',
    models: (j['models'] as List?)?.map((e) => e.toString()).toList() ?? const [],
    capabilities: j['capabilities'] as List? ?? const [],
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'currentModel': currentModel,
    'models': models,
    'capabilities': capabilities,
  };
}

class WireAddModel implements WireMessage {
  @override
  String get type => 'add_model';

  final String sessionId;
  final String name;
  final String modelType;
  final String baseUrl;
  final String apiKey;
  final String modelName;
  final Map<String, dynamic> extraConfig;

  WireAddModel({
    this.sessionId = '',
    this.name = '',
    this.modelType = 'openai',
    this.baseUrl = '',
    this.apiKey = '',
    this.modelName = '',
    this.extraConfig = const {},
  });

  factory WireAddModel.fromJson(Map<String, dynamic> j) => WireAddModel(
    sessionId: j['sessionId'] as String? ?? '',
    name: j['name'] as String? ?? '',
    modelType: j['modelType'] as String? ?? 'openai',
    baseUrl: j['baseUrl'] as String? ?? '',
    apiKey: j['apiKey'] as String? ?? '',
    modelName: j['modelName'] as String? ?? '',
    extraConfig: j['extraConfig'] as Map<String, dynamic>? ?? const {},
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'name': name,
    'modelType': modelType,
    'baseUrl': baseUrl,
    'apiKey': apiKey,
    'modelName': modelName,
    if (extraConfig.isNotEmpty) 'extraConfig': extraConfig,
  };
}

class WireAddModelResult implements WireMessage {
  @override
  String get type => 'add_model_result';

  final bool ok;
  final String name;
  final String error;

  WireAddModelResult({this.ok = false, this.name = '', this.error = ''});

  factory WireAddModelResult.fromJson(Map<String, dynamic> j) => WireAddModelResult(
    ok: j['ok'] as bool? ?? false,
    name: j['name'] as String? ?? '',
    error: j['error'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'ok': ok,
    'name': name,
    'error': error,
  };
}

class WireRemoveModel implements WireMessage {
  @override
  String get type => 'remove_model';

  final String sessionId;
  final String name;

  WireRemoveModel({this.sessionId = '', this.name = ''});

  factory WireRemoveModel.fromJson(Map<String, dynamic> j) => WireRemoveModel(
    sessionId: j['sessionId'] as String? ?? '',
    name: j['name'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'name': name,
  };
}

class WireRemoveModelResult implements WireMessage {
  @override
  String get type => 'remove_model_result';

  final bool ok;
  final String name;
  final String error;

  WireRemoveModelResult({this.ok = false, this.name = '', this.error = ''});

  factory WireRemoveModelResult.fromJson(Map<String, dynamic> j) => WireRemoveModelResult(
    ok: j['ok'] as bool? ?? false,
    name: j['name'] as String? ?? '',
    error: j['error'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'ok': ok,
    'name': name,
    'error': error,
  };
}

class WireHostToolInfo {
  final String name;
  final String description;
  final Map<String, dynamic> inputSchema;
  final int timeoutSec;
  final int maxConcurrent;

  WireHostToolInfo({
    this.name = '',
    this.description = '',
    this.inputSchema = const {},
    this.timeoutSec = 0,
    this.maxConcurrent = 0,
  });

  factory WireHostToolInfo.fromJson(Map<String, dynamic> j) => WireHostToolInfo(
    name: j['name'] as String? ?? '',
    description: j['description'] as String? ?? '',
    inputSchema: j['inputSchema'] as Map<String, dynamic>? ?? const {},
    timeoutSec: j['timeoutSec'] as int? ?? 0,
    maxConcurrent: j['maxConcurrent'] as int? ?? 0,
  );

  Map<String, dynamic> toJson() => {
    'name': name,
    'description': description,
    'inputSchema': inputSchema,
    'timeoutSec': timeoutSec,
    'maxConcurrent': maxConcurrent,
  };
}

class WireHostToolRegister implements WireMessage {
  @override
  String get type => 'host_tool_register';

  final String sessionId;
  final List<WireHostToolInfo> tools;

  WireHostToolRegister({this.sessionId = '', this.tools = const []});

  factory WireHostToolRegister.fromJson(Map<String, dynamic> j) => WireHostToolRegister(
    sessionId: j['sessionId'] as String? ?? '',
    tools: (j['tools'] as List?)
        ?.whereType<Map<String, dynamic>>()
        .map(WireHostToolInfo.fromJson)
        .toList() ?? const [],
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'tools': tools.map((e) => e.toJson()).toList(),
  };
}

class WireHostToolUnregister implements WireMessage {
  @override
  String get type => 'host_tool_unregister';

  final String sessionId;
  final List<String> names;

  WireHostToolUnregister({this.sessionId = '', this.names = const []});

  factory WireHostToolUnregister.fromJson(Map<String, dynamic> j) => WireHostToolUnregister(
    sessionId: j['sessionId'] as String? ?? '',
    names: (j['names'] as List?)?.map((e) => e.toString()).toList() ?? const [],
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'sessionId': sessionId,
    'names': names,
  };
}

class WireHostToolCall implements WireMessage {
  @override
  String get type => 'host_tool_call';

  final int callId;
  final String sessionId;
  final String name;
  final String argsJson;
  final int timeoutSec;

  WireHostToolCall({
    this.callId = 0,
    this.sessionId = '',
    this.name = '',
    this.argsJson = '',
    this.timeoutSec = 0,
  });

  factory WireHostToolCall.fromJson(Map<String, dynamic> j) => WireHostToolCall(
    callId: j['callId'] as int? ?? 0,
    sessionId: j['sessionId'] as String? ?? '',
    name: j['name'] as String? ?? '',
    argsJson: j['argsJson'] as String? ?? '',
    timeoutSec: j['timeoutSec'] as int? ?? 0,
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'callId': callId,
    'sessionId': sessionId,
    'name': name,
    'argsJson': argsJson,
    'timeoutSec': timeoutSec,
  };
}

class WireHostToolResult implements WireMessage {
  @override
  String get type => 'host_tool_result';

  final int callId;
  final bool ok;
  final String resultJson;
  final String errorMessage;

  WireHostToolResult({
    this.callId = 0,
    this.ok = false,
    this.resultJson = '',
    this.errorMessage = '',
  });

  factory WireHostToolResult.fromJson(Map<String, dynamic> j) => WireHostToolResult(
    callId: j['callId'] as int? ?? 0,
    ok: j['ok'] as bool? ?? false,
    resultJson: j['resultJson'] as String? ?? '',
    errorMessage: j['errorMessage'] as String? ?? '',
  );

  @override
  Map<String, dynamic> toJson() => {
    'type': type,
    'callId': callId,
    'ok': ok,
    'resultJson': resultJson,
    'errorMessage': errorMessage,
  };
}

class WireGenericMessage implements WireMessage {
  @override
  final String type;
  final Map<String, dynamic> data;

  WireGenericMessage({required this.type, required this.data});

  @override
  Map<String, dynamic> toJson() => data;
}

WireMessage? parseWireMessage(Map<String, dynamic> j) {
  final type = j['type'] as String?;
  if (type == null) return null;

  switch (type) {
    case 'hello':
      return WireHello.fromJson(j);
    case 'hello_ack':
      return WireHelloAck.fromJson(j);
    case 'user_input':
      return WireUserInput.fromJson(j);
    case 'input_ack':
      return WireInputAck.fromJson(j);
    case 'cancel':
      return WireCancel.fromJson(j);
    case 'select_model':
      return WireSelectModel.fromJson(j);
    case 'delta':
      return WireDelta.fromJson(j);
    case 'sync':
      return WireSync.fromJson(j);
    case 'turn_result':
      return WireTurnResult.fromJson(j);
    case 'context_stats':
      return WireContextStats.fromJson(j);
    case 'error':
      return WireError.fromJson(j);
    case 'model_info':
      return WireModelInfo.fromJson(j);
    case 'add_model':
      return WireAddModel.fromJson(j);
    case 'add_model_result':
      return WireAddModelResult.fromJson(j);
    case 'remove_model':
      return WireRemoveModel.fromJson(j);
    case 'remove_model_result':
      return WireRemoveModelResult.fromJson(j);
    case 'host_tool_register':
      return WireHostToolRegister.fromJson(j);
    case 'host_tool_unregister':
      return WireHostToolUnregister.fromJson(j);
    case 'host_tool_call':
      return WireHostToolCall.fromJson(j);
    case 'host_tool_result':
      return WireHostToolResult.fromJson(j);
    default:
      return WireGenericMessage(type: type, data: j);
  }
}
