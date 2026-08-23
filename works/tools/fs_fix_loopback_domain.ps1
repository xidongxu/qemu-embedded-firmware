# fs_fix_loopback_domain.ps1
# The earlier "alias=true" domain for 10.0.2.2 is NOT honored by FreeSWITCH
# for REGISTER user lookup (still: "Can't find user [1000@10.0.2.2]").
# This script replaces it with a full explicit <domain name="10.0.2.2"> block
# that includes the same user files (default/*.xml), so the QEMU guest can
# register as 1000@10.0.2.2 and be authenticated (password 1234).
#
# Run this in an ADMIN PowerShell:
#   powershell -ExecutionPolicy Bypass -File works\tools\fs_fix_loopback_domain.ps1
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).
# Afterwards reload mod_sofia:  fs_cli -x "reload mod_sofia"

$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host '[X] Please run as Administrator' -ForegroundColor Red
    exit 1
}

$f = 'C:\Program Files\FreeSWITCH\conf\directory\default.xml'
if (-not (Test-Path $f)) { Write-Host "[X] Not found: $f" -ForegroundColor Red; exit 1 }

$c = Get-Content $f -Raw

# 1) Remove the (ineffective) alias line if present
if ($c -match '<domain name="10\.0\.2\.2" alias="true"/>') {
    $c = $c -replace '[\r\n]+\s*<domain name="10\.0\.2\.2" alias="true"/>', ''
    Write-Host '[*] Removed ineffective alias line' -ForegroundColor Yellow
}

# 2) Skip if a full 10.0.2.2 domain block already exists
if ($c -match '<domain name="10\.0\.2\.2">') {
    Write-Host '[i] Full 10.0.2.2 domain block already present - nothing to do' -ForegroundColor Yellow
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

# 3) Insert a full explicit domain block before </include>
$block = @'
  <domain name="10.0.2.2">
    <params>
      <param name="dial-string" value="{^^:sip_invite_domain=${dialed_domain}:presence_id=${dialed_user}@${dialed_domain}}${sofia_contact(*/${dialed_user}@${dialed_domain})},${verto_contact(${dialed_user}@${dialed_domain})}"/>
    </params>
    <variables>
      <variable name="record_stereo" value="true"/>
      <variable name="default_gateway" value="$${default_provider}"/>
      <variable name="default_areacode" value="$${default_areacode}"/>
      <variable name="transfer_fallback_extension" value="operator"/>
    </variables>
    <groups>
      <group name="default">
        <users>
          <X-PRE-PROCESS cmd="include" data="default/*.xml"/>
        </users>
      </group>
    </groups>
  </domain>
'@

$c = $c.Replace('</include>', $block + [Environment]::NewLine + '</include>')
Set-Content -Path $f -Value $c -Encoding UTF8

Write-Host '[*] Inserted full 10.0.2.2 domain block' -ForegroundColor Green
Write-Host '--- tail of default.xml ---'
Get-Content $f -Tail 20 | ForEach-Object { $_.Trim() }
Write-Host ''
Write-Host 'Next: reload mod_sofia, e.g.:'
Write-Host '  fs_cli -x "reload mod_sofia"'
