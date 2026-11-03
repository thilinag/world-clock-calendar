<#
.SYNOPSIS
  One-time helper: gets a Google OAuth refresh token for the clock's calendar page.

.DESCRIPTION
  Uses the "Desktop app" OAuth flow (loopback redirect + PKCE). It opens your browser,
  you sign in with the Google account whose calendar should be shown, and the script
  prints the three #define lines to paste into clock/secrets.h.

  Scope requested: calendar.events.readonly (read-only access to calendar events).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\get_google_refresh_token.ps1 -ClientId "xxx.apps.googleusercontent.com" -ClientSecret "GOCSPX-..."
#>
param(
  [Parameter(Mandatory = $true)][string]$ClientId,
  [Parameter(Mandatory = $true)][string]$ClientSecret
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function ConvertTo-Base64Url([byte[]]$bytes) {
  [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$verifierBytes = New-Object byte[] 32; $rng.GetBytes($verifierBytes)
$codeVerifier  = ConvertTo-Base64Url $verifierBytes
$sha256        = [Security.Cryptography.SHA256]::Create()
$codeChallenge = ConvertTo-Base64Url ($sha256.ComputeHash([Text.Encoding]::ASCII.GetBytes($codeVerifier)))
$stateBytes = New-Object byte[] 16; $rng.GetBytes($stateBytes)
$state = ConvertTo-Base64Url $stateBytes

# Loopback listener on a free port (raw TCP, so no admin / URL ACL needed).
$listener = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$redirectUri = "http://127.0.0.1:$port"

$scope = 'https://www.googleapis.com/auth/calendar.events.readonly'
$authUrl = 'https://accounts.google.com/o/oauth2/v2/auth' +
  '?client_id=' + [Uri]::EscapeDataString($ClientId) +
  '&redirect_uri=' + [Uri]::EscapeDataString($redirectUri) +
  '&response_type=code' +
  '&scope=' + [Uri]::EscapeDataString($scope) +
  '&access_type=offline&prompt=consent' +
  '&code_challenge=' + $codeChallenge + '&code_challenge_method=S256' +
  '&state=' + $state

Write-Host "Opening your browser to sign in to Google..."
Write-Host "If it doesn't open, paste this URL into a browser:`n$authUrl`n"
Start-Process $authUrl

# Wait for Google to redirect back to http://127.0.0.1:<port>/?code=...&state=...
$query = $null
try {
  while (-not $query) {
    $client = $listener.AcceptTcpClient()
    $stream = $client.GetStream()
    $reader = New-Object IO.StreamReader($stream)
    $requestLine = $reader.ReadLine()
    $target = ($requestLine -split ' ')[1]
    $ok = $target -match '[?&](code|error)='
    $body = if ($ok) {
      '<html><body style="font-family:sans-serif;padding:40px"><h2>Done - you can close this tab.</h2>Return to the PowerShell window.</body></html>'
    } else { '' }
    $status = if ($ok) { '200 OK' } else { '404 Not Found' }
    $bytes = [Text.Encoding]::UTF8.GetBytes("HTTP/1.1 $status`r`nContent-Type: text/html; charset=utf-8`r`nContent-Length: $([Text.Encoding]::UTF8.GetByteCount($body))`r`nConnection: close`r`n`r`n$body")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    $client.Close()
    if ($ok) { $query = $target.Substring($target.IndexOf('?') + 1) }   # ignore e.g. /favicon.ico
  }
} finally {
  $listener.Stop()
}

$params = @{}
foreach ($pair in $query -split '&') {
  $kv = $pair -split '=', 2
  $params[[Uri]::UnescapeDataString($kv[0])] = [Uri]::UnescapeDataString(($kv[1] -replace '\+', ' '))
}
if ($params['error']) { throw "Google returned an error: $($params['error'])" }
if ($params['state'] -ne $state) { throw 'State mismatch - aborting.' }

$token = Invoke-RestMethod -Method Post -Uri 'https://oauth2.googleapis.com/token' -Body @{
  code          = $params['code']
  client_id     = $ClientId
  client_secret = $ClientSecret
  redirect_uri  = $redirectUri
  grant_type    = 'authorization_code'
  code_verifier = $codeVerifier
}

if (-not $token.refresh_token) {
  throw 'No refresh_token in the response. Remove the app at https://myaccount.google.com/permissions and run this again.'
}

# Quick check that the token can read the calendar.
$now = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$events = Invoke-RestMethod -Headers @{ Authorization = "Bearer $($token.access_token)" } -Uri ("https://www.googleapis.com/calendar/v3/calendars/primary/events?singleEvents=true&orderBy=startTime&maxResults=1&timeMin=" + [Uri]::EscapeDataString($now))
$next = if ($events.items) { $events.items[0].summary } else { '(no upcoming events)' }
Write-Host "`nCalendar access works. Next event: $next`n" -ForegroundColor Green

Write-Host "Paste these lines into clock\secrets.h (replacing the empty GOOGLE_* lines):`n" -ForegroundColor Yellow
Write-Host "#define GOOGLE_CLIENT_ID      `"$ClientId`""
Write-Host "#define GOOGLE_CLIENT_SECRET  `"$ClientSecret`""
Write-Host "#define GOOGLE_REFRESH_TOKEN  `"$($token.refresh_token)`""
Write-Host "`nKeep these private - secrets.h is git-ignored."
