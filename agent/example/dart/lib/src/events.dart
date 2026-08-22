/// agent 事件模型 —— FFI 事件 payload (wire 协议 JSON) 的 Dart 侧解析。
///
/// 事件种类对应 ffi_api.h 的 AgentxxEventType; 各 payload 字段与服务端
/// wire 协议一致 (agent/lib/include/agentxx/agent/io/wire_protocol.h):
/// - delta: {"kind","seq","text","msgId","tool_name","tool_call_id",
///           "arguments","result","hasError","tipType",...}
/// - turn_result: {"sessionId","hasError","errorMessage"(可选),"interrupted"}
/// - interrupt_request: {"interruptId","sessionId","node","value",
///                       "argJson": "<内嵌 JSON 字符串>"}
library;

import 'dart:convert';

import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart' as bind;

/// 解析后的 agent 事件基类
sealed class AgentEvent {
  AgentEvent(this.raw);

  /// 原始 payload JSON 文本 (排障/透传用)
  final Map<String, dynamic> raw;
}

/// EVT_READY: 服务端就绪
class ReadyEvent extends AgentEvent {
  ReadyEvent(super.raw);

  String get sessionId => raw['sessionId'] as String? ?? '';
}

/// EVT_SYNC: 全量/部分历史同步 (会话恢复)
class SyncEvent extends AgentEvent {
  SyncEvent(super.raw);
}

/// EVT_DELTA: 流式增量事件 (文本/思考/工具/节点/系统提示等)
class DeltaEvent extends AgentEvent {
  DeltaEvent(super.raw);

  /// kind: text_token|thinking_token|tool_start|tool_end|turn_start|
  ///       turn_end|node_start|node_end|message_tip|system_message
  String get kind => raw['kind'] as String? ?? 'text_token';
  int get seq => (raw['seq'] as num?)?.toInt() ?? 0;
  String get text => raw['text'] as String? ?? '';
  String get toolName => raw['tool_name'] as String? ?? '';
  String get toolCallId => raw['tool_call_id'] as String? ?? '';
  String get arguments => raw['arguments'] as String? ?? '';
  String get result => raw['result'] as String? ?? '';
  bool get hasError => raw['hasError'] as bool? ?? false;
  String get tipType => raw['tipType'] as String? ?? 'info';

  /// 轮次统计字段 (kind == turn_end 时使用)
  double get tps => (raw['tps'] as num?)?.toDouble() ?? 0;
}

/// EVT_TURN_END: 一轮对话结束
class TurnEndEvent extends AgentEvent {
  TurnEndEvent(super.raw);

  bool get hasError => raw['hasError'] as bool? ?? false;
  String get errorMessage => raw['errorMessage'] as String? ?? '';
  bool get interrupted => raw['interrupted'] as bool? ?? false;
  int get durationMs => (raw['durationMs'] as num?)?.toInt() ?? 0;
}

/// EVT_CONTEXT_STATS: 上下文统计
class ContextStatsEvent extends AgentEvent {
  ContextStatsEvent(super.raw);

  int get contextTokens => (raw['contextTokens'] as num?)?.toInt() ?? 0;
  int get maxContextTokens => (raw['maxContextTokens'] as num?)?.toInt() ?? 0;
  double get tps => (raw['tps'] as num?)?.toDouble() ?? 0;
}

/// EVT_MODEL_INFO: 当前模型信息 / 可用模型列表
class ModelInfoEvent extends AgentEvent {
  ModelInfoEvent(super.raw);

  String get currentModel => raw['currentModel'] as String? ?? '';
  List<dynamic> get models => raw['models'] as List<dynamic>? ?? const [];
}

/// EVT_COMPONENTS: 启动组件信息 (MCP/Skill/Memory/插件)
class ComponentsEvent extends AgentEvent {
  ComponentsEvent(super.raw);
}

/// EVT_PLUGIN_DATA: 插件事件转发
class PluginDataEvent extends AgentEvent {
  PluginDataEvent(super.raw);
}

/// HIL 中断输入项 (InterruptHandleArg::InterruptHandleInputItem 对应)
class InterruptInputItem {
  InterruptInputItem._(this.label, this.depict, this.type, this.defaultValue, this.enumValues);

  factory InterruptInputItem.fromJson(Map<String, dynamic> j) {
    return InterruptInputItem._(
      j['label'] as String? ?? '',
      j['depict'] as String? ?? '',
      j['type'] as String? ?? '',
      j['defaultValue'] as String? ?? '',
      (j['enumValues'] as List<dynamic>?)?.cast<String>() ?? const [],
    );
  }

  final String label;
  final String depict;

  /// '' | bool | int | double | string | enum
  final String type;
  final String defaultValue;
  final List<String> enumValues;
}

/// EVT_INTERRUPT_REQ: HIL 中断询问 (权限确认 / 输入收集)
class InterruptReqEvent extends AgentEvent {
  InterruptReqEvent(super.raw) {
    // argJson 为内嵌 JSON 字符串 (InterruptHandleArg): {name,arg,inputs,resultId}
    var arg = raw['argJson'];
    if (arg is String && arg.isNotEmpty) {
      try {
        arg = jsonDecode(arg);
      } catch (_) {
        arg = null;
      }
    }
    if (arg is Map<String, dynamic>) {
      interruptName = arg['name'] as String? ?? '';
      argData = arg['arg'];
      inputs = (arg['inputs'] as List<dynamic>? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map(InterruptInputItem.fromJson)
          .toList(growable: false);
    }
  }

  int get interruptId => (raw['interruptId'] as num?)?.toInt() ?? 0;
  String get node => raw['node'] as String? ?? '';

  /// 触发中断的原始值 (权限中断时为工具调用参数 JSON 文本)
  String get value => raw['value'] as String? ?? '';

  /// 中断名: "permission" | "subagent" | 自定义输入收集 ...
  String interruptName = '';
  dynamic argData;
  List<InterruptInputItem> inputs = const [];

  /// 权限中断上下文: argJson.arg = {"category": "filesystem_read|...", "target": "path"}
  String get permissionCategory {
    if (argData is Map<String, dynamic>) {
      return argData['category'] as String? ?? '';
    }
    return '';
  }

  String get permissionTarget {
    if (argData is Map<String, dynamic>) {
      return argData['target'] as String? ?? '';
    }
    return '';
  }

  /// 是否为文件读写权限询问
  bool get isPermissionAsk => interruptName == 'permission';
}

/// EVT_INTERRUPT_EXPIRED: 中断已过期/取消 (未在超时内应答)
class InterruptExpiredEvent extends AgentEvent {
  InterruptExpiredEvent(super.raw);

  int get interruptId => (raw['interruptId'] as num?)?.toInt() ?? 0;
}

/// EVT_ERROR: 内部错误
class ErrorEvent extends AgentEvent {
  ErrorEvent(super.raw);

  int get code => (raw['code'] as num?)?.toInt() ?? 0;
  String get message => raw['message'] as String? ?? '';
}

/// 按 FFI 事件类型值解析 payload 为具体事件; 未知类型返回 null (调用方忽略)
AgentEvent? parseAgentEvent(int typeValue, String payloadJson) {
  Map<String, dynamic> json;
  try {
    final decoded = jsonDecode(payloadJson.isEmpty || payloadJson.trim().isEmpty ? '{}' : payloadJson);
    if (decoded is! Map<String, dynamic>) {
      return null;
    }
    json = decoded;
  } on FormatException {
    return null;
  }

  final bind.AgentxxEventType type;
  try {
    type = bind.AgentxxEventType.fromValue(typeValue);
  } on ArgumentError {
    return null; // 未知事件种类 (新版本动态库新增): 忽略
  }

  return switch (type) {
    bind.AgentxxEventType.AGENTXX_EVT_READY => ReadyEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_SYNC => SyncEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_DELTA => DeltaEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_TURN_END => TurnEndEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_CONTEXT_STATS => ContextStatsEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_MODEL_INFO => ModelInfoEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_COMPONENTS => ComponentsEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_INTERRUPT_REQ => InterruptReqEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_INTERRUPT_EXPIRED => InterruptExpiredEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_PLUGIN_DATA => PluginDataEvent(json),
    bind.AgentxxEventType.AGENTXX_EVT_ERROR => ErrorEvent(json),
  };
}
