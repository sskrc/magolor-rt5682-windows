# SPDX-License-Identifier: GPL-2.0-only
# Build driver\jslcodec.c (KMDF 1.15) with clang-cl + WDK headers/libs, then package and test-sign it into pkg\.
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -LlvmBin <dir> -WdkRoot <dir> -SdkRoot <dir>
param(
    [string]$LlvmBin     = $(if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' }),
    [string]$WdkRoot     = $env:WDK_ROOT,
    [string]$SdkRoot     = $env:SDK_ROOT,
    [string]$KitVersion  = '10.0.26100.0',
    [string]$CertSubject = 'CN=JSL Audio Test'
)
$ErrorActionPreference = 'Stop'
if (-not $WdkRoot -or -not $SdkRoot) { throw 'set -WdkRoot and -SdkRoot (or WDK_ROOT / SDK_ROOT)' }
$root = Split-Path $PSScriptRoot -Parent
$out  = Join-Path $root 'out'
$pkg  = Join-Path $root 'pkg'
New-Item -ItemType Directory -Force $out, $pkg | Out-Null

$inc = @("$WdkRoot\Include\$KitVersion\km", "$WdkRoot\Include\$KitVersion\km\crt", "$WdkRoot\Include\$KitVersion\shared",
         "$WdkRoot\Include\wdf\kmdf\1.15", "$SdkRoot\Include\$KitVersion\shared")
$cl = @('/c','/kernel','/GS-','/Gy','/Zi','/O1','/W3','-Wno-everything','-fms-extensions','-m64',
        '/D_AMD64_','/DAMD64','/D_WIN64','/DNDEBUG','/DNTDDI_VERSION=0x0A00000C','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00',
        '/DKMDF_VERSION_MAJOR=1','/DKMDF_VERSION_MINOR=15','/DPOOL_NX_OPTIN=1')
foreach ($i in $inc) { $cl += '-imsvc'; $cl += $i }
& "$LlvmBin\clang-cl.exe" @cl (Join-Path $root 'driver\jslcodec.c') "/Fo$out\jslcodec.obj"
if ($LASTEXITCODE) { throw 'compile failed' }

$ln = @('/DRIVER','/SUBSYSTEM:NATIVE','/ENTRY:FxDriverEntry','/NODEFAULTLIB','/RELEASE','/NXCOMPAT','/DYNAMICBASE',
        '/MERGE:_TEXT=.text','/MERGE:.retplne=.rdata','/MERGE:_guard_c=.rdata','/MERGE:_guard_d=.rdata','/MERGE:memset_=.text',
        '/MERGE:_PAGE=PAGE','/GUARD:NO','/HIGHENTROPYVA:NO','/TSAWARE:NO','/BASE:0x10000','/INTEGRITYCHECK','/OPT:REF',
        '/DEBUG','/MACHINE:X64', "/OUT:$out\jslcodec.sys", "$out\jslcodec.obj",
        'ntoskrnl.lib','hal.lib','ntstrsafe.lib','wdfldr.lib','wdfdriverentry.lib','BufferOverflowFastFailK.lib',
        "/LIBPATH:$WdkRoot\Lib\$KitVersion\km\x64", "/LIBPATH:$WdkRoot\Lib\wdf\kmdf\x64\1.15")
& "$LlvmBin\lld-link.exe" @ln
if ($LASTEXITCODE) { throw 'link failed' }

Copy-Item "$out\jslcodec.sys", (Join-Path $root 'driver\jslcodec.inf') $pkg -Force
$bin = "$WdkRoot\bin\$KitVersion"
& "$bin\x64\stampinf.exe" -f "$pkg\jslcodec.inf" -a amd64 -k 1.15 -v '*' | Out-Null
& "$bin\x86\Inf2Cat.exe" /driver:$pkg /os:10_x64 | Select-Object -Last 1
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $CertSubject } | Select-Object -First 1
if (-not $cert) { throw "no code-signing certificate '$CertSubject' in CurrentUser\My" }
& "$SdkRoot\bin\$KitVersion\x64\signtool.exe" sign /fd SHA256 /sha1 $cert.Thumbprint "$pkg\jslcodec.sys" "$pkg\jslcodec.cat" | Select-Object -Last 2
