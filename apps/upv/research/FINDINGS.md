# miUPV timetable — reverse-engineering notes

> **RESOLUTION (what we actually shipped):** the miUPV app authenticates with an
> OAuth2 client whose id/secret are **fetched from the server at runtime** (not in
> the APK, not reachable anonymously) — a dead end for a standalone client. Instead
> the Nokia app (`src\upv-build`) uses UPV's **personal iCal feed**
> `https://www.upv.es/ical/<token>`: one HTTPS GET, parse `VEVENT`s, no OAuth. The
> OAuth reverse-engineering below is kept for the record but is **not** how the
> phone app works. Get the iCal URL from the UPV intranet / PoliformaT calendar.

**Goal:** let a registered UPV student display their **own** class timetable
(the PoliformaT / miUPV "horario" day view) on the Nokia 216, without an iPhone.
Personal-interoperability project: own credentials, own data. AI features come
*after* the plain day view works.

**Source material:**
- `miUPV_13.0.0_APKPure.apk` (`es.upv.miUpv`) — a **Flutter** app. Dart logic is
  AOT-compiled into `lib/<abi>/libapp.so`, so there is no readable Java/Kotlin for
  the business logic (unlike VivaGym). The thin `classes*.dex` is only the Flutter
  embedder shell.
- The decisive artifact: **`assets/flutter_assets/assets/jsons/init-config-default/default-config.json`**
  (110 KB) — the offline bootstrap copy of the server endpoint catalog, in
  **plaintext**. 281 production services with their full URL templates.
- `libapp.so` strings — confirm the auth model, the login UI, and the event data model.

Everything below is from static analysis. The four values marked **(confirm live)**
are the only things a single real login is needed to pin down.

---

## 1. Architecture in one paragraph

miUPV is a thin client over UPV's **Oracle PL/SQL REST** gateways
(`www.upv.es/pls/<gw>/<package>.<procedure>`). Two gateway families matter:

| Gateway prefix | Meaning | Auth |
|---|---|---|
| `sorest` / `dorest` | open/"public" services | none (or app token) |
| `soalu` / `doalu` | **secure student (ALUmno)** services | **`Authorization: Bearer <TA>`** |

(`s*` = production, `d*` = development; the config ships both.) The app discovers
every URL from `default-config.json` (and refreshes it from
`sic_portal_apps.config_version` at startup), then fills `{p_...}` placeholders.

---

## 2. The timetable endpoint — the key finding

The class schedule is the **iCal events** feed, service name `getIcals`:

```
GET https://www.upv.es/pls/soalu/sic_app_ical.geteventos
      ?p_fecha_ini={DD-MM-YYYY}
      &p_fecha_fin={DD-MM-YYYY}
      &p_idioma={es|va|en}
      &p_modo={MODE}            # (confirm live) — selects horario / exámenes / all
Authorization: Bearer <access_token>
```

- `soalu` ⇒ **requires the user bearer token** from the OAuth step below.
- The app requests a **date range** (`p_fecha_ini`..`p_fecha_fin`), so a "day view"
  is just `fecha_ini = fecha_fin = today`, and a week view is Mon..Sun. This maps
  perfectly onto the Nokia day view the user wants.
- Response is **JSON**, an array of event objects (not raw `.ics`). Field names
  recovered from `libapp.so` (§4).
- `p_idioma=es` for Spanish.

Related (not needed for the timetable, but in the catalog — see
`endpoints_production.txt`): `getHome` (`sorest/sic_app_miUpv.get_eventos_home`,
public home banner events), and the user-event CRUD
(`sic_app_ical_user.insert/modify/delete_ical_user`) for personal events.

---

## 3. Authentication flow (OAuth2, headless — portable to C)

Login is a **native username/password form** inside the app
(`loginPage.login_user_hint_text`, `login_password_hint_text`,
`auth._validateCredentials`), **not** a WebView SSO redirect. The WebView/cookie
code in the APK belongs to other features (content pages, UPV digital signature).

```
service "login":  POST https://siupv.upv.es/oauth/tokens/
service "logout": POST https://siupv.upv.es/oauth/logout/
```

The POST carries (field names confirmed in `libapp.so`):
```
grant_type   = password            # (confirm live: literal value)
username     = <DNI / user>
password     = <UPV password>
client_id     = <app identity>     # (confirm live) — compiled into libapp.so, NOT in the json
client_secret = <app identity>     # (confirm live)
```
Response (standard OAuth2 shape, labels seen in binary):
```json
{ "access_token": "<TA>", "refresh_token": "<TR>",
  "token_type": "Bearer", "expires_in": <s> }
```
- Internally the app calls these **TA** (token de acceso) and **TR** (token de
  refresco); the bridge class is `OauthCuaBridge` ("CUA" = UPV single sign-on).
- **Attach to every `soalu` call:** `Authorization: Bearer <TA>`.
- **Refresh:** same endpoint, `grant_type=refresh_token` + `refresh_token=<TR>`.
  On refresh failure the app re-logs in with user+password.

This is the **same shape as VivaGym** (app-identity creds + user creds → bearer →
authenticated GET), so the VivaGym C code is a direct template.

---

## 4. Event data model (JSON fields from libapp.so)

A `geteventos` item carries at least:

| Field | Meaning |
|---|---|
| `asignaturaNombre` (`...Ingles`/`...Valenciano`) | subject name |
| `asignaturaCodigo` | subject code |
| `grupo`, `grupoNombre`, `codgrupo`, `grupoHorario` | class group |
| `grupoEspacio` / `gruposEspacios` | **classroom / aula** |
| `fechaIni` / `fechaInicio` / `fechaInicial` | start date |
| `horaIni`, `horaFin` (`horaInicial`) | start / end time |
| `descripcion` (`...Ingles`/`...Valenciano`) | description |
| `TITULO` | event title |
| `color`, `colorEstado` | UI colour (type of event) |
| `eventoGrupo`, `examenes`, `horarioFinDeSemana` | flags: group event / exam / weekend |

So on the Nokia a day is: for each event on `today`, show
`horaIni–horaFin  asignaturaNombre  (grupoEspacio)`. That is the PoliformaT day
view, rendered from the raw server data — **AI is not involved** at this stage.

---

## 5. What a Nokia (C/MRE) client must do

```
0. POST siupv.upv.es/oauth/tokens/  (grant_type=password, username, password,
                                      client_id, client_secret)        -> TA + TR
1. GET  www.upv.es/pls/soalu/sic_app_ical.geteventos
        ?p_fecha_ini=DD-MM-YYYY&p_fecha_fin=DD-MM-YYYY&p_idioma=es&p_modo=MODE
        Authorization: Bearer <TA>                                     -> JSON events
2. Parse events, draw the day (and L/R to change day).
3. Cache TA/TR + the last fetched week to C:\upv.cfg, so it opens offline.
4. Renew TA via grant_type=refresh_token before re-asking for the password.
```
Reuses from Nokia Web / VivaGym: BearSSL TLS 1.2, APN setup, DNS, TCP coroutine,
`json_get`-style parsing (needs a small array/object walker for the event list),
UTF-8 text wrapping, hex-obfuscated settings file, DNI+password from a gitignored
`secrets.h`.

### TLS caveat (must verify early, like VivaGym did)
BearSSL here is **TLS 1.2 only**. Both `siupv.upv.es` and `www.upv.es` must accept
TLS 1.2 and chain to one of the bundled roots. Quick host test (reuses web-build):
`webtest.exe https://www.upv.es/` and `https://siupv.upv.es/`. A TLS 1.3-only host
would force a relay (the same relay idea as the music/LLM assessment).

---

## 6. Open items to confirm with ONE real login (mitmproxy, or a go-ahead to try live)
1. `client_id` / `client_secret` — the app identity. Compiled into `libapp.so`;
   not in the json. Capture from the login POST, or extract from the binary.
2. `grant_type` literal (expected `password`) and exact field names
   (`username` vs `dni`).
3. `p_modo` value(s): which selects the lectivo class timetable vs exams vs all.
4. Date format of `p_fecha_ini/fin` (expected `DD-MM-YYYY`) and the exact JSON
   envelope (bare array vs `{...}` wrapper).

None of these change the design; they're the constants the C client hardcodes.

## 7. Files in this folder
```
FINDINGS.md               ← this file
endpoints_production.txt  ← all 281 production services (servicio -> URI)
apk/                      ← the analysed APK
work/                     ← extracted libapp.so, default-config.json, strings dumps
```

## ⚠️ Security / privacy
- `client_id`/`client_secret` are the **app's** identity (to be extracted), not the
  user's — but don't publish them. The repo's public mirror must exclude them.
- The user's **DNI, UPV password and TA/TR tokens are sensitive**: compile the
  password in from a gitignored `secrets.h` (VivaGym pattern), keep tokens out of
  logs/screenshots, store the cfg hex-obfuscated. Own-account access only.
