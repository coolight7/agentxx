# 区分输入附件来源
- 难度: A
- 类型: 新增功能
- 基于commit: dc9d72cbbf10b6ff649e69cbd8ac6d73d4fc2013
- 时间: 2026-09-16 09:33
- 需求:
```md
检查tui选择附件来源，增加区分 server 跟 client 不同设备时，选择附件弹窗分两个tab区域支持选择附件，并且，不考虑保留历史版本兼容。请仔细思考规划实施方案
```
- 判断点:
    -   如果使用 deviceId 判断是否在同一设备，从 hostname 等名称取值则最终 key 建议转 md5，避免不同字符编码、特殊字符导致的各种问题
    -   选择 server 端的文件时，不应传输到 client 计算 base64 url 再发送 server，应当直接指定 path，由server自己加载