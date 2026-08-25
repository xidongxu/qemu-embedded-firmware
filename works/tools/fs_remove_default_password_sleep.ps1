# fs_remove_default_password_sleep.ps1
# FreeSWITCH's default.xml "global" extension sleeps 10 seconds on EVERY call
# while default_password is still "1234" (the built-in security nag).  This
# produced the ~11 s delay before the callee rings, in BOTH directions, in the
# QEMU phone app.  This script neutralises that sleep (data=10000 -> 0) WITHOUT
# changing the password, so all existing registrations keep working.
#
# Run in an ADMIN PowerShell:
#   powershell -ExecutionPolicy Bypass -File works\tools\fs_remove_default_password_sleep.ps1
# The script tries `fs_cli -x "reloadxml"`; if FreeSWITCH is not running or the
# reload fails, just restart FreeSWITCH afterwards.
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).

$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host '[X] Please run as Administrator' -ForegroundColor Red
    exit 1
}

$f = 'C:\Program Files\FreeSWITCH\conf\dialplan\default.xml'
if (-not (Test-Path $f)) { Write-Host "[X] Not found: $f" -ForegroundColor Red; exit 1 }

$c = Get-Content $f -Raw

# Already fixed?
if ($c -notmatch '<action application="sleep" data="10000"/>') {
    Write-Host '[i] sleep 10000 already gone - nothing to do' -ForegroundColor Yellow
} else {
    $bak = "$f.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Copy-Item $f $bak -Force
    Write-Host "[*] Backup: $bak" -ForegroundColor Cyan

    $c = $c.Replace('<action application="sleep" data="10000"/>',
                    '<action application="sleep" data="0"/>')

    # Write without BOM, preserving the file's existing CRLF line endings.
    [System.IO.File]::WriteAllText($f, $c, (New-Object System.Text.UTF8Encoding $false))
    Write-Host '[*] default.xml: sleep 10000 -> sleep 0' -ForegroundColor Green
}

# Reload the dialplan if FreeSWITCH is running.
$cli = 'C:\Program Files\FreeSWITCH\fs_cli.exe'
if (Test-Path $cli) {
    try {
        & $cli -x "reloadxml" 2>$null | Out-Null
        Write-Host '[*] reloadxml issued' -ForegroundColor Green
    } catch {
        Write-Host '[i] reloadxml failed - restart FreeSWITCH instead' -ForegroundColor Yellow
    }
} else {
    Write-Host '[i] fs_cli not found - restart FreeSWITCH after editing' -ForegroundColor Yellow
}

Write-Host 'Done. Next QEMU call should ring in well under a second.'
