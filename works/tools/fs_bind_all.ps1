# fs_bind_all.ps1
# Make FreeSWITCH bind SIP/RTP on 0.0.0.0 (all IPv4 NICs incl. loopback),
# so the QEMU guest can register/dial via the fixed 10.0.2.2 instead of
# the host's DHCP-assigned LAN IP.
#
# Run this in an ADMIN PowerShell:
#   powershell -ExecutionPolicy Bypass -File works\tools\fs_bind_all.ps1
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage, so non-ASCII text can corrupt parsing).

$ErrorActionPreference = 'Stop'

# Permission check
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host '[X] Please run as Administrator (right-click PowerShell -> Run as administrator)' -ForegroundColor Red
    exit 1
}

$conf  = 'C:\Program Files\FreeSWITCH\conf\sip_profiles\internal.xml'
$exe   = 'C:\Program Files\FreeSWITCH\FreeSwitchConsole.exe'
$fscli = 'C:\Program Files\FreeSWITCH\fs_cli.exe'

if (-not (Test-Path $conf)) { Write-Host "[X] Config not found: $conf" -ForegroundColor Red; exit 1 }

# 1) Backup
$bak = "$conf.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item $conf $bak -Force
Write-Host "[*] Backup written to: $bak" -ForegroundColor Cyan

# 2) Rewrite rtp-ip / sip-ip -> 0.0.0.0
$c = Get-Content $conf -Raw
if ($c -match '<param name="sip-ip" value="0\.0\.0\.0"/>') {
    Write-Host '[i] Already 0.0.0.0, nothing to change' -ForegroundColor Yellow
} else {
    $before = $c
    $c = $c -creplace '<param name="rtp-ip" value="\$\$\{local_ip_v4\}"/>', '<param name="rtp-ip" value="0.0.0.0"/>'
    $c = $c -creplace '<param name="sip-ip" value="\$\$\{local_ip_v4\}"/>', '<param name="sip-ip" value="0.0.0.0"/>'
    if ($c -eq $before) {
        Write-Host '[X] No matching lines found - please check internal.xml' -ForegroundColor Red
        exit 1
    }
    Set-Content -Path $conf -Value $c -Encoding UTF8 -NoNewline
    Write-Host '[*] rtp-ip / sip-ip set to 0.0.0.0' -ForegroundColor Green
    Write-Host '--- changed lines ---'
    Get-Content $conf | Select-String -Pattern 'name="(rtp|sip)-ip"' | ForEach-Object { $_.Line.Trim() }
}

# 3) Restart FreeSWITCH
Write-Host '[*] Stopping FreeSwitchConsole ...'
Get-Process -Name FreeSwitchConsole -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3
Write-Host '[*] Starting FreeSwitchConsole ...'
Start-Process -FilePath $exe -WorkingDirectory 'C:\Program Files\FreeSWITCH'
Start-Sleep -Seconds 12

# 4) Verify
Write-Host '=== 5060 listeners (expect 0.0.0.0 and 127.0.0.1) ==='
netstat -ano | Select-String ':5060' | ForEach-Object { $_.Line.Trim() }
Write-Host '=== sofia profile internal ==='
& $fscli -x "sofia status profile internal" 2>&1 | Select-String -Pattern 'Internal Profile','SIP-TLS','SIP-TP','Name','sip-ip','rtp-ip','sip-port' | ForEach-Object { $_.Line }
Write-Host '=== registrations ==='
& $fscli -x "sofia status profile internal reg" 2>&1 | Select-String -Pattern 'User:|Total|registered' | ForEach-Object { $_.Line }
