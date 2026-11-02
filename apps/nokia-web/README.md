# Nokia Web

Text web browser for the Nokia 216 (MediaTek MRE) that fetches pages itself, HTTPS included
(BearSSL, TLS 1.2, 26 Mozilla roots). Redirects, chunked bodies, UTF-8/Latin-1, DuckDuckGo Lite search.

Keys: Up/Down links, Left/Right or 2/8 page, OK/5 open, LSK menu, RSK back/stop,
1 address, 3 search, 0 home, * top, # bottom.

Limits: no CSS/JS/images/forms, ~24k characters per page, TLS 1.3-only sites fail.

    make APP_FILE_NAME=NokiaWeb PYTHON=python

PC test against live sites (same web.c, MRE calls stubbed), with w64devkit gcc:

    gcc -O1 -w -c tools/host_net.c -o host_net.o
    gcc -O1 -w -I ../../sdk/include -I src/include -I src -D MRE -D __MRE_SDK__ -o webtest.exe tools/host_test.c src/bearssl_impl.c host_net.o -lws2_32 -ladvapi32
    webtest.exe https://lite.duckduckgo.com/lite/?q=nokia

Regenerate the root certificates: `python tools/mkcerts.py src/certificates.h`.
