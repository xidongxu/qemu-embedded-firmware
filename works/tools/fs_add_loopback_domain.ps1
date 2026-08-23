# fs_add_loopback_domain.ps1
# Add a directory domain alias "10.0.2.2" so the QEMU guest can REGISTER as
# 1000@10.0.2.2 (fixed slirp address) and still be authenticated against the
# same user database. Without this, FreeSWITCH returns 403 Forbidden because
# users 1000/1005 are only defined under domain $${domain} (192.168.23.7).
#
# Run this in an ADMIN PowerShell (needs write access to Program Files):
#   powershell -ExecutionPolicy Bypass -File works\tools\fs_add_loopback_domain.ps1
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).
# Afterwards reload the XML config:  fs_cli -x "reloadxml"

$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host '[X] Please run as Administrator' -ForegroundColor Red
    exit 1
}

$f = 'C:\Program Files\FreeSWITCH\conf\directory\default.xml'
if (-not (Test-Path $f)) { Write-Host "[X] Not found: $f" -ForegroundColor Red; exit 1 }

$c = Get-Content $f -Raw
if ($c -match '<domain name="10\.0\.2\.2"') {
    Write-Host '[i] 10.0.2.2 domain alias already present - nothing to do' -ForegroundColor Yellow
    exit 0
}
if ($c -notmatch '</include>') {
    Write-Host '[X] No </include> tag found - aborting' -ForegroundColor Red
    exit 1
}

# Backup
$bak = "$f.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $f $bak -Force
Write-Host "[*] Backup: $bak" -ForegroundColor Cyan

# Insert alias domain just before </include>
$alias = '<domain name="10.0.2.2" alias="true"/>'
$c = $c.Replace('</include>', $alias + [Environment]::NewLine + '</include>')
Set-Content -Path $f -Value $c -Encoding UTF8

Write-Host '[*] Added domain alias for 10.0.2.2' -ForegroundColor Green
Write-Host '--- tail of default.xml ---'
Get-Content $f -Tail 5 | ForEach-Object { $_.Trim() }
Write-Host ''
Write-Host 'Next: reload XML config, e.g.:'
Write-Host '  fs_cli -x "reloadxml"'
