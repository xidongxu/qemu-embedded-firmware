# fs_add_loopback_profile.ps1
# Create a loopback-only sofia profile ("internal-lo") so the QEMU guest can
# register/dial via the FIXED 10.0.2.2 (slirp maps 10.0.2.2 -> host 127.0.0.1),
# independent of the host's DHCP-assigned LAN IP.
#
# The Android phone keeps using the normal "internal" profile (LAN IP:5060);
# this new profile binds SIP on 127.0.0.1:5060 only. RTP still binds the host
# LAN IP ($${local_ip_v4}) so the guest can reach media through slirp.
#
# Run this in an ADMIN PowerShell (needs write access to Program Files):
#   powershell -ExecutionPolicy Bypass -File works\tools\fs_add_loopback_profile.ps1
# NOTE: keep this file ASCII-only (PowerShell 5.1 decodes .ps1 without BOM
# using the system ANSI codepage, so non-ASCII text can corrupt parsing).
# It only writes the file - reload mod_sofia afterwards (e.g. via fs_cli).

$ErrorActionPreference = 'Stop'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host '[X] Please run as Administrator' -ForegroundColor Red
    exit 1
}

$dir = 'C:\Program Files\FreeSWITCH\conf\sip_profiles'
$out = Join-Path $dir 'internal-lo.xml'
if (-not (Test-Path $dir)) { Write-Host "[X] Not found: $dir" -ForegroundColor Red; exit 1 }

if (Test-Path $out) {
    Write-Host '[i] internal-lo.xml already exists - overwriting' -ForegroundColor Yellow
}

$xml = @'
<profile name="internal-lo">
  <aliases/>
  <gateways/>
  <domains>
    <domain name="all" alias="true" parse="false"/>
  </domains>
  <settings>
    <param name="debug" value="0"/>
    <param name="sip-trace" value="no"/>
    <param name="sip-capture" value="no"/>
    <param name="context" value="public"/>
    <param name="rfc2833-pt" value="101"/>
    <param name="sip-port" value="5060"/>
    <param name="dialplan" value="XML"/>
    <param name="dtmf-duration" value="2000"/>
    <param name="inbound-codec-prefs" value="$${global_codec_prefs}"/>
    <param name="outbound-codec-prefs" value="$${global_codec_prefs}"/>
    <param name="rtp-timer-name" value="soft"/>
    <!-- Loopback profile for QEMU guest via 10.0.2.2 (slirp -> host 127.0.0.1).
         SIP binds host loopback only; RTP binds the host LAN IP so the guest
         can reach media through slirp. -->
    <param name="rtp-ip" value="$${local_ip_v4}"/>
    <param name="sip-ip" value="127.0.0.1"/>
    <param name="hold-music" value="$${hold_music}"/>
    <param name="apply-nat-acl" value="nat.auto"/>
    <param name="local-network-acl" value="localnet.auto"/>
    <param name="apply-inbound-acl" value="domains"/>
    <param name="inbound-late-negotiation" value="true"/>
    <param name="nonce-ttl" value="60"/>
    <param name="auth-calls" value="$${internal_auth_calls}"/>
    <param name="auth-subscriptions" value="true"/>
    <param name="inbound-reg-force-matching-username" value="true"/>
    <param name="auth-all-packets" value="false"/>
    <param name="ext-rtp-ip" value="$${local_ip_v4}"/>
    <param name="ext-sip-ip" value="$${local_ip_v4}"/>
  </settings>
</profile>
'@

Set-Content -Path $out -Value $xml -Encoding UTF8
Write-Host '[*] Wrote internal-lo.xml' -ForegroundColor Green
Write-Host '--- key settings ---'
Get-Content $out | Select-String -Pattern 'sip-ip','rtp-ip','sip-port','name="internal-lo"' | ForEach-Object { $_.Line.Trim() }
Write-Host ''
Write-Host 'Next: reload mod_sofia (fs_cli) to load the new profile, e.g.:'
Write-Host '  fs_cli -x "reload mod_sofia"'
