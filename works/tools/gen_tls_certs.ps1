# gen_tls_certs.ps1 - regenerate the FreeSWITCH SIPS/TLS test certificates.
#
# Outputs (overwrites) into libutils/pjproject/ports/freeswitch/certs/:
#   agent.pem   = server cert + private key (FreeSWITCH TLS server)
#   cafile.pem  = CA certificate
#   ca.pem      = CA certificate (embedded into the guest as ca_cert.h)
#
# SAN covers IP:172.16.23.1 so the guest's TLS verify (hostname 172.16.23.1)
# passes.  These are SELF-SIGNED TEST certs for QEMU only - do not use in
# production.
#
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage).
$ErrorActionPreference = 'Stop'

$ssl  = 'C:\Program Files\Git\usr\bin\openssl.exe'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dst  = Join-Path $root 'libutils\pjproject\ports\freeswitch\certs'

if (-not (Test-Path $ssl)) {
    Write-Error "openssl not found at $ssl (install Git for Windows)"
}

New-Item -ItemType Directory -Force -Path $dst | Out-Null

$tmp = Join-Path $env:TEMP 'qemu-tls-certs'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
Set-Location $tmp

# CA
& $ssl genrsa -out ca.key 2048 2>$null
& $ssl req -x509 -new -key ca.key -days 3650 `
    -subj "/C=CN/O=QEMU-Phone/CN=QEMU-CA" -out ca.pem 2>$null

# Server key + CSR (CN = host tap0 IP)
& $ssl genrsa -out server.key 2048 2>$null
& $ssl req -new -key server.key `
    -subj "/C=CN/O=QEMU-Phone/CN=172.16.23.1" -out server.csr 2>$null

# Sign with SAN (IP + DNS for the tap0 host address)
'subjectAltName=IP:172.16.23.1,DNS:172.16.23.1,DNS:localhost' |
    Set-Content san.cnf -Encoding ascii
& $ssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial `
    -days 3650 -extfile san.cnf -out server.pem 2>$null

# Bundle: agent.pem = server cert + private key
Get-Content server.pem, server.key | Set-Content (Join-Path $dst 'agent.pem')
Copy-Item ca.pem (Join-Path $dst 'cafile.pem') -Force
Copy-Item ca.pem (Join-Path $dst 'ca.pem') -Force

Write-Host 'TLS test certs written to:'
Get-ChildItem $dst | ForEach-Object { "  " + $_.Name }
Write-Host ''
Write-Host 'Then run setup_fs_tls.ps1 (as Administrator) to install into'
Write-Host 'FreeSWITCH, and regenerate the guest CA header if needed:'
Write-Host '  boards\mps2-an505\FreeRTOS\application\ca_cert.h  <- from ca.pem'
