# Credits

This project stands on other people's work. Thank you.

| What | Who | License | Used for |
|---|---|---|---|
| [mre-makefile](https://github.com/gtrxAC/mre-makefile) | gtrxAC | MIT (`sdk/LICENSE.mre-makefile`) | GCC build for MRE apps; the `sdk/` folder and each app's Makefile/startup code |
| [BearSSL](https://bearssl.org/) | Thomas Pornin | MIT | TLS 1.2 on the phone (Nokia Web, VivaGym) |
| [mre_https_get_gz](https://github.com/RDZDX/mre_https_get_gz) | RDZDX | MIT (`apps/*/src/include/LICENSE.mre_https_get_gz`) | how to wire BearSSL + libco into MRE, default APN trick |
| [libco](https://github.com/higan-emu/libco) | byuu and contributors | ISC | coroutines, so network code can "block" inside MRE's callback loop |
| Mozilla CA list | Mozilla | MPL 2.0 (data) | root certificates in `certificates.h` |
| [Project Nayuki QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) | Nayuki | MIT | structure of `apps/vivagym/src/qr.c` |
| [vxpatch](https://vxpatch.luxferre.top/) / [vxp-dev-sign](https://github.com/paulober/vxp-dev-sign) | Luxferre, paulober | - | the IMSI signing scheme that `tools/vxpsign.py` reimplements offline |
| [TelnetVXP](https://github.com/XimikBoda/TelnetVXP) | XimikBoda | none stated | I ported it to the GCC build locally; not redistributed here |
| [mtmine64](https://github.com/gtrxAC/mtmine64) | gtrxAC | MIT | ran it on the phone; my one-line GCC 14 fix is in `patches/` |
| peanut.vxp (Game Boy emulator for MRE) | community | see upstream | ran it on the phone; not redistributed here |
| [Tobu Tobu Girl](https://tangramgames.itch.io/tobutobugirl) | Tangram Games | free homebrew | the Game Boy game I play on it |

Not affiliated with Nokia/HMD, MediaTek, VivaGym, MyVitale or Exerp.
