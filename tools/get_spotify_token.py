#!/usr/bin/env python3
"""
One-time helper: log in to Spotify and print a refresh token for the ESP32.

Run:  python3 get_spotify_token.py
Needs only the Python standard library. Your browser opens, you click "Agree",
and the script prints three lines to paste into config.h.

Your Spotify app must have this exact Redirect URI:
    http://127.0.0.1:8888/callback
"""
import base64
import http.server
import json
import os
import secrets
import ssl
import sys
import urllib.parse
import urllib.request
import webbrowser

REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-currently-playing user-read-playback-state"


def ssl_context():
    """python.org's macOS Python ships without CA certificates; fall back to the system's."""
    ctx = ssl.create_default_context()
    if ctx.cert_store_stats().get("x509_ca", 0) == 0:
        for path in ("/etc/ssl/cert.pem", "/opt/homebrew/etc/ca-certificates/cert.pem"):
            if os.path.exists(path):
                ctx.load_verify_locations(cafile=path)
                break
    return ctx


def main():
    print("Paste the values from your app at https://developer.spotify.com/dashboard\n")
    client_id = input("Client ID:     ").strip()
    client_secret = input("Client Secret: ").strip()
    if not client_id or not client_secret:
        sys.exit("Both values are needed.")

    state = secrets.token_urlsafe(16)
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": SCOPES,
        "state": state,
    })

    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            q = urllib.parse.parse_qs(parsed.query)
            result["code"] = q.get("code", [None])[0]
            result["state"] = q.get("state", [None])[0]
            result["error"] = q.get("error", [None])[0]
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            msg = "Done! You can close this tab and go back to the terminal." if result["code"] \
                else f"Spotify said: {result['error']}"
            self.wfile.write(f"<html><body style='font-family:sans-serif;padding:3em'>"
                             f"<h2>{msg}</h2></body></html>".encode())

        def log_message(self, *args):
            pass

    try:
        server = http.server.HTTPServer(("127.0.0.1", 8888), Handler)
    except OSError:
        sys.exit("Port 8888 is busy. Close whatever uses it and try again.")

    print("\nOpening your browser to log in to Spotify...")
    print("If it doesn't open, copy this link into your browser:\n" + auth_url + "\n")
    webbrowser.open(auth_url)
    while "code" not in result:
        server.handle_request()
    server.server_close()

    if result.get("error") or not result.get("code"):
        sys.exit(f"Login failed: {result.get('error')}")
    if result.get("state") != state:
        sys.exit("State mismatch - please try again.")

    basic = base64.b64encode(f"{client_id}:{client_secret}".encode()).decode()
    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT_URI,
        }).encode(),
        headers={"Authorization": f"Basic {basic}",
                 "Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, context=ssl_context()) as resp:
            tokens = json.load(resp)
    except urllib.error.HTTPError as e:
        sys.exit(f"Token exchange failed ({e.code}): {e.read().decode()}")

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit(f"No refresh token in the response: {tokens}")

    print("\nSuccess! Paste these three lines into config.h:\n")
    print(f'#define SPOTIFY_CLIENT_ID      "{client_id}"')
    print(f'#define SPOTIFY_CLIENT_SECRET  "{client_secret}"')
    print(f'#define SPOTIFY_REFRESH_TOKEN  "{refresh}"')
    print()


if __name__ == "__main__":
    main()
