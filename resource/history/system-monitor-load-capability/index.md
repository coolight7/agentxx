# agentxx_system_monitor 能力: 读取当前系统负担 (占用 + 内存/显存可用量与总量)
- 难度: B
- 类型: 功能
- 基于commit: a550ea42a962a852ef0fd15f00196de388dc3524
- 时间: 2026-10-20 22:09
- 需求:
```md
请修改插件 agentxx_system_monitor 加载时向主程序注册能力 `读取当前系统负担`，提供 cpu 占用、gpu 占用、内存占用、显存占用，其中内存和显存要提供 可用量和总量。以便其他插件、ffi 可以调用该能力
```
- 实现要点:
    - 能力名沿用既有公开契约 `agentxx.system_usage` (agent 侧周期采集、client 侧 Info
      段、测试模块 `cpu_gpu` 都按此名取值), 本次只改载荷与调用方式, 不新增第二个名字
    - 内存可用量改为**平台直接读取**并进结构体 (`MemoryInfo::availablePhysicalMB`):
      Windows `GlobalMemoryStatusEx().ullAvailPhys`、Linux/Android `/proc/meminfo` 的
      `MemAvailable` (老内核缺该行时退回 `MemFree`, 并按"可用量 ≤ 总量"夹紧, 避免已用量
      下溢)、macOS `HOST_VM_INFO64` 的可回收页合计; 各平台已用量本就由可用量反推,
      三者互补, 能力载荷里的 `mem_available_mb` 优先用它, 缺失时按 `total - used` 兜底
    - 能力载荷 (`usageToJson`, 同时是工具文本与周期事件的数据源) 新增:
      - `mem_available_mb` (与 `mem_total_mb`/`mem_used_mb`/`mem_percent` 并列)
      - 顶层合计 `gpu_count` / `gpu_peak_percent` / `vram_total_mb` /
        `vram_used_mb` / `vram_available_mb` (不想遍历数组的调用方直接读)
      - 逐卡 `vram_source` (`dedicated`/`shared`/`none`) + `vram_total_mb` /
        `vram_used_mb` / `vram_available_mb`, 以及 `dedicated_vram_*` /
        `shared_vram_*` 的 total/used/**available** 三件套 (原有字段全部保留, 向后兼容)
    - 显存取值集中到一个 helper (`vramOf`): 有独立显存用独立显存, 统一内存 GPU / 核显
      退回共享显存, 都没有时 `none` —— 工具文本与能力载荷共用, 不会出现两套口径;
      "可用量 = 总量 - 已用", 驱动不上报已用时可用量等于总量 (Linux NVIDIA 等)
    - 能力从同步处理改为**异步** (`Task<std::string>` + `offload`): 查询要读系统/GPU
      计数器 (实测 >100ms), 原先在 IO 线程同步执行会触发
      `Plugin operation ... start blocked IO for over 100ms`; 改后其他插件/主程序
      可随时调用而不压住主程序 IO 线程
    - 工具 `agentxx_get_system_core_info` 文本同步补上可用量:
      `Memory: 58.2% · Used: 30.2G / Total: 51.9G · Available: 21.7G`、
      `GPU 0 [..]: GPU Usage: 12.4%, VRAM: 1614MB Used / 4079MB Total / 2465MB Available`;
      驱动没有任何显存字段时不再输出 `0MB Used / 0MB Total`
    - 版本: 插件 `1.1.0` → `1.2.0` (agent/client 导出口与 `plugin.yaml` 同步), 描述写明
      能力的语义
    - 调用路径 (文档 `docs/{zh-cn,en}/design/plugins.md` §8 新增"内置能力"段说明):
      其他插件 `invoke_cap` / `invoke_capability_blocking`; 主程序侧
      `PluginManager::invokeCapabilityAsync` (须携带调用方插件实例租约, 主程序自身调用
      传提供者实例即可; FFI 的 C 接口目前没有单独的能力调用入口)
    - 测试模块 `cpu_gpu` 补断言: `mem_available_mb` 与 `mem_used_mb` 互补、
      逐卡 `vram_source` 与所选来源字段一致、可用量 = 总量 - 已用、顶层合计等于逐卡求和
