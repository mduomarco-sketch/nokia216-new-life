// UPV timetable - shows your UPV class schedule (PoliformaT "horario") on a
// Nokia 216 (S30+, MediaTek MRE), straight from your personal iCal feed.
//
//   GET https://www.upv.es/ical/<token>   (the token IS the credential)
//   -> stream-parse VEVENTs, keep the whole feed in RAM, cache the raw .ics
//      to the SD card so it opens offline.
// The phone talks HTTPS itself (BearSSL, TLS 1.2), no proxy. Times are UTC in
// the feed; converted to Europe/Madrid (CET/CEST) by upv_ical.c.
//
// Keys: Left/Right (or 4/6) previous/next day, 0 today, OK/5 refresh,
//       Up/Down scroll, LSK menu, RSK exit.
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
#include "upv_ical.h"
#include "secrets.h"
#include <string.h>
#include <stdio.h>

/* ---------- server ---------- */
#ifndef UPV_PORT
#define UPV_PORT 443
#endif
#ifndef UPV_TLS
#define UPV_TLS 1
#endif
#define UA "Mozilla/5.0"
#define NET_TIMEOUT_MS 45000
#define CACHE_NAME "upv.ics"
#define MAX_STORE 1024          // whole-feed event cap (~150 KB)

// ---- AI ("explain my day"): direct to an OpenAI-compatible API over TLS 1.2 ----
// Defaults target DeepSeek (api.deepseek.com, verified TLS 1.2 -> Amazon root).
// All overridable from secrets.h.
#ifndef OPENAI_HOST
#define OPENAI_HOST "api.deepseek.com"
#endif
#ifndef OPENAI_PATH
#define OPENAI_PATH "/chat/completions"
#endif
#ifndef OPENAI_MODEL
#define OPENAI_MODEL "deepseek-chat"
#endif
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""       // set in secrets.h; blank = AI disabled
#endif

static char g_host[80], g_path[220];
static void parse_url(void);

/* ---------- state ---------- */
enum { S_START, S_BUSY, S_DAY, S_ERROR, S_ABOUT, S_AI };
static int screen = S_START;
enum { J_NONE, J_REFRESH, J_AI };
static volatile int job_req = J_NONE, busy;
static char status_msg[96], err_msg[260];

static char ai_text[1200];       // the AI's plain-language answer
static int  ai_scroll;
static int  ai_y, ai_mo, ai_d;   // which day the answer is about
static int  ever_shown;          // first successful data load done
static void snap_to_events(void);

static ical_event store[MAX_STORE];
static int n_store;             // published count (atomic swap at end of rebuild)
static int have_data;

static int cur_y, cur_mo, cur_d;        // displayed day
static int today_y, today_mo, today_d;
static int scroll;                      // pixel scroll within the day
static int menu_open, menu_sel;
#define MENU_N 5

/* ---------- screen ---------- */
static int scr_w = 240, scr_h = 320;
static VMINT layer = -1;
static VMUINT8 *buf = 0;
static VMINT timer_id = -1;
static int fh = 14, lh = 17;
#define BAR_H (lh + 4)
#define MARGIN 4
static int dirty = 1;
static VMUINT16 c_bg, c_text, c_bar, c_bar_text, c_dim, c_err, c_accent, c_black, c_white, c_line;

static const char *WDAY[7] = {"Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado"};
static const char *WDAY_SHORT[7] = {"Dom","Lun","Mar","Mie","Jue","Vie","Sab"};
static const char *MON[12] = {"ene","feb","mar","abr","may","jun","jul","ago","sep","oct","nov","dic"};

/* ===================================================================== */
/*  Entropy for BearSSL                                                  */
/* ===================================================================== */
static br_sha256_context ent_ctx;
static int ent_ready;
static void ent_add(const void *data, size_t len) {
	if (!ent_ready) { br_sha256_init(&ent_ctx); ent_ready = 1; }
	br_sha256_update(&ent_ctx, data, len);
}
static void ent_tick(void) { VMINT t = vm_get_tick_count(); ent_add(&t, sizeof t); }

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
	VMWCHAR path[48]; unsigned char h[32]; br_sha256_context c = ent_ctx; VMFILE f; VMUINT n;
	ent_add("save", 4); br_sha256_out(&c, h);
	drive_path(path, sizeof path, removable_drive(), "upv.seed");
	f = vm_file_open(path, MODE_CREATE_ALWAYS_WRITE, TRUE);
	if (f < 0) return;
	vm_file_write(f, h, sizeof h, &n); vm_file_close(f);
}
static void seed_load(void) {
	VMWCHAR path[48]; unsigned char h[32]; VMFILE f; VMUINT n = 0; VMSTR imei; vm_time_t t; int i;
	drive_path(path, sizeof path, removable_drive(), "upv.seed");
	f = vm_file_open(path, MODE_READ, TRUE);
	if (f >= 0) { if (vm_file_read(f, h, sizeof h, &n) >= 0 && n > 0) ent_add(h, n); vm_file_close(f); }
	imei = vm_get_imei(); if (imei) ent_add(imei, strlen(imei));
	vm_get_time(&t); ent_add(&t, sizeof t);
	for (i = 0; i < 64; i++) ent_tick();
	seed_save();
}
void br_mre_get_entropy(unsigned char *out, size_t len) {
	unsigned char h[32]; br_sha256_context c; vm_time_t t; int i;
	vm_get_time(&t); ent_add(&t, sizeof t);
	for (i = 0; i < 32; i++) ent_tick();
	c = ent_ctx; br_sha256_out(&c, h);
	memcpy(out, h, len < 32 ? len : 32);
	ent_add(h, sizeof h); seed_save();
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
	int i = 0; while (src[i] && i < size - 1) { dst[i] = src[i]; i++; } dst[i] = 0;
}
static int hexval(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c); if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1;
}

/* ---- date arithmetic ---- */
static int is_leap(int y){ return (y%4==0 && y%100!=0) || y%400==0; }
static int mdays(int y,int m){ static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31}; return (m==2&&is_leap(y))?29:d[m-1]; }
static long day_number(int y,int m,int d){            // days since 1970-01-01
	long n=0; int i;
	if (y>=1970) for(i=1970;i<y;i++) n+=is_leap(i)?366:365;
	else for(i=y;i<1970;i++) n-=is_leap(i)?366:365;
	for(i=1;i<m;i++) n+=mdays(y,i);
	return n + d - 1;
}
static void from_day_number(long n,int *y,int *m,int *d){
	int yy=1970; while(1){ int dy=is_leap(yy)?366:365; if(n>=dy){n-=dy;yy++;} else if(n<0){yy--;n+=is_leap(yy)?366:365;} else break; }
	int mm=1; while(n>=mdays(yy,mm)){ n-=mdays(yy,mm); mm++; }
	*y=yy; *m=mm; *d=(int)n+1;
}
static void date_add(int *y,int *m,int *d,int delta){
	long n=day_number(*y,*m,*d)+delta; from_day_number(n,y,m,d);
}
static int ev_before(const ical_event *a,const ical_event *b){
	long da=day_number(a->start.y,a->start.mo,a->start.d);
	long db=day_number(b->start.y,b->start.mo,b->start.d);
	if(da!=db) return da<db;
	if(a->start.h!=b->start.h) return a->start.h<b->start.h;
	return a->start.mi<b->start.mi;
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
static const char *net_host = g_host;   // host for the current connection

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
	static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
	unsigned long days; y--; days = 365UL*(y+1) + y/4 - y/100 + y/400; y++;
	days += cum[m-1] + d - 1;
	if (m > 2 && ((y%4==0 && y%100!=0) || y%400==0)) days++;
	return days;
}
struct newlib_timeval { long long tv_sec; long tv_usec; };
int _gettimeofday(struct newlib_timeval *tv, void *tz) {
	vm_time_t t; (void)tz; vm_get_time(&t);
	if (tv) { tv->tv_sec = (long long)((days_since_0(t.year,t.mon,t.day)-days_since_0(1970,1,1))*86400UL + t.hour*3600UL + t.min*60UL + t.sec); tv->tv_usec = 0; }
	return 0;
}
static int apn_mode = -1;
static void apn_setup(void) {
	vm_apn_info_ext apn;
	if (apn_mode >= 0) return;
	memset(&apn, 0, sizeof apn);
	if (vm_get_default_apn_info(&apn) == 0) { vm_dtacct_set(0, apn.apn_info_id); apn_mode = VM_APN_USER_DEFINE; }
	else apn_mode = VM_TCP_APN_CMNET;
}
static volatile int dns_done;
static vm_soc_dns_result dns_res;
static VMINT dns_cb(vm_soc_dns_result *r) { if (r) dns_res = *r; dns_done = 1; return 0; }
static int resolve_host(const char *host, char *ip) {
	VMINT r; unsigned char *a;
	memset(&dns_res, 0, sizeof dns_res); dns_done = 0;
	r = vm_soc_get_host_by_name(apn_mode, host, &dns_res, dns_cb);
	if (r == VM_E_SOC_WOULDBLOCK) { while (!dns_done) { if (timed_out()) return 0; thread_next(); } }
	else if (r != VM_E_SOC_SUCCESS) return 0;
	if (dns_res.num <= 0) return 0;
	a = (unsigned char *)&dns_res.address[0];
	sprintf(ip, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
	return 1;
}
static const char *tcp_open(const char *host, int port) {
	tcp_connected = tcp_err = 0;
	tcp_h = vm_tcp_connect(host, port, apn_mode, tcp_cb);
	if (tcp_h < 0) return "No se pudo conectar. Datos moviles activados?";
	while (!tcp_connected) {
		if (tcp_err == 2) return "dns";
		if (tcp_err) return "Conexion rechazada o cerrada.";
		if (timed_out()) return "Tiempo de conexion agotado.";
		thread_next();
	}
	return 0;
}
static void net_close(void) { if (tcp_h >= 0) vm_tcp_close(tcp_h); tcp_h = -1; }
static const char *net_open(void) {
	const char *err; char ip[16];
	net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS;
	apn_setup();
	err = tcp_open(net_host, UPV_PORT);
	if (err && !strcmp(err, "dns")) {
		net_close();
		if (!resolve_host(net_host, ip)) return "Servidor no encontrado (DNS). Datos moviles?";
		err = tcp_open(ip, UPV_PORT);
		if (err && !strcmp(err, "dns")) err = "No se pudo contactar el servidor.";
	}
	if (err) return err;
	if (UPV_TLS) {
		vm_time_t t;
		br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
		vm_get_time(&t);
		if (t.year < 2026) { t.year = 2026; t.mon = 11; t.day = 1; t.hour = 12; t.min = t.sec = 0; }
		br_x509_minimal_set_time(&xc, days_since_0(t.year,t.mon,t.day), t.hour*3600UL + t.min*60UL + t.sec);
		br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
		br_ssl_client_reset(&sc, net_host, 0);
		br_sslio_init(&ioc, &sc.eng, raw_read, 0, raw_write, 0);
	}
	return 0;
}
static int net_write(const char *s, int n) {
	if (UPV_TLS) { if (br_sslio_write_all(&ioc, s, n) < 0) return -1; return br_sslio_flush(&ioc); }
	while (n > 0) { int r = raw_write(0, (const unsigned char *)s, n); if (r < 0) return -1; s += r; n -= r; }
	return 0;
}
static int net_read(char *b, int n) {
	if (UPV_TLS) return br_sslio_read(&ioc, b, n);
	return raw_read(0, (unsigned char *)b, n);
}
static const char *tls_error_text(void) {
	static char msg[64]; int err;
	if (!UPV_TLS) return "Conexion cerrada.";
	err = br_ssl_engine_last_error(&sc.eng);
	switch (err) {
	case 0: return "El servidor cerro la conexion.";
	case BR_ERR_X509_EXPIRED: return "Certificado caducado o fecha del telefono mal. Ajusta fecha/hora.";
	case BR_ERR_X509_NOT_TRUSTED: return "Certificado del servidor no confiable.";
	}
	sprintf(msg, "Fallo de conexion segura (TLS %d).", err);
	return msg;
}

/* ===================================================================== */
/*  Minimal JSON (for the OpenAI reply) + buffered HTTPS POST            */
/* ===================================================================== */
static char resp[8192]; static int resp_len;
static char req[640];      // shared request-header buffer
static char netbuf[512];   // shared socket read buffer

// Decode a JSON string literal starting just after the opening quote.
static void json_unescape(const char *p, char *out, int size) {
	int o = 0;
	while (*p && *p != '"' && o < size - 1) {
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': out[o++] = '\n'; break;
			case 't': out[o++] = ' '; break;
			case 'r': break;
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

// Find "key": "string" and unescape it. Good enough for the one field we need.
static int json_get_str(const char *json, const char *key, char *out, int size) {
	char pat[32]; const char *p;
	sprintf(pat, "\"%s\"", key);
	out[0] = 0;
	p = strstr(json, pat);
	if (!p) return 0;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (*p != ':') return 0;
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (*p != '"') return 0;
	json_unescape(p + 1, out, size);
	return 1;
}

// JSON-escape a UTF-8 string into dst (quotes, backslash, control -> \n).
static void json_escape(const char *s, char *dst, int size) {
	int o = 0;
	for (; *s && o < size - 2; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
		else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
		else if (c == '\r' || c == '\t') { dst[o++] = ' '; }
		else if (c < 0x20) { /* skip other control chars */ }
		else dst[o++] = c;
	}
	dst[o] = 0;
}

// Buffered HTTPS POST into resp[]. Returns HTTP status or -1 (err_msg filled).
static int http_post(const char *host, const char *path, const char *body, const char *bearer) {
	char line[300]; int status = -1, chunked = 0, body_left = -1, len, i;
	int cstate = 0, cleft = 0; const char *err;
	resp_len = 0; resp[0] = 0;
	net_host = host;
	err = net_open();
	if (err) { net_close(); copy_str(err_msg, err, sizeof err_msg); return -1; }
	len = sprintf(req, "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA
		"\r\nAccept: application/json\r\nContent-Type: application/json\r\n", path, host);
	if (bearer && bearer[0]) len += sprintf(req + len, "Authorization: Bearer %s\r\n", bearer);
	len += sprintf(req + len, "Content-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(body));
	if (net_write(req, len) < 0 || net_write(body, strlen(body)) < 0) {
		copy_str(err_msg, tls_error_text(), sizeof err_msg); net_close(); return -1;
	}
	/* headers */
	for (;;) {
		char c; int got = 0, hl = 0;
		for (;;) { int r = net_read(&c, 1); if (r <= 0) break; got = 1; if (c == '\n') break; if (c != '\r' && hl < (int)sizeof(line)-1) line[hl++] = c; }
		line[hl] = 0;
		if (!got) { status = -2; break; }
		if (status == -1) {
			if (!starts_with_ci(line, "HTTP/") || hl < 12) { status = -2; break; }
			status = 0; for (i = 9; line[i] >= '0' && line[i] <= '9'; i++) status = status*10 + (line[i]-'0');
			continue;
		}
		if (!hl) break;
		if (starts_with_ci(line, "Transfer-Encoding:") && strstr_ci(line, "chunked")) chunked = 1;
		else if (starts_with_ci(line, "Content-Length:")) { const char *v = line + 15; while (*v == ' ') v++; body_left = 0; while (*v >= '0' && *v <= '9') body_left = body_left*10 + (*v++ - '0'); }
	}
	if (status < 0) { copy_str(err_msg, status == -2 && UPV_TLS ? tls_error_text() : "El servidor no respondio.", sizeof err_msg); net_close(); return -1; }
	/* body -> resp[] */
	{
		int done = body_left == 0;
		while (!done) {
			int r = net_read(netbuf, sizeof netbuf);
			if (r <= 0) break;
			for (i = 0; i < r && !done; i++) {
				unsigned char b = (unsigned char)netbuf[i]; int keep = 0;
				if (!chunked) { keep = 1; if (body_left > 0 && --body_left == 0) done = 1; }
				else switch (cstate) {
				case 0: if (hexval(b) >= 0) cleft = cleft*16 + hexval(b); else if (b=='\n') { if(!cleft) done=1; else cstate=2; } else cstate=1; break;
				case 1: if (b=='\n') { if(!cleft) done=1; else cstate=2; } break;
				case 2: keep = 1; if (--cleft == 0) cstate = 3; break;
				case 3: if (b=='\n') { cstate=0; cleft=0; } break;
				}
				if (keep && resp_len < (int)sizeof(resp)-1) resp[resp_len++] = (char)b;
			}
		}
	}
	resp[resp_len] = 0;
	net_close(); seed_save();
	return status;
}

/* ===================================================================== */
/*  Event store                                                          */
/* ===================================================================== */
static int build_cnt;
static void store_emit(const ical_event *ev, void *user) {
	(void)user;
	if (build_cnt >= MAX_STORE) return;
	if (ev->start.y < 2000 || !ev->summary[0]) return;
	store[build_cnt++] = *ev;
}
static void sort_store(void) {   // simple insertion sort (store is nearly sorted)
	int i, j;
	for (i = 1; i < n_store; i++) {
		ical_event k = store[i];
		for (j = i - 1; j >= 0 && ev_before(&k, &store[j]); j--) store[j+1] = store[j];
		store[j+1] = k;
	}
}

/* ===================================================================== */
/*  Fetch: stream the feed to the SD cache, then rebuild the store       */
/* ===================================================================== */
static char fbuf[2048]; static int fbuf_n;    // cache write buffer
static VMFILE cache_f = -1;

#define CACHE_TMP "upv.tmp"
// Download to a TEMP file, then atomically replace the real cache only on success,
// so a failed/empty refresh can never wipe the good offline copy.
static void cache_open_write(void) {
	VMWCHAR path[48];
	drive_path(path, sizeof path, removable_drive(), CACHE_TMP);
	cache_f = vm_file_open(path, MODE_CREATE_ALWAYS_WRITE, TRUE);
	fbuf_n = 0;
}
static void cache_put(const char *b, int n) {
	if (cache_f < 0) return;
	int i;
	for (i = 0; i < n; i++) {
		fbuf[fbuf_n++] = b[i];
		if (fbuf_n == sizeof fbuf) { VMUINT w; vm_file_write(cache_f, fbuf, fbuf_n, &w); fbuf_n = 0; }
	}
}
// commit!=0: temp replaces the real cache. commit==0: keep the old cache, drop temp.
static void cache_close_write(int commit) {
	VMWCHAR tmp[48], dst[48]; int drv = removable_drive();
	if (cache_f < 0) return;
	if (fbuf_n) { VMUINT w; vm_file_write(cache_f, fbuf, fbuf_n, &w); fbuf_n = 0; }
	vm_file_close(cache_f); cache_f = -1;
	drive_path(tmp, sizeof tmp, drv, CACHE_TMP);
	drive_path(dst, sizeof dst, drv, CACHE_NAME);
	if (commit) { vm_file_delete(dst); vm_file_rename(tmp, dst); }
	else vm_file_delete(tmp);
}

static void set_status(const char *s) { copy_str(status_msg, s, sizeof status_msg); dirty = 1; }

// Download the .ics to the SD cache (de-chunked). Returns HTTP status or -1.
static int download_cache(void) {
	char line[300]; int status = -1, chunked = 0, body_left = -1, len, i;
	int cstate = 0, cleft = 0, total = 0;
	const char *err;

	net_host = g_host;
	err = net_open();
	if (err) { net_close(); copy_str(err_msg, err, sizeof err_msg); return -1; }
	len = sprintf(req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA
		"\r\nAccept: text/calendar\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", g_path, g_host);
	if (net_write(req, len) < 0) { copy_str(err_msg, tls_error_text(), sizeof err_msg); net_close(); return -1; }

	/* headers */
	for (;;) {
		char c; int got = 0, hl = 0;
		for (;;) { int r = net_read(&c, 1); if (r <= 0) break; got = 1; if (c == '\n') break; if (c != '\r' && hl < (int)sizeof(line)-1) line[hl++] = c; }
		line[hl] = 0;
		if (!got) { status = -2; break; }
		if (status == -1) {
			if (!starts_with_ci(line, "HTTP/") || hl < 12) { status = -2; break; }
			status = 0; for (i = 9; line[i] >= '0' && line[i] <= '9'; i++) status = status*10 + (line[i]-'0');
			continue;
		}
		if (!hl) break;
		if (starts_with_ci(line, "Transfer-Encoding:") && strstr_ci(line, "chunked")) chunked = 1;
		else if (starts_with_ci(line, "Content-Length:")) { const char *v = line + 15; while (*v == ' ') v++; body_left = 0; while (*v >= '0' && *v <= '9') body_left = body_left*10 + (*v++ - '0'); }
	}
	if (status < 0) { copy_str(err_msg, status == -2 && UPV_TLS ? tls_error_text() : "El servidor no respondio.", sizeof err_msg); net_close(); return -1; }
	if (status != 200) { net_close(); return status; }

	/* body -> cache file, de-chunking on the fly */
	cache_open_write();
	{
		int done = body_left == 0;
		while (!done) {
			int r = net_read(netbuf, sizeof netbuf);
			if (r <= 0) break;
			for (i = 0; i < r && !done; i++) {
				unsigned char b = (unsigned char)netbuf[i]; int keep = 0;
				if (!chunked) { keep = 1; if (body_left > 0 && --body_left == 0) done = 1; }
				else switch (cstate) {
				case 0: if (hexval(b) >= 0) cleft = cleft*16 + hexval(b); else if (b=='\n') { if(!cleft) done=1; else cstate=2; } else cstate=1; break;
				case 1: if (b=='\n') { if(!cleft) done=1; else cstate=2; } break;
				case 2: keep = 1; if (--cleft == 0) cstate = 3; break;
				case 3: if (b=='\n') { cstate=0; cleft=0; } break;
				}
				if (keep) { char cc=(char)b; cache_put(&cc,1); if ((++total & 0x3FF)==0) { char m[40]; sprintf(m,"Descargando %d KB...", total>>10); set_status(m); } }
			}
		}
	}
	cache_close_write(total > 1000);   // only replace the good cache on a real download
	net_close();
	seed_save();
	if (total <= 1000) { copy_str(err_msg, "Descarga vacia; se mantiene el horario guardado.", sizeof err_msg); return -1; }
	return status;
}

// Stream-parse the SD cache into the store. Atomic from the UI's view (no yields).
static int rebuild_from_cache(void) {
	VMWCHAR path[48]; VMFILE f; VMUINT n; ical_parser p; char rb[1024];
	drive_path(path, sizeof path, removable_drive(), CACHE_NAME);
	f = vm_file_open(path, MODE_READ, TRUE);
	if (f < 0) return 0;
	build_cnt = 0;
	ical_init(&p, 0, 0, 0, store_emit, 0);   // ty=0: emit every event
	for (;;) {
		if (vm_file_read(f, rb, sizeof rb, &n) < 0 || n == 0) break;
		ical_feed_bytes(&p, rb, (int)n);
	}
	ical_flush(&p);
	vm_file_close(f);
	n_store = build_cnt;        // publish
	sort_store();
	have_data = n_store > 0;
	return have_data;
}

static void job_refresh(void) {
	int st;
	err_msg[0] = 0;
	set_status("Conectando con la UPV...");
	st = download_cache();
	if (st == 200) {
		set_status("Leyendo horario...");
		if (rebuild_from_cache()) { screen = S_DAY; return; }
		copy_str(err_msg, "Descargado pero no se pudo leer el horario.", sizeof err_msg);
	} else if (st > 0) {
		sprintf(err_msg, "El servidor respondio HTTP %d. Revisa la URL del iCal.", st);
	}
	if (have_data) { if (!ever_shown) {  ever_shown = 1; } screen = S_DAY; }
	else screen = S_ERROR;
}

/* ---------- AI: "explain my day" via OpenAI ---------- */
static char ai_daytext[900], ai_user[1100], ai_body[2300];

static int build_day_text(int y, int mo, int d, char *out, int size) {
	int i, count = 0, o = 0;
	for (i = 0; i < n_store; i++) {
		ical_event *e = &store[i];
		if (!(e->start.y==y && e->start.mo==mo && e->start.d==d)) continue;
		o += sprintf(out + o, "%02d:%02d-%02d:%02d %s%s%s\n", e->start.h, e->start.mi, e->end.h, e->end.mi,
			e->summary, e->location[0] ? " en " : "", e->location);
		count++;
		if (o > size - 140) break;
	}
	out[o] = 0;
	return count;
}

static void job_ai(void) {
	char hdr[48]; int wd = ical_dow(ai_y, ai_mo, ai_d), n, st;
	err_msg[0] = 0;
	if (!OPENAI_API_KEY[0]) { copy_str(ai_text, "IA no configurada: falta la clave de OpenAI en secrets.h (OPENAI_API_KEY).", sizeof ai_text); screen = S_AI; return; }
	n = build_day_text(ai_y, ai_mo, ai_d, ai_daytext, sizeof ai_daytext);
	sprintf(hdr, "%s %d/%d", WDAY[wd], ai_d, ai_mo);
	{
		static char prompt[1000];
		if (n == 0) sprintf(prompt, "Dia: %s. No tengo ninguna clase ese dia.", hdr);
		else sprintf(prompt, "Dia: %s. Mis clases:\n%s", hdr, ai_daytext);
		json_escape(prompt, ai_user, sizeof ai_user);
	}
	sprintf(ai_body,
		"{\"model\":\"" OPENAI_MODEL "\",\"max_tokens\":260,\"temperature\":0.5,"
		"\"messages\":[{\"role\":\"system\",\"content\":\"Eres un asistente breve y cercano. "
		"Resume en espanol sencillo el dia de clases del alumno: que tiene, a que hora y donde, "
		"y un consejo util (huecos entre clases, cambios de edificio). Maximo 5 frases. No inventes datos.\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}]}", ai_user);
	set_status("Pensando...");
	st = http_post(OPENAI_HOST, OPENAI_PATH, ai_body, OPENAI_API_KEY);
	if (st == 200 && json_get_str(resp, "content", ai_text, sizeof ai_text) && ai_text[0]) { screen = S_AI; return; }
	if (st == 401) { copy_str(ai_text, "Clave de IA rechazada (401). Revisa OPENAI_API_KEY en secrets.h.", sizeof ai_text); screen = S_AI; return; }
	if (st > 0) {
		char m[180];
		if (json_get_str(resp, "message", m, sizeof m) && m[0]) sprintf(ai_text, "Error de IA (HTTP %d): %s", st, m);
		else sprintf(ai_text, "Error de IA (HTTP %d).", st);
		screen = S_AI; return;
	}
	sprintf(ai_text, "No se pudo contactar con la IA. %s", err_msg);
	screen = S_AI;
}

static void worker(void) {
	for (;;) {
		int j;
		while (job_req == J_NONE) thread_next();
		j = job_req; job_req = J_NONE; busy = 1; dirty = 1;
		if (j == J_AI) job_ai(); else job_refresh();
		busy = 0; status_msg[0] = 0; dirty = 1;
	}
}
static void request(int job) {
	if (busy) return;
	job_req = job; busy = 1;
	if (job == J_AI) { screen = S_AI; ai_scroll = 0; }
	else if (!have_data) screen = S_BUSY;
	dirty = 1;
}
static void ask_ai(void) {
	if (busy) { copy_str(ai_text, "Ocupado (actualizando horario). Reintenta en unos segundos.", sizeof ai_text); screen = S_AI; ai_scroll = 0; dirty = 1; return; }
	ai_y = cur_y; ai_mo = cur_mo; ai_d = cur_d; ai_text[0] = 0; request(J_AI);
}

/* ===================================================================== */
/*  Drawing                                                              */
/* ===================================================================== */
static void rect(int x, int y, int w, int h, VMUINT16 c) { vm_graphic_fill_rect(buf, x, y, w, h, c, c); }
static int to_ucs2(const char *s, int n, VMWCHAR *w) {
	const unsigned char *p = (const unsigned char *)s, *end = p + n; int o = 0;
	while (p < end && o < 127) {
		unsigned cp = *p++;
		if (cp >= 0xC0 && cp < 0xE0 && p < end && (*p & 0xC0) == 0x80) cp = ((cp & 31) << 6) | (*p++ & 63);
		else if (cp >= 0xE0 && p + 1 < end && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
		w[o++] = (VMWCHAR)cp;
	}
	w[o] = 0; return o;
}
static int text_w(const char *s, int n) { VMWCHAR w[128]; to_ucs2(s, n, w); return vm_graphic_get_string_width(w); }
static void text_at(int x, int y, const char *s, int n, VMUINT16 color) { VMWCHAR w[128]; int len = to_ucs2(s, n, w); vm_graphic_textout(buf, x, y, w, len, color); }
static void text_center(int y, const char *s, VMUINT16 color) { int n = strlen(s); text_at((scr_w - text_w(s, n)) / 2, y, s, n, color); }

// Word-wrapped text; returns y below. Only draws lines with y in [clip_top,clip_bot).
static int text_wrap_clip(int x, int y, int maxw, const char *s, VMUINT16 color, int clip_top, int clip_bot) {
	while (*s) {
		int n = 0, last_space = -1;
		while (s[n] && s[n] != '\n') {
			if (s[n] == ' ') last_space = n;
			{ int next = n + 1; while ((s[next] & 0xC0) == 0x80) next++; if (text_w(s, next) > maxw) break; n = next; }
		}
		if (s[n] && s[n] != '\n' && last_space > 0) n = last_space;
		if (n == 0) { n = 1; while ((s[n] & 0xC0) == 0x80) n++; }
		if (y >= clip_top && y < clip_bot) text_at(x, y, s, n, color);
		y += lh; s += n;
		while (*s == ' ' || *s == '\n') s++;
	}
	return y;
}
static int text_wrap(int x, int y, int maxw, const char *s, VMUINT16 color) { return text_wrap_clip(x, y, maxw, s, color, -10000, 10000); }

static void draw_bar(int y, const char *left, const char *mid, const char *right) {
	rect(0, y, scr_w, BAR_H, c_bar);
	if (left) text_at(MARGIN, y + 2, left, strlen(left), c_bar_text);
	if (mid) text_center(y + 2, mid, c_bar_text);
	if (right) text_at(scr_w - MARGIN - text_w(right, strlen(right)), y + 2, right, strlen(right), c_bar_text);
}

static void draw_menu(void) {
	static const char *items[MENU_N] = {"1 Actualizar", "2 Hoy", "3 Explicar con IA", "4 Acerca de", "5 Salir"};
	int i, w = scr_w - 40, h = MENU_N * lh + 10, x = 20, y = scr_h - BAR_H - h - 4;
	rect(x - 2, y - 2, w + 4, h + 4, c_text);
	rect(x, y, w, h, c_bg);
	for (i = 0; i < MENU_N; i++) {
		int yy = y + 5 + i * lh;
		if (i == menu_sel) rect(x + 2, yy - 1, w - 4, lh, c_accent);
		text_at(x + 6, yy, items[i], strlen(items[i]), i == menu_sel ? c_white : c_text);
	}
}

// Count the events on the displayed day and the total content height.
static int day_first(void) {   // index of first event on cur day, or n_store
	int i;
	for (i = 0; i < n_store; i++)
		if (store[i].start.y==cur_y && store[i].start.mo==cur_mo && store[i].start.d==cur_d) return i;
	return n_store;
}

static void draw_day(void) {
	char hdr[64]; int top = BAR_H, bot = scr_h - BAR_H, y, i, idx, count = 0;
	int is_today = (cur_y==today_y && cur_mo==today_mo && cur_d==today_d);
	int wd = ical_dow(cur_y, cur_mo, cur_d);   // 0=Sun..6=Sat (tested)

	sprintf(hdr, "%s %d %s%s", WDAY[wd], cur_d, MON[cur_mo-1], is_today ? " (hoy)" : "");
	draw_bar(0, "<", hdr, ">");

	rect(0, top, scr_w, bot - top, c_bg);
	y = top + 6 - scroll;
	idx = day_first();
	for (i = idx; i < n_store; i++) {
		ical_event *e = &store[i];
		char t[80];
		if (!(e->start.y==cur_y && e->start.mo==cur_mo && e->start.d==cur_d)) break;
		count++;
		sprintf(t, "%02d:%02d-%02d:%02d  %s", e->start.h, e->start.mi, e->end.h, e->end.mi, e->summary);
		if (y >= top && y < bot) rect(MARGIN-1, y-1, 3, lh, c_accent);
		y = text_wrap_clip(MARGIN + 6, y, scr_w - 2*MARGIN - 6, t, c_text, top, bot);
		if (e->location[0]) y = text_wrap_clip(MARGIN + 14, y, scr_w - 2*MARGIN - 14, e->location, c_dim, top, bot);
		y += 4;
		if (y >= bot && i + 1 < n_store && store[i+1].start.y==cur_y && store[i+1].start.mo==cur_mo && store[i+1].start.d==cur_d) {
			text_at(scr_w - 14, bot - lh, "v", 1, c_accent);  // more below
			break;
		}
	}
	if (count == 0) {
		rect(0, top, scr_w, bot - top, c_bg);
		text_center(top + 50, is_today ? "Hoy no tienes clase" : "Sin clases", c_dim);
		if (!have_data) text_center(top + 50 + lh*2, busy ? "" : "Pulsa OK para descargar", c_dim);
	}
}

static void draw_ai(void) {
	char hdr[48]; int wd = ical_dow(ai_y, ai_mo, ai_d);
	int top = BAR_H, bot = scr_h - BAR_H;
	sprintf(hdr, "IA - %s %d/%d", WDAY_SHORT[wd], ai_d, ai_mo);
	draw_bar(0, 0, hdr, 0);
	rect(0, top, scr_w, bot - top, c_bg);
	if (busy && !ai_text[0]) {
		text_center(top + 50, "Pensando...", c_dim);
		rect(40, top + 80, scr_w - 80, 4, c_dim);
		rect(40 + (int)((vm_get_tick_count()/15) % (scr_w - 100)), top + 80, 20, 4, c_accent);
		return;
	}
	text_wrap_clip(MARGIN*2, top + 6 - ai_scroll, scr_w - 4*MARGIN, ai_text, c_text, top, bot);
}

static void draw(void) {
	if (!buf) return;
	dirty = 0;
	vm_graphic_set_font(VM_SMALL_FONT);
	rect(0, 0, scr_w, scr_h, c_bg);

	if (screen == S_DAY) {
		draw_day();
	} else if (screen == S_AI) {
		draw_ai();
	} else {
		draw_bar(0, "Horario UPV", 0, 0);
		int y = BAR_H + 16;
		switch (screen) {
		case S_START: case S_BUSY:
			text_center(y + 50, status_msg[0] ? status_msg : "Iniciando...", c_text);
			rect(40, y + 80, scr_w - 80, 4, c_dim);
			rect(40 + (int)((vm_get_tick_count()/15) % (scr_w - 100)), y + 80, 20, 4, c_accent);
			break;
		case S_ERROR:
			y = text_wrap(MARGIN*2, y, scr_w - 4*MARGIN, "No se pudo cargar el horario", c_err); y += 6;
			y = text_wrap(MARGIN*2, y, scr_w - 4*MARGIN, err_msg, c_text); y += 10;
			text_wrap(MARGIN*2, y, scr_w - 4*MARGIN, "OK: reintentar.  Datos moviles activados?", c_dim);
			break;
		case S_ABOUT:
			y = text_wrap(MARGIN*2, y, scr_w - 4*MARGIN, "Horario UPV 1.0 para Nokia 216.", c_text); y += 6;
			y = text_wrap(MARGIN*2, y, scr_w - 4*MARGIN, "Muestra tu horario de clases desde tu feed iCal personal de la UPV. Se descarga una vez y se guarda para verlo sin cobertura.", c_text); y += 6;
			{ char l[48]; sprintf(l, "%d clases en memoria", n_store); text_wrap(MARGIN*2, y, scr_w-4*MARGIN, l, c_dim); }
			break;
		}
	}
	{
		const char *mid = menu_open ? 0 : screen == S_DAY ? "# IA" : screen == S_ERROR ? "OK Reintentar" : 0;
		const char *right = menu_open ? "Cerrar" : (screen == S_ABOUT || screen == S_AI) ? "Volver" : "Salir";
		draw_bar(scr_h - BAR_H, menu_open ? "Elegir" : "Menu", mid, right);
	}
	if (menu_open) draw_menu();
	vm_graphic_flush_layer(&layer, 1);
}

/* ===================================================================== */
/*  Events                                                               */
/* ===================================================================== */
static void set_today(void) {
	vm_time_t t; vm_get_time(&t);
	today_y = t.year; today_mo = t.mon; today_d = t.day;
	if (today_y < 2000) { today_y = 2026; today_mo = 11; today_d = 5; }  // clock unset fallback
}
static void go_today(void) { set_today(); cur_y=today_y; cur_mo=today_mo; cur_d=today_d; scroll=0; dirty=1; }

// If the phone's clock is wrong (feature phones lose the date), "today" may land
// on an empty day. On first load, snap the view to the class day nearest to it so
// the user sees something real regardless of the clock.
static void snap_to_events(void) {
	long t, best = -1, bestd = 0; int i;
	if (n_store == 0) return;
	t = day_number(cur_y, cur_mo, cur_d);
	for (i = 0; i < n_store; i++) {
		long d = day_number(store[i].start.y, store[i].start.mo, store[i].start.d);
		long dist = d >= t ? d - t : t - d;
		if (d == t) return;                 // the shown day already has classes
		if (best < 0 || dist < bestd) { best = d; bestd = dist; }
	}
	if (best >= 0) { from_day_number(best, &cur_y, &cur_mo, &cur_d); scroll = 0; }
}
static void go_day(int delta) { date_add(&cur_y,&cur_mo,&cur_d,delta); scroll=0; dirty=1; }

static void tick(VMINT tid) {
	(void)tid;
	ent_tick(); thread_next();
	if (!buf) return;
	if (dirty) draw();
	else if (busy || screen == S_BUSY || screen == S_START) {
		static VMINT last_anim; VMINT now = vm_get_tick_count();
		if (now - last_anim > 200) { last_anim = now; draw(); }
	}
}

static void menu_pick(int i) {
	menu_open = 0;
	switch (i) {
	case 0: request(J_REFRESH); break;
	case 1: go_today(); break;
	case 2: ask_ai(); break;
	case 3: screen = S_ABOUT; break;
	case 4: vm_exit_app(); break;
	}
	dirty = 1;
}

static void handle_key(VMINT event, VMINT key) {
	ent_tick(); ent_add(&key, sizeof key);
	if (event != VM_KEY_EVENT_DOWN) return;
	dirty = 1;

	if (menu_open) {
		if (key == VM_KEY_UP) menu_sel = (menu_sel + MENU_N - 1) % MENU_N;
		else if (key == VM_KEY_DOWN) menu_sel = (menu_sel + 1) % MENU_N;
		else if (key == VM_KEY_OK || key == VM_KEY_LEFT_SOFTKEY) menu_pick(menu_sel);
		else if (key >= VM_KEY_NUM1 && key < VM_KEY_NUM1 + MENU_N) menu_pick(key - VM_KEY_NUM1);
		else if (key == VM_KEY_RIGHT_SOFTKEY) menu_open = 0;
		return;
	}

	if (screen == S_AI) {
		if (key == VM_KEY_UP) { if (ai_scroll > 0) { ai_scroll -= lh*2; if (ai_scroll < 0) ai_scroll = 0; } }
		else if (key == VM_KEY_DOWN) ai_scroll += lh*2;
		else if (key == VM_KEY_LEFT_SOFTKEY) { menu_open = 1; menu_sel = 0; }
		else if (key == VM_KEY_RIGHT_SOFTKEY || key == VM_KEY_OK || key == VM_KEY_NUM5) screen = have_data ? S_DAY : S_ERROR;
		return;
	}

	switch (key) {
	case VM_KEY_LEFT_SOFTKEY: menu_open = 1; menu_sel = 0; break;
	case VM_KEY_RIGHT_SOFTKEY:
		if (screen == S_ABOUT) screen = have_data ? S_DAY : S_ERROR;
		else vm_exit_app();
		break;
	case VM_KEY_LEFT: case VM_KEY_NUM4: if (screen == S_DAY) go_day(-1); break;
	case VM_KEY_RIGHT: case VM_KEY_NUM6: if (screen == S_DAY) go_day(1); break;
	case VM_KEY_NUM0: if (screen == S_DAY) go_today(); break;
	case VM_KEY_POUND: case VM_KEY_STAR: if (screen == S_DAY) ask_ai(); break;
	case VM_KEY_UP: if (screen == S_DAY && scroll > 0) { scroll -= lh*2; if (scroll < 0) scroll = 0; } break;
	case VM_KEY_DOWN: if (screen == S_DAY) scroll += lh*2; break;
	case VM_KEY_OK: case VM_KEY_NUM5:
		if (screen == S_DAY || screen == S_ERROR) request(J_REFRESH);
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
		if (message == VM_MSG_CREATE) {
			parse_url();
			seed_load();
			go_today();
			thread_init();
			thread_create(0x10000, worker);
			if (rebuild_from_cache()) { ever_shown = 1; screen = S_DAY; }  // instant offline view
			else request(J_REFRESH);   // only auto-download when there's no cache (keeps AI/keys responsive)
		}
		if (timer_id < 0) timer_id = vm_create_timer(60, tick);
		dirty = 1; draw();
		break;
	case VM_MSG_PAINT: dirty = 1; draw(); break;
	case VM_MSG_INACTIVE:
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1; buf = 0;
		break;
	case VM_MSG_QUIT:
		if (timer_id >= 0) vm_delete_timer(timer_id);
		timer_id = -1; net_close();
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1; buf = 0;
		break;
	}
}

static void parse_url(void) {
	const char *u = UPV_ICAL_URL; int i = 0;
	if (starts_with_ci(u, "https://")) u += 8; else if (starts_with_ci(u, "http://")) u += 7;
	while (u[i] && u[i] != '/' && i < (int)sizeof(g_host)-1) { g_host[i] = u[i]; i++; }
	g_host[i] = 0;
	copy_str(g_path, u[i] ? u + i : "/", sizeof g_path);
}

void vm_main(void) {
	c_bg = VM_COLOR_888_TO_565(255, 255, 255);
	c_white = VM_COLOR_888_TO_565(255, 255, 255);
	c_black = VM_COLOR_888_TO_565(0, 0, 0);
	c_text = VM_COLOR_888_TO_565(20, 20, 20);
	c_bar = VM_COLOR_888_TO_565(0, 60, 150);        // UPV blue
	c_bar_text = VM_COLOR_888_TO_565(245, 245, 245);
	c_dim = VM_COLOR_888_TO_565(110, 110, 120);
	c_err = VM_COLOR_888_TO_565(190, 0, 0);
	c_accent = VM_COLOR_888_TO_565(225, 140, 0);
	c_line = VM_COLOR_888_TO_565(220, 220, 220);
	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_key);
}
