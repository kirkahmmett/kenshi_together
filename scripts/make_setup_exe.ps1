<#
.SYNOPSIS
  Wrap the mod kit (dist\mod-kit) into ONE double-click setup exe with
  Windows' built-in IExpress: no unzipping, no hunting for the .cmd.

.DESCRIPTION
  IExpress packages a flat file list (no folders), so the KenshiCoop mod
  folder rides inside as a zip. The exe unpacks to a temp folder and runs
  KenshiTogether-Install.cmd, which expands that zip next to itself and hands
  over to the regular KenshiCoop-Setup.cmd (find Kenshi, RE_Kenshi, mod, enable).
  The temp folder is removed when the setup window closes.

  The exe is unsigned: Windows SmartScreen may say "Windows protected your PC"
  on first run ("More info" -> "Run anyway").

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\make_setup_exe.ps1 -Version 0.65.0
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,
    # Default: <repo>\dist\KenshiTogether-Setup-v<Version>.exe
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$kitDir   = Join-Path $repoRoot "dist\mod-kit"
if (-not (Test-Path (Join-Path $kitDir "KenshiCoop\KenshiCoop.dll"))) {
    throw "no kit in $kitDir - run make_mod_kit.ps1 first"
}
if (-not $Out) { $Out = Join-Path $repoRoot "dist\KenshiTogether-Setup-v$Version.exe" }
$iexpress = Join-Path $env:WINDIR "System32\iexpress.exe"
if (-not (Test-Path $iexpress)) { throw "IExpress not found ($iexpress)" }

# Flat staging folder.
$stage = Join-Path $env:TEMP "KenshiTogetherSetupStage"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Compress-Archive -Path (Join-Path $kitDir "KenshiCoop") -DestinationPath (Join-Path $stage "KenshiCoop-mod.zip")
$flat = @("KenshiCoop-Setup.ps1", "KenshiCoop-Setup.cmd", "README.txt", "KURULUM_TR.txt", "PROVENANCE.json")
foreach ($f in $flat) {
    $src = Join-Path $kitDir $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $stage $f) }
}
# Bootstrap: expand the mod folder next to us, then the regular installer.
@'
@echo off
setlocal
cd /d "%~dp0"
title Kenshi Together setup
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -Force -LiteralPath '%~dp0KenshiCoop-mod.zip' -DestinationPath '%~dp0'"
if errorlevel 1 (
  echo Could not unpack the mod files.
  pause
  exit /b 1
)
call "%~dp0KenshiCoop-Setup.cmd" %*
exit /b %ERRORLEVEL%
'@ | Set-Content (Join-Path $stage "KenshiTogether-Install.cmd") -Encoding ASCII

$files = @(Get-ChildItem -LiteralPath $stage -File | Select-Object -ExpandProperty Name)
$strings = New-Object System.Text.StringBuilder
$list    = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $files.Count; $i++) {
    [void]$strings.AppendLine("FILE$i=`"$($files[$i])`"")
    [void]$list.AppendLine("%FILE$i%=")
}
$sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles
[Strings]
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$Out
FriendlyName=Kenshi Together $Version setup
AppLaunched=cmd /c KenshiTogether-Install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
$($strings.ToString())
[SourceFiles]
SourceFiles0=$stage\
[SourceFiles0]
$($list.ToString())
"@
$sedPath = Join-Path $stage "..\KenshiTogetherSetup.sed"
Set-Content -LiteralPath $sedPath -Value $sed -Encoding ASCII
if (Test-Path $Out) { Remove-Item $Out -Force }
$p = Start-Process -FilePath $iexpress -ArgumentList @("/N", "/Q", $sedPath) -Wait -PassThru -WindowStyle Hidden
if (-not (Test-Path $Out)) { throw "IExpress did not produce $Out (exit $($p.ExitCode))" }
Remove-Item $stage -Recurse -Force
Remove-Item $sedPath -Force
$size = [Math]::Round((Get-Item $Out).Length / 1KB)
Write-Host "Setup exe: $Out ($size KB)"
