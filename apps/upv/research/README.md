# UPV timetable — research

How the timetable mechanism was found.

- **FINDINGS.md** — the full reverse-engineering writeup. Short version: the miUPV
  Android app authenticates with an OAuth2 client whose id/secret are fetched from
  the server at runtime (not in the APK) — a dead end for a standalone client.
  The app we ship instead uses UPV's **personal iCal feed**
  `https://www.upv.es/ical/<token>`: one HTTPS GET, parse `VEVENT`s, no OAuth.
- **ical.py** — a tiny reference client: reads your iCal URL from a local
  `ical_url.txt` (never committed), fetches it, prints your week. This is exactly
  what the phone app does, in Python, for testing. The C app streams the same feed.

Get your iCal URL from the UPV intranet / PoliformaT calendar ("subscribe / export
calendar"). The token in the URL is a credential — treat the whole URL as a secret.
