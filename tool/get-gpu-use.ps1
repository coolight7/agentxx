# 获取所有 GPU 引擎占用率
$counters = Get-Counter "\GPU Engine(*)\Utilization Percentage"
$samples = $counters.CounterSamples

# ---- 提取完整 LUID（两个 32 位部分）作为分组键 ----
$luidGroups = $samples | Group-Object {
    if ($_.InstanceName -match "luid_(0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)") {
        "$($matches[1])"   # 合并为完整 LUID，如 "0x00000000_0x000108F6"
    }
} | Where-Object { $_.Name -ne $null }

# ---- 获取系统中实际的视频控制器（用于名称和顺序） ----
$gpuControllers = Get-CimInstance -ClassName Win32_VideoController |
Where-Object { $_.Name -notmatch "Microsoft|Remote|Virtual|Hyper-V" } |
Sort-Object DeviceID

# ---- 准备结果容器 ----
$result = @()

# ---- 按完整 LUID 排序（先按第一部分，再按第二部分） ----
$luidSorted = $luidGroups | Sort-Object {
    $parts = $_.Name -split '_'
    [Convert]::ToInt64($parts[0], 16) * 1e9 + [Convert]::ToInt64($parts[1], 16)
}
$memCounter = Get-Counter "\GPU Process Memory(*)\Dedicated Usage" -ErrorAction SilentlyContinue

$index = 0
foreach ($group in $luidSorted) {
    $fullLuid = $group.Name   # 形如 "00000000_000108F6"
    $items = $group.Group

    # 过滤无效值
    $valid = $items | Where-Object { $_.CookedValue -ne $null }
    if (-not $valid) { continue }

    # 计算最大和平均占用
    $maxUtil = ($valid | Measure-Object -Property CookedValue -Maximum).Maximum
    $avgUtil = ($valid | Measure-Object -Property CookedValue -Average).Average

    # ---- 根据引擎类型判断厂商 ----
    $engineTypes = $items | ForEach-Object {
        if ($_.InstanceName -match "engtype_(\w+)") { $matches[1] }
    } | Sort-Object -Unique

    $vendor = "未知"
    if ($engineTypes -contains "Cuda") { $vendor = "NVIDIA" }
    elseif ($engineTypes -contains "High Priority Compute") { $vendor = "AMD" }
    elseif ($engineTypes -contains "Video JPEG") { $vendor = "AMD" }

    # 如果引擎无法判断，尝试从对应的控制器名称推断
    if ($vendor -eq "未知" -and $index -lt $gpuControllers.Count) {
        $ctrlName = $gpuControllers[$index].Name
        if ($ctrlName -match "NVIDIA") { $vendor = "NVIDIA" }
        elseif ($ctrlName -match "AMD|Radeon|ATI") { $vendor = "AMD" }
    }

    # ---- 获取控制器名称（按顺序对应，可手动调整） ----
    $controllerName = if ($index -lt $gpuControllers.Count) { $gpuControllers[$index].Name } else { "未知设备" }
    # 额外校准：如果名称包含 NVIDIA/AMD，但 vendor 仍未知，则纠正
    if ($controllerName -match "NVIDIA" -and $vendor -eq "未知") { $vendor = "NVIDIA" }
    if ($controllerName -match "AMD|Radeon|ATI" -and $vendor -eq "未知") { $vendor = "AMD" }

    # ---- 显存占用（专用显存） ----
    $memTotalMB = $null
    if ($memCounter) {
        $memSamples = $memCounter.CounterSamples | Where-Object {
            $_.InstanceName -match "luid_$fullLuid"   # 匹配完整 LUID
        }
        if ($memSamples) {
            $memTotalMB = ($memSamples | Measure-Object -Property CookedValue -Sum).Sum / 1MB
        }
    }

    # 构建输出对象
    $result += [PSCustomObject]@{
        GPU_Index = "GPU$index"
        完整LUID    = "$fullLuid"
        厂商        = $vendor
        控制器名称     = $controllerName
        最大占用      = "{0:F2}%" -f $maxUtil
        平均占用      = "{0:F2}%" -f $avgUtil
        显存占用      = if ($memTotalMB) { "{0:F2} MB" -f $memTotalMB } else { "无法获取" }
    }
    $index++
}

# 输出表格
$result | Format-Table -AutoSize