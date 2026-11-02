// VivaGym check-in - shows your own VivaGym check-in QR on a Nokia 216 (S30+, MediaTek MRE).
//
// Same flow as the official app (see vivagym_checkin_handoff/FINDINGS.md):
//   0. POST oauth/v2/token (app client credentials)          -> temp token
//   1. POST api/v2.0/es/exerp/newAuth (temp token, email, pw) -> access + refresh token
//   2. GET  api/v2.0/exerp/qr  (Authorization: Bearer)        -> "exerp:checkin:<id>-<ts>-<hash>"
//   3. Draw that string as a QR. Re-fetch every minute while the screen is in use.
// The phone talks HTTPS itself (BearSSL, TLS 1.2), no proxy.
//
// Keys: OK/5 refresh, LSK menu, RSK exit.
#define inline
#include "bearssl.h"
#include "certificates.h"
#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "vmtimer.h"
#include "vmsock.h"
#include "thread.h"
#include "qr.h"
#include "secrets.h"
#include <string.h>
#include <stdio.h>

/* ---------- server ---------- */

#ifndef VG_HOST
#define VG_HOST "vivagym.myvitale.com"
#endif
#ifndef VG_PORT
#define VG_PORT 443
#endif
#ifndef VG_TLS
#define VG_TLS 1
#endif
#define VG_LOCALE "es"
#define VG_APPNAME "vivagym"
// App-level OAuth client from the official APK (lives in secrets.h, see secrets.example.h)
#define CLIENT_ID VG_CLIENT_ID
#define CLIENT_SECRET VG_CLIENT_SECRET
#define UA "okhttp/4.9.0"

#define NET_TIMEOUT_MS 45000
#define QR_REFRESH_MS 60000      // fetch a fresh code every minute while in use
#define ACTIVE_WINDOW_MS 300000  // stop auto-refresh / screen-on 5 min after the last key
#define CFG_NAME "vivagym.cfg"

/* ---------- state ---------- */

static char cfg_email[128], cfg_pass[128], cfg_access[512], cfg_refresh[512];

enum { S_START, S_NEED_LOGIN, S_BUSY, S_QR, S_ERROR, S_ABOUT };
static int screen = S_START;
enum { J_NONE, J_QR, J_LOGIN };
static volatile int job_req = J_NONE, busy;
static char status_msg[96];
static char err_msg[300];

static qr_t qr;
static int have_qr;
static char qr_text[256];
static int qr_warn;              // string didn't look like exerp:checkin:...
static VMINT qr_tick;            // when the code was fetched
static char qr_err[96];          // last refresh failed (old code still shown)
static VMINT last_input;
static int screen_kept_on;

static int menu_open, menu_sel;
#define MENU_N 6

/* ---------- screen ---------- */

static int scr_w = 240, scr_h = 320;
static VMINT layer = -1;
static VMUINT8 *buf = 0;
static VMINT timer_id = -1;
static int fh = 14, lh = 17;
#define BAR_H (lh + 4)
#define MARGIN 4
static int dirty = 1;
static VMUINT16 c_bg, c_text, c_bar, c_bar_text, c_dim, c_err, c_accent, c_black, c_white;

/* ===================================================================== */
/*  Entropy for BearSSL                                                  */
/* ===================================================================== */

static br_sha256_context ent_ctx;
static int ent_ready;

static void ent_add(const void *data, size_t len) {
	if (!ent_ready) { br_sha256_init(&ent_ctx); ent_ready = 1; }
	br_sha256_update(&ent_ctx, data, len);
}

static void ent_tick(void) {
	VMINT t = vm_get_tick_count();
	ent_add(&t, sizeof t);
}

static void drive_path(VMWCHAR *out, int size, int drive, const char *name) {
	char p[48];
	sprintf(p, "%c:\\%s", (char)drive, name);
	vm_ascii_to_ucs2(out, size, p);
}

static int removable_drive(void) {
	VMINT d = vm_get_removable_driver();
	return d < 0 ? vm_get_system_driver() : d;
}

static void seed_save(void) {
	VMWCHAR path[48];
	unsigned char h[32];
	br_sha256_context c = ent_ctx;
	VMFILE f;
	VMUINT n;
	ent_add("save", 4);
	br_sha256_out(&c, h);
	drive_path(path, sizeof path, removable_drive(), "vivagym.seed");
	f = vm_file_open(path, MODE_CREATE_ALWAYS_WRITE, TRUE);
	if (f < 0) return;
	vm_file_write(f, h, sizeof h, &n);
	vm_file_close(f);
}

static void seed_load(void) {
	VMWCHAR path[48];
	unsigned char h[32];
	VMFILE f;
	VMUINT n = 0;
	VMSTR imei;
	vm_time_t t;
	int i;
	drive_path(path, sizeof path, removable_drive(), "vivagym.seed");
	f = vm_file_open(path, MODE_READ, TRUE);
	if (f >= 0) {
		if (vm_file_read(f, h, sizeof h, &n) >= 0 && n > 0) ent_add(h, n);
		vm_file_close(f);
	}
	imei = vm_get_imei();
	if (imei) ent_add(imei, strlen(imei));
	vm_get_time(&t);
	ent_add(&t, sizeof t);
	for (i = 0; i < 64; i++) ent_tick();
	seed_save();
}

void br_mre_get_entropy(unsigned char *out, size_t len) {
	unsigned char h[32];
	br_sha256_context c;
	vm_time_t t;
	int i;
	vm_get_time(&t);
	ent_add(&t, sizeof t);
	for (i = 0; i < 32; i++) ent_tick();
	c = ent_ctx;
	br_sha256_out(&c, h);
	memcpy(out, h, len < 32 ? len : 32);
	ent_add(h, sizeof h);
	seed_save();
}

/* ===================================================================== */
/*  Helpers                                                              */
/* ===================================================================== */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int starts_with_ci(const char *s, const char *prefix) {
	while (*prefix) if (lower(*s++) != lower(*prefix++)) return 0;
	return 1;
}

static const char *strstr_ci(const char *hay, const char *needle) {
	for (; *hay; hay++) if (starts_with_ci(hay, needle)) return hay;
	return 0;
}

static void copy_str(char *dst, const char *src, int size) {
	int i = 0;
	while (src[i] && i < size - 1) { dst[i] = src[i]; i++; }
	dst[i] = 0;
}

static int hexval(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

// application/x-www-form-urlencoded / query component
static void url_encode(const char *src, char *dst, int size) {
	static const char hex[] = "0123456789ABCDEF";
	int i = 0;
	for (; *src && i < size - 4; src++) {
		unsigned char c = (unsigned char)*src;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			dst[i++] = c;
		else { dst[i++] = '%'; dst[i++] = hex[c >> 4]; dst[i++] = hex[c & 15]; }
	}
	dst[i] = 0;
}

static int ucs2_to_utf8(const VMWCHAR *src, char *dst, int size) {
	int i = 0;
	for (; *src; src++) {
		unsigned c = *src;
		if (c < 0x80) { if (i + 1 >= size) break; dst[i++] = (char)c; }
		else if (c < 0x800) { if (i + 2 >= size) break; dst[i++] = 0xC0 | (c >> 6); dst[i++] = 0x80 | (c & 63); }
		else { if (i + 3 >= size) break; dst[i++] = 0xE0 | (c >> 12); dst[i++] = 0x80 | ((c >> 6) & 63); dst[i++] = 0x80 | (c & 63); }
	}
	dst[i] = 0;
	return i;
}

static void trim(char *s) {
	int n = strlen(s), i = 0;
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0;
	while (s[i] == ' ' || s[i] == '\r' || s[i] == '\n' || s[i] == '\t') i++;
	if (i) memmove(s, s + i, n - i + 1);
}

// Decodes a JSON string literal starting at p (just after the opening quote).
static void json_unescape(const char *p, char *out, int size) {
	int o = 0;
	while (*p && *p != '"' && o < size - 1) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': out[o++] = '\n'; break;
			case 't': out[o++] = '\t'; break;
			case 'r': out[o++] = '\r'; break;
			case 'b': case 'f': break;
			case 'u': {
				int cp = 0, k;
				for (k = 1; k <= 4 && hexval(p[k]) >= 0; k++) cp = cp * 16 + hexval(p[k]);
				p += k - 1;
				if (cp < 0x80) out[o++] = (char)cp;
				else if (cp < 0x800 && o + 2 < size) { out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 63); }
				else if (o + 3 < size) { out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 63); out[o++] = 0x80 | (cp & 63); }
				break;
			}
			default: out[o++] = *p; // \" \\ \/
			}
			p++;
		} else out[o++] = *p++;
	}
	out[o] = 0;
}

// Finds "key": value in a flat JSON object. Strings are unescaped, numbers/literals copied.
static int json_get(const char *json, const char *key, char *out, int size) {
	char pat[48];
	const char *p = json;
	sprintf(pat, "\"%s\"", key);
	out[0] = 0;
	while ((p = strstr(p, pat)) != 0) {
		const char *q = p + strlen(pat);
		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
		if (*q != ':') { p = q; continue; }
		q++;
		while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
		if (*q == '"') json_unescape(q + 1, out, size);
		else {
			int o = 0;
			while (*q && *q != ',' && *q != '}' && *q != ' ' && *q != '\r' && *q != '\n' && o < size - 1) out[o++] = *q++;
			out[o] = 0;
			if (!strcmp(out, "null")) out[0] = 0;
		}
		return 1;
	}
	return 0;
}

/* ===================================================================== */
/*  Settings file: email, password, tokens (obfuscated, not encrypted)   */
/* ===================================================================== */

static const char OBF_KEY[] = "N216-vivagym-checkin";

static void obf_line(const char *s, char *out, int size) {
	static const char hex[] = "0123456789abcdef";
	int i, o = 0;
	for (i = 0; s[i] && o < size - 3; i++) {
		unsigned char c = (unsigned char)s[i] ^ (unsigned char)OBF_KEY[i % (sizeof OBF_KEY - 1)];
		out[o++] = hex[c >> 4];
		out[o++] = hex[c & 15];
	}
	out[o] = 0;
}

static void deobf_line(const char *s, char *out, int size) {
	int i, o = 0;
	for (i = 0; hexval(s[i]) >= 0 && hexval(s[i + 1]) >= 0 && o < size - 1; i += 2, o++)
		out[o] = (char)((hexval(s[i]) * 16 + hexval(s[i + 1])) ^ (unsigned char)OBF_KEY[o % (sizeof OBF_KEY - 1)]);
	out[o] = 0;
}

static char cfg_buf[2600];

static void cfg_save(void) {
	VMWCHAR path[48];
	char line[1100];
	VMFILE f;
	VMUINT n;
	int drives[2], i;
	cfg_buf[0] = 0;
	strcat(cfg_buf, "VG1\n");
	obf_line(cfg_email, line, sizeof line); strcat(cfg_buf, line); strcat(cfg_buf, "\n");
	obf_line(cfg_pass, line, sizeof line); strcat(cfg_buf, line); strcat(cfg_buf, "\n");
	obf_line(cfg_access, line, sizeof line); strcat(cfg_buf, line); strcat(cfg_buf, "\n");
	obf_line(cfg_refresh, line, sizeof line); strcat(cfg_buf, line); strcat(cfg_buf, "\n");
	// phone memory first (keeps it off the SD card), memory card as fallback
	drives[0] = vm_get_system_driver();
	drives[1] = vm_get_removable_driver();
	for (i = 0; i < 2; i++) {
		if (drives[i] < 0) continue;
		drive_path(path, sizeof path, drives[i], CFG_NAME);
		f = vm_file_open(path, MODE_CREATE_ALWAYS_WRITE, TRUE);
		if (f < 0) continue;
		vm_file_write(f, cfg_buf, strlen(cfg_buf), &n);
		vm_file_close(f);
		if (n == strlen(cfg_buf)) {
			if (i == 0 && drives[1] >= 0) { drive_path(path, sizeof path, drives[1], CFG_NAME); vm_file_delete(path); }
			return;
		}
	}
}

static void cfg_load(void) {
	VMWCHAR path[48];
	VMFILE f = -1;
	VMUINT n = 0;
	int drives[2], i;
	char *lines[5], *p;
	drives[0] = vm_get_system_driver();
	drives[1] = vm_get_removable_driver();
	for (i = 0; i < 2 && f < 0; i++) {
		if (drives[i] < 0) continue;
		drive_path(path, sizeof path, drives[i], CFG_NAME);
		f = vm_file_open(path, MODE_READ, TRUE);
	}
	if (f >= 0) {
		if (vm_file_read(f, cfg_buf, sizeof cfg_buf - 1, &n) < 0) n = 0;
		vm_file_close(f);
	}
	cfg_buf[n] = 0;
	for (i = 0, p = cfg_buf; i < 5; i++) {
		lines[i] = p;
		p = strchr(p, '\n');
		if (p) *p++ = 0; else p = cfg_buf + n;
	}
	if (!strcmp(lines[0], "VG1")) {
		deobf_line(lines[1], cfg_email, sizeof cfg_email);
		deobf_line(lines[2], cfg_pass, sizeof cfg_pass);
		deobf_line(lines[3], cfg_access, sizeof cfg_access);
		deobf_line(lines[4], cfg_refresh, sizeof cfg_refresh);
	}
	if (!cfg_email[0]) copy_str(cfg_email, VG_DEFAULT_EMAIL, sizeof cfg_email);
	if (!cfg_pass[0]) copy_str(cfg_pass, VG_DEFAULT_PASSWORD, sizeof cfg_pass);
}

/* ===================================================================== */
/*  Networking (runs inside the worker coroutine)                        */
/* ===================================================================== */

static VMINT tcp_h = -1;
static volatile int tcp_connected, tcp_err;
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static VMINT net_deadline;

static void tcp_cb(VMINT h, VMINT ev) {
	ent_tick();
	if (h != tcp_h) return;
	if (ev == VM_TCP_EVT_CONNECTED) tcp_connected = 1;
	else if (ev == VM_TCP_EVT_HOST_NOT_FOUND) tcp_err = 2;
	else if (ev == VM_TCP_EVT_PIPE_BROKEN || ev == VM_TCP_EVT_PIPE_CLOSED) tcp_err = 1;
}

static int timed_out(void) { return vm_get_tick_count() - net_deadline > 0; }

static int raw_read(void *ctx, unsigned char *b, size_t n) {
	(void)ctx;
	for (;;) {
		int r = vm_tcp_read(tcp_h, b, n);
		if (r > 0) { net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS; return r; }
		if (tcp_err || timed_out()) return -1;
		thread_next();
	}
}

static int raw_write(void *ctx, const unsigned char *b, size_t n) {
	(void)ctx;
	for (;;) {
		int r = vm_tcp_write(tcp_h, (void *)b, n);
		if (r > 0) { net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS; return r; }
		if (tcp_err || timed_out()) return -1;
		thread_next();
	}
}

static unsigned long days_since_0(int y, int m, int d) {
	static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	unsigned long days;
	y--;
	days = 365UL * (y + 1) + y / 4 - y / 100 + y / 400;
	y++;
	days += cum[m - 1] + d - 1;
	if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
	return days;
}

// newlib's time() lands here; MRE has no syscall layer, so answer from the phone clock.
struct newlib_timeval { long long tv_sec; long tv_usec; };
int _gettimeofday(struct newlib_timeval *tv, void *tz) {
	vm_time_t t;
	(void)tz;
	vm_get_time(&t);
	if (tv) {
		tv->tv_sec = (long long)((days_since_0(t.year, t.mon, t.day) - days_since_0(1970, 1, 1)) * 86400UL +
			t.hour * 3600UL + t.min * 60UL + t.sec);
		tv->tv_usec = 0;
	}
	return 0;
}

// Use the phone's default internet data account (CMNET preset breaks DNS on S30+).
static int apn_mode = -1;
static void apn_setup(void) {
	vm_apn_info_ext apn;
	if (apn_mode >= 0) return;
	memset(&apn, 0, sizeof apn);
	if (vm_get_default_apn_info(&apn) == 0) {
		vm_dtacct_set(0, apn.apn_info_id);
		apn_mode = VM_APN_USER_DEFINE;
	} else apn_mode = VM_TCP_APN_CMNET;
}

static volatile int dns_done;
static vm_soc_dns_result dns_res;
static VMINT dns_cb(vm_soc_dns_result *r) {
	if (r) dns_res = *r;
	dns_done = 1;
	return 0;
}

static int resolve_host(const char *host, char *ip) {
	VMINT r;
	unsigned char *a;
	memset(&dns_res, 0, sizeof dns_res);
	dns_done = 0;
	r = vm_soc_get_host_by_name(apn_mode, host, &dns_res, dns_cb);
	if (r == VM_E_SOC_WOULDBLOCK) {
		while (!dns_done) {
			if (timed_out()) return 0;
			thread_next();
		}
	} else if (r != VM_E_SOC_SUCCESS) return 0;
	if (dns_res.num <= 0) return 0;
	a = (unsigned char *)&dns_res.address[0];
	sprintf(ip, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
	return 1;
}

static const char *tcp_open(const char *host, int port) {
	tcp_connected = tcp_err = 0;
	tcp_h = vm_tcp_connect(host, port, apn_mode, tcp_cb);
	if (tcp_h < 0) return "Could not open a connection. Is mobile data on?";
	while (!tcp_connected) {
		if (tcp_err == 2) return "dns";
		if (tcp_err) return "Connection refused or dropped.";
		if (timed_out()) return "Connection timed out. Check mobile data.";
		thread_next();
	}
	return 0;
}

static void net_close(void) {
	if (tcp_h >= 0) vm_tcp_close(tcp_h);
	tcp_h = -1;
}

static const char *net_open(void) {
	const char *err;
	char ip[16];
	net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS;
	apn_setup();
	err = tcp_open(VG_HOST, VG_PORT);
	if (err && !strcmp(err, "dns")) {
		net_close();
		if (!resolve_host(VG_HOST, ip)) return "Server not found (DNS). Is mobile data on?";
		err = tcp_open(ip, VG_PORT);
		if (err && !strcmp(err, "dns")) err = "Could not reach the server.";
	}
	if (err) return err;
	if (VG_TLS) {
		vm_time_t t;
		br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
		vm_get_time(&t);
		if (t.year < 2026) { t.year = 2026; t.mon = 11; t.day = 1; t.hour = 12; t.min = t.sec = 0; } // clock never set
		br_x509_minimal_set_time(&xc, days_since_0(t.year, t.mon, t.day), t.hour * 3600UL + t.min * 60UL + t.sec);
		br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
		br_ssl_client_reset(&sc, VG_HOST, 0);
		br_sslio_init(&ioc, &sc.eng, raw_read, 0, raw_write, 0);
	}
	return 0;
}

static int net_write(const char *s, int n) {
	if (VG_TLS) {
		if (br_sslio_write_all(&ioc, s, n) < 0) return -1;
		return br_sslio_flush(&ioc);
	}
	while (n > 0) {
		int r = raw_write(0, (const unsigned char *)s, n);
		if (r < 0) return -1;
		s += r; n -= r;
	}
	return 0;
}

static int net_read(char *b, int n) {
	if (VG_TLS) return br_sslio_read(&ioc, b, n);
	return raw_read(0, (unsigned char *)b, n);
}

static const char *tls_error_text(void) {
	static char msg[64];
	int err;
	if (!VG_TLS) return "Connection closed.";
	err = br_ssl_engine_last_error(&sc.eng);
	switch (err) {
	case 0: return "Connection closed by the server.";
	case BR_ERR_X509_EXPIRED: return "Certificate expired or the phone's date is wrong. Set date/time in Settings.";
	case BR_ERR_X509_NOT_TRUSTED: return "Server certificate not trusted.";
	}
	sprintf(msg, "Secure connection failed (TLS error %d).", err);
	return msg;
}

/* ---------- one HTTP request ---------- */

static char resp[4096];
static int resp_len;
static char req[2048];

// Returns the HTTP status, or -1 on a network error (err_msg filled).
static int http(const char *method, const char *path, const char *ctype, const char *body, const char *bearer) {
	char line[600], chunk[256];
	int status = -1, chunked = 0, body_left = -1, len, i;
	int cstate = 0, cleft = 0;
	const char *err;

	resp_len = 0;
	resp[0] = 0;
	err = net_open();
	if (err) { net_close(); copy_str(err_msg, err, sizeof err_msg); return -1; }

	len = sprintf(req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA "\r\nAccept: application/json\r\n"
		"Accept-Encoding: identity\r\nConnection: close\r\n", method, path, VG_HOST);
	if (bearer && bearer[0]) len += sprintf(req + len, "Authorization: Bearer %s\r\n", bearer);
	if (body) len += sprintf(req + len, "Content-Type: %s\r\nContent-Length: %d\r\n", ctype, (int)strlen(body));
	len += sprintf(req + len, "\r\n");
	if (net_write(req, len) < 0 || (body && body[0] && net_write(body, strlen(body)) < 0)) {
		copy_str(err_msg, tls_error_text(), sizeof err_msg);
		net_close();
		return -1;
	}

	/* headers */
	for (;;) {
		char c;
		int got = 0, hl = 0;
		for (;;) {
			int r = net_read(&c, 1);
			if (r <= 0) break;
			got = 1;
			if (c == '\n') break;
			if (c != '\r' && hl < (int)sizeof(line) - 1) line[hl++] = c;
		}
		line[hl] = 0;
		if (!got) { status = -2; break; }
		if (status == -1) {
			if (!starts_with_ci(line, "HTTP/") || hl < 12) { status = -2; break; }
			status = 0;
			for (i = 9; line[i] >= '0' && line[i] <= '9'; i++) status = status * 10 + (line[i] - '0');
			continue;
		}
		if (!hl) break;
		if (starts_with_ci(line, "Transfer-Encoding:") && strstr_ci(line, "chunked")) chunked = 1;
		else if (starts_with_ci(line, "Content-Length:")) {
			const char *v = line + 15;
			while (*v == ' ') v++;
			body_left = 0;
			while (*v >= '0' && *v <= '9') body_left = body_left * 10 + (*v++ - '0');
		}
	}
	if (status < 0) {
		copy_str(err_msg, status == -2 && VG_TLS ? tls_error_text() : "The server sent no valid response.", sizeof err_msg);
		net_close();
		return -1;
	}

	/* body */
	{
		int done = body_left == 0;
		while (!done) {
			int r = net_read(chunk, sizeof chunk);
			if (r <= 0) break;
			for (i = 0; i < r && !done; i++) {
				unsigned char b = (unsigned char)chunk[i];
				int keep = 0;
				if (!chunked) {
					keep = 1;
					if (body_left > 0 && --body_left == 0) done = 1;
				} else switch (cstate) {
				case 0:
					if (hexval(b) >= 0) cleft = cleft * 16 + hexval(b);
					else if (b == '\n') { if (!cleft) done = 1; else cstate = 2; }
					else cstate = 1;
					break;
				case 1:
					if (b == '\n') { if (!cleft) done = 1; else cstate = 2; }
					break;
				case 2:
					keep = 1;
					if (--cleft == 0) cstate = 3;
					break;
				case 3:
					if (b == '\n') { cstate = 0; cleft = 0; }
					break;
				}
				if (keep && resp_len < (int)sizeof(resp) - 1) resp[resp_len++] = (char)b;
			}
		}
	}
	resp[resp_len] = 0;
	net_close();
	seed_save();
	return status;
}

/* ---------- VivaGym API ---------- */

static void set_status(const char *s) { copy_str(status_msg, s, sizeof status_msg); dirty = 1; }

static void server_message(char *out, int size) {
	char m[200];
	out[0] = 0;
	if (json_get(resp, "message", m, sizeof m) && m[0]) copy_str(out, m, size);
	else if (json_get(resp, "error_description", m, sizeof m) && m[0]) copy_str(out, m, size);
	else if (json_get(resp, "error", m, sizeof m) && m[0]) copy_str(out, m, size);
	else { copy_str(out, resp, size > 120 ? 120 : size); trim(out); }
}

// Step 0: app-level client-credentials token.
static int get_temp_token(char *tok, int size) {
	static const char json_body[] =
		"{\"grant_type\":\"client_credentials\",\"client_id\":\"" CLIENT_ID "\",\"client_secret\":\"" CLIENT_SECRET "\"}";
	int st;
	set_status("Connecting to VivaGym...");
	st = http("POST", "/oauth/v2/token", "application/json", json_body, 0);
	if (st == 200 && json_get(resp, "access_token", tok, size) && tok[0]) return 1;
	if (st < 0) return 0;
	// the Retrofit interface declares it as GET with query parameters: try that too
	st = http("GET", "/oauth/v2/token?grant_type=client_credentials&client_id=" CLIENT_ID "&client_secret=" CLIENT_SECRET, 0, 0, 0);
	if (st == 200 && json_get(resp, "access_token", tok, size) && tok[0]) return 1;
	if (st >= 0) {
		char m[120];
		server_message(m, sizeof m);
		sprintf(err_msg, "App token refused (HTTP %d). %s", st, m);
	}
	return 0;
}

// Steps 0+1: full login with the saved email/password.
static int do_login(void) {
	char temp[512], e_email[400], e_pass[400], e_tok[1600];
	static char body[2600];
	int st;
	if (!cfg_email[0] || !cfg_pass[0]) { copy_str(err_msg, "Email or password missing.", sizeof err_msg); return 0; }
	if (!get_temp_token(temp, sizeof temp)) return 0;
	url_encode(temp, e_tok, sizeof e_tok);
	url_encode(cfg_email, e_email, sizeof e_email);
	url_encode(cfg_pass, e_pass, sizeof e_pass);
	sprintf(body, "access_token=%s&password=%s&email=%s&appName=" VG_APPNAME, e_tok, e_pass, e_email);
	set_status("Signing in...");
	st = http("POST", "/api/v2.0/" VG_LOCALE "/exerp/newAuth", "application/x-www-form-urlencoded", body, 0);
	memset(body, 0, sizeof body);
	if (st < 0) return 0;
	if (st == 200 && json_get(resp, "access_token", cfg_access, sizeof cfg_access) && cfg_access[0]) {
		json_get(resp, "refresh_token", cfg_refresh, sizeof cfg_refresh);
		cfg_save();
		return 1;
	}
	{
		char m[200];
		server_message(m, sizeof m);
		sprintf(err_msg, "Login failed (HTTP %d). %s", st, m[0] ? m : "Check email and password.");
	}
	cfg_access[0] = 0;
	return -1; // credentials rejected
}

static int do_refresh(void) {
	char e[1600], path[1700];
	int st;
	if (!cfg_refresh[0]) return 0;
	url_encode(cfg_refresh, e, sizeof e);
	sprintf(path, "/api/email/refresh?refresh_token=%s", e);
	set_status("Renewing session...");
	st = http("GET", path, 0, 0, 0);
	if (st == 200 && json_get(resp, "access_token", cfg_access, sizeof cfg_access) && cfg_access[0]) {
		char r[512];
		if (json_get(resp, "refresh_token", r, sizeof r) && r[0]) copy_str(cfg_refresh, r, sizeof cfg_refresh);
		cfg_save();
		return 1;
	}
	if (st >= 0) { cfg_refresh[0] = 0; cfg_access[0] = 0; }
	return 0;
}

// Pulls the check-in string out of whatever shape the body has.
static int extract_qr(char *out, int size) {
	char tmp[600];
	int i;
	copy_str(tmp, resp, sizeof tmp);
	trim(tmp);
	if (tmp[0] == '"') json_unescape(tmp + 1, out, size);
	else if (tmp[0] == '{') {
		if (!json_get(tmp, "qr", out, size) && !json_get(tmp, "data", out, size) &&
			!json_get(tmp, "code", out, size) && !json_get(tmp, "value", out, size)) out[0] = 0;
		// a numeric "code" is the error/status field of LoginResponse-style bodies
		if (out[0] && hexval(out[0]) >= 0 && strlen(out) < 4) out[0] = 0;
	} else copy_str(out, tmp, size);
	trim(out);
	if (!out[0] || strlen(out) > 200) return 0;
	for (i = 0; out[i]; i++) if ((unsigned char)out[i] < 32) return 0;
	return 1;
}

// 0 = network/other error, 1 = ok, 2 = not authorised
static int get_qr_once(void) {
	char text[256];
	int st;
	set_status("Getting your QR...");
	st = http("GET", "/api/v2.0/exerp/qr", 0, 0, cfg_access);
	if (st < 0) return 0;
	if (st == 401 || st == 403) return 2;
	if (st == 200 && extract_qr(text, sizeof text)) {
		qr_t tmp;
		if (!qr_encode((const unsigned char *)text, strlen(text), -1, &tmp)) {
			copy_str(err_msg, "QR text too long to draw.", sizeof err_msg);
			return 0;
		}
		qr = tmp;
		copy_str(qr_text, text, sizeof qr_text);
		qr_warn = !starts_with_ci(text, "exerp:checkin:");
		have_qr = 1;
		qr_tick = vm_get_tick_count();
		qr_err[0] = 0;
		return 1;
	}
	{
		char m[160];
		server_message(m, sizeof m);
		// some servers answer an expired token with 400/500 + an auth message
		if (st >= 400 && (strstr_ci(m, "token") || strstr_ci(m, "auth") || strstr_ci(m, "expired"))) return 2;
		sprintf(err_msg, "Server error getting the QR (HTTP %d). %s", st, m);
	}
	return 0;
}

// Full chain: cached token -> refresh token -> password login.
static void job_qr(int force_login) {
	int r = 2, lr;
	err_msg[0] = 0;
	if (!force_login && cfg_access[0]) r = get_qr_once();
	if (r == 2 && !force_login && do_refresh()) r = get_qr_once();
	if (r == 2) {
		if (!cfg_email[0] || !cfg_pass[0]) { screen = S_NEED_LOGIN; return; }
		lr = do_login();
		if (lr == 1) r = get_qr_once();
		else r = 0;
		if (r == 2) copy_str(err_msg, "Signed in, but the server refused the QR request.", sizeof err_msg);
	}
	if (r == 1) { screen = S_QR; return; }
	if (have_qr) {
		// keep showing the last code; it may still be accepted
		copy_str(qr_err, err_msg[0] ? err_msg : "Update failed.", sizeof qr_err);
		screen = S_QR;
	} else screen = S_ERROR;
}

static void worker(void) {
	for (;;) {
		int j;
		while (job_req == J_NONE) thread_next();
		j = job_req;
		job_req = J_NONE;
		busy = 1;
		dirty = 1;
		job_qr(j == J_LOGIN);
		busy = 0;
		status_msg[0] = 0;
		dirty = 1;
	}
}

static void request(int job) {
	if (busy) return;
	job_req = job;
	busy = 1;
	if (!have_qr) screen = S_BUSY;
	dirty = 1;
}

/* ===================================================================== */
/*  Text input (email / password)                                        */
/* ===================================================================== */

static VMWCHAR input_buf[130];
static void ask_password(void);

static void set_editor_title(const char *t) {
	VMWCHAR w[60];
	vm_ascii_to_ucs2(w, sizeof w, (VMSTR)t);
	vm_input_set_editor_title(w);
}

static void password_done(VMINT state, VMWSTR text) {
	if (state != VM_INPUT_OK || !text || !text[0]) { dirty = 1; return; }
	ucs2_to_utf8(text, cfg_pass, sizeof cfg_pass);
	cfg_access[0] = cfg_refresh[0] = 0;
	cfg_save();
	request(J_LOGIN);
}

static void email_done(VMINT state, VMWSTR text) {
	if (state != VM_INPUT_OK || !text || !text[0]) { dirty = 1; return; }
	ucs2_to_utf8(text, cfg_email, sizeof cfg_email);
	trim(cfg_email);
	cfg_access[0] = cfg_refresh[0] = 0;
	cfg_save();
	if (cfg_pass[0]) request(J_LOGIN);
	else ask_password();
}

static void ask_email(void) {
	vm_ascii_to_ucs2(input_buf, sizeof input_buf, (VMSTR)cfg_email);
	set_editor_title("VivaGym email");
	vm_input_text3(input_buf, 100, VM_INPUT_METHOD_ALPHABETIC_EXT, email_done);
}

static void ask_password(void) {
	input_buf[0] = 0;
	set_editor_title("VivaGym password");
	vm_input_text3(input_buf, 100, VM_INPUT_METHOD_ALPHABETIC_EXT, password_done);
}

/* ===================================================================== */
/*  Drawing                                                              */
/* ===================================================================== */

static void rect(int x, int y, int w, int h, VMUINT16 c) { vm_graphic_fill_rect(buf, x, y, w, h, c, c); }

// n bytes of UTF-8 -> UCS-2 (server messages are Spanish: "contraseña")
static int to_ucs2(const char *s, int n, VMWCHAR *w) {
	const unsigned char *p = (const unsigned char *)s, *end = p + n;
	int o = 0;
	while (p < end && o < 127) {
		unsigned cp = *p++;
		if (cp >= 0xC0 && cp < 0xE0 && p < end && (*p & 0xC0) == 0x80) cp = ((cp & 31) << 6) | (*p++ & 63);
		else if (cp >= 0xE0 && p + 1 < end && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
		w[o++] = (VMWCHAR)cp;
	}
	w[o] = 0;
	return o;
}

static int text_w(const char *s, int n) {
	VMWCHAR w[128];
	to_ucs2(s, n, w);
	return vm_graphic_get_string_width(w);
}

static void text_at(int x, int y, const char *s, int n, VMUINT16 color) {
	VMWCHAR w[128];
	int len = to_ucs2(s, n, w);
	vm_graphic_textout(buf, x, y, w, len, color);
}

static void text_center(int y, const char *s, VMUINT16 color) {
	int n = strlen(s);
	text_at((scr_w - text_w(s, n)) / 2, y, s, n, color);
}

// Word-wrapped paragraph; returns the y below it.
static int text_wrap(int x, int y, int maxw, const char *s, VMUINT16 color) {
	while (*s) {
		int n = 0, last_space = -1;
		while (s[n] && s[n] != '\n') {
			if (s[n] == ' ') last_space = n;
			{
				int next = n + 1;
				while ((s[next] & 0xC0) == 0x80) next++; // whole UTF-8 character
				if (text_w(s, next) > maxw) break;
				n = next;
			}
		}
		if (s[n] && s[n] != '\n' && last_space > 0) n = last_space;
		if (n == 0) { n = 1; while ((s[n] & 0xC0) == 0x80) n++; }
		text_at(x, y, s, n, color);
		y += lh;
		s += n;
		while (*s == ' ' || *s == '\n') s++;
	}
	return y;
}

static void draw_bar(int y, const char *left, const char *mid, const char *right) {
	rect(0, y, scr_w, BAR_H, c_bar);
	if (left) text_at(MARGIN, y + 2, left, strlen(left), c_bar_text);
	if (mid) text_center(y + 2, mid, c_bar_text);
	if (right) text_at(scr_w - MARGIN - text_w(right, strlen(right)), y + 2, right, strlen(right), c_bar_text);
}

static void draw_qr(void) {
	int n = qr.size, top = BAR_H, bottom = scr_h - BAR_H - lh - 2;
	int avail_h = bottom - top, quiet = 4, px, x0, y0, x, y;
	px = scr_w / (n + 2 * quiet);
	if (avail_h / (n + 2 * quiet) < px) px = avail_h / (n + 2 * quiet);
	if (px < 2) { quiet = 2; px = scr_w / (n + 4); if (avail_h / (n + 4) < px) px = avail_h / (n + 4); }
	if (px < 1) px = 1;
	rect(0, top, scr_w, bottom - top + lh + 2, c_white);
	x0 = (scr_w - n * px) / 2;
	y0 = top + (avail_h - n * px) / 2;
	for (y = 0; y < n; y++) {
		int run = -1;
		for (x = 0; x <= n; x++) {
			int dark = x < n && qr.m[y][x];
			if (dark && run < 0) run = x;
			if (!dark && run >= 0) { rect(x0 + run * px, y0 + y * px, (x - run) * px, px, c_black); run = -1; }
		}
	}
	{
		// member id under the code: exerp:checkin:<member>-...
		char id[48];
		const char *p = qr_text;
		int i = 0;
		if (starts_with_ci(p, "exerp:checkin:")) p += 14;
		while (p[i] && p[i] != '-' && i < 40) { id[i] = p[i]; i++; }
		id[i] = 0;
		if (qr_err[0]) text_center(bottom, "Update failed - OK to retry", c_err);
		else if (qr_warn) text_center(bottom, "Unexpected QR format", c_err);
		else text_center(bottom, id, c_dim);
	}
}

static void draw_menu(void) {
	static const char *items[MENU_N] = {"1 Refresh QR", "2 Sign in again", "3 Change email", "4 Change password", "5 About", "6 Exit"};
	int i, w = scr_w - 40, h = MENU_N * lh + 10, x = 20, y = scr_h - BAR_H - h - 4;
	rect(x - 2, y - 2, w + 4, h + 4, c_text);
	rect(x, y, w, h, c_bg);
	for (i = 0; i < MENU_N; i++) {
		int yy = y + 5 + i * lh;
		if (i == menu_sel) rect(x + 2, yy - 1, w - 4, lh, c_accent);
		text_at(x + 6, yy, items[i], strlen(items[i]), i == menu_sel ? c_white : c_text);
	}
}

static void draw(void) {
	char title[64];
	int y;
	if (!buf) return;
	dirty = 0;
	vm_graphic_set_font(VM_SMALL_FONT);
	rect(0, 0, scr_w, scr_h, c_bg);

	if (screen == S_QR && have_qr) {
		int age = (int)((vm_get_tick_count() - qr_tick) / 1000);
		draw_qr();
		if (busy) copy_str(title, status_msg[0] ? status_msg : "Updating...", sizeof title);
		else if (age < 60) sprintf(title, "VivaGym  -  updated %ds ago", age);
		else sprintf(title, "VivaGym  -  updated %dm ago", age / 60);
		draw_bar(0, title, 0, 0);
		if (busy) rect(0, BAR_H - 2, (vm_get_tick_count() / 30) % scr_w, 2, c_accent);
	} else {
		draw_bar(0, "VivaGym check-in", 0, 0);
		y = BAR_H + 16;
		switch (screen) {
		case S_START:
		case S_BUSY:
			text_center(y + 60, status_msg[0] ? status_msg : "Starting...", c_text);
			rect(40, y + 90, scr_w - 80, 4, c_dim);
			rect(40 + (int)((vm_get_tick_count() / 15) % (scr_w - 100)), y + 90, 20, 4, c_accent);
			break;
		case S_NEED_LOGIN:
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "Show your gym check-in QR without a smartphone.", c_text);
			y += 10;
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, cfg_email[0] ? "Press OK to enter your password." : "Press OK and type the email of your VivaGym account.", c_text);
			y += 10;
			text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "Mobile data must be on.", c_dim);
			break;
		case S_ERROR:
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "Could not get your QR", c_err);
			y += 6;
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, err_msg, c_text);
			y += 10;
			text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "OK: try again.  Menu: change email or password.", c_dim);
			break;
		case S_ABOUT:
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "VivaGym check-in 1.0 for Nokia 216.", c_text);
			y += 6;
			y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "Signs in like the VivaGym app and shows your own check-in QR. The code refreshes every minute while you use the phone.", c_text);
			y += 6;
			if (cfg_email[0]) { sprintf(title, "Account: %.40s", cfg_email); y = text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, title, c_dim); y += 6; }
			text_wrap(MARGIN * 2, y, scr_w - 4 * MARGIN, "HTTPS: BearSSL (MIT). Coroutines: libco (ISC).", c_dim);
			break;
		}
	}

	{
		const char *mid = menu_open ? 0 : (screen == S_QR || screen == S_ERROR) ? "Refresh" : screen == S_NEED_LOGIN ? "Sign in" : 0;
		const char *right = menu_open ? "Close" : screen == S_ABOUT ? "Back" : "Exit";
		draw_bar(scr_h - BAR_H, menu_open ? "Select" : "Menu", mid, right);
	}
	if (menu_open) draw_menu();
	vm_graphic_flush_layer(&layer, 1);
}

/* ===================================================================== */
/*  Events                                                               */
/* ===================================================================== */

static void keep_screen_on(int on) {
	if (on == screen_kept_on) return;
	vm_switch_power_saving_mode(on ? turn_off_mode : turn_on_mode);
	screen_kept_on = on;
}

static void tick(VMINT tid) {
	static int last_age = -1;
	VMINT now = vm_get_tick_count();
	int in_use = now - last_input < ACTIVE_WINDOW_MS;
	(void)tid;
	ent_tick();
	thread_next(); // let the network coroutine run
	if (!buf) return;
	keep_screen_on(in_use && screen == S_QR);
	if (screen == S_QR && in_use && !busy && now - qr_tick > QR_REFRESH_MS) request(J_QR);
	if (screen == S_QR) {
		int age = (int)((now - qr_tick) / 1000);
		if (age != last_age) { last_age = age; dirty = 1; }
	}
	if (dirty) draw();
	else if (busy || screen == S_BUSY || screen == S_START) {
		static VMINT last_anim;
		if (now - last_anim > 250) { last_anim = now; draw(); } // progress animation
	}
}

static void menu_pick(int i) {
	menu_open = 0;
	switch (i) {
	case 0: request(J_QR); break;
	case 1: if (cfg_email[0] && cfg_pass[0]) request(J_LOGIN); else { screen = S_NEED_LOGIN; } break;
	case 2: ask_email(); break;
	case 3: ask_password(); break;
	case 4: screen = S_ABOUT; break;
	case 5: vm_exit_app(); break;
	}
	dirty = 1;
}

static void handle_key(VMINT event, VMINT key) {
	VMINT now = vm_get_tick_count();
	int was_idle = now - last_input >= ACTIVE_WINDOW_MS;
	ent_tick();
	ent_add(&key, sizeof key);
	if (event != VM_KEY_EVENT_DOWN) return;
	last_input = now;
	dirty = 1;

	if (menu_open) {
		if (key == VM_KEY_UP) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
		else if (key == VM_KEY_DOWN) menu_sel = (menu_sel + 1) % MENU_N;
		else if (key == VM_KEY_OK || key == VM_KEY_LEFT_SOFTKEY) menu_pick(menu_sel);
		else if (key >= VM_KEY_NUM1 && key < VM_KEY_NUM1 + MENU_N) menu_pick(key - VM_KEY_NUM1);
		else if (key == VM_KEY_RIGHT_SOFTKEY) menu_open = 0;
		return;
	}
	// waking up after a long idle: get a fresh code straight away
	if (was_idle && screen == S_QR && now - qr_tick > 20000) request(J_QR);

	switch (key) {
	case VM_KEY_LEFT_SOFTKEY: menu_open = 1; menu_sel = 0; break;
	case VM_KEY_RIGHT_SOFTKEY:
		if (screen == S_ABOUT) screen = have_qr ? S_QR : (cfg_email[0] && cfg_pass[0] ? S_ERROR : S_NEED_LOGIN);
		else vm_exit_app();
		break;
	case VM_KEY_OK: case VM_KEY_NUM5:
		if (screen == S_NEED_LOGIN) { if (!cfg_email[0]) ask_email(); else if (!cfg_pass[0]) ask_password(); else request(J_LOGIN); }
		else if (screen == S_QR || screen == S_ERROR) request(J_QR);
		break;
	}
}

static void handle_sysevt(VMINT message, VMINT param) {
	(void)param;
	switch (message) {
	case VM_MSG_CREATE:
	case VM_MSG_ACTIVE:
		scr_w = vm_graphic_get_screen_width();
		scr_h = vm_graphic_get_screen_height();
		layer = vm_graphic_create_layer(0, 0, scr_w, scr_h, -1);
		buf = vm_graphic_get_layer_buffer(layer);
		vm_graphic_set_clip(0, 0, scr_w, scr_h);
		vm_graphic_set_font(VM_SMALL_FONT);
		fh = vm_graphic_get_character_height();
		if (fh < 8 || fh > 30) fh = 14;
		lh = fh + 3;
		last_input = vm_get_tick_count();
		if (message == VM_MSG_CREATE) {
			seed_load();
			cfg_load();
			thread_init();
			thread_create(0x10000, worker);
			if (cfg_email[0] && cfg_pass[0]) request(J_QR);
			else screen = S_NEED_LOGIN;
		} else if (screen == S_QR && vm_get_tick_count() - qr_tick > 20000) request(J_QR);
		if (timer_id < 0) timer_id = vm_create_timer(60, tick);
		dirty = 1;
		draw();
		break;
	case VM_MSG_PAINT:
		dirty = 1;
		draw();
		break;
	case VM_MSG_INACTIVE:
		keep_screen_on(0);
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1;
		buf = 0;
		break;
	case VM_MSG_QUIT:
		keep_screen_on(0);
		if (timer_id >= 0) vm_delete_timer(timer_id);
		timer_id = -1;
		net_close();
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1;
		buf = 0;
		break;
	}
}

void vm_main(void) {
	c_bg = VM_COLOR_888_TO_565(255, 255, 255);
	c_white = VM_COLOR_888_TO_565(255, 255, 255);
	c_black = VM_COLOR_888_TO_565(0, 0, 0);
	c_text = VM_COLOR_888_TO_565(20, 20, 20);
	c_bar = VM_COLOR_888_TO_565(25, 25, 25);
	c_bar_text = VM_COLOR_888_TO_565(240, 240, 240);
	c_dim = VM_COLOR_888_TO_565(110, 110, 120);
	c_err = VM_COLOR_888_TO_565(190, 0, 0);
	c_accent = VM_COLOR_888_TO_565(225, 90, 0);
	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_key);
}
