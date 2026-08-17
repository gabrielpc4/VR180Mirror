# One-time Windows Firewall setup for the VR180 spectator pipeline.
# Opens the inbound ports the spectator Quest connects to, restricted to
# private/domain networks (never public) and to the local subnet.
# Self-elevates via UAC if not run as administrator.

$rules = @(
    @{ Name = "VR180Mirror Web (HTTPS 8443)"; Port = 8443; Proto = "TCP" },
    @{ Name = "VR180Mirror Web (HTTP 9080)";  Port = 9080; Proto = "TCP" },
    @{ Name = "VR180Mirror HLS (9888)";       Port = 9888; Proto = "TCP" },
    @{ Name = "VR180Mirror WebRTC (9889)";    Port = 9889; Proto = "TCP" },
    @{ Name = "VR180Mirror WebRTC media (9189 UDP)"; Port = 9189; Proto = "UDP" },
    @{ Name = "VR180Mirror WebRTC media (9189 TCP)"; Port = 9189; Proto = "TCP" }
)

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host "Requesting administrator rights to add firewall rules (UAC prompt)..."
    Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy","Bypass","-File","`"$PSCommandPath`""
    exit
}

foreach ($r in $rules) {
    if (-not (Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName $r.Name -Direction Inbound -Action Allow `
            -Protocol $r.Proto -LocalPort $r.Port -Profile Domain,Private `
            -RemoteAddress LocalSubnet | Out-Null
        Write-Host "Added: $($r.Name)"
    } else {
        Write-Host "Exists: $($r.Name)"
    }
}
Write-Host "Firewall ready. This window closes in 5s."
Start-Sleep 5
