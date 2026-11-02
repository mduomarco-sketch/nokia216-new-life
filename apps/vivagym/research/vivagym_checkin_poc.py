#!/usr/bin/env python3
"""
VivaGym / MyVitale check-in QR — proof of concept.

Replicates the gym app's check-in auth engine for YOUR OWN membership:

    1. POST api/v2.0/exerp/newAuth   (email + password)      -> bearer token
    2. GET  api/v2.0/exerp/qr        (Authorization: Bearer) -> "exerp:checkin:<id>-<ts>-<hash>"
    3. Render that exact string as a QR code.

The server generates the timestamp and the 32-hex digest. There is NO
client-side secret: the device only needs to authenticate and fetch the string.
See ../FINDINGS.md for the full reverse-engineering writeup.

This is intended as a reference for porting to KaiOS / BlackBerry — the whole
engine is one HTTPS login + one authenticated GET + a QR render.

Usage:
    python vivagym_checkin_poc.py --email you@example.com            # prompts for password
    VIVAGYM_PASSWORD=... python vivagym_checkin_poc.py --email you@example.com
    python vivagym_checkin_poc.py --token <access_token>             # skip login, use existing token
    python vivagym_checkin_poc.py --email ... --probe-validity       # measure QR refresh window

Only dependency is `requests`. QR rendering uses `qrcode` if installed,
otherwise falls back to an ASCII QR (also handy for low-end displays).
"""
from __future__ import annotations

import argparse
import getpass
import json
import os
import re
import sys
import time

import requests

BASE_URL = "https://vivagym.myvitale.com/"
APP_NAME = "vivagym"
# zxing/OkHttp User-Agent is not required, but mirror a mobile client politely.
USER_AGENT = "okhttp/4.9.0"

# OAuth2 client-credentials, hardcoded in the APK (Symfony FOSOAuthServerBundle,
# endpoint oauth/v2/token). This is the APP identity, not a user secret — it only
# yields a short-lived "temp token" that is then required as the access_token
# field of exerp/newAuth. Two pairs ship in the app; BuildConfig is the one the
# live login path (ApiHelperImpl.authenticate) uses.
CLIENT_ID = os.environ.get("VIVAGYM_CLIENT_ID", "")
CLIENT_SECRET = os.environ.get("VIVAGYM_CLIENT_SECRET", "")
# Alternative pair (com.vitale.coredata.data.storage.Authentication), kept as fallback:
CLIENT_ID_ALT = os.environ.get("VIVAGYM_CLIENT_ID_ALT", "")
CLIENT_SECRET_ALT = os.environ.get("VIVAGYM_CLIENT_SECRET_ALT", "")

QR_RE = re.compile(r"^exerp:checkin:(?P<member>[^-]+)-(?P<ts>\d+)-(?P<hash>[0-9a-f]{32})$")


class VivaGymClient:
    def __init__(self, base_url: str = BASE_URL, locale: str = "es",
                 client_id: str = CLIENT_ID, client_secret: str = CLIENT_SECRET,
                 verbose: bool = False):
        self.base_url = base_url.rstrip("/") + "/"
        self.locale = locale            # exerp/newAuth path segment, e.g. "es"
        self.client_id = client_id
        self.client_secret = client_secret
        self.verbose = verbose
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": USER_AGENT, "Accept": "application/json"})
        self.access_token: str | None = None
        self.refresh_token: str | None = None

    def _log(self, *a):
        if self.verbose:
            print("[debug]", *a, file=sys.stderr)

    # ---- 1a. OAuth client-credentials -> short-lived "temp token" -------
    def client_credentials_token(self) -> str:
        """POST/GET oauth/v2/token with the app's client_id/secret.
        Returns the temp access_token that exerp/newAuth requires."""
        url = self.base_url + "oauth/v2/token"
        body = {
            "grant_type": "client_credentials",
            "client_id": self.client_id,
            "client_secret": self.client_secret,
        }
        # The current app POSTs a JSON body; standard FOSOAuthServer also accepts
        # GET query params or POST form. Try in that order.
        attempts = (
            ("POST json", lambda: self.session.post(url, json=body, timeout=30)),
            ("GET query", lambda: self.session.get(url, params=body, timeout=30)),
            ("POST form", lambda: self.session.post(url, data=body, timeout=30)),
        )
        last = None
        for label, call in attempts:
            r = call()
            self._log("oauth", label, "->", r.status_code, r.text[:200])
            if r.ok:
                tok = r.json().get("access_token")
                if tok:
                    self._log("oauth got temp token via", label)
                    return tok
            last = r
        raise RuntimeError(
            f"client-credentials failed (last {last.status_code}): {last.text[:400] if last else '?'}"
        )

    # ---- 1b. user login -------------------------------------------------
    def login(self, email: str, password: str) -> dict:
        """Two-step: get temp token, then POST exerp/newAuth with it."""
        temp_token = self.client_credentials_token()
        url = self.base_url + f"api/v2.0/{self.locale}/exerp/newAuth"
        data = {
            "access_token": temp_token,   # <-- the app-level temp token from step 1a
            "password": password,
            "email": email,
            "appName": APP_NAME,
        }
        self._log("POST", url)
        r = self.session.post(url, data=data, timeout=30)
        self._log("status", r.status_code, "body", r.text[:300])
        if not r.ok:
            raise RuntimeError(f"Login HTTP {r.status_code}: {r.text[:500]}")
        body = r.json()
        if body.get("code", 0) != 0 and not body.get("access_token"):
            raise RuntimeError(f"Login rejected: code={body.get('code')} message={body.get('message')}")
        self.access_token = body.get("access_token")
        self.refresh_token = body.get("refresh_token")
        if not self.access_token:
            raise RuntimeError(f"No access_token in login response: {body}")
        return body

    # ---- token refresh --------------------------------------------------
    def refresh(self) -> dict:
        if not self.refresh_token:
            raise RuntimeError("No refresh_token available")
        url = self.base_url + "api/email/refresh"
        r = self.session.get(url, params={"refresh_token": self.refresh_token}, timeout=30)
        r.raise_for_status()
        body = r.json()
        self.access_token = body.get("access_token", self.access_token)
        self.refresh_token = body.get("refresh_token", self.refresh_token)
        return body

    # ---- 2. fetch QR ----------------------------------------------------
    def get_qr(self) -> str:
        """GET exerp/qr with Bearer token. Returns the raw check-in string."""
        if not self.access_token:
            raise RuntimeError("Not authenticated; call login() or set a token first")
        url = self.base_url + "api/v2.0/exerp/qr"
        headers = {"Authorization": f"Bearer {self.access_token}"}
        self._log("GET", url)
        r = self.session.get(url, headers=headers, timeout=30)
        self._log("status", r.status_code, "body", r.text[:200])
        r.raise_for_status()
        payload = r.text.strip()
        # Call<String> + Gson => body is usually a JSON-quoted string literal.
        if payload.startswith('"') and payload.endswith('"'):
            payload = json.loads(payload)
        elif payload.startswith("{"):
            # defensive: some deployments wrap it in an object
            obj = json.loads(payload)
            payload = obj.get("qr") or obj.get("data") or obj.get("code") or payload
        return payload.strip()


# ---- QR rendering -------------------------------------------------------
def render_qr(data: str, png_path: str | None = None) -> None:
    try:
        import qrcode  # type: ignore
    except ImportError:
        print("(install `qrcode` for PNG output; showing ASCII only)\n")
        _ascii_qr_fallback(data)
        return

    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=2)
    qr.add_data(data)
    qr.make(fit=True)
    # Terminal render. On Windows the default console codec (cp1252) can't encode
    # the block glyphs print_ascii uses, so force UTF-8 and fall back to ASCII.
    try:
        sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
    except Exception:
        pass
    try:
        qr.print_ascii(invert=True)
    except UnicodeEncodeError:
        _ascii_qr_fallback_matrix(qr)
    if png_path:
        img = qr.make_image(fill_color="black", back_color="white")
        img.save(png_path)
        print(f"\nSaved QR PNG -> {png_path}")


def _ascii_qr_fallback_matrix(qr) -> None:
    """Render the QR with plain ASCII (## / spaces) when Unicode blocks fail."""
    matrix = qr.get_matrix()
    for row in matrix:
        print("".join("##" if cell else "  " for cell in row))


def _ascii_qr_fallback(data: str) -> None:
    # Minimal: only used when `qrcode` is missing. Prints the payload so it can
    # be re-encoded elsewhere.
    print("QR payload (encode this as a QR):")
    print("   ", data)


def analyze(data: str) -> None:
    m = QR_RE.match(data)
    print("\n--- token analysis ---")
    print("raw:   ", data)
    if not m:
        print("(! did not match expected exerp:checkin:<id>-<ts>-<hash> shape)")
        return
    ts_ms = int(m.group("ts"))
    print("member:", m.group("member"))
    print("ts(ms):", ts_ms, "=", time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(ts_ms / 1000)))
    print("digest:", m.group("hash"), "(128-bit, server-computed)")
    print("age:   ", round(time.time() - ts_ms / 1000, 1), "s old at fetch time")


def probe_validity(client: VivaGymClient, samples: int = 6, interval: float = 10.0) -> None:
    """Fetch the QR repeatedly to see how/when the token rolls — informs the
    dumbphone re-fetch cadence. Does not hit the turnstile; read-only."""
    print(f"\n--- probing QR rollover ({samples} samples, {interval}s apart) ---")
    last = None
    for i in range(samples):
        qr = client.get_qr()
        m = QR_RE.match(qr)
        ts = m.group("ts") if m else "?"
        changed = "" if qr == last else "  <-- changed"
        print(f"[{i}] ts={ts} hash={qr.split('-')[-1][:12]}...{changed}")
        last = qr
        if i < samples - 1:
            time.sleep(interval)


def main() -> int:
    ap = argparse.ArgumentParser(description="VivaGym check-in QR PoC")
    ap.add_argument("--email", help="membership email")
    ap.add_argument("--password", help="membership password (else env VIVAGYM_PASSWORD or prompt)")
    ap.add_argument("--token", help="use an existing access_token instead of logging in")
    ap.add_argument("--locale", default="es", help="locale segment for exerp/newAuth (default: es)")
    ap.add_argument("--alt-client", action="store_true", help="use the alternate hardcoded client_id/secret pair")
    ap.add_argument("--png", default="checkin_qr.png", help="output PNG path (default: checkin_qr.png)")
    ap.add_argument("--probe-validity", action="store_true", help="repeatedly fetch to observe token rollover")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    cid, csec = (CLIENT_ID_ALT, CLIENT_SECRET_ALT) if args.alt_client else (CLIENT_ID, CLIENT_SECRET)
    client = VivaGymClient(locale=args.locale, client_id=cid, client_secret=csec, verbose=args.verbose)

    if args.token:
        client.access_token = args.token
    else:
        if not args.email:
            ap.error("--email is required (or pass --token)")
        password = args.password or os.environ.get("VIVAGYM_PASSWORD") or getpass.getpass("Password: ")
        print("Logging in...")
        info = client.login(args.email, password)
        print(f"OK. token_type={info.get('token_type')} expires_in={info.get('expires_in')}s")

    print("Fetching check-in QR...")
    qr = client.get_qr()
    analyze(qr)
    print()
    render_qr(qr, png_path=args.png)

    if args.probe_validity:
        probe_validity(client)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
