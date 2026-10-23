<#
.SYNOPSIS
  One-click KenshiCoop installer: finds Kenshi, installs RE_Kenshi (with the
  1.0.65 executable it needs on a 1.0.68 install), installs + enables the
  KenshiCoop mod, and can undo all of it.

.DESCRIPTION
  Every step here is one a player otherwise does by hand, and every one of them
  has a way to go silently wrong (plugin never loads, mod not enabled, wrong
  game version). The installer checks each precondition and says what it found.

    1. Locate Kenshi (Steam libraries, or -KenshiDir).
    2. Identify the executable by MD5 against RE_Kenshi's own version table.
    3. Get RE_Kenshi: a local -ReKenshiZip, or the pinned GitHub release
       (SHA-256 verified before anything is extracted).
    4. Install RE_Kenshi's files and register it in Plugins_x64.cfg. On Kenshi
       1.0.68 also build RE_Kenshi\<exe> - the 1.0.65 executable RE_Kenshi
       restarts into - with the bundled courgette patch, and refuse to keep it
       unless its MD5 is the 1.0.65 one. The original executable is never touched.
    5. Copy the KenshiCoop folder next to this script into <Kenshi>\mods and
       enable it in data\mods.cfg. An existing coop_config.json is kept.

  Config files are backed up once (*.kenshicoop.bak) before the first edit, and
  a manifest of what was installed is written to mods\KenshiCoop so -Uninstall
  removes only what this installer added.

.EXAMPLE
  KenshiCoop-Setup.cmd                      (double-click: auto-detect, confirm, install)
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File KenshiCoop-Setup.ps1 -KenshiDir "D:\Games\Kenshi" -Yes
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File KenshiCoop-Setup.ps1 -Uninstall -RemoveReKenshi
#>
[CmdletBinding()]
param(
    [string]$KenshiDir = "",
    # Local RE_Kenshi "loose" release zip; skips the download.
    [string]$ReKenshiZip = "",
    # The KenshiCoop mod folder to install (default: .\KenshiCoop next to this script).
    [string]$ModSource = "",
    [switch]$Uninstall,
    # With -Uninstall: also remove RE_Kenshi, but only if this installer put it there.
    [switch]$RemoveReKenshi,
    # Report what would happen and change nothing.
    [switch]$DryRun,
    # No confirmation prompt.
    [switch]$Yes
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Pinned RE_Kenshi release. Bump all three together; the hash is the one GitHub
# publishes for the asset.
$REK_VERSION = "0.3.5"
$REK_URL     = "https://github.com/BFrizzleFoShizzle/RE_Kenshi/releases/download/v0.3.5/RE_Kenshi_v0.3.5_loose.zip"
$REK_SHA256  = "02245ec39c3a33872579e5d48067619223dfaa63d12b5329aa92d97532a46d6b"

$MANIFEST_NAME = "install-manifest.json"
$BAK_SUFFIX    = ".kenshicoop.bak"

function Say([string]$m)  { Write-Host "  $m" }
function Ok([string]$m)   { Write-Host "  [ok]   $m" -ForegroundColor Green }
function Warn([string]$m) { Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Step([string]$m) { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Fail([string]$m) { throw $m }

function Get-Md5([string]$path) { (Get-FileHash -Algorithm MD5 -LiteralPath $path).Hash.ToLower() }

# ---- locate Kenshi -----------------------------------------------------------

function Find-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]
    $steam = $null
    try { $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction Stop).SteamPath } catch {}
    if (-not $steam) { $steam = "${env:ProgramFiles(x86)}\Steam" }
    $steam = $steam -replace '/', '\'
    if (Test-Path $steam) { $roots.Add($steam) }
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -LiteralPath $vdf -Pattern '"path"\s+"([^"]+)"')) {
            $p = $m.Matches[0].Groups[1].Value -replace '\\\\', '\'
            if (-not ($roots -contains $p)) { $roots.Add($p) }
        }
    }
    return $roots
}

function Find-Kenshi {
    $found = @()
    foreach ($lib in (Find-SteamLibraries)) {
        $k = Join-Path $lib "steamapps\common\Kenshi"
        if (Test-Path (Join-Path $k "kenshi_x64.exe")) { $found += $k }
    }
    return $found
}

# ---- RE_Kenshi package -------------------------------------------------------

function Get-ReKenshiPackage([string]$work) {
    $zip = $ReKenshiZip
    if (-not $zip) {
        $zip = Join-Path $work "RE_Kenshi_v${REK_VERSION}_loose.zip"
        if (-not (Test-Path $zip)) {
            Say "downloading RE_Kenshi $REK_VERSION (~21 MB) from github.com/BFrizzleFoShizzle/RE_Kenshi ..."
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -Uri $REK_URL -OutFile $zip -UseBasicParsing
        }
    }
    if (-not (Test-Path $zip)) { Fail "RE_Kenshi package not found: $zip" }
    $sha = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLower()
    if (-not $ReKenshiZip -and $sha -ne $REK_SHA256) {
        Remove-Item -LiteralPath $zip -Force
        Fail "RE_Kenshi download failed verification (sha256 $sha, expected $REK_SHA256)."
    }
    if ($ReKenshiZip -and $sha -ne $REK_SHA256) {
        Warn "local RE_Kenshi zip is not the pinned $REK_VERSION release (sha256 $sha); using it as given"
    } else {
        Ok "RE_Kenshi $REK_VERSION package verified (sha256 $($sha.Substring(0,12))...)"
    }
    $x = Join-Path $work "rek"
    if (Test-Path $x) { Remove-Item -Recurse -Force $x }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $x)
    foreach ($need in "install\RE_Kenshi.dll", "install\KenshiLib.dll", "install\RE_Kenshi", "config.json") {
        if (-not (Test-Path (Join-Path $x $need))) { Fail "RE_Kenshi package is missing '$need'" }
    }
    return $x
}

# RE_Kenshi's own version table: { "Steam 1.0.65": md5, ... }.
function Read-VersionTable([string]$rek) {
    $raw = Get-Content -Raw -LiteralPath (Join-Path $rek "config.json")
    $obj = $raw | ConvertFrom-Json
    $t = @{}
    foreach ($p in $obj.supportedVersions.PSObject.Properties) { $t[$p.Name] = $p.Value.ToLower() }
    return $t
}

function Get-KenshiVersion([string]$dir, $table) {
    foreach ($exe in "kenshi_x64.exe", "kenshi_GOG_x64.exe") {
        $p = Join-Path $dir $exe
        if (-not (Test-Path $p)) { continue }
        $md5 = Get-Md5 $p
        foreach ($k in $table.Keys) {
            if ($table[$k] -eq $md5) { return @{ Name = $k; Exe = $exe; Md5 = $md5 } }
        }
        return @{ Name = "unknown"; Exe = $exe; Md5 = $md5 }
    }
    return $null
}

# ---- small file helpers ------------------------------------------------------

function Backup-Once([string]$path) {
    $bak = "$path$BAK_SUFFIX"
    if ((Test-Path $path) -and -not (Test-Path $bak)) {
        if (-not $DryRun) { Copy-Item -LiteralPath $path $bak }
        Say "backup: $(Split-Path -Leaf $bak)"
    }
}

function Write-TextKeepEol([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding $false))
}

function Get-Eol([string]$text) { if ($text -match "`r`n") { "`r`n" } else { "`n" } }

function Test-Writable([string]$dir) {
    $probe = Join-Path $dir (".kenshicoop_probe_" + [guid]::NewGuid().ToString("N"))
    try { [IO.File]::WriteAllText($probe, "x"); Remove-Item -LiteralPath $probe -Force; return $true }
    catch { return $false }
}

function Assert-KenshiClosed([string]$dir) {
    # Only a Kenshi running FROM this install holds its files (RE_Kenshi\ included).
    $root = $dir.TrimEnd('\') + '\'
    $p = @(Get-Process -Name "kenshi_x64", "kenshi_GOG_x64" -ErrorAction SilentlyContinue |
           Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) })
    if ($p.Count -gt 0) { Fail "Kenshi is running from this folder. Close it and run the installer again." }
}

# ---- install -----------------------------------------------------------------

function Install-ReKenshi([string]$k, [string]$rek, $ver, $table, $manifest) {
    Step "RE_Kenshi"
    $src = Join-Path $rek "install"
    $files = @("RE_Kenshi.dll", "KenshiLib.dll", "CompressToolsLib.dll")
    $already = Test-Path (Join-Path $k "RE_Kenshi.dll")
    foreach ($f in $files) {
        $from = Join-Path $src $f
        if (-not (Test-Path $from)) { continue }
        if (-not $DryRun) { Copy-Item -LiteralPath $from (Join-Path $k $f) -Force }
        if (-not $already) { $manifest.reKenshiFiles += $f }
    }
    if (-not $DryRun) { Copy-Item -LiteralPath (Join-Path $src "RE_Kenshi") $k -Recurse -Force }
    if (-not $already) { $manifest.reKenshiFiles += "RE_Kenshi\" }
    Ok ("RE_Kenshi $REK_VERSION files " + $(if ($already) { "updated" } else { "installed" }))

    # Plugins_x64.cfg: RE_Kenshi must load first, before the render system.
    $cfg = Join-Path $k "Plugins_x64.cfg"
    if (-not (Test-Path $cfg)) { Fail "Plugins_x64.cfg not found in $k" }
    $raw = [IO.File]::ReadAllText($cfg)
    if ($raw -match '(?m)^Plugin=RE_Kenshi\s*$') {
        Ok "Plugins_x64.cfg already loads RE_Kenshi"
    } else {
        Backup-Once $cfg
        $eol = Get-Eol $raw
        if ($raw -match '(?m)^Plugin=RenderSystem_Direct3D11_x64') {
            $new = [regex]::Replace($raw, '(?m)^(Plugin=RenderSystem_Direct3D11_x64)', "Plugin=RE_Kenshi$eol`$1", 1)
        } else {
            $new = $raw.TrimEnd() + $eol + "Plugin=RE_Kenshi" + $eol
        }
        if (-not $DryRun) { Write-TextKeepEol $cfg $new }
        $manifest.pluginsCfgEdited = $true
        Ok "Plugins_x64.cfg: added Plugin=RE_Kenshi"
    }

    # Kenshi 1.0.68: RE_Kenshi restarts into RE_Kenshi\<same exe name>, which
    # must be the 1.0.65 build. Patch it from the installed exe and accept the
    # result only if its MD5 is in RE_Kenshi's table as 1.0.65.
    if ($ver.Name -like "* 1.0.68") {
        $platform = $ver.Name.Split(' ')[0]           # Steam | GOG
        $want = $table["$platform 1.0.65"]
        $target = Join-Path $k ("RE_Kenshi\" + $ver.Exe)
        if ((Test-Path $target) -and ((Get-Md5 $target) -eq $want)) {
            Ok "1.0.65 executable already in place ($($ver.Exe))"
        } else {
            $patch = Join-Path $rek ("tools\" + $ver.Exe + ".patch")
            $courgette = Join-Path $rek "tools\courgette64.exe"
            if (-not (Test-Path $patch) -or -not (Test-Path $courgette)) { Fail "RE_Kenshi package has no downgrade patch for $($ver.Name)" }
            if ($DryRun) {
                Say "would build the 1.0.65 executable at RE_Kenshi\$($ver.Exe)"
            } else {
                $tmp = Join-Path (Split-Path -Parent $rek) ("downgraded_" + $ver.Exe)
                if (Test-Path $tmp) { Remove-Item -LiteralPath $tmp -Force }
                & $courgette -apply (Join-Path $k $ver.Exe) $patch $tmp | Out-Null
                if (-not (Test-Path $tmp)) { Fail "building the 1.0.65 executable failed (courgette exit $LASTEXITCODE)" }
                $got = Get-Md5 $tmp
                if ($got -ne $want) { Remove-Item -LiteralPath $tmp -Force; Fail "built executable has md5 $got, expected $want - not installed" }
                Copy-Item -LiteralPath $tmp $target -Force
                Remove-Item -LiteralPath $tmp -Force
                Ok "built the 1.0.65 executable RE_Kenshi\$($ver.Exe) (md5 verified); your $($ver.Name) executable is unchanged"
            }
        }
    }
}

function Install-KenshiCoop([string]$k, [string]$src, $manifest) {
    Step "KenshiCoop"
    $dst = Join-Path $k "mods\KenshiCoop"
    $keepCfg = Test-Path (Join-Path $dst "coop_config.json")
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        foreach ($f in Get-ChildItem -LiteralPath $src -File) {
            if ($f.Name -eq "coop_config.json" -and $keepCfg) { continue }
            Copy-Item -LiteralPath $f.FullName (Join-Path $dst $f.Name) -Force
        }
        Get-ChildItem -LiteralPath $dst -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
    }
    Ok ("mod files copied to mods\KenshiCoop" + $(if ($keepCfg) { " (kept your coop_config.json)" } else { "" }))

    $mods = Join-Path $k "data\mods.cfg"
    $raw = if (Test-Path $mods) { [IO.File]::ReadAllText($mods) } else { "" }
    if ($raw -match '(?m)^KenshiCoop\.mod\s*$') {
        Ok "KenshiCoop already enabled in data\mods.cfg"
    } else {
        Backup-Once $mods
        $eol = if ($raw) { Get-Eol $raw } else { "`r`n" }
        $new = $raw
        if ($new -and -not $new.EndsWith("`n")) { $new += $eol }
        $new += "KenshiCoop.mod" + $eol
        if (-not $DryRun) { Write-TextKeepEol $mods $new }
        $manifest.modsCfgEdited = $true
        Ok "enabled KenshiCoop in data\mods.cfg"
    }
}

# ---- uninstall ---------------------------------------------------------------

function Remove-Line([string]$path, [string]$pattern) {
    if (-not (Test-Path $path)) { return $false }
    $raw = [IO.File]::ReadAllText($path)
    $new = [regex]::Replace($raw, "(?m)^$pattern\s*(\r?\n)?", "")
    if ($new -eq $raw) { return $false }
    if (-not $DryRun) { Write-TextKeepEol $path $new }
    return $true
}

function Invoke-Uninstall([string]$k) {
    $dst = Join-Path $k "mods\KenshiCoop"
    $manifest = $null
    $mf = Join-Path $dst $MANIFEST_NAME
    if (Test-Path $mf) { $manifest = Get-Content -Raw -LiteralPath $mf | ConvertFrom-Json }

    Step "KenshiCoop"
    if (Remove-Line (Join-Path $k "data\mods.cfg") 'KenshiCoop\.mod') { Ok "disabled KenshiCoop in data\mods.cfg" }
    if (Test-Path $dst) {
        if (-not $DryRun) { Remove-Item -LiteralPath $dst -Recurse -Force }
        Ok "removed mods\KenshiCoop"
    } else { Say "mods\KenshiCoop not present" }

    if ($RemoveReKenshi) {
        Step "RE_Kenshi"
        if (-not $manifest -or @($manifest.reKenshiFiles).Count -eq 0) {
            Warn "RE_Kenshi was not installed by this installer - left in place"
            return
        }
        if (Remove-Line (Join-Path $k "Plugins_x64.cfg") 'Plugin=RE_Kenshi') { Ok "removed Plugin=RE_Kenshi from Plugins_x64.cfg" }
        foreach ($f in $manifest.reKenshiFiles) {
            $p = Join-Path $k $f.TrimEnd('\')
            if (Test-Path $p) { if (-not $DryRun) { Remove-Item -LiteralPath $p -Recurse -Force }; Ok "removed $f" }
        }
    }
}

# ---- main --------------------------------------------------------------------

Write-Host ""
Write-Host "Kenshi Together setup (KenshiCoop mod)" -ForegroundColor White
if ($DryRun) { Warn "dry run - nothing will be changed" }

Step "Kenshi"
if (-not $KenshiDir) {
    $c = @(Find-Kenshi)
    if ($c.Count -eq 0) { Fail "Kenshi not found in your Steam libraries. Run again with -KenshiDir `"<path to Kenshi>`"." }
    if ($c.Count -gt 1) { Warn "several Kenshi installs found; using the first. Pass -KenshiDir to pick another:"; $c | ForEach-Object { Say $_ } }
    $KenshiDir = $c[0]
}
$KenshiDir = (Resolve-Path -LiteralPath $KenshiDir).Path
if (-not (Test-Path (Join-Path $KenshiDir "kenshi_x64.exe")) -and -not (Test-Path (Join-Path $KenshiDir "kenshi_GOG_x64.exe"))) {
    Fail "no Kenshi executable in $KenshiDir"
}
Ok "Kenshi: $KenshiDir"
Assert-KenshiClosed $KenshiDir

if (-not $DryRun -and -not (Test-Writable $KenshiDir)) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Fail "cannot write to $KenshiDir" }
    Warn "the Kenshi folder needs administrator rights - asking Windows for permission ..."
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$($MyInvocation.MyCommand.Path)`"", "-KenshiDir", "`"$KenshiDir`"", "-Yes")
    if ($ReKenshiZip)    { $argList += @("-ReKenshiZip", "`"$ReKenshiZip`"") }
    if ($ModSource)      { $argList += @("-ModSource", "`"$ModSource`"") }
    if ($Uninstall)      { $argList += "-Uninstall" }
    if ($RemoveReKenshi) { $argList += "-RemoveReKenshi" }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

if ($Uninstall) {
    if (-not $Yes -and -not $DryRun) {
        $a = Read-Host "  Remove KenshiCoop$(if ($RemoveReKenshi) { ' and RE_Kenshi' }) from this Kenshi install? [y/N]"
        if ($a -notmatch '^[yY]') { Say "cancelled"; exit 1 }
    }
    Invoke-Uninstall $KenshiDir
    Write-Host ""; Ok "uninstall finished"
    exit 0
}

if (-not $ModSource) { $ModSource = Join-Path $here "KenshiCoop" }
foreach ($f in "KenshiCoop.dll", "KenshiCoop.mod", "RE_Kenshi.json") {
    if (-not (Test-Path (Join-Path $ModSource $f))) { Fail "the KenshiCoop mod folder is incomplete ($f missing in $ModSource)" }
}

$work = Join-Path $env:TEMP "KenshiCoopSetup"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$rek   = Get-ReKenshiPackage $work
$table = Read-VersionTable $rek
$ver   = Get-KenshiVersion $KenshiDir $table
if ($ver.Name -eq "unknown") {
    Fail "this Kenshi executable (md5 $($ver.Md5)) is not a version RE_Kenshi $REK_VERSION supports ($(($table.Keys | Sort-Object) -join ', '))."
}
Ok "game version: $($ver.Name)"
if ($ver.Name -like "* 1.0.68") { Say "RE_Kenshi runs Kenshi as 1.0.65; the installer builds that executable next to your 1.0.68 one." }

if (-not $Yes -and -not $DryRun) {
    $a = Read-Host "  Install RE_Kenshi $REK_VERSION and KenshiCoop into this Kenshi? [Y/n]"
    if ($a -match '^[nN]') { Say "cancelled"; exit 1 }
}

$manifest = [pscustomobject]@{
    installedAt      = (Get-Date).ToString("s")
    reKenshiVersion  = $REK_VERSION
    gameVersion      = $ver.Name
    reKenshiFiles    = @()
    pluginsCfgEdited = $false
    modsCfgEdited    = $false
}
# Keep what an earlier run recorded, so a re-run never forgets that it owns RE_Kenshi.
$oldMf = Join-Path $KenshiDir "mods\KenshiCoop\$MANIFEST_NAME"
if (Test-Path $oldMf) {
    $old = Get-Content -Raw -LiteralPath $oldMf | ConvertFrom-Json
    $manifest.reKenshiFiles    = @($old.reKenshiFiles)
    $manifest.pluginsCfgEdited = [bool]$old.pluginsCfgEdited
    $manifest.modsCfgEdited    = [bool]$old.modsCfgEdited
}

Install-ReKenshi $KenshiDir $rek $ver $table $manifest
Install-KenshiCoop $KenshiDir $ModSource $manifest
$manifest.reKenshiFiles = @($manifest.reKenshiFiles | Select-Object -Unique)
if (-not $DryRun) {
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $KenshiDir "mods\KenshiCoop\$MANIFEST_NAME") -Encoding UTF8
}

Write-Host ""
Ok "done. Start Kenshi normally - the main menu should read 'RE_Kenshi $REK_VERSION', and F2 opens the co-op panel."
exit 0
