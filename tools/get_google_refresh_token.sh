#!/usr/bin/env bash
# One-time helper (macOS / Linux / Git Bash): gets a Google OAuth refresh token for the
# clock's calendar page. Equivalent of get_google_refresh_token.ps1.
#
# Uses the "Desktop app" OAuth flow with PKCE. Needs only curl + openssl (both ship with macOS).
#
# Usage:
#   bash tools/get_google_refresh_token.sh "CLIENT_ID.apps.googleusercontent.com" "GOCSPX-CLIENT_SECRET"
#
# After you sign in, the browser is sent to http://127.0.0.1:53682/?code=... and shows
# "can't connect" - that's expected. Copy the full address from the address bar and paste it here.
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "Usage: bash $0 <client_id> <client_secret>" >&2
  exit 1
fi
CLIENT_ID="$1"
CLIENT_SECRET="$2"
REDIRECT_URI="http://127.0.0.1:53682"
SCOPE="https://www.googleapis.com/auth/calendar.events.readonly"

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
urlencode() {
  local s="$1" out="" c i
  for (( i = 0; i < ${#s}; i++ )); do
    c="${s:i:1}"
    case "$c" in
      [a-zA-Z0-9.~_-]) out+="$c" ;;
      *) out+=$(printf '%%%02X' "'$c") ;;
    esac
  done
  printf '%s' "$out"
}
urldecode() { local s="${1//+/ }"; printf '%b' "${s//%/\\x}"; }
query_param() {   # query_param <url> <name>
  printf '%s' "$1" | sed -n "s/.*[?&]$2=\([^&#]*\).*/\1/p"
}
json_field() {    # json_field <json> <name>  (flat string fields only)
  printf '%s' "$1" | tr -d '\n' | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p"
}

# PKCE + state
CODE_VERIFIER=$(openssl rand 48 | b64url)
CODE_CHALLENGE=$(printf '%s' "$CODE_VERIFIER" | openssl dgst -sha256 -binary | b64url)
STATE=$(openssl rand 16 | b64url)

AUTH_URL="https://accounts.google.com/o/oauth2/v2/auth?client_id=$(urlencode "$CLIENT_ID")&redirect_uri=$(urlencode "$REDIRECT_URI")&response_type=code&scope=$(urlencode "$SCOPE")&access_type=offline&prompt=consent&code_challenge=$CODE_CHALLENGE&code_challenge_method=S256&state=$STATE"

echo "Opening your browser to sign in to Google..."
echo "If it doesn't open, paste this URL into a browser:"
echo
echo "$AUTH_URL"
echo
if command -v open >/dev/null 2>&1; then open "$AUTH_URL"        # macOS
elif command -v xdg-open >/dev/null 2>&1; then xdg-open "$AUTH_URL" >/dev/null 2>&1 || true
elif command -v start >/dev/null 2>&1; then start "" "$AUTH_URL"
fi

echo "After approving, the browser will show a \"can't connect\" page at 127.0.0.1 - that's expected."
printf 'Paste the FULL address from the browser address bar here: '
read -r REDIRECTED

ERR=$(query_param "$REDIRECTED" error)
if [ -n "$ERR" ]; then echo "Google returned an error: $(urldecode "$ERR")" >&2; exit 1; fi
GOT_STATE=$(urldecode "$(query_param "$REDIRECTED" state)")
if [ "$GOT_STATE" != "$STATE" ]; then echo "State mismatch - did you paste the right address?" >&2; exit 1; fi
CODE=$(urldecode "$(query_param "$REDIRECTED" code)")
if [ -z "$CODE" ]; then echo "No code found in that address." >&2; exit 1; fi

TOKEN_JSON=$(curl -sS https://oauth2.googleapis.com/token \
  --data-urlencode "code=$CODE" \
  --data-urlencode "client_id=$CLIENT_ID" \
  --data-urlencode "client_secret=$CLIENT_SECRET" \
  --data-urlencode "redirect_uri=$REDIRECT_URI" \
  --data-urlencode "grant_type=authorization_code" \
  --data-urlencode "code_verifier=$CODE_VERIFIER")

REFRESH_TOKEN=$(json_field "$TOKEN_JSON" refresh_token)
ACCESS_TOKEN=$(json_field "$TOKEN_JSON" access_token)
if [ -z "$REFRESH_TOKEN" ]; then
  echo "No refresh_token in Google's response:" >&2
  echo "$TOKEN_JSON" >&2
  echo "If the response looks OK, remove the app at https://myaccount.google.com/permissions and run this again." >&2
  exit 1
fi

# Quick check that the token can read the calendar.
NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EVENTS=$(curl -sS -H "Authorization: Bearer $ACCESS_TOKEN" \
  "https://www.googleapis.com/calendar/v3/calendars/primary/events?singleEvents=true&orderBy=startTime&maxResults=1&fields=items(summary)&timeMin=$(urlencode "$NOW")")
if printf '%s' "$EVENTS" | grep -q '"error"'; then
  echo "Got a token, but reading the calendar failed:" >&2
  echo "$EVENTS" >&2
  echo "(Is the Google Calendar API enabled for this project?)" >&2
  exit 1
fi
NEXT=$(json_field "$EVENTS" summary)
echo
echo "Calendar access works. Next event: ${NEXT:-(no upcoming events)}"
echo
echo "Paste these lines into clock/secrets.h (replacing the empty GOOGLE_* lines):"
echo
echo "#define GOOGLE_CLIENT_ID      \"$CLIENT_ID\""
echo "#define GOOGLE_CLIENT_SECRET  \"$CLIENT_SECRET\""
echo "#define GOOGLE_REFRESH_TOKEN  \"$REFRESH_TOKEN\""
echo
echo "Keep these private - secrets.h is git-ignored."
