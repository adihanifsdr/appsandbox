<#
  deploy-local.ps1 - rebuild the host app from this checkout and drop it into a
  local install folder (the one a desktop shortcut points at), without signing
  or drivers.

  Builds AppSandboxCore, AppSandbox and iso-patch (Release|x64) with the guest
  source repo/branch pinned to THIS checkout's fork + branch (so a Linux VM
  created by the deployed app fetches its guest bits from the same commit),
  then copies the host binaries, web\, resources\ and headless-api\ into -Dest.
  drivers\ and any log/config files already in -Dest are left alone.

  USAGE:
    .\tools\deploy-local.ps1                                  # -> D:\Nestbox (migrates D:\AppSandbox-0.1.4-win-x64 on first run)
    .\tools\deploy-local.ps1 -Dest C:\path\to\install
    .\tools\deploy-local.ps1 -NoBuild                         # copy only
    .\tools\deploy-local.ps1 -SourceRepo you/appsandbox -SourceBranch my-branch
#>
[CmdletBinding()]
param(
  [string]$Dest = 'D:\Nestbox',
  [string]$OldDest = 'D:\AppSandbox-0.1.4-win-x64',   # pre-rename install folder to migrate from
  [string]$SourceRepo = '',     # empty: derived from `git remote` (fork, then origin)
  [string]$SourceBranch = '',   # empty: current branch
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin  = Join-Path $repo 'bin\Release'

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $root = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
    foreach ($c in @("$root\MSBuild\Current\Bin\amd64\MSBuild.exe", "$root\MSBuild\Current\Bin\MSBuild.exe")) {
        if (Test-Path $c) { return $c }
    }
    throw "MSBuild not found"
}

if (-not $SourceBranch) { $SourceBranch = (git -C $repo branch --show-current).Trim() }
if (-not $SourceRepo) {
    $url = (git -C $repo remote get-url fork 2>$null); if (-not $url) { $url = git -C $repo remote get-url origin }
    if ($url -match 'github\.com[:/]([^/]+/[^/.]+)') { $SourceRepo = $matches[1] } else { throw "cannot derive owner/name from remote '$url'" }
}
Write-Host "Guest sources: github.com/$SourceRepo @ $SourceBranch"
if ((git -C $repo status --porcelain) -or ((git -C $repo rev-list "$SourceBranch@{upstream}..$SourceBranch" 2>$null | Measure-Object).Count -gt 0)) {
    Write-Warning "Uncommitted or unpushed changes: a VM created by this build fetches the PUSHED branch, not this working tree."
}

if (-not $NoBuild) {
    $env:ASB_SKIP_PACKAGE_PROJECT = '1'
    $msbuild = Find-MSBuild
    & $msbuild (Join-Path $repo 'AppSandbox.sln') -t:AppSandboxCore -t:AppSandbox -t:iso-patch `
        -p:Configuration=Release -p:Platform=x64 `
        "-p:AsbSourceRepo=$SourceRepo" "-p:AsbSourceBranch=$SourceBranch" -m -v:minimal -nologo
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

# First run after the rename: carry the old install folder over (drivers,
# resources, log) and point every shortcut at the new exe.
if (-not (Test-Path (Join-Path $Dest 'drivers')) -and (Test-Path $OldDest)) {
    Write-Host "Migrating $OldDest -> $Dest"
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    robocopy $OldDest $Dest /E /XF AppSandbox.exe appsandbox_core.dll AppSandbox.pdb appsandbox_core.pdb | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "migration copy failed" }
    $sh = New-Object -ComObject WScript.Shell
    $oldExe = Join-Path $OldDest 'AppSandbox.exe'
    $lnkDirs = @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'),
                 "$env:APPDATA\Microsoft\Windows\Start Menu\Programs",
                 "$env:APPDATA\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar")
    foreach ($d in $lnkDirs) {
        if (-not (Test-Path $d)) { continue }
        Get-ChildItem $d -Filter *.lnk -ErrorAction SilentlyContinue | ForEach-Object {
            $l = $sh.CreateShortcut($_.FullName)
            if ($l.TargetPath -ieq $oldExe) {
                $l.TargetPath = Join-Path $Dest 'Nestbox.exe'
                $l.WorkingDirectory = $Dest
                $l.IconLocation = (Join-Path $Dest 'Nestbox.exe') + ',0'
                $l.Description = 'Nestbox'
                $l.Save()
                Write-Host "  shortcut repointed: $($_.FullName)"
            }
        }
    }
}
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
# Root binaries: skip unchanged ones (the app may be running from -Dest, which
# locks them); a changed but locked binary is reported instead of aborting.
$locked = @()
foreach ($b in @('Nestbox.exe', 'nestbox_core.dll', 'iso-patch.exe', 'WebView2Loader.dll')) {
    $src = Join-Path $bin $b; $dst = Join-Path $Dest $b
    if ((Test-Path $dst) -and ((Get-FileHash $src).Hash -eq (Get-FileHash $dst).Hash)) { continue }
    try { Copy-Item $src $dst -Force -ErrorAction Stop } catch { $locked += $b }
}
# web\ comes straight from the checkout (the build's post-build xcopy only
# refreshes bin\Release\web on a build, so -NoBuild would ship stale files).
robocopy (Join-Path $repo 'web') (Join-Path $Dest 'web') /E | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy web failed" }
robocopy (Join-Path $bin 'resources') (Join-Path $Dest 'resources') /E /XF *.pdb *.lib *.exp *.ilk *.iobj *.ipdb | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy resources failed" }
$sdk = Join-Path $repo 'tools\headless-api'
New-Item -ItemType Directory -Force -Path (Join-Path $Dest 'headless-api') | Out-Null
foreach ($f in @('asb.py', 'README.md')) { Copy-Item (Join-Path $sdk $f) (Join-Path $Dest "headless-api\$f") -Force }
foreach ($sub in @('examples', 'tests')) {
    robocopy (Join-Path $sdk $sub) (Join-Path $Dest "headless-api\$sub") /E /XD __pycache__ /XF *.pyc | Out-Null
}
$global:LASTEXITCODE = 0
if ($locked.Count) {
    Write-Warning ("NOT updated (in use - close Nestbox and re-run with -NoBuild): " + ($locked -join ', '))
}
Write-Host "Deployed to $Dest"
Get-Item (Join-Path $Dest 'Nestbox.exe'), (Join-Path $Dest 'nestbox_core.dll'), (Join-Path $Dest 'iso-patch.exe') |
    Format-Table Name, Length, LastWriteTime -AutoSize
