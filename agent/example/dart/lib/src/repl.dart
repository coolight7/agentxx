/// CLI 交互层 —— agent 事件的终端渲染 + 用户输入循环 + HIL 中断应答。
///
/// 输入模型 (与 stdio client 类似的单行协议):
/// - 有挂起的 HIL 中断询问时, 输入行优先作为当前询问的应答
/// - 其余输入行以 '/' 开头解析为本地命令, 否则作为用户输入发送给 agent
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'console_setup.dart' show Ansi;
import 'agent_client.dart';
import 'events.dart';

/// 单个 HIL 中断的应答流程 (多输入项按顺序逐项收集)
class _InterruptFlow {
  _InterruptFlow(this.event);

  final InterruptReqEvent event;
  final List<String> answers = <String>[];

  /// 权限询问选择 "always" 时置位 (应答后额外注册路径规则)
  bool rememberPermission = false;

  bool get done => answers.length >= event.inputs.length;

  InterruptInputItem get currentItem => event.inputs[answers.length];
}

/// CLI 渲染器: 把 agent 事件渲染为终端输出
class CliRenderer {
  /// thinking_token 流式着色状态机 (避免逐 token 包裹转义序列)
  bool _inThinking = false;

  void render(AgentEvent event) {
    switch (event) {
      case ReadyEvent():
        _endThinking();
        stdout.writeln(
            Ansi.paint('✓ Agent 就绪 (sessionId: ${event.sessionId})', Ansi.cyan));
      case SyncEvent():
        _endThinking();
        stdout.writeln(Ansi.paint('[历史会话已同步]', Ansi.gray));
      case DeltaEvent():
        _renderDelta(event);
      case TurnEndEvent():
        _endThinking();
        stdout.writeln();
        if (event.interrupted) {
          stdout.writeln(Ansi.paint('⏹ 已中断本轮', Ansi.yellow));
        } else if (event.hasError) {
          final msg = event.errorMessage.isEmpty ? '未知错误' : event.errorMessage;
          stdout.writeln(Ansi.paint('✗ 轮次出错: $msg', Ansi.red));
        } else {
          final stats = StringBuffer('✓ 完成');
          if (event.durationMs > 0) {
            stats.write(' ${(event.durationMs / 1000).toStringAsFixed(1)}s');
          }
          stdout.writeln(Ansi.paint(stats.toString(), Ansi.green));
        }
      case ContextStatsEvent():
        stdout.writeln(Ansi.paint(
            '[上下文 ${event.contextTokens}${event.maxContextTokens > 0 ? '/${event.maxContextTokens}' : ''}]',
            Ansi.gray));
      case ModelInfoEvent():
        _endThinking();
        stdout.writeln(Ansi.paint(
            '模型: ${event.currentModel} (共 ${event.models.length} 个可用)', Ansi.cyan));
      case ComponentsEvent():
        stdout.writeln(Ansi.paint('[组件信息已加载]', Ansi.gray));
      case PluginDataEvent():
        // 插件数据透传展示 (截断避免刷屏)
        final raw = const JsonEncoder.withIndent('  ').convert(event.raw);
        stdout.writeln(Ansi.paint('[插件数据] ', Ansi.magenta) +
            (raw.length > 400 ? '${raw.substring(0, 400)}…' : raw));
      case ErrorEvent():
        _endThinking();
        stdout.writeln(Ansi.paint('✗ 内部错误(${event.code}): ${event.message}', Ansi.red));
      case InterruptReqEvent():
        // 由 CliRepl 处理交互 (渲染在 _printFlowHeader), 此处不输出
        break;
      case InterruptExpiredEvent():
        stdout.writeln(Ansi.paint(
            '⚠ 中断 ${event.interruptId} 已过期或被取消', Ansi.yellow));
    }
  }

  void _renderDelta(DeltaEvent d) {
    switch (d.kind) {
      case 'text_token':
        _endThinking();
        stdout.write(d.text);
      case 'thinking_token':
        if (!_inThinking) {
          _inThinking = true;
          stdout.write(Ansi.gray); // 进入思考块整体着灰
        }
        stdout.write(d.text);
      case 'tool_start':
        _endThinking();
        final args = _oneLine(d.arguments, maxChars: 240);
        stdout.writeln(Ansi.paint(
            '▶ 工具调用 ${d.toolName}${args.isEmpty ? '' : '($args)'}', Ansi.cyan));
      case 'tool_end':
        _endThinking();
        final result = _preview(d.result, maxLines: 4, maxChars: 480);
        final mark = d.hasError ? Ansi.paint('✗ 结果', Ansi.red) : Ansi.paint('✓ 结果', Ansi.green);
        stdout.writeln('  $mark${result.isEmpty ? '' : ': $result'}');
      case 'turn_start':
      case 'node_start':
      case 'node_end':
      case 'turn_end':
        break; // 噪音事件不渲染 (轮次结束另有 TURN_END 汇总)
      case 'message_tip':
      case 'system_message':
        _endThinking();
        final icon = switch (d.tipType) {
          'warning' => '⚠',
          'error' => '✗',
          _ => 'ℹ',
        };
        stdout.writeln(Ansi.paint('$icon ${d.text}', Ansi.yellow));
      default:
        break;
    }
  }

  /// 结束思考块着色状态
  void _endThinking() {
    if (_inThinking) {
      _inThinking = false;
      stdout.write(Ansi.reset);
    }
  }

  static String _oneLine(String s, {int maxChars = 200}) {
    var t = s.replaceAll('\n', '\\n').trim();
    if (t.length > maxChars) {
      t = '${t.substring(0, maxChars)}…';
    }
    return t;
  }

  static String _preview(String s, {int maxLines = 4, int maxChars = 400}) {
    final lines = s.split('\n').take(maxLines).map((l) => l.trim()).toList();
    if (s.split('\n').length > maxLines) {
      lines.add('…');
    }
    var t = lines.join(' ⏎ ').trim();
    if (t.length > maxChars) {
      t = '${t.substring(0, maxChars)}…';
    }
    return t;
  }
}

/// CLI REPL: 组合事件渲染、本地命令、用户输入发送与 HIL 中断应答
class CliRepl {
  CliRepl(this.client);

  final AgentClient client;
  final renderer = CliRenderer();

  /// FIFO 中断流程队列 (同一时刻通常仅一个在途)
  final _flows = <_InterruptFlow>[];

  final _doneCompleter = Completer<void>();
  bool _exitRequested = false;

  /// REPL 结束 (/exit 或 Ctrl+C)
  Future<void> get done => _doneCompleter.future;

  StreamSubscription<AgentEvent>? _eventSub;

  /// 启动 (订阅事件流并打印欢迎信息)
  void start() {
    _eventSub = client.events.listen(
      (e) {
        if (e is InterruptReqEvent) {
          _beginInterrupt(e);
          return;
        }
        if (e is InterruptExpiredEvent) {
          _flows.removeWhere((f) {
            if (f.event.interruptId == e.interruptId) {
              stdout.writeln(Ansi.paint('(等待中的询问已失效)', Ansi.yellow));
              _showPrompt();
              return true;
            }
            return false;
          });
        }
        renderer.render(e);
        if (e is TurnEndEvent) {
          _showPrompt();
        }
      },
      onError: (Object err) {
        stdout.writeln(Ansi.paint('✗ 事件流错误: $err', Ansi.red));
      },
      cancelOnError: false,
    );

    stdout.writeln(Ansi.paint(
        'Agentxx Dart CLI —— 经 libagentxx FFI 驱动内置 agent 会话', Ansi.bold));
    stdout.writeln('输入内容直接对话; /help 查看命令; Ctrl+C 或 /exit 退出。');
    _showPrompt();
  }

  /// 处理一行用户输入 (中断应答优先于命令/对话)
  Future<void> handleLine(String rawLine) async {
    final line = rawLine.trimRight();

    if (_flows.isNotEmpty) {
      _feedAnswer(line);
      return;
    }

    final trimmed = line.trim();
    if (trimmed.startsWith('/')) {
      await _runCommand(trimmed);
      return;
    }
    if (trimmed.isEmpty) {
      _showPrompt();
      return;
    }

    try {
      client.sendInput(line);
      stdout.writeln(); // 与后续流式输出分隔
    } on AgentxxException catch (e) {
      stdout.writeln(Ansi.paint('✗ 发送失败: $e', Ansi.red));
      _showPrompt();
    } on Object catch (e) {
      // 兜底: 输入循环运行在 unawaited future 中, 任何异常外漏都会
      // 以未处理异步错误终止进程, 必须就地消化并保持 REPL 可用
      stdout.writeln(Ansi.paint('✗ 发送失败: $e', Ansi.red));
      _showPrompt();
    }
  }

  /// 请求退出 (/exit / Ctrl+C)
  void requestExit() {
    if (_exitRequested) {
      return; // 二次触发直接忽略 (dispose 流程幂等)
    }
    _exitRequested = true;
    stdout.writeln(Ansi.paint('\n正在停止 agent 会话…', Ansi.gray));
    if (!_doneCompleter.isCompleted) {
      _doneCompleter.complete();
    }
  }

  // -------------------------------------------------------------------------
  // HIL 中断
  // -------------------------------------------------------------------------

  void _beginInterrupt(InterruptReqEvent e) {
    final flow = _InterruptFlow(e);
    _flows.add(flow);

    // 无需输入的中断 (inputs 为空): 直接空数组应答, 不进入逐项询问
    if (!e.isPermissionAsk && e.inputs.isEmpty) {
      stdout.writeln();
      stdout.writeln(Ansi.paint('❓ 需要确认 (${e.interruptName}, 无输入项)', Ansi.magenta));
      _respondFlow(flow);
      return;
    }

    if (e.isPermissionAsk) {
      final op = e.permissionCategory.contains('write') ? '写入' : '读取';
      stdout.writeln();
      stdout.writeln(Ansi.paint('🔐 权限确认 [$op]', Ansi.magenta + Ansi.bold));
      stdout.writeln('   目标: ${e.permissionTarget}');
      stdout.writeln('   允许本次访问? [y=允许 / n=拒绝 / a=允许并记住该路径]');
    } else {
      stdout.writeln();
      stdout.writeln(Ansi.paint('❓ 需要补充输入 (${e.interruptName})', Ansi.magenta));
      for (final item in e.inputs) {
        final depict = item.depict.isEmpty ? '' : ' —— ${item.depict}';
        final enumHint =
            item.enumValues.isNotEmpty ? ' 可选: ${item.enumValues.join('|')}' : '';
        final defHint = item.defaultValue.isEmpty ? '' : ' (回车默认: ${item.defaultValue})';
        stdout.writeln('   · ${item.label}$depict$enumHint$defHint');
      }
      stdout.writeln('   请按顺序逐项回答:');
    }
    _askCurrentItem(flow);
  }

  void _askCurrentItem(_InterruptFlow flow) {
    if (flow.done) {
      return;
    }
    final item = flow.currentItem;
    if (flow.event.isPermissionAsk) {
      return; // 权限询问的提示已在头部打印
    }
    final enumHint = item.type == 'enum'
        ? ' [1-${item.enumValues.length}]'
        : (item.type == 'bool' ? ' [y/n]' : '');
    stdout.write('   ${item.label}$enumHint > ');
  }

  void _feedAnswer(String line) {
    final flow = _flows.first;
    final input = flow.currentItem;
    final trimmed = line.trim();

    String? value;
    switch (input.type) {
      case 'bool':
        if (flow.event.isPermissionAsk && (trimmed == 'a' || trimmed == 'A')) {
          flow.rememberPermission = true;
          value = 'true';
          break;
        }
        value = trimmed.isEmpty
            ? _normalizeBool(input.defaultValue)
            : _parseBool(trimmed);
        if (value != null) {
          break;
        }
        // 显式输入非法: 置空走统一的无效重问流程
        value = null;
      case 'int':
        final v = trimmed.isEmpty ? input.defaultValue : trimmed;
        if (int.tryParse(v) != null) {
          value = v;
        }
      case 'double':
        final v = trimmed.isEmpty ? input.defaultValue : trimmed;
        if (double.tryParse(v) != null || int.tryParse(v) != null) {
          value = v;
        }
      case 'enum':
        // 无默认值时空输入必须重问, 不得发送空串占位
        if (trimmed.isEmpty) {
          if (input.defaultValue.isNotEmpty) {
            value = input.defaultValue;
          }
          break;
        }
        final byIndex = int.tryParse(trimmed);
        if (byIndex != null &&
            byIndex >= 1 &&
            byIndex <= input.enumValues.length) {
          value = input.enumValues[byIndex - 1];
        } else if (input.enumValues.contains(trimmed)) {
          value = trimmed;
        }
      default: // string 或未知类型按文本处理
        value = trimmed.isEmpty ? input.defaultValue : trimmed;
    }

    if (value == null) {
      stdout.writeln(Ansi.paint(
          '   输入无效, 请重新输入 (${input.type == 'enum' ? '可选: ${input.enumValues.join('|')}'
              : '期望类型: ${input.type.isEmpty ? '文本' : input.type}'})', Ansi.yellow));
      _askCurrentItem(flow);
      return;
    }

    flow.answers.add(value);
    if (!flow.done) {
      _askCurrentItem(flow);
    } else {
      _respondFlow(flow);
    }
  }

  void _respondFlow(_InterruptFlow flow) {
    try {
      client.interruptRespond(
          flow.event.interruptId, jsonEncode(flow.answers));
      stdout.writeln(Ansi.paint('   已应答 (${flow.answers.join(", ")})', Ansi.gray));
      // 权限 "always": 注册路径规则, 同路径后续不再询问
      if (flow.rememberPermission && flow.event.isPermissionAsk) {
        try {
          client.setPermission(
            flow.event.permissionTarget,
            allow: true,
            op: flow.event.permissionCategory.contains('write') ? 1 : 0,
          );
          stdout.writeln(Ansi.paint(
              '   已记住路径权限: ${flow.event.permissionTarget}', Ansi.gray));
        } on AgentxxException catch (err) {
          stdout.writeln(Ansi.paint('   记住权限失败: $err', Ansi.yellow));
        }
      }
    } on AgentxxException catch (err) {
      stdout.writeln(Ansi.paint('   应答失败: $err', Ansi.red));
    } finally {
      _flows.remove(flow);
      _showPrompt();
    }
  }

  static String? _parseBool(String s) {
    switch (s.toLowerCase()) {
      case 'y':
      case 'yes':
      case 'true':
        return 'true';
      case 'n':
      case 'no':
      case 'false':
        return 'false';
      default:
        return null;
    }
  }

  /// 把中断输入项的 defaultValue ("no"/"yes"/"true"/"false") 规范化为
  /// 应答协议要求的 "true"/"false"; 无法识别返回 null (走无效重问)
  static String? _normalizeBool(String s) => _parseBool(s);

  // -------------------------------------------------------------------------
  // 本地命令
  // -------------------------------------------------------------------------

  Future<void> _runCommand(String cmdLine) async {
    final parts =
        cmdLine.substring(1).split(RegExp(r'\s+')).where((p) => p.isNotEmpty).toList();
    final cmd = parts.isEmpty ? '' : parts.first;
    final arg = parts.skip(1).join(' ');

    try {
      switch (cmd) {
        case 'help':
        case 'h':
          _printHelp();
        case 'exit':
        case 'quit':
        case 'q':
          requestExit();
          return;
        case 'cancel':
          client.cancel();
          stdout.writeln(Ansi.paint('已请求取消当前轮次', Ansi.gray));
        case 'model':
          if (arg.isEmpty) {
            final info = client.getModelInfo();
            if (info != null) {
              stdout.writeln(const JsonEncoder.withIndent('  ')
                  .convert(info)
                  .split('\n')
                  .take(40)
                  .join('\n'));
            } else {
              stdout.writeln(Ansi.paint('查询失败: ${client.lastQueryError}', Ansi.red));
            }
          } else {
            client.selectModel(arg);
            stdout.writeln(Ansi.paint("已请求切换到 '$arg' (结果经 MODEL_INFO 通知)", Ansi.gray));
          }
        case 'sessions':
          final data = client.listSessions();
          if (data == null) {
            stdout.writeln(Ansi.paint(
                '查询失败: ${client.lastQueryError} (需要开启会话持久化)', Ansi.red));
          } else {
            final sessions = data['sessions'] as List<dynamic>? ?? const [];
            if (sessions.isEmpty) {
              stdout.writeln('(无持久化会话)');
            }
            for (final s in sessions) {
              final m = s as Map<String, dynamic>;
              final title = (m['title'] as String?) ?? '(无标题)';
              final sid = (m['sessionId'] as String?) ?? '';
              stdout.writeln('  ${Ansi.paint(sid, Ansi.cyan)}  $title');
            }
          }
        case 'switch':
          if (arg.isEmpty) {
            stdout.writeln('用法: /switch <sessionId>');
          } else {
            client.switchSession(arg);
            stdout.writeln(Ansi.paint("已请求切换会话 '$arg'", Ansi.gray));
          }
        case 'context':
          final data = client.getContextMessages();
          if (data == null) {
            stdout.writeln(Ansi.paint('查询失败: ${client.lastQueryError}', Ansi.red));
          } else {
            final messages = data['messages'] as List<dynamic>? ?? const [];
            stdout.writeln('当前 LLM 上下文共 ${messages.length} 条消息:');
            for (final m in messages.take(10)) {
              if (m is Map<String, dynamic>) {
                final role = m['role'] ?? '?';
                final content = (m['content']?.toString() ?? '')
                    .replaceAll('\n', ' ')
                    .trim();
                final preview =
                    content.length > 60 ? '${content.substring(0, 60)}…' : content;
                stdout.writeln('  [$role] $preview');
              }
            }
          }
        case 'status':
          stdout.writeln('sessionId: ${client.sessionId ?? '(未就绪)'}');
          final info = client.getModelInfo();
          stdout.writeln('currentModel: ${info?['currentModel'] ?? '?'}');
        case 'logs':
          final logs = client.drainLogs();
          if (logs == null || logs.isEmpty) {
            stdout.writeln('(无积压日志)');
          } else {
            for (final entry in logs.take(50)) {
              if (entry is Map<String, dynamic>) {
                stdout.writeln(
                    '[${entry['level']}] ${entry['message']}');
              }
            }
          }
        case 'clear':
          stdout.write('\x1B[2J\x1B[H');
        default:
          stdout.writeln('未知命令: /$cmd (输入 /help 查看帮助)');
      }
    } on AgentxxException catch (e) {
      stdout.writeln(Ansi.paint('✗ 命令执行失败: $e', Ansi.red));
    } finally {
      if (!_exitRequested) {
        _showPrompt();
      }
    }
  }

  void _printHelp() {
    stdout.writeln('''
命令列表:
  /help                 显示本帮助
  /cancel               取消当前正在进行的轮次
  /model                查看当前模型与可用列表
  /model <name>         切换模型
  /sessions             列出持久化会话 (需开启 --enable-session-store)
  /switch <sessionId>   切换到指定会话
  /context              查看当前 LLM 上下文消息
  /status               查看连接状态
  /logs                 取走运行日志 (排障)
  /clear                清屏
  /exit                 退出 (Ctrl+C 同效)
其余输入将作为对话发送给 agent; 出现 🔐 权限确认 / ❓ 补充输入时,
输入行优先用于回答询问 (y/n/a 或按提示)。''');
  }

  void _showPrompt() {
    stdout.write('> ');
  }

  Future<void> dispose() async {
    await _eventSub?.cancel();
  }
}
