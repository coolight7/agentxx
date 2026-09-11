#!/usr/bin/env bash
# check_sdk_negative_compile.sh —— SDK 反例编译检查 (plugin.md §11.1)
#
# 验证"错误签名必须编译失败"：hook/capability/tool 的 SDK helper 按返回类型
# 严格分发，错误签名（如 hook 返回 int）必须在编译期被拒绝，而不是运行期
# 静默降级。
#
# 实现方式：
# - 从测试构建的 compile_commands.json 提取 `plugin/test_plugin_sdk.cpp` 的真实
#   编译命令（include 路径/宏/标准/PCH 与正式构建一致），替换输入文件后逐个
#   编译 agent/test/plugin/negative_compile/*.cpp；
# - 除 positive_control.cpp（必须编译成功，确认环境有效）外，其余片段必须编译
#   失败；任一行为不符即退出码非 0。
#
# 用法: agent/script/check_sdk_negative_compile.sh [compile_commands_dir]
#   默认目录: agent/build/linux-debug/agentxx_test_repo-prefix/src/agentxx_test_repo-build
set -uo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
build_dir="${1:-${repo_root}/agent/build/linux-debug/agentxx_test_repo-prefix/src/agentxx_test_repo-build}"
neg_dir="${repo_root}/agent/test/plugin/negative_compile"

if ! command -v python3 >/dev/null 2>&1; then
    echo "[check_sdk_negative_compile] python3 not found" >&2
    exit 2
fi
if [ ! -f "${build_dir}/compile_commands.json" ]; then
    echo "[check_sdk_negative_compile] compile_commands.json not found under: ${build_dir}" >&2
    echo "  hint: build agentxx_test_repo first (CMake export ON)" >&2
    exit 2
fi
if [ ! -d "${neg_dir}" ]; then
    echo "[check_sdk_negative_compile] negative snippets not found: ${neg_dir}" >&2
    exit 2
fi

python3 - "${build_dir}" "${neg_dir}" <<'PY'
import json
import os
import shlex
import subprocess
import sys

build_dir, neg_dir = sys.argv[1], sys.argv[2]
with open(os.path.join(build_dir, "compile_commands.json"), encoding="utf-8") as f:
    entries = json.load(f)

anchor = next((e for e in entries if e.get("file", "").endswith("plugin/test_plugin_sdk.cpp")), None)
if anchor is None:
    print(
        "[check_sdk_negative_compile] anchor translation unit "
        "plugin/test_plugin_sdk.cpp not found in compile_commands.json",
        file=sys.stderr,
    )
    sys.exit(2)

raw = anchor.get("command")
cmd = shlex.split(raw) if raw else list(anchor.get("arguments", []))
if not cmd:
    print("[check_sdk_negative_compile] empty compile command for anchor TU", file=sys.stderr)
    sys.exit(2)

# 去掉源文件/输出与依赖文件参数: 只保留编译环境 (-I/-D/-std/PCH 等)
filtered = []
skip_next = False
drop_with_value = {"-o", "-MF", "-MT", "-MQ", "-MJ"}
for token in cmd:
    if skip_next:
        skip_next = False
        continue
    if token in drop_with_value:
        skip_next = True
        continue
    if token in {"-c", "-MD", "-MMD", anchor["file"]}:
        continue
    if token.startswith("-MF") and len(token) > 3:
        continue
    filtered.append(token)
filtered.append("-fsyntax-only")

workdir = anchor.get("directory") or build_dir

def compile_snippet(path):
    return subprocess.run(
        filtered + [path],
        cwd=workdir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

results = []
names = sorted(f for f in os.listdir(neg_dir) if f.endswith(".cpp"))
if not names:
    print("[check_sdk_negative_compile] no snippets found", file=sys.stderr)
    sys.exit(2)

for name in names:
    proc = compile_snippet(os.path.join(neg_dir, name))
    compiled = proc.returncode == 0
    expected_ok = name == "positive_control.cpp"
    passed = compiled if expected_ok else not compiled
    results.append((name, expected_ok, compiled, passed, proc))
    verdict = "OK" if passed else "FAIL"
    detail = "compiled" if compiled else "rejected"
    print(f"[check_sdk_negative_compile] {verdict}: {name} ({detail})")

bad = [r for r in results if not r[3]]
if bad:
    for name, expected_ok, compiled, _passed, proc in bad:
        want = "compile" if expected_ok else "be rejected"
        print(f"--- {name}: expected to {want}, but it {'compiled' if compiled else 'was rejected'} ---", file=sys.stderr)
        print((proc.stdout or "")[-4000:], file=sys.stderr)
    sys.exit(1)

print(f"[check_sdk_negative_compile] OK: {len(results)} snippets behave as expected")
PY
