# SPDX-License-Identifier: GPL-2.0-only
# (Administrator) Install pkg\jslcodec.inf on the RT5682 codec (ACPI\10EC5682), replacing any previous
# jslcodec package, restart the device and print its state.
#   -LogPath <file>   write the driver log to <file> instead of %SystemRoot%\Temp\jslcodec.log
param([string]$LogPath)
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Write-Error "Run this script from an elevated (Administrator) PowerShell."; exit 1 }
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$inf  = Join-Path $root 'pkg\jslcodec.inf'
if (-not (Test-Path $inf)) { throw "build first: $inf not found" }
$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\jslcodec'
function Set-LogPath { if ($LogPath -and (Test-Path $svc)) { Set-ItemProperty $svc -Name LogPath -Value ('\??\' + [IO.Path]::GetFullPath($LogPath)) } }

$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like 'ACPI\10EC5682*' } | Select-Object -First 1
if (-not $dev) { throw 'RT5682 (ACPI\10EC5682) not found' }

$old = (pnputil /enum-drivers) -join "`n" |
    Select-String -AllMatches -Pattern 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+jslcodec\.inf' |
    ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }
foreach ($x in $old) { pnputil /delete-driver $x /uninstall /force }

Set-LogPath
pnputil /add-driver $inf /install
Set-LogPath
pnputil /restart-device $dev.InstanceId
Start-Sleep 3
$d = Get-PnpDevice -InstanceId $dev.InstanceId
"Device: $($d.FriendlyName)  Status=$($d.Status)  Problem=$($d.Problem)"
