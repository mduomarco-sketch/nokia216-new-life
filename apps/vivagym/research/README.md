# VivaGym check-in QR — Python PoC

Reference implementation of the gym check-in auth engine for your own
membership. See `../FINDINGS.md` for the reverse-engineering writeup.

## The engine, in three steps
1. `POST api/v2.0/exerp/newAuth` (email + password, appName=vivagym) -> bearer token
2. `GET  api/v2.0/exerp/qr` with `Authorization: Bearer <token>` -> `exerp:checkin:<id>-<ts>-<hash>`
3. Render that exact string as a QR. (The timestamp + hash are server-side; no device secret.)

## Install & run
```
pip install -r requirements.txt
python vivagym_checkin_poc.py --email you@example.com         # prompts for password
```
Other options:
```
--token <access_token>     use an existing bearer token, skip login
--locale es                use api/v2.0/es/exerp/newAuth instead of the legacy path
--probe-validity           fetch repeatedly to observe how often the token rolls
--png out.png              where to save the QR image (default checkin_qr.png)
-v                         verbose HTTP logging
```

## For the KaiOS / BlackBerry port
This is literally `fetch()` + a QR library. The whole porting surface:
- login POST -> store access_token (+ refresh_token)
- GET exerp/qr -> render string
- re-fetch periodically (use --probe-validity here to pick the interval)
- refresh via `GET api/email/refresh?refresh_token=...` to avoid re-entering the password

## App client credentials
The PoC reads the app's OAuth client from the environment
(`VIVAGYM_CLIENT_ID`, `VIVAGYM_CLIENT_SECRET`). They are not published here: decompile
your own copy of the Android app with jadx and look at its `BuildConfig`.
