// Copy this file to secrets.h and fill it in. secrets.h is gitignored.
#pragma once

// Your VivaGym account. Leave empty and the phone asks on first start (then saves it).
#define VG_DEFAULT_EMAIL ""
#define VG_DEFAULT_PASSWORD ""

// The app's OAuth2 client (client_credentials grant on oauth/v2/token). It is baked
// into the official Android app: decompile your own copy with jadx and look at the
// app module's BuildConfig (CLIENT_ID / CLIENT_SECRET).
#define VG_CLIENT_ID ""
#define VG_CLIENT_SECRET ""
