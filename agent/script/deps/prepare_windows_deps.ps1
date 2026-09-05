# =============================================================================
# agentxx Windows dependency self-build script (Hermetic deps, PowerShell)
# -----------------------------------------------------------------------------
# Called by windows_debug_build.bat / windows_release_build.bat.
# Goal: Boost / OpenSSL / ragel are all built/fetched locally by this repo,
#       not from system (choco/vcpkg) packages.
#
# Artifacts are placed under {SrcDir}/third_party/:
#   boost-windows-build-{debug|release}  -> Boost 1.92 built with local MSVC b2
#   OpenSSL-windows-build                -> OpenSSL 4.x prebuilt for MSVC
#        downloaded from slproweb (https://slproweb.com/products/Win32OpenSSL.html)
#        and silently installed to this dir. No perl / nasm needed.
#   tools/ragel-<ver>/bin/ragel.exe      -> locally staged ragel (hyperscan parser)
#
# OpenSSL version selection:
#   - default: the newest 4.x from slproweb hash manifest
#     (https://slproweb.com/download/win32_openssl_hashes.json), with a pinned
#     4.0.2 last-chance fallback when the manifest is unreachable
#   - override: set env AGENTXX_OPENSSL_VERSION, e.g. 4.0.1 / 4.0.2
# Existing artifacts in OpenSSL-windows-build are reused as-is.
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

# Download helper with optional sha256 check.
# Uses WebClient with TLS1.2+ and retries: Invoke-WebRequest often dies with
# "unexpected EOF" on slproweb's large installers.
function Download-File {
    param([string]$Url, [string]$Dest, [string]$Sha = "")
    if ((Test-Path $Dest) -and $Sha) {
        $h = (Get-FileHash -Algorithm SHA256 $Dest).Hash.ToLower()
        if ($h -eq $Sha.ToLower()) { Write-Info "cache hit: $(Split-Path $Dest -Leaf)"; return }
    }
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
    $wc = New-Object System.Net.WebClient
    $attempt = 0
    while ($true) {
        $attempt++
        try {
            Write-Info "download $Url (attempt $attempt)"
            $wc.DownloadFile($Url, $Dest)
            break
        } catch {
            if ($attempt -ge 3) { throw "download failed after 3 attempts: $($_.Exception.Message)" }
            Write-Warn "download attempt $attempt failed: $($_.Exception.Message), retrying ..."
            Start-Sleep -Seconds 3
        }
    }
    $wc.Dispose()
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

# ===== locate MSVC developer prompt (vswhere first, dir scan fallback) =====
function Find-VsDevCmd {
    $vswhere = Get-Vswhere
    if ($vswhere) {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vs) {
            $p = Join-Path $vs "Common7\Tools\VsDevCmd.bat"
            if (Test-Path $p) { return $p }
        }
    }
    # fallback: scan any "<version>\<edition>" layout under both ProgramFiles
    # roots (VS2022/2026 use C:\Program Files\Microsoft Visual Studio\NN\...,
    # VS2019 used C:\Program Files (x86)\...), no hard-coded install paths.
    foreach ($pf in @(${env:ProgramFiles}, ${env:ProgramFiles(x86)})) {
        if (-not $pf -or -not (Test-Path $pf)) { continue }
        foreach ($verDir in Get-ChildItem -Path (Join-Path $pf "Microsoft Visual Studio") -Directory -ErrorAction SilentlyContinue) {
            foreach ($edDir in Get-ChildItem -Path $verDir.FullName -Directory -ErrorAction SilentlyContinue) {
                $p = Join-Path $edDir.FullName "Common7\Tools\VsDevCmd.bat"
                if (Test-Path $p) { return $p }
            }
        }
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

# ===== OpenSSL (slproweb prebuilt for MSVC, silent install, no perl) =====
# slproweb provides Win64 dev installers which contain everything CMake
# needs (include/ + lib/VC/x64/{MD,MDd,MT,MTd} + bin/). We download the EXE
# pinned by sha256 from the official hash manifest and install it straight
# into third_party/OpenSSL-windows-build.
# NOTE: the slproweb 4.x installer requires elevation. When this script is
# not running elevated (typical desktop use) the install step triggers one
# UAC prompt via -Verb RunAs; on CI (admin runner) it installs silently.
# perl / nasm are never needed.
function Get-OpenSSL-Meta {
    param([string]$Version, [string]$JsonUrl, [string]$FileKey)
    # returns @{ version; url; sha256; installer }
    try {
        $json = Invoke-RestMethod -Uri $JsonUrl -UseBasicParsing
        $f = $json.files.$FileKey
        if (-not $f) { throw "file entry not found: $FileKey" }
        return @{ version = $Version; url = $f.url; sha256 = $f.sha256; installer = $f.installer }
    } catch {
        Write-Warn "failed to resolve OpenSSL $Version metadata: $($_.Exception.Message)"
        return $null
    }
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Ensure-OpenSSL {
    $install = Join-Path $SrcDir "OpenSSL-windows-build"
    # Reuse existing artifacts (user prebuilt install / previous success).
    # Accept both install layouts:
    #   - this script's slproweb install:  include/ + lib/VC/x64/MD/libssl_static.lib
    #   - manual nmake install_sw:         include/ + lib/libssl.lib
    $haveInclude = Test-Path "$install\include\openssl\opensslv.h"
    $haveLib = (Test-Path "$install\lib\libssl.lib") -or
               (Test-Path "$install\lib\VC\x64\MD\libssl_static.lib")
    if ($haveInclude -and $haveLib) {
        Write-Info "OpenSSL artifacts exist, reuse: $install"
        return
    }
    $jsonUrl = "https://slproweb.com/download/win32_openssl_hashes.json"
    $toolsDir = Join-Path $SrcDir "tools"
    New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null

    # --- resolve target version + installer file ---
    $version = $env:AGENTXX_OPENSSL_VERSION
    $meta = $null
    if ($version) {
        # exact version requested (e.g. AGENTXX_OPENSSL_VERSION=4.0.2):
        # use the x64 (INTEL/64-bit) non-light EXE from the hash manifest
        $key = "Win64OpenSSL-$($version -replace '\.', '_').exe"
        Write-Info "AGENTXX_OPENSSL_VERSION set: OpenSSL $version"
        $meta = Get-OpenSSL-Meta $version $jsonUrl $key
        if (-not $meta) { throw "AGENTXX_OPENSSL_VERSION $version not found in slproweb hash manifest" }
    } else {
        # default: newest OpenSSL 4.x from the manifest
        Write-Info "resolving newest OpenSSL 4.x from $jsonUrl ..."
        try {
            $json = Invoke-RestMethod -Uri $jsonUrl -UseBasicParsing
            $pick = $json.files.PSObject.Properties |
                Where-Object { $_.Name -match '^Win64OpenSSL-(4_\d+_\d+).exe$' } |
                Sort-Object { [version]($_.Name -replace '^Win64OpenSSL-|\.exe$', '' -replace '_', '.') } -Descending |
                Select-Object -First 1
            if (-not $pick) { throw "no Win64OpenSSL 4.x in manifest" }
            $meta = @{ version = $pick.Name -replace '^Win64OpenSSL-|\.exe$', '' -replace '_', '.'
                       url = $pick.Value.url; sha256 = $pick.Value.sha256; installer = $pick.Value.installer }
        } catch {
            Write-Warn "failed to resolve newest 4.x from manifest: $($_.Exception.Message)"
        }
        if (-not $meta) {
            # Last-chance fallback when the manifest is unreachable:
            # pin the newest known Win64 dev EXE (update when slproweb bumps).
            Write-Warn "fallback to pinned Win64OpenSSL-4.0.2"
            $meta = @{
                version   = "4.0.2"
                url       = "https://slproweb.com/download/Win64OpenSSL-4_0_2.exe"
                sha256    = "39e840f7b94e2bf63e8edf4c57b383a8d11b883227d610a426b1a40626c8d9fc"
                installer = "exe"
            }
        }
    }
    Write-Info "OpenSSL prebuilt: $($meta.version) ($($meta.installer)), sha256 pinned"

    $arc = Join-Path $toolsDir "Win64OpenSSL-$($meta.version -replace '\.', '_').exe"
    Download-File $meta.url $arc $meta.sha256
    if (-not (Test-Path $arc)) { throw "OpenSSL installer download failed: $arc" }

    # --- install straight into OpenSSL-windows-build ---
    # "/DIR=" must be the last option passed to the NSIS installer.
    $oldOpenSslDir = Join-Path $SrcDir "OpenSSL-windows-build"
    if (Test-Path $oldOpenSslDir) {
        Write-Warn "replacing existing (possibly partial) OpenSSL-windows-build"
        Remove-Item -Recurse -Force $oldOpenSslDir
    }
    New-Item -ItemType Directory -Force -Path $install | Out-Null
    Write-Info "installing OpenSSL into $install ..."
    $args = @("/S", "/DIR=`"$install`"")
    $p = $null
    if (Test-IsAdmin) {
        # CI / elevated shells: silent install, no UAC
        $p = Start-Process -FilePath $arc -ArgumentList $args -PassThru -Wait
    } else {
        # Desktop: slproweb 4.x installer needs elevation; trigger one UAC
        # prompt (RunAs). User must click "Yes" once.
        Write-Warn "OpenSSL installer needs administrator rights, requesting elevation (accept the UAC prompt) ..."
        try {
            $p = Start-Process -FilePath $arc -ArgumentList $args -Verb RunAs -PassThru -Wait
        } catch {
            throw "elevated OpenSSL install failed/cancelled: $($_.Exception.Message)"
        }
    }
    if ($p.ExitCode -ne 0) { throw "OpenSSL installer failed with exit code $($p.ExitCode)" }
    if (-not (Test-Path "$install\include\openssl\opensslv.h")) {
        throw "OpenSSL install artifacts missing: $install (installer ran but layout unexpected?)"
    }
    Write-Info "OpenSSL $($meta.version) prebuilt installed: $install"
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
