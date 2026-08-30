# Send a SIP request over TLS to 172.16.23.1:5061 to probe FreeSWITCH inbound
# handling (temporary diagnostic tool).
#   -Method OPTIONS|INVITE   method to send
#   -WithSdp                append a simple SDP body
#   -WithCrypto             add SRTP a=crypto line to the SDP
#   -WithRoute              add Route header (outbound proxy style)
#   -WithOb                 add ;ob + reg-id to Contact and ;alias to Via
#   -FromAor                From: sip:1000@192.168.23.7 (registered AOR)
#   -UriPort5060            Request-URI sip:9196@192.168.23.7:5060 (guest style)
param([string]$Method = "OPTIONS", [switch]$WithSdp, [switch]$WithCrypto, [switch]$WithRoute, [switch]$WithOb, [switch]$FromAor, [switch]$UriPort5060)

$ErrorActionPreference = 'Stop'

$tcp = New-Object System.Net.Sockets.TcpClient('172.16.23.1', 5061)
$tcp.ReceiveTimeout = 4000
$tcp.SendTimeout = 4000
$ssl = New-Object System.Net.Security.SslStream($tcp.GetStream(), $false, ({ $true }))
$ssl.AuthenticateAsClient('172.16.23.1')

$body = ""
$ct = ""
if ($WithSdp) {
    $crypto = if ($WithCrypto) {
        "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=`r`n"
    } else { "" }
    $body = "v=0`r`no=- 1 1 IN IP4 172.16.23.50`r`ns=-`r`nc=IN IP4 172.16.23.50`r`nt=0 0`r`n" +
            "m=audio 4001 RTP/SAVP 97 9 0 8`r`n" +
            "a=rtpmap:97 opus/48000/2`r`n" +
            "a=fmtp:97 useinbandfec=1;maxplaybackrate=48000`r`n" +
            "a=rtpmap:9 G722/8000/1`r`n" +
            "a=rtpmap:0 PCMU/8000/1`r`n" +
            "a=rtpmap:8 PCMA/8000/1`r`n" +
            "a=sendrecv`r`n" +
            $crypto
    $ct = "Content-Type: application/sdp`r`n"
}

$route = if ($WithRoute) { "Route: <sips:172.16.23.1:5061;lr>`r`n" } else { "" }
$viaX = if ($WithOb) { ";alias" } else { "" }
if ($WithOb) {
    $contactX = ";transport=TLS;ob>;reg-id=1;+sip.instance=`"<urn:uuid:00000000-0000-0000-0000-0000f8607449>`"`r`nSupported: outbound, path"
} else {
    $contactX = ";transport=TLS"
}
$from = if ($FromAor) { "sip:1000@192.168.23.7" } else { "sip:opt@172.16.23.50" }
$ruri = if ($UriPort5060) { "sip:9196@192.168.23.7:5060" } else { "sip:9196@192.168.23.7" }

$msg = "$Method $ruri SIP/2.0`r`n" +
       "Via: SIP/2.0/TLS 172.16.23.50:19000;rport;branch=z9hG4bKopt01$viaX`r`n" +
       "From: <$from>;tag=opt1`r`n" +
       "To: <sip:9196@192.168.23.7>`r`n" +
       "Call-ID: opttest-001@172.16.23.50`r`n" +
       "CSeq: 1 $Method`r`n" +
       "$route" +
       "Contact: <sip:opt@172.16.23.50:19000$contactX>`r`n" +
       "Max-Forwards: 70`r`n" +
       "$ct" +
       "Content-Length: $($body.Length)`r`n`r`n" +
       $body
$bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
$ssl.Write($bytes, 0, $bytes.Length)
$ssl.Flush()

$buf = New-Object byte[] 8192
$all = ""
for ($i = 0; $i -lt 6; $i++) {
    try {
        $n = $ssl.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $all += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        if ($all -match "\r\n\r\n") {
            # If the message has a body we wait; otherwise try to flush
            # additional provisional responses.
        }
        # Print first line of each SIP response received
        $all -split "`r`n" | Where-Object { $_ -match '^SIP/2.0' } | ForEach-Object { Write-Host $_ }
    } catch {
        Write-Host "READ-ERR/EOF: $($_.Exception.Message)"
        break
    }
}
$ssl.Close()
$tcp.Close()
