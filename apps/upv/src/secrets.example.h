// Copy this file to secrets.h and fill it in. secrets.h is gitignored and must
// NEVER be committed or shared.
//
// THE SIMPLE ROUTE (what we use): UPV publishes your class timetable as a personal
// iCal subscription feed. Get your URL from the UPV intranet / PoliformaT calendar
// ("subscribe / export calendar"). The token in the URL *is* the only credential —
// treat the whole URL as a secret (anyone with it can read your schedule).
#pragma once

// Your personal UPV iCal feed. HTTPS works (BearSSL TLS 1.2); http:// also offered.
#define UPV_ICAL_URL  "https://www.upv.es/ical/REPLACE_WITH_YOUR_TOKEN"

// Timezone: the feed is in UTC (Z). UPV classes are Europe/Madrid = UTC+1 (CET)
// in winter, UTC+2 (CEST) in summer (DST: last Sun Mar .. last Sun Oct).
#define UPV_TZ_WINTER_OFFSET_MIN  60
#define UPV_TZ_SUMMER_OFFSET_MIN  120

// ---- AI "explain my day" (optional) ----
// Uses an OpenAI-COMPATIBLE API. Defaults target DeepSeek. Leave the key blank to
// disable the AI feature (the timetable works without it). The key is compiled into
// the .vxp on the SD card, so anyone with the card could read it: use a key with a
// low spending cap. (A relay server would keep the key off the phone — ask.)
#define OPENAI_API_KEY  ""                     // your DeepSeek key (sk-...)
#define OPENAI_HOST     "api.deepseek.com"
#define OPENAI_PATH     "/chat/completions"
#define OPENAI_MODEL    "deepseek-chat"        // or "deepseek-reasoner"
