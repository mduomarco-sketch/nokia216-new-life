# Horario UPV (src\upv-build)

Shows your UPV class timetable (the PoliformaT "horario") on the Nokia 216,
straight from your **personal iCal feed** — no app login, no OAuth, no server.

## How it works
UPV publishes each student's timetable as an iCal subscription URL
(`https://www.upv.es/ical/<token>`; the token is the only credential). The app:
1. HTTPS GET of that feed (BearSSL, TLS 1.2 — reuses Nokia Web's stack).
2. **Streams** the response line-by-line through `upv_ical.c`, so the 341 KB /
   ~2-year feed never sits in RAM; events are parsed into a store (`MAX_STORE`).
3. Caches the raw `.ics` to the SD card (`upv.ics`) so it opens offline; on launch
   it shows the cached data instantly, then refreshes in the background.
4. Converts the feed's UTC times to Europe/Madrid (CET/CEST, EU DST rule).

See `..\..\upv\FINDINGS.md` for how the feed was found (the miUPV app itself uses
server-fetched OAuth creds — a dead end; the iCal URL sidesteps all of it).

## Keys
- **Left / Right** (or 4 / 6): previous / next day
- **0**: jump to today
- **#**: explain this day with AI
- **OK / 5**: refresh from the server
- **Up / Down**: scroll within a busy day
- **LSK**: menu (Actualizar, Hoy, Explicar con IA, Acerca de, Salir) · **RSK**: exit

## AI "explain my day" (addition, not replacement)
Press **#** (or menu → Explicar con IA) and the app sends the *currently shown day's*
events to an OpenAI-compatible LLM and shows a plain-Spanish summary (gaps between
classes, building changes, etc.). The authoritative timetable is always the iCal day
view; the AI is an optional overlay.
- Provider is set in `src\secrets.h`: `OPENAI_HOST` / `OPENAI_PATH` / `OPENAI_MODEL`
  / `OPENAI_API_KEY`. Defaults target **DeepSeek** (`api.deepseek.com`,
  `deepseek-chat`) — verified reachable over BearSSL TLS 1.2 (Amazon root).
- The phone calls the API directly (no relay). ⚠️ The key is compiled into the `.vxp`
  on the SD card — use a key with a low spending cap.
- Blank `OPENAI_API_KEY` disables the feature cleanly ("IA no configurada").

## Secrets
Your iCal URL lives in `src\secrets.h` (gitignored). Copy `src\secrets.example.h`
to `src\secrets.h` and paste your URL. Treat the whole URL as a password.

## Build + sign + install
```
cd src\upv-build
set PATH=%CD%\..\..\toolchain\arm-gcc\bin;%CD%\..\..\toolchain\w64devkit\bin;%PATH%
make PYTHON=..\..\toolchain\pyenv\Scripts\python.exe APP_FILE_NAME=UPV
python -I ..\..\vxpsign.py sign UPV.vxp <IMSI> ..\..\signed\UPV.vxp
copy ..\..\signed\UPV.vxp D:\Apps\
```
On the phone: File manager → Memory card → Apps → UPV.vxp.
(`required_ram` is 4096 KB in `manifest.json` for the whole-feed store; lower it
toward ~2500 with a smaller `MAX_STORE` if the phone refuses to launch it.)

## Host test (the parser, on PC)
```
..\..\toolchain\w64devkit\bin\gcc.exe -O1 -w -I src -o host_ical.exe tools\host_ical.c src\upv_ical.c
host_ical.exe ..\..\upv\poc\timetable.ics 2026 11 4
```

## Files
- `src\upv.c` — the MRE app (UI, keypad, streaming download, SD cache, net/TLS)
- `src\upv_ical.c` / `.h` — pure-C streaming iCal parser (host-testable)
- `tools\host_ical.c` — PC harness that feeds an .ics in small chunks
- net/TLS/entropy layer (`bearssl_impl.c`, `libco`, `thread`, certs) reused from VivaGym
