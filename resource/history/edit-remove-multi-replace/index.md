# 移除 edit 工具的 `multi_replace` 参数与功能
- 难度: B
- 类型: 功能移除 (简化)
- 基于commit: 9806ea68f4ce69509c354e179863fd0a7b52e402
- 时间: 2026-10-18 21:05
- 需求:
```md
很好，现在请移除 其 multi_replace 参数和功能
```
- 实现要点:
    - 语义统一为"**每段只替换第一处匹配**": 要改多处就写多段 (`edits`); 因此
      `applyEditSegments` 去掉 `multiReplace` 形参, 只做一次 `find` + `replace`,
      也不再返回处数列表 (返回 void)。同步 (`fileEditExecuteImpl`) 与协程
      (`fileEditExecuteAsyncImpl`) 两个执行体都去掉 `multi_replace` 取值
    - 结果文本统一为 `success` (与 `write`/`read` 等工具一致): 每段各替换一处,
      原先 `Success, Replace N hits` / `Success, Replace T hits in K segments: ...`
      是为 `multi_replace` 的"不知替换了几处"而设, 一并删除 `editResultText`
    - client 特化渲染随之简化: 折叠头摘要只按参数算 (`diffStatTextOfSegments(segments)`
      各段逐行 diff 求和), 删掉结果文本解析 `parseEditSegmentHits` (及 `hits` 形参) —
      折叠头在工具运行中与完成后不再有差异, 也不再需要"结果文本格式与解析必须同步"
      的隐式约定
    - 参数与提示词: 插件工具模式去掉 `multi_replace` 属性, `edits`/`old_str` 描述补
      "each item replaces the first match"; lib 的 `prompt.cpp` 同一份描述同步
      (含 `edits` 例子); 插件与 lib 两处文案保持一致
    - 受影响测试改为覆盖新语义: `test_edit_text_file_first_match_only` (同一文本出现
      3 次只改第 1 处)、`test_edit_text_file_crlf_first_match` (CRLF 文件只改第 1 处
      并落盘为 LF)、`test_edit_text_file_multi_segments_first_match` (多段各自只改
      第一处); 删除 `multi_replace` / 处数解析相关用例 (共约 60 条断言)
    - 文档: `docs/{zh-cn,en}/design/index.md` 工具表的 edit 行补"每段只替换第一处"
