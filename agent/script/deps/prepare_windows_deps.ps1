# =============================================================================
# agentxx Windows dependency self-build script (Hermetic deps, PowerShell)
# -----------------------------------------------------------------------------
# Called by windows_debug_build.bat / windows_release_build.bat.
# Goal: Boost / OpenSSL / ragel are all built/fetched locally by this repo,
#       not from system (choco/vcpkg/prebuilt exe) packages.
#
# Artifacts are placed under {SrcDir}/third_party/:
#   boost-windows-build-{debug|release}  -> Boost 1.92 built with local MSVC b2
#   OpenSSL-windows-build                -> OpenSSL 4.0.1 built with local MSVC nmake
#   tools/ragel-<ver>/bin/ragel.exe      -> locally built ragel (hyperscan parser)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File deps\prepare_windows_deps.ps1 `
#       -Mode Debug|Release -SrcDir <agent\third_party> [-SkipBoost] [-SkipOpenSSL]
# Exit code: 0 = ok; non-zero = failed
# =============================================================================
param(
    [Parameter(Mandatory=$true)][ValidateSet("Debug","Release")][string]$Mode,
    [Parameter(Mandatory=$true)][string]$SrcDir,
    [switch]$SkipBoost,
    [switch]$SkipOpenSSL,
    [int]$Parallel = 4
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # speed up Invoke-WebRequest

function Write-Info  { Write-Host "[deps] $args" -ForegroundColor Cyan }
function Write-Warn  { Write-Host "[deps][WARN] $args" -ForegroundColor Yellow }
function Write-Err   { Write-Host "[deps][ERROR] $args" -ForegroundColor Red }

# Download helper with optional sha256 check
function Download-File {
    param([string]$Url, [string]$Dest, [string]$Sha = "")
    if ((Test-Path $Dest) -and $Sha) {
        $h = (Get-FileHash -Algorithm SHA256 $Dest).Hash.ToLower()
        if ($h -eq $Sha.ToLower()) { Write-Info "cache hit: $(Split-Path $Dest -Leaf)"; return }
    }
    Write-Info "download $Url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
    if ($Sha) {
        $h = (Get-FileHash -Algorithm SHA256 $Dest).Hash.ToLower()
        if ($h -ne $Sha.ToLower()) { Write-Err "sha256 mismatch: $Dest"; throw "sha256 mismatch" }
    }
}

function Get-Vswhere {
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return ""
}

# ===== locate MSVC developer prompt =====
function Find-VsDevCmd {
    $vswhere = Get-Vswhere
    if ($vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $p = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($p in @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat")) {
        if (Test-Path $p) { return $p }
    }
    return ""
}

# run a command line inside the MSVC developer environment (cmd), return stdout
function Invoke-InVcEnv {
    param([string]$VsDevCmd, [string]$Command)
    $tmp = Join-Path $env:TEMP "agentxx_vcenv_$PID.cmd"
    Set-Content -Path $tmp -Value "@call `"$VsDevCmd`" -arch=x64 -host_arch=x64 >nul 2>&1`r`n@$Command`r`n" -Encoding Ascii
    try {
        return (& cmd.exe /c "`"$tmp`"" 2>&1)
    } finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
}

# ===== Boost (MSVC b2) =====
function Ensure-Boost {
    $variant = if ($Mode -eq "Debug") { "debug" } else { "release" }
    $install  = Join-Path $SrcDir "boost-windows-build-$variant"
    $boostSrc = Join-Path $SrcDir "boost"
    # Reuse existing artifacts (user prebuilt / previous success); a partial
    # install missing key files triggers a rebuild.
    if ((Test-Path "$install\include\boost\version.hpp") -and
        (Test-Path "$install\lib\libboost_filesystem.lib")) {
        Write-Info "Boost($variant) artifacts exist, reuse: $install"
        return
    }
    if (-not (Test-Path "$boostSrc\bootstrap.bat")) {
        Write-Info "Boost source not found: $boostSrc, downloading boost-1.92.0 ..."
        New-Item -ItemType Directory -Force -Path (Join-Path $SrcDir "tools") | Out-Null
        $arc = Join-Path $SrcDir "tools\boost_1_92_0.tar.gz"
        Download-File "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.gz" $arc
        $tmpDir = Join-Path $SrcDir "tools\_boost_extract"
        if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
        New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
        tar -xzf $arc -C $tmpDir
        if (-not (Test-Path "$tmpDir\boost_1_92_0")) { throw "boost archive layout unexpected" }
        if (Test-Path $boostSrc) { Remove-Item -Recurse -Force $boostSrc }
        Move-Item "$tmpDir\boost_1_92_0" $boostSrc
        Remove-Item -Recurse -Force $tmpDir
        Remove-Item $arc -ErrorAction SilentlyContinue
    }
    $vsDevCmd = Find-VsDevCmd
    if (-not $vsDevCmd) { throw "Visual Studio not found (need VsDevCmd.bat)" }

    Write-Info "=============================================="
    Write-Info "start self-building Boost($variant) with MSVC"
    Write-Info "  source: $boostSrc"
    Write-Info "  install: $install"
    Write-Info "=============================================="
    New-Item -ItemType Directory -Force -Path $install | Out-Null

    # bootstrap b2
    if (-not (Test-Path "$boostSrc\b2.exe")) {
        Write-Info "bootstrapping b2 ..."
        Invoke-InVcEnv $vsDevCmd "cd /d `"$boostSrc`" && bootstrap.bat" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bootstrap.bat failed" }
    }
    $rtdebug = if ($Mode -eq "Debug") { "on" } else { "off" }
    $b2cmd = "cd /d `"$boostSrc`" && b2.exe install --layout=system --prefix=`"$install`" link=static runtime-link=shared runtime-debugging=$rtdebug address-model=64 variant=$variant -j$Parallel"
    Invoke-InVcEnv $vsDevCmd $b2cmd | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "b2 build Boost failed" }

    if (-not (Test-Path "$install\include\boost\version.hpp") -or
        -not (Test-Path "$install\lib\libboost_filesystem.lib")) {
        throw "Boost($variant) install artifacts missing: $install"
    }
    Write-Info "Boost($variant) done: $install"
}

# ===== OpenSSL (MSVC nmake, no-asm to skip nasm) =====
function Ensure-OpenSSL {
    $install = Join-Path $SrcDir "OpenSSL-windows-build"
    $osslSrc = Join-Path $SrcDir "openssl-4.0.1"
    if (-not (Test-Path "$osslSrc\Configure")) {
        # accept other openssl* dir names
        $cand = Get-ChildItem -Path $SrcDir -Directory -Filter "openssl*" | Where-Object { Test-Path "$($_.FullName)\Configure" } | Select-Object -First 1
        if ($cand) {
            $osslSrc = $cand.FullName
        } else {
            # source dir is not in git, auto-download openssl-4.0.1
            Write-Info "OpenSSL source not found, downloading openssl-4.0.1 ..."
            New-Item -ItemType Directory -Force -Path (Join-Path $SrcDir "tools") | Out-Null
            $arc = Join-Path $SrcDir "tools\_openssl-4.0.1.tar.gz"
            Download-File "https://github.com/openssl/openssl/archive/refs/tags/openssl-4.0.1.tar.gz" $arc
            $tmpDir = Join-Path $SrcDir "tools\_ossl_extract"
            if (Test-Path $tmpDir) { Remove-Item -Recurse -Force $tmpDir }
            New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
            tar -xzf $arc -C $tmpDir
            # github archive extracts as openssl-openssl-<tag>
            $extDir = Get-ChildItem -Path $tmpDir -Directory -Filter "openssl-*" | Select-Object -First 1
            if (-not $extDir) { throw "openssl archive layout unexpected" }
            if (Test-Path $osslSrc) { Remove-Item -Recurse -Force $osslSrc }
            Move-Item $extDir.FullName $osslSrc
            Remove-Item -Recurse -Force $tmpDir
            Remove-Item $arc -ErrorAction SilentlyContinue
        }
    }
    # Reuse existing artifacts (user prebuilt install / previous success)
    if ((Test-Path "$install\include\openssl\opensslv.h") -and
        ((Test-Path "$install\lib\libssl.lib") -or (Test-Path "$install\lib64\libssl.lib"))) {
        Write-Info "OpenSSL artifacts exist, reuse: $install"
        return
    }
    $vsDevCmd = Find-VsDevCmd
    if (-not $vsDevCmd) { throw "Visual Studio not found (need VsDevCmd.bat)" }

    Write-Info "=============================================="
    Write-Info "start self-building OpenSSL with MSVC (no-asm, no nasm needed)"
    Write-Info "  source: $osslSrc"
    Write-Info "  install: $install"
    Write-Info "=============================================="
    New-Item -ItemType Directory -Force -Path $install | Out-Null

    if (-not (Get-Command perl -ErrorAction SilentlyContinue)) {
        throw "OpenSSL MSVC build needs perl on PATH (install Strawberry Perl)"
    }

    $steps = @(
        "cd /d `"$osslSrc`" && perl Configure VC-WIN64A no-shared no-asm no-tests no-docs --prefix=`"$install`" --openssldir=`"$install`"",
        "cd /d `"$osslSrc`" && nmake",
        "cd /d `"$osslSrc`" && nmake install_sw"
    )
    foreach ($s in $steps) {
        Write-Info "exec: $s"
        Invoke-InVcEnv $vsDevCmd $s | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "OpenSSL build step failed: $s" }
    }
    if (-not (Test-Path "$install\include\openssl\opensslv.h") -or
        -not (Test-Path "$install\lib\libssl.lib")) {
        throw "OpenSSL install artifacts missing: $install"
    }
    Write-Info "OpenSSL done: $install"
}

# ===== ragel (hyperscan parser generator; no official Windows build) =====
# Prefer locally staged exe, else fall back to system (winget) install.
# Download source: PolarGoose/Ragel-for-Windows release (from winget manifest
# PolarGoose.Ragel 6.10; sha256 pinned by winget upstream):
#   https://github.com/PolarGoose/Ragel-for-Windows/releases/download/ragel-6.10/Ragel.zip
function Ensure-Ragel {
    $ver = if ($env:AGENTXX_RAGEL_VERSION) { $env:AGENTXX_RAGEL_VERSION } else { "7.0.4" }
    $toolsDir = Join-Path $SrcDir "tools"
    $ragelDir  = Join-Path $toolsDir "ragel-$ver"
    $ragelExe  = Join-Path $ragelDir "bin\ragel.exe"
    if (Test-Path $ragelExe) {
        Write-Info "ragel artifacts exist, reuse: $ragelExe"
        return
    }
    $sysRagel = Get-Command ragel -ErrorAction SilentlyContinue
    if ($sysRagel -and $env:AGENTXX_DEPS_FORCE -ne "1") {
        Write-Warn "system ragel found: $($sysRagel.Source), use it (not self-built)"
        return
    }
    # self-stage ragel.exe from the winget-pinned prebuilt zip
    $zipUrl = "https://github.com/PolarGoose/Ragel-for-Windows/releases/download/ragel-6.10/Ragel.zip"
    $zipSha = "35A3690BADCEF862A209096140437CB81E0A9DF257078E3C42D9203042F3902C"
    $tmpZip = Join-Path $toolsDir "_ragel.zip"
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $ragelDir "bin") | Out-Null
        Write-Info "download ragel 6.10 prebuilt zip"
        Download-File $zipUrl $tmpZip $zipSha
        # zip contains Ragel.exe at top level
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead($tmpZip)
        try {
            $entry = $zip.Entries | Where-Object { $_.Name -ieq "Ragel.exe" } | Select-Object -First 1
            if (-not $entry) { throw "Ragel.exe not found in zip" }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $ragelExe, $true)
        } finally {
            $zip.Dispose()
        }
        Remove-Item $tmpZip -ErrorAction SilentlyContinue
        Write-Info "ragel staged: $ragelExe"
    } catch {
        Write-Warn "staging ragel failed: $($_.Exception.Message)"
        Write-Warn "fallback hints:"
        Write-Warn "  1) winget install PolarGoose.Ragel   (system fallback)"
        Write-Warn "  2) put ragel.exe into $ragelExe"
        Write-Warn "  3) disable hyperscan: add -DAGENTXX_ENABLE_HYPERSCAN=OFF to build script"
        $sysRagel2 = Get-Command ragel -ErrorAction SilentlyContinue
        if ($sysRagel2) {
            Write-Warn "system ragel found: $($sysRagel2.Source), use it"
            return
        }
        throw "ragel unavailable (hyperscan needs it)"
    }
}

# ===== main =====
Write-Info "prepare_windows_deps.ps1 Mode=$Mode SrcDir=$SrcDir"
New-Item -ItemType Directory -Force -Path $SrcDir | Out-Null

# Skip* switches, or existing artifacts, avoid redundant work
if (-not $SkipBoost)   { Ensure-Boost }
if (-not $SkipOpenSSL) { Ensure-OpenSSL }
Ensure-Ragel

Write-Info "windows deps ready"
exit 0
