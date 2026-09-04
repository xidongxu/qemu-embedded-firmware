# Install FreeSWITCH SIPS/TLS profile + certs (run as Administrator).
# Usage: right-click "Run with PowerShell", or:
#   powershell -ExecutionPolicy Bypass -File .\setup_fs_tls.ps1
$ErrorActionPreference = 'Stop'

$fs  = 'C:\Program Files\FreeSWITCH'
$src = Join-Path $PSScriptRoot '..\..\libutils\pjproject\ports\freeswitch'

if (-not (Test-Path "$fs\cert")) {
    New-Item -ItemType Directory -Path "$fs\cert" | Out-Null
}
Copy-Item (Join-Path $src 'certs\agent.pem')  "$fs\cert\" -Force
Copy-Item (Join-Path $src 'certs\cafile.pem') "$fs\cert\" -Force

Copy-Item (Join-Path $src 'sip_profiles\internal-tls.xml') "$fs\conf\sip_profiles\" -Force

Write-Host 'FS TLS files installed:'
Get-ChildItem "$fs\cert\agent.pem","$fs\cert\cafile.pem","$fs\conf\sip_profiles\internal-tls.xml" | ForEach-Object { "  " + $_.FullName }

Write-Host ''
Write-Host 'Next steps:'
Write-Host '  1) Restart FreeSWITCH (or fs_cli: sofia profile internal-tls restart)'
Write-Host '  2) Verify: fs_cli -x "sofia status" shows internal-tls on 172.16.23.1:5061'
