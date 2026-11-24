/// agent 事件模型 —— FFI 事件 payload (JSON) 的 Dart 侧解析。
///
/// 事件种类对应 ffi_api.h 的 AgentxxFFIEventType; 各 payload 字段与服务端
/// wire 协议一致 (agent/lib/include/agentxx/agent/io/wire_protocol.h):
/// - result: {reqId, op, ok, result?|error:{code,name,message}}
/// - delta: {"kind","seq","text","msgId","tool_name","tool_call_id",
///           "arguments","result","hasError","tipType",...}
/// - delta_text_batch: {"msgId","kind","text"}  (合批窗口内的文本增量)
/// - turn_result: {"sessionId","hasError","errorMessage"(可选),"interrupted"}
/// - interrupt_request: {"interruptId","sessionId","node","value",
///                       "argJson": "<内嵌 JSON 字符串>"}
library;

import 'dart:convert';

import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart' as bind;

/// 解析后的 agent 事件基类
sealed class AgentEvent {
  AgentEvent(this.raw);

  /// 原始 payload JSON 文本对应的对象 (排障/透传用)
  final Map<String, dynamic> raw;
}

/// EVT_READY: 服务端就绪 (此后提交的调用按当前会话处理)
class ReadyEvent extends AgentEvent {
  ReadyEvent(super.raw);

  String get sessionId => raw['sessionId'] as String? ?? '';
}

/// EVT_STOPPED: 收尾完成 —— **最后一个**事件, 之后不会再有回调
class StoppedEvent extends AgentEvent {
  StoppedEvent(super.raw);
}

/// 调用失败的结构化原因
class CallError {
  CallError(this.code, this.name, this.message);

  factory CallError.fromJson(Map<String, dynamic> j) => CallError(
        (j['code'] as num?)?.toInt() ?? 0,
        j['name'] as String? ?? 'rejected',
        j['message'] as String? ?? '',
      );

  final int code;

  /// 稳定字符串 (SDK 按它判定, 不受错误码调整影响)
  final String name;
  final String message;

  @override
  String toString() => 'CallError($code $name${message.isEmpty ? '' : ': $message'})';
}

/// EVT_RESULT: 一次 `call` 的回执 (每个受理的 reqId 恰好一条)
class ResultEvent extends AgentEvent {
  ResultEvent(super.raw);

  int get reqId => (raw['reqId'] as num?)?.toInt() ?? 0;
  String get op => raw['op'] as String? ?? '';
  bool get ok => raw['ok'] as bool? ?? false;

  /// 成功时的结果对象 (无结果的 op 为空对象)
  Map<String, dynamic> get result =>
      (raw['result'] as Map<String, dynamic>?) ?? const <String, dynamic>{};

  /// 失败原因 (ok == true 时为 null)
  CallError? get error {
    final e = raw['error'];
    if (e is Map<String, dynamic>) {
      return CallError.fromJson(e);
    }
    return null;
  }
}

/// EVT_SYNC: 全量/尾窗历史同步 (会话恢复)
class SyncEvent extends AgentEvent {
  SyncEvent(super.raw);

  /// 尾窗同步的起始下标 (0 = 完整同步)
  int get fromIndex => (raw['fromIndex'] as num?)?.toInt() ?? 0;
  int get totalMessages => (raw['totalMessages'] as num?)?.toInt() ?? 0;
}

/// EVT_DELTA: 流式增量事件 (文本/思考/工具/节点/系统提示等)
class DeltaEvent extends AgentEvent {
  DeltaEvent(super.raw);

  /// kind: text_token|thinking_token|tool_start|tool_end|turn_start|
  ///       turn_end|node_start|node_end|message_tip|system_message
  String get kind => raw['kind'] as String? ?? 'text_token';
  int get seq => (raw['seq'] as num?)?.toInt() ?? 0;
  String get text => raw['text'] as String? ?? '';
  String get msgId => raw['msgId'] as String? ?? '';
  String get toolName => raw['tool_name'] as String? ?? '';
  String get toolCallId => raw['tool_call_id'] as String? ?? '';
  String get arguments => raw['arguments'] as String? ?? '';
  String get result => raw['result'] as String? ?? '';
  bool get hasError => raw['hasError'] as bool? ?? false;
  String get tipType => raw['tipType'] as String? ?? 'info';

  /// 轮次统计字段 (kind == turn_end 时使用)
  double get tps => (raw['tps'] as num?)?.toDouble() ?? 0;
}

/// EVT_DELTA_TEXT_BATCH: 合批窗口内的文本/思考增量 `{msgId, kind, text}`
class DeltaTextBatchEvent extends AgentEvent {
  DeltaTextBatchEvent(super.raw);

  String get msgId => raw['msgId'] as String? ?? '';
  String get kind => raw['kind'] as String? ?? 'text_token';
  String get text => raw['text'] as String? ?? '';
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

  /// 本会话累计用量 (新主程序才下发; 老主程序缺字段时为 0)
  int get promptTokens => (raw['promptTokens'] as num?)?.toInt() ?? 0;
  int get completionTokens => (raw['completionTokens'] as num?)?.toInt() ?? 0;
  int get totalTokens => (raw['totalTokens'] as num?)?.toInt() ?? 0;
  int get cachedPromptTokens =>
      (raw['cachedPromptTokens'] as num?)?.toInt() ?? 0;
}

/// 单个模型的多模态输入能力 (capabilities[] 单项)
///
/// 来自 agent 侧模型配置 (model_json 的 imageInput/audioInput/videoInput 或
/// yaml 的 image_input/audio_input/video_input); 宿主据此判断是否展示
/// 图片/音频/视频输入入口 —— 与 TUI 的附件按钮判定同源。
class ModelCapability {
  ModelCapability._(this.name, this.imageInput, this.audioInput, this.videoInput);

  factory ModelCapability.fromJson(Map<String, dynamic> j) => ModelCapability._(
        j['name'] as String? ?? '',
        j['image_input'] as bool? ?? false,
        j['audio_input'] as bool? ?? false,
        j['video_input'] as bool? ?? false,
      );

  final String name;
  final bool imageInput;
  final bool audioInput;
  final bool videoInput;

  /// 是否支持任一多模态输入
  bool get hasMultimodalInput => imageInput || audioInput || videoInput;
}

/// EVT_MODEL_INFO: 当前模型信息 / 可用模型列表 / 各模型多模态能力
///
/// 既可能是 `model.list` 的回执, 也可能是切换会话/增删模型后的广播
/// (广播对应的 payload 与回执一致; 回执走 [ResultEvent])
class ModelInfoEvent extends AgentEvent {
  ModelInfoEvent(super.raw);

  String get currentModel => raw['currentModel'] as String? ?? '';
  List<dynamic> get models => raw['models'] as List<dynamic>? ?? const [];

  /// 各模型的多模态输入能力 (旧版 agent 可能不带该字段, 此时为空列表)
  List<ModelCapability> get capabilities =>
      (raw['capabilities'] as List<dynamic>? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map(ModelCapability.fromJson)
          .toList(growable: false);

  /// 当前模型的多模态输入能力 (列表中无匹配项时按不支持处理)
  ModelCapability get currentCapability => capabilities.firstWhere(
        (c) => c.name == currentModel,
        orElse: () => ModelCapability._(currentModel, false, false, false),
      );
}

/// EVT_COMPONENTS: 启动组件信息 (MCP/Skill/Memory/插件)
class ComponentsEvent extends AgentEvent {
  ComponentsEvent(super.raw);
}

/// EVT_MESSAGE_QUEUE: 消息队列变化 `{sessionId, items[], state?}`
class MessageQueueEvent extends AgentEvent {
  MessageQueueEvent(super.raw);

  String get sessionId => raw['sessionId'] as String? ?? '';
  String get state => raw['state'] as String? ?? '';
  List<dynamic> get items => raw['items'] as List<dynamic>? ?? const [];
}

/// EVT_PERMISSION_STATE: 权限状态广播 `{fullAuth}`
class PermissionStateEvent extends AgentEvent {
  PermissionStateEvent(super.raw);

  bool get fullAuth => raw['fullAuth'] as bool? ?? false;
}

/// EVT_PLUGIN_DATA: 插件事件转发
class PluginDataEvent extends AgentEvent {
  PluginDataEvent(super.raw);
}

/// 中断描述中的候选项 (control: buttons / select)
class InterruptOption {
  InterruptOption._(this.value, this.label);

  factory InterruptOption.fromJson(Map<String, dynamic> j) {
    final v = j['value'];
    return InterruptOption._(
      v ?? '',
      j['label'] as String? ?? (v is String ? v : '${v ?? ''}'),
    );
  }

  /// 候选项原始值 (字符串/数值/布尔; 应答原样回传)
  final dynamic value;
  final String label;
}

/// 中断描述中的控件块 (kind == "control")
///
/// 形态即语义 (协议内没有"参数类型"): buttons / select / checkbox / text / number
class InterruptControl {
  InterruptControl._({
    required this.id,
    required this.control,
    required this.label,
    required this.help,
    required this.options,
    required this.defaultValue,
    required this.integer,
    required this.min,
    required this.max,
  });

  factory InterruptControl.fromJson(Map<String, dynamic> j) {
    return InterruptControl._(
      id: j['id'] as String? ?? 'value',
      control: j['control'] as String? ?? 'text',
      label: j['label'] as String? ?? '',
      help: j['help'] as String? ?? '',
      options: (j['options'] as List<dynamic>? ?? const [])
          .whereType<Map<String, dynamic>>()
          .map(InterruptOption.fromJson)
          .toList(growable: false),
      defaultValue: j['defaultValue'],
      integer: j['integer'] as bool? ?? false,
      min: (j['min'] as num?)?.toDouble(),
      max: (j['max'] as num?)?.toDouble(),
    );
  }

  /// 结果键 (应答 JSON 的 values 键)
  final String id;
  final String control;
  final String label;
  final String help;
  final List<InterruptOption> options;
  final dynamic defaultValue;
  final bool integer;
  final double? min;
  final double? max;

  /// 结果值的 JSON 形态 (checkbox=布尔 / number=数值 / 其余=字符串/原始值)
  bool get isBoolean => control == 'checkbox' || control == 'switch';
  bool get isNumber => control == 'number';
  bool get isChoice => control == 'buttons' || control == 'select';
}

/// EVT_INTERRUPT_REQ: HIL 中断询问
class InterruptReqEvent extends AgentEvent {
  InterruptReqEvent(super.raw) {
    // argJson 为内嵌 JSON 字符串 (InterruptHandleArg): {name,arg,resultId,ui}
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
      final ui = arg['ui'];
      if (ui is Map<String, dynamic>) {
        uiBlocks = (ui['blocks'] as List<dynamic>? ?? const [])
            .whereType<Map<String, dynamic>>()
            .toList(growable: false);
      }
    }
  }

  int get interruptId => (raw['interruptId'] as num?)?.toInt() ?? 0;
  String get node => raw['node'] as String? ?? '';

  /// 触发中断的原始值 (权限中断时为工具调用参数 JSON 文本)
  String get value => raw['value'] as String? ?? '';

  /// 中断名: "permission" | "subagent" | 自定义输入收集 ...
  String interruptName = '';
  dynamic argData;

  /// 描述块 (原始 JSON; 内容块用于展示, 控件块解析为 [controls])
  List<Map<String, dynamic>> uiBlocks = const [];

  /// 控件块 (按描述顺序; 应答 values 的键为控件 id)
  List<InterruptControl> get controls => uiBlocks
      .where((b) => b['kind'] == 'control')
      .map(InterruptControl.fromJson)
      .toList(growable: false);

  /// 内容块纯文本 (text/markdown 原文; 供控制台宿主展示)
  List<String> get contentLines => uiBlocks
      .where((b) => b['kind'] == 'text' || b['kind'] == 'markdown')
      .map((b) => b['text'] as String? ?? '')
      .where((s) => s.isNotEmpty)
      .toList(growable: false);

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

/// EVT_HOST_TOOL_CALL: 嵌入方程序工具调用请求
class HostToolCallEvent extends AgentEvent {
  HostToolCallEvent(super.raw);

  int get callId => (raw['callId'] as num?)?.toInt() ?? 0;
  String get name => raw['name'] as String? ?? '';
  String get argsJson => raw['argsJson'] as String? ?? '';
  int get timeoutSec => (raw['timeoutSec'] as num?)?.toInt() ?? 0;
}

/// EVT_HOST_TOOL_CANCELED: 嵌入方程序工具调用取消 `{callId, reason}`
class HostToolCanceledEvent extends AgentEvent {
  HostToolCanceledEvent(super.raw);

  int get callId => (raw['callId'] as num?)?.toInt() ?? 0;
  String get reason => raw['reason'] as String? ?? '';
}

/// EVT_ERROR: 内部错误
class ErrorEvent extends AgentEvent {
  ErrorEvent(super.raw);

  int get code => (raw['code'] as num?)?.toInt() ?? 0;
  String get name => raw['name'] as String? ?? '';
  String get message => raw['message'] as String? ?? '';
}

/// 按 FFI 事件类型值解析 payload 为具体事件; 未知类型返回 null (调用方忽略)
AgentEvent? parseAgentEvent(int typeValue, String payloadJson) {
  Map<String, dynamic> json;
  try {
    final decoded = jsonDecode(
        payloadJson.isEmpty || payloadJson.trim().isEmpty ? '{}' : payloadJson);
    if (decoded is! Map<String, dynamic>) {
      return null;
    }
    json = decoded;
  } on FormatException {
    return null;
  }

  final bind.AgentxxFFIEventType type;
  try {
    type = bind.AgentxxFFIEventType.fromValue(typeValue);
  } on ArgumentError {
    return null; // 未知事件种类 (新版本动态库新增): 忽略
  }

  return switch (type) {
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_READY => ReadyEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_STOPPED => StoppedEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_RESULT => ResultEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_ERROR => ErrorEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_SYNC => SyncEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_DELTA => DeltaEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_DELTA_TEXT_BATCH =>
      DeltaTextBatchEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_TURN_END => TurnEndEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_CONTEXT_STATS =>
      ContextStatsEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_MODEL_INFO => ModelInfoEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_COMPONENTS => ComponentsEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_MESSAGE_QUEUE =>
      MessageQueueEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_PERMISSION_STATE =>
      PermissionStateEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_INTERRUPT_REQ => InterruptReqEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_INTERRUPT_EXPIRED =>
      InterruptExpiredEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_HOST_TOOL_CALL =>
      HostToolCallEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_HOST_TOOL_CANCELED =>
      HostToolCanceledEvent(json),
    bind.AgentxxFFIEventType.AGENTXX_FFI_EVT_PLUGIN_DATA => PluginDataEvent(json),
  };
}
