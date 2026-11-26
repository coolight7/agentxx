/// agentxx id 清单生成器
///
/// 用法（在仓库根目录下）：
/// ```
/// dart run agent/tool/gen_ids.dart            # 生成三份头
/// dart run agent/tool/gen_ids.dart --check    # 只校验（生成物与声明一致时退出码 0）
/// ```
///
/// 声明源只有一份：`agent/schema/ids.def.json`（应用能力 / 功能埋点 / 广播频道）。
/// 产物（提交物，不要手改）：
/// | 产物 | 用途 |
/// |---|---|
/// | `agent/lib/include/agentxx/plugin/api/agentxx_capability_ids.h` | 主程序声明的应用能力 id |
/// | `agent/lib/include/agentxx/plugin/api/agentxx_point_ids.h` | 功能埋点 id（能力面 + 同名频道） |
/// | `agent/lib/include/agentxx/plugin/api/agentxx_broadcast_ids.h` | 主程序声明的应用频道 id |
///
/// 声明表仍然读这三份头的常量（`kAppCapabilities` / `kNotifyPoints` / `kValuePoints`），
/// 所以"声明表"与"id 清单"不会各写一份。
library;

import 'dart:convert';
import 'dart:io';

/* ==================== 产物路径 ==================== */

/// 声明源（相对仓库根目录）
const String idsDefPath = 'agent/schema/ids.def.json';

/// 应用能力 id 头
const String capabilityIdsOut =
    'agent/lib/include/agentxx/plugin/api/agentxx_capability_ids.h';

/// 功能埋点 id 头
const String pointIdsOut =
    'agent/lib/include/agentxx/plugin/api/agentxx_point_ids.h';

/// 广播频道 id 头
const String broadcastIdsOut =
    'agent/lib/include/agentxx/plugin/api/agentxx_broadcast_ids.h';

/* ==================== 模型 ==================== */

class _IdDecl {
  _IdDecl({required this.name, required this.id, required this.doc});

  /// C++ 常量名
  final String name;

  /// id 文本
  final String id;

  /// 逐行的文档注释（不带 `///`）
  final List<String> doc;
}

class _Ids {
  _Ids({
    required this.capabilities,
    required this.notifyPoints,
    required this.valuePoints,
    required this.broadcasts,
    required this.broadcastPrefixes,
  });

  final List<_IdDecl> capabilities;
  final List<_IdDecl> notifyPoints;
  final List<_IdDecl> valuePoints;
  final List<_IdDecl> broadcasts;
  final List<_IdDecl> broadcastPrefixes;
}

/* ==================== 读取与校验 ==================== */

List<_IdDecl> _readList(Object? raw, String where) {
  if (raw is! List) {
    throw StateError('$where: 需要数组');
  }
  final List<_IdDecl> out = <_IdDecl>[];
  final Set<String> names = <String>{};
  final Set<String> ids = <String>{};
  for (final Object? item in raw) {
    if (item is! Map) {
      throw StateError('$where: 每一项都要是对象');
    }
    final Object? name = item['name'];
    final Object? id = item['id'];
    final Object? doc = item['doc'];
    if (name is! String || name.isEmpty) {
      throw StateError('$where: 缺少 name');
    }
    if (id is! String || id.isEmpty) {
      throw StateError('$where: `$name` 缺少 id');
    }
    if (!names.add(name)) {
      throw StateError('$where: 常量名 `$name` 重复');
    }
    if (!ids.add(id)) {
      throw StateError('$where: id `$id` 重复');
    }
    final List<String> docLines = <String>[];
    if (doc != null) {
      if (doc is! List) {
        throw StateError('$where: `$name` 的 doc 需要数组');
      }
      for (final Object? line in doc) {
        if (line is! String) {
          throw StateError('$where: `$name` 的 doc 只写字符串');
        }
        docLines.add(line);
      }
    }
    out.add(_IdDecl(name: name, id: id, doc: docLines));
  }
  return out;
}

_Ids loadIds({String path = idsDefPath}) {
  final File file = File(path);
  if (!file.existsSync()) {
    throw StateError('找不到 id 声明: $path');
  }
  final Map<String, Object?> def =
      jsonDecode(file.readAsStringSync()) as Map<String, Object?>;
  final Object? points = def['points'];
  if (points is! Map) {
    throw StateError('points: 需要对象（notify / value 两段）');
  }
  final _Ids ids = _Ids(
    capabilities: _readList(def['capabilities'], 'capabilities'),
    notifyPoints: _readList(points['notify'], 'points.notify'),
    valuePoints: _readList(points['value'], 'points.value'),
    broadcasts: _readList(def['broadcasts'], 'broadcasts'),
    broadcastPrefixes:
        _readList(def['broadcastPrefixes'], 'broadcastPrefixes'),
  );
  if (ids.capabilities.isEmpty) {
    throw StateError('capabilities: 至少要有一条');
  }
  if (ids.notifyPoints.isEmpty) {
    throw StateError('points.notify: 至少要有一条');
  }
  if (ids.broadcasts.isEmpty) {
    throw StateError('broadcasts: 至少要有一条');
  }
  return ids;
}

/* ==================== 生成 ==================== */

void _writeDoc(StringBuffer sb, List<String> doc) {
  for (final String line in doc) {
    sb.writeln(line.isEmpty ? '///' : '/// $line');
  }
}

void _writeConstants(StringBuffer sb, List<_IdDecl> decls) {
  for (int i = 0; i < decls.length; i += 1) {
    final _IdDecl decl = decls[i];
    _writeDoc(sb, decl.doc);
    sb.writeln(
      'inline constexpr std::string_view ${decl.name} = "${decl.id}";',
    );
    if (i != decls.length - 1) {
      sb.writeln();
    }
  }
}

void _writeArray(StringBuffer sb, String name, List<_IdDecl> decls) {
  sb.writeln('inline constexpr std::string_view $name[] = {');
  for (final _IdDecl decl in decls) {
    sb.writeln('    ${decl.name},');
  }
  sb.writeln('};');
}

String generateCapabilityIds(_Ids ids) {
  final StringBuffer sb = StringBuffer()
    ..writeln('/// agentxx 应用能力 id 清单 (主程序声明的能力, 插件只提供实现行)')
    ..writeln('///')
    ..writeln(
      '/// 本文件由 `agent/tool/gen_ids.dart` 从 `agent/schema/ids.def.json` 生成, 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// 能力分两类, 声明方不同:')
    ..writeln('/// - **应用声明的能力** (本头): id 落在 `<应用名>.` 段下, 由主程序在启动阶段声明')
    ..writeln('///   (几个实现、结果怎么合并、开放哪些阶段)。插件不能自己声明这类 id, 只能给它登记')
    ..writeln('///   实现行; 没声明就登记会被主程序拒绝 (`... is not declared by the application`)。')
    ..writeln('///   这样"有哪些官方能力"只在一处说了算, 插件之间的调用点不必各自记名字。')
    ..writeln('/// - **插件自己的能力**: id 写短名, 主程序补成 `plugin.<插件 id>.<短名>`,')
    ..writeln('///   声明与实现都由插件自己给。')
    ..writeln('///')
    ..writeln('/// 功能埋点 (`agentxx.agent.point.*`) 也是一批应用声明的能力, 但它们的清单在')
    ..writeln('/// `agentxx_point_ids.h` (那边同时还要声明同名广播频道), 不在这里。')
    ..writeln('#ifndef AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H')
    ..writeln('#define AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H')
    ..writeln()
    ..writeln('#include <string_view>')
    ..writeln()
    ..writeln('namespace agentxx {')
    ..writeln('namespace plugin {')
    ..writeln('namespace capabilities {')
    ..writeln();
  _writeConstants(sb, ids.capabilities);
  sb
    ..writeln()
    ..writeln('/// 全部应用声明的能力 (主程序声明表与测试按它清点)');
  _writeArray(sb, 'kAppCapabilities', ids.capabilities);
  sb
    ..writeln()
    ..writeln('} // namespace capabilities')
    ..writeln('} // namespace plugin')
    ..writeln('} // namespace agentxx')
    ..writeln()
    ..writeln('#endif /* AGENTXX_PLUGIN_API_AGENTXX_CAPABILITY_IDS_H */');
  return sb.toString();
}

String generatePointIds(_Ids ids) {
  final StringBuffer sb = StringBuffer()
    ..writeln('/// agentxx 功能埋点 id 清单 (唯一真源)')
    ..writeln('///')
    ..writeln(
      '/// 本文件由 `agent/tool/gen_ids.dart` 从 `agent/schema/ids.def.json` 生成, 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// 一个"功能埋点"= 一个 id, 同时是')
    ..writeln('/// - 一条**能力** (`<应用名>.capabilities`, 声明为 `impls: many` + `merge: first`, 只开 `impl`);')
    ..writeln('/// - 一个**同名广播频道** (`<应用名>.broadcasts`)。')
    ..writeln('///')
    ..writeln('/// 插件两种参与方式 (可都选):')
    ..writeln('/// - **实现**: 登记一条实现行, 返回 JSON 文本 (需要返回值的功能埋点才看这个);')
    ..writeln('/// - **订阅**: 订阅同名频道, 只收通知 (纯通知型功能埋点的等价物就是它)。')
    ..writeln('///')
    ..writeln('/// 载荷字段 (广播与实现行入参见下表) 里一律带 `agentId`。**通知型**点的载荷固定为')
    ..writeln('/// `{"agentId": <挂载的 agent id>, "sessionId": <会话 id>, "point": <点名称>}`')
    ..writeln('/// (agentId / sessionId 之外的细节在各调用点自己带了才加, 不给整份会话内容);')
    ..writeln('/// 订阅侧用它过滤掉其它 agent 的通知 (同一个插件可能同时挂在多个 agent 上)。')
    ..writeln('///')
    ..writeln('/// 与主程序声明表的一致性由测试模块 `boundaries` 校验: 本头的 id 清单必须与')
    ..writeln('/// `PluginManager` 的功能埋点声明表逐条相同。')
    ..writeln('#ifndef AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H')
    ..writeln('#define AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H')
    ..writeln()
    ..writeln('#include <string_view>')
    ..writeln()
    ..writeln('namespace agentxx {')
    ..writeln('namespace plugin {')
    ..writeln('namespace points {')
    ..writeln();
  _writeConstants(sb, <_IdDecl>[...ids.notifyPoints, ...ids.valuePoints]);
  sb.writeln();
  sb.writeln('/// 全部通知型功能埋点 (声明表与测试按它清点)');
  _writeArray(sb, 'kNotifyPoints', ids.notifyPoints);
  sb.writeln();
  sb.writeln('/// 全部取值型功能埋点');
  _writeArray(sb, 'kValuePoints', ids.valuePoints);
  sb
    ..writeln()
    ..writeln('} // namespace points')
    ..writeln('} // namespace plugin')
    ..writeln('} // namespace agentxx')
    ..writeln()
    ..writeln('#endif /* AGENTXX_PLUGIN_API_AGENTXX_POINT_IDS_H */');
  return sb.toString();
}

String generateBroadcastIds(_Ids ids) {
  final StringBuffer sb = StringBuffer()
    ..writeln('/// agentxx 广播频道 id 清单 (主程序声明的应用频道)')
    ..writeln('///')
    ..writeln(
      '/// 本文件由 `agent/tool/gen_ids.dart` 从 `agent/schema/ids.def.json` 生成, 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// 频道的职责划分 (见 docs/zh-cn/design/events.md 与 plugins.md):')
    ..writeln('/// - **功能埋点**频道 (`agentxx.agent.point.*`): 见 `agentxx_point_ids.h`;')
    ..writeln('/// - **生命周期通知** (本头): "某一刻发生了什么", 没有等待者、没有值语义;')
    ..writeln('/// - **事件中枢的值事件** (`agentxx.agent.event.*` / `agentxx.agent.async.done`):')
    ..writeln('///   由中枢自己发布, 名字规则在 `agentxx/event/naming.h`, 不在本头。')
    ..writeln('///')
    ..writeln('/// 插件自建频道用短名 (`declare_broadcast` 的 `id` 不给前缀), 主程序会补成')
    ..writeln('/// `plugin.<插件 id>.<短名>`; 本头的名字是主程序声明并发布的, 插件只能订阅。')
    ..writeln('#ifndef AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H')
    ..writeln('#define AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H')
    ..writeln()
    ..writeln('#include <string_view>')
    ..writeln()
    ..writeln('namespace agentxx {')
    ..writeln('namespace plugin {')
    ..writeln('namespace broadcasts {')
    ..writeln();
  _writeConstants(sb, ids.broadcasts);
  if (ids.broadcastPrefixes.isNotEmpty) {
    sb.writeln();
    _writeConstants(sb, ids.broadcastPrefixes);
  }
  sb
    ..writeln()
    ..writeln('} // namespace broadcasts')
    ..writeln('} // namespace plugin')
    ..writeln('} // namespace agentxx')
    ..writeln()
    ..writeln('#endif /* AGENTXX_PLUGIN_API_AGENTXX_BROADCAST_IDS_H */');
  return sb.toString();
}

Map<String, String> buildIds(_Ids ids) => <String, String>{
      capabilityIdsOut: generateCapabilityIds(ids),
      pointIdsOut: generatePointIds(ids),
      broadcastIdsOut: generateBroadcastIds(ids),
    };

/* ==================== 入口 ==================== */

void main(List<String> args) {
  final bool check = args.contains('--check');
  final _Ids ids = loadIds();
  final Map<String, String> files = buildIds(ids);
  final List<String> drift = <String>[];

  files.forEach((String path, String content) {
    final File file = File(path);
    if (check) {
      final String existing = file.existsSync() ? file.readAsStringSync() : '';
      if (existing != content) {
        drift.add(path);
      }
      return;
    }
    file.parent.createSync(recursive: true);
    file.writeAsStringSync(content);
    stdout.writeln('  生成: $path');
  });

  if (check && drift.isNotEmpty) {
    stderr.writeln('id 头与声明不一致（请运行 dart run agent/tool/gen_ids.dart 后提交）:');
    for (final String path in drift) {
      stderr.writeln('  $path');
    }
    exit(1);
  }
  stdout.writeln(check ? 'id 头与声明一致' : 'id 头已生成');
}
