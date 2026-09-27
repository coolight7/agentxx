// 本文件由 tools/gen_ui.dart 生成，请勿手工修改。
// 定义来源：schema/ui.def.json / schema/kit.def.json；扩展 kit 定义：agent/schema/agentxx-ui-kit.def.json
// kit（JS 插件用）：随插件目录分发，脚本里直接用全局 pluginxx.ui.kit。
//   const kit = pluginxx.ui.kit;
//   const row = kit.listRow({ title: '切歌次数', trailing: '3' }, env);
(function (global) {
  'use strict';
  var root = global.pluginxx || (global.pluginxx = {});
  var ui = root.ui || (root.ui = {});
  var kit = ui.kit || (ui.kit = {});
  kit.kitVersion = 1;
  kit.apiVersion = 1;
  kit.defaultGap = 12;
  kit.cell = { width: 8, height: 20 };

  // 组件模板：变体数组 + 参数默认值
  var TEMPLATES = {"title":{"variants":[{"template":{"kind":"Text","text":"$text","type":"title"}}],"params":{"text":null}},"hint":{"variants":[{"template":{"kind":"Text","text":"$text","type":"caption","tone":"hint"}}],"params":{"text":null}},"text":{"variants":[{"template":{"kind":"Text","text":"$text","tone":"$tone","mono":"$mono"}}],"params":{"text":null,"tone":"normal","mono":false}},"badge":{"variants":[{"template":{"kind":"Badge","text":"$text","tone":"$tone"}}],"params":{"text":null,"tone":"accent"}},"icon":{"variants":[{"requires":["Icon"],"template":{"kind":"Icon","name":"$name","glyph":"$glyph","size":"$size","tone":"$tone"}},{"template":{"kind":"Text","text":"$glyph","mono":true,"tone":"$tone"}}],"params":{"name":null,"glyph":null,"size":null,"tone":"normal"}},"gap":{"variants":[{"template":{"kind":"Gap","size":"$size"}}],"params":{"size":null}},"divider":{"variants":[{"template":{"kind":"Divider"}}],"params":{}},"button":{"variants":[{"template":{"kind":"Button","label":"$label","variant":"$variant","icon":"$icon","disabled":"$disabled","action":"$action"}}],"params":{"label":null,"variant":"secondary","icon":null,"disabled":false,"action":null}},"actionsRow":{"variants":[{"template":{"kind":"Row","gap":12,"children":{"$map":"buttons","wrap":{"kind":"Expanded","children":["$item"]}}}}],"params":{"buttons":null}},"card":{"variants":[{"template":{"kind":"Block","title":"$title","variant":"$variant","padding":"$padding","margin":"$margin","children":"$children"}}],"params":{"title":null,"variant":"card","padding":{"horizontal":8,"vertical":4},"margin":null,"children":null}},"listRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"action":"$action","children":[{"kind":"Row","gap":8,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","tone":"hint"}]}]}}],"params":{"title":null,"subtitle":null,"trailing":null,"action":null}},"section":{"variants":[{"template":{"kind":"Column","gap":8,"children":[{"kind":"Text","text":"$title","type":"title"},{"kind":"Column","children":"$rows"}]}}],"params":{"title":null,"rows":null}},"kv":{"variants":[{"template":{"kind":"KV","pairs":"$pairs","sep":"$sep","keyWidth":"auto"}}],"params":{"pairs":null,"sep":null}},"table":{"variants":[{"template":{"kind":"Table","header":"$header","columns":"$columns","rows":"$rows"}}],"params":{"columns":null,"rows":null,"header":true}},"tree":{"variants":[{"template":{"kind":"Tree","connector":"$connector","nodes":"$nodes"}}],"params":{"nodes":null,"connector":true}},"sparkline":{"variants":[{"template":{"kind":"Sparkline","data":"$data","height":"$height","glyphStyle":"$glyphStyle","showLast":"$showLast","tone":"$tone"}}],"params":{"data":null,"height":1,"glyphStyle":"block","showLast":true,"tone":"accent"}},"progressRow":{"variants":[{"template":{"kind":"Row","gap":8,"cross":"center","children":[{"$require":"label","kind":"Text","text":"$label"},{"kind":"Expanded","children":[{"kind":"Progress","value":"$value","total":"$total","unit":"$unit"}]}]}}],"params":{"label":null,"value":null,"total":100,"unit":"%"}},"toolCallRow":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"action":"$action","children":[{"kind":"Row","gap":8,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$name"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"$require":"status","kind":"Badge","text":"$status","tone":"$statusTone"}]}]}}],"params":{"name":null,"depict":null,"status":null,"statusTone":"hint","action":null}},"thinkingBlock":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"label","kind":"Text","text":"$label","type":"caption","tone":"hint"},{"kind":"Text","text":"$content","tone":"thinking"}]}]}}],"params":{"content":null,"label":null}},"sessionStats":{"variants":[{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"title"},{"kind":"Table","header":true,"columns":"$columns","rows":"$rows"}]}]}}],"params":{"title":null,"columns":null,"rows":null}},"pathDiffRow":{"variants":[{"requires":["Diff"],"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"caption","tone":"hint","mono":true},{"kind":"Diff","path":"$path","oldStr":"$oldStr","newStr":"$newStr"}]}]}},{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"kind":"Text","text":"$path","type":"caption","tone":"hint","mono":true},{"kind":"Text","text":"${oldStr}\n${newStr}","mono":true}]}]}}],"params":{"path":null,"oldStr":null,"newStr":null,"title":null}},"diagramBlock":{"variants":[{"requires":["Diagram"],"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Column","gap":4,"children":[{"$require":"title","kind":"Text","text":"$title","type":"caption","tone":"hint"},{"kind":"Diagram","mermaid":"$mermaid"}]}]}},{"template":{"kind":"Block","variant":"inset","padding":{"horizontal":8,"vertical":4},"children":[{"kind":"Text","text":"$mermaid","mono":true,"tone":"hint"}]}}],"params":{"mermaid":null,"title":null}},"interruptRow":{"variants":[{"template":{"kind":"Row","gap":8,"main":"end","children":[{"kind":"Expanded","children":[{"kind":"Button","label":"$confirmLabel","variant":"primary","action":"$confirmAction"}]},{"kind":"Expanded","children":[{"kind":"Button","label":"$cancelLabel","variant":"ghost","action":"$cancelAction"}]}]}}],"params":{"confirmLabel":null,"cancelLabel":null,"confirmAction":"__submit","cancelAction":"__cancel"}}};

  function present(v) {
    if (v === undefined || v === null) return false;
    if (typeof v === 'string') return v.length > 0;
    return true;
  }

  function isEmpty(v) {
    if (!present(v)) return true;
    return Array.isArray(v) && v.length === 0;
  }

  function pickVariant(name, env) {
    var t = TEMPLATES[name];
    if (!t) return null;
    var variants = t.variants;
    // 目标未知（没有 env）：用第一个变体（插件作者认为最合适的那一个）
    if (!env) return variants.length ? variants[0].template : null;
    var fallback = null;
    for (var i = 0; i < variants.length; i++) {
      var need = variants[i].requires || [];
      if (need.length === 0) {
        if (fallback === null) fallback = variants[i];
        continue;
      }
      var ok = true;
      for (var j = 0; j < need.length; j++) {
        if (!env.blocks || env.blocks.indexOf(need[j]) < 0) { ok = false; break; }
      }
      if (ok) return variants[i].template;
    }
    var chosen = fallback !== null ? fallback : variants[0];
    return chosen ? chosen.template : null;
  }

  function stringify(v) {
    if (v === undefined || v === null) return '';
    if (typeof v === 'string') return v;
    if (typeof v === 'number' || typeof v === 'boolean') return String(v);
    return JSON.stringify(v);
  }

  function interpolate(text, values) {
    return text.replace(/\$\{([A-Za-z0-9_]+)\}/g, function (_, name) {
      return stringify(values[name]);
    });
  }

  function expandMap(node, values) {
    var out = [];
    if (node === null || typeof node !== 'object' || Array.isArray(node) || node.$map === undefined) {
      var single = expandNode(node, values);
      if (single !== undefined) out.push(single);
      return out;
    }
    var list = values[node.$map];
    if (!Array.isArray(list)) return out;
    for (var i = 0; i < list.length; i++) {
      var scope = {};
      for (var key in values) scope[key] = values[key];
      scope.item = list[i];
      var item = expandNode(node.wrap, scope);
      if (item !== undefined) out.push(item);
    }
    return out;
  }

  function expandNode(node, values) {
    if (typeof node === 'string') {
      if (node.length > 1 && node.charAt(0) === '$' && node.indexOf('{') < 0) {
        var value = values[node.substring(1)];
        return value === undefined ? null : value;
      }
      return node.indexOf('${') >= 0 ? interpolate(node, values) : node;
    }
    if (Array.isArray(node)) {
      var list = [];
      for (var i = 0; i < node.length; i++) list = list.concat(expandMap(node[i], values));
      return list;
    }
    if (node === null || typeof node !== 'object') return node;
    if (node.$map !== undefined) return expandMap(node, values);
    if (node.$require !== undefined && isEmpty(values[node.$require])) return undefined;
    var obj = {};
    for (var key in node) {
      if (key === '$require') continue;
      var child = expandNode(node[key], values);
      if (child === undefined || child === null) continue;
      obj[key] = child;
    }
    return obj;
  }

  function expand(name, params, env) {
    var template = pickVariant(name, env);
    if (template === null) return null;
    var declared = (TEMPLATES[name] || {}).params || {};
    var values = {};
    for (var key in declared) {
      values[key] = params[key] === undefined ? declared[key] : params[key];
    }
    for (var given in params) {
      if (values[given] === undefined) values[given] = params[given];
    }
    var result = expandNode(template, values);
    return result === undefined ? null : result;
  }

  kit.expand = expand;


  // 按格换算成 u（n 列）
  kit.cols = function (count, env) {
    return count * ((env && env.cell && env.cell.width) || kit.cell.width);
  };
  // 按格换算成 u（n 行）
  kit.rows = function (count, env) {
    return count * ((env && env.cell && env.cell.height) || kit.cell.height);
  };

  // 标题行
  // 参数：text(text, 必填)
  kit.title = function (params, env) { return expand("title", params || {}, env); };

  // 次要说明行
  // 参数：text(text, 必填)
  kit.hint = function (params, env) { return expand("hint", params || {}, env); };

  // 正文行
  // 参数：text(text, 必填)、tone(tone, 默认 normal)、mono(bool, 默认 false)
  kit.text = function (params, env) { return expand("text", params || {}, env); };

  // 状态小标签
  // 参数：text(text, 必填)、tone(tone, 默认 accent)
  kit.badge = function (params, env) { return expand("badge", params || {}, env); };

  // 图标（目标不支持 Icon 时退化成 glyph 文本）
  // 参数：name(string)、glyph(string)、size(size)、tone(tone, 默认 normal)
  kit.icon = function (params, env) { return expand("icon", params || {}, env); };

  // 竖直留白（缺省用客户端默认行距）
  // 参数：size(size, 默认 gap)
  kit.gap = function (params, env) { return expand("gap", params || {}, env); };

  // 分隔线
  // 参数：
  kit.divider = function (params, env) { return expand("divider", params || {}, env); };

  // 按钮
  // 参数：label(text, 必填)、variant(enum:buttonVariant, 默认 secondary)、icon(string)、disabled(bool, 默认 false)、action(action)
  kit.button = function (params, env) { return expand("button", params || {}, env); };

  // 一排等宽按钮（按钮列表里的每一项占一等份）
  // 参数：buttons(items, 必填)
  kit.actionsRow = function (params, env) { return expand("actionsRow", params || {}, env); };

  // 内容块（覆盖基础 kit：终端卡片的留白更紧）
  // 参数：title(text)、variant(string, 默认 card)、padding(edges, 默认 {horizontal: 8, vertical: 4})、margin(edges)、children(items)
  kit.card = function (params, env) { return expand("card", params || {}, env); };

  // 卡片里的一行（覆盖基础 kit：终端行更紧，右侧文字用说明色调）
  // 参数：title(text, 必填)、subtitle(text)、trailing(text)、action(action)
  kit.listRow = function (params, env) { return expand("listRow", params || {}, env); };

  // 小节标题 + 若干行
  // 参数：title(text, 必填)、rows(items, 必填)
  kit.section = function (params, env) { return expand("section", params || {}, env); };

  // 键值块（键列按最长键自适应）
  // 参数：pairs(pairs, 必填)、sep(string)
  kit.kv = function (params, env) { return expand("kv", params || {}, env); };

  // 表格（未指定的列宽由客户端自动分配）
  // 参数：columns(columns, 必填)、rows(rows)、header(bool, 默认 true)
  kit.table = function (params, env) { return expand("table", params || {}, env); };

  // 层级列表
  // 参数：nodes(nodes, 必填)、connector(bool, 默认 true)
  kit.tree = function (params, env) { return expand("tree", params || {}, env); };

  // 迷你趋势图
  // 参数：data(numbers, 必填)、height(int, 默认 1)、glyphStyle(enum:sparkStyle, 默认 block)、showLast(bool, 默认 true)、tone(tone, 默认 accent)
  kit.sparkline = function (params, env) { return expand("sparkline", params || {}, env); };

  // 一行进度（覆盖基础 kit：agentxx 默认按百分比显示数值）
  // 参数：label(text)、value(float, 必填)、total(float, 默认 100)、unit(string, 默认 %)
  kit.progressRow = function (params, env) { return expand("progressRow", params || {}, env); };

  // 工具调用行：工具名 + 一行摘要 + 可选状态标签
  // 参数：name(text, 必填)、depict(text)、status(text)、statusTone(tone, 默认 hint)、action(action)
  kit.toolCallRow = function (params, env) { return expand("toolCallRow", params || {}, env); };

  // 思考 / 说明段落：可选小标题 + 次要色调的正文
  // 参数：content(text, 必填)、label(text)
  kit.thinkingBlock = function (params, env) { return expand("thinkingBlock", params || {}, env); };

  // 统计表：可选标题 + 表格（列宽由客户端自动分配）
  // 参数：title(text)、columns(columns, 必填)、rows(rows, 必填)
  kit.sessionStats = function (params, env) { return expand("sessionStats", params || {}, env); };

  // 路径差异：可选标题 + 差异对比（目标不支持 Diff 时退化成等宽文本）
  // 参数：path(string, 必填)、oldStr(string, 必填)、newStr(string, 必填)、title(text)
  kit.pathDiffRow = function (params, env) { return expand("pathDiffRow", params || {}, env); };

  // 状态图：可选标题 + mermaid 描述（目标不支持 Diagram 时退化成等宽源码）
  // 参数：mermaid(string, 必填)、title(text)
  kit.diagramBlock = function (params, env) { return expand("diagramBlock", params || {}, env); };

  // 确认 / 取消行（动作名缺省用中断域内约定：__submit / __cancel）
  // 参数：confirmLabel(text, 必填)、cancelLabel(text, 必填)、confirmAction(action, 默认 __submit)、cancelAction(action, 默认 __cancel)
  kit.interruptRow = function (params, env) { return expand("interruptRow", params || {}, env); };
})(
  typeof globalThis !== 'undefined' ? globalThis : this
);
