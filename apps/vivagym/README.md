# VivaGym check-in

Shows my own VivaGym check-in QR on the Nokia 216. See the main README, section 7, for the
whole story, and `research/` for the API notes and the Python proof of concept.

1. `copy src\secrets.example.h src\secrets.h` and fill it in. Email and password can stay
   empty: the phone asks once and saves them (hex-obfuscated, not encrypted) in `vivagym.cfg`.
2. `make APP_FILE_NAME=VivaGym PYTHON=python`
3. Sign with `../../tools/vxpsign.py` and copy to `D:\Apps\`. If secrets.h has your
   password in it, the .vxp does too: keep it to yourself.

Keys: OK/5 refresh, LSK menu (refresh, sign in again, change email/password, about, exit), RSK exit.
The code refreshes every 60 s and the screen stays on for 5 minutes after the last key press.

PC test against a fake API (build command at the top of `tools/host_vg.c`):

    python tools/mock_server.py <password>
    vgtest.exe
