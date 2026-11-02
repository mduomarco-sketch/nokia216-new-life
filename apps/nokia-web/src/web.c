// Nokia Web - a tiny text-mode web browser for Nokia S30+ (MediaTek MRE).
//
// Fetches pages itself over HTTP or HTTPS (BearSSL running on the phone, TLS 1.2),
// turns HTML into wrapped text with numbered-free, focusable links, and keeps
// everything in fixed buffers sized for a 16 MB feature phone.
//
// Keys: Up/Down move between links (or scroll), Left/Right or 2/8 page,
// OK/5 open link, LSK menu, RSK back/stop, 1 address, 3 search, 0 home.
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
#include "vmsim.h"
#include "thread.h"
#include <string.h>
#include <stdio.h>

/* ---------- limits ---------- */

#define DOC_MAX 24000       // UCS-2 chars of page text kept
#define LINK_MAX 400
#define SPAN_MAX 96         // headings
#define POOL_MAX 20000      // link targets
#define LINE_MAX 3200
#define URL_MAX 512
#define HIST_MAX 12
#define MAX_REDIRECTS 6
#define NET_TIMEOUT_MS 45000

#define UA "Opera/9.80 (Series 60; Opera Mini/7.1.32444/191.256; U; es) Presto/2.12.423 Version/12.16"
#define HOME_URL "about:home"
#define SEARCH_URL "https://lite.duckduckgo.com/lite/?q="

/* ---------- colours ---------- */

static VMUINT16 c_bg, c_text, c_link, c_focus_bg, c_head, c_bar, c_bar_text, c_dim, c_err;

/* ---------- screen ---------- */

static int scr_w = 240, scr_h = 320;
static VMINT layer = -1;
static VMUINT8 *buf = 0;
static VMINT timer_id = -1;
static int fh = 14, lh = 16;        // font height, line height
#define BAR_H (lh + 4)
#define BODY_Y BAR_H
#define BODY_H (scr_h - 2 * BAR_H)
#define MARGIN 4
static int dirty = 1;

/* ---------- document ---------- */

typedef struct { int start, end, href; } link_t;
typedef struct { int start, end; } span_t;
typedef struct { int start, end; } line_t;

static VMWCHAR doc[DOC_MAX];
static int doc_len;
static link_t links[LINK_MAX];
static int nlinks;
static span_t spans[SPAN_MAX];
static int nspans;
static char pool[POOL_MAX];
static int pool_len;
static line_t lines[LINE_MAX];
static int nlines;
static int top_line, focus = -1;
static char page_title[96];
static char cur_url[URL_MAX];
static char refresh_url[URL_MAX];
static int truncated;

static char history[HIST_MAX][URL_MAX];
static int hist_n;

/* ---------- loader state (shared with the worker coroutine) ---------- */

enum { L_IDLE, L_LOADING, L_DONE, L_ERROR };
static volatile int load_state = L_IDLE;
static volatile int req_pending, cancel_req;
static char req_url[URL_MAX];
static int req_push_history;
static char status_msg[96];
static int bytes_in;
static int strict_certs = 1;

static int menu_open, menu_sel;
static int show_about;

/* ===================================================================== */
/*  Entropy (BearSSL asks for it through br_mre_get_entropy)             */
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

static void seed_path(VMWCHAR *out, int size, const char *name) {
	char p[40];
	VMINT drive = vm_get_removable_driver();
	if (drive < 0) drive = vm_get_system_driver();
	sprintf(p, "%c:\\%s", (char)drive, name);
	vm_ascii_to_ucs2(out, size, p);
}

static void seed_save(void) {
	VMWCHAR path[48];
	unsigned char h[32];
	br_sha256_context c = ent_ctx;
	VMFILE f;
	VMUINT n;
	ent_add("save", 4);
	br_sha256_out(&c, h);
	seed_path(path, sizeof path, "nokiaweb.seed");
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
	seed_path(path, sizeof path, "nokiaweb.seed");
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
	ent_add(h, sizeof h); // ratchet so the next draw differs
	seed_save();
}

/* ===================================================================== */
/*  Small string helpers                                                 */
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

static void url_decode(const char *src, char *dst, int size) {
	int i = 0;
	while (*src && i < size - 1) {
		if (*src == '%' && hexval(src[1]) >= 0 && hexval(src[2]) >= 0) {
			dst[i++] = (char)(hexval(src[1]) * 16 + hexval(src[2]));
			src += 3;
		} else if (*src == '+') {
			dst[i++] = ' '; src++;
		} else dst[i++] = *src++;
	}
	dst[i] = 0;
}

static void url_encode(const char *src, char *dst, int size) {
	static const char hex[] = "0123456789ABCDEF";
	int i = 0;
	for (; *src && i < size - 4; src++) {
		unsigned char c = (unsigned char)*src;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			dst[i++] = c;
		else if (c == ' ') dst[i++] = '+';
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

/* ===================================================================== */
/*  URLs                                                                 */
/* ===================================================================== */

typedef struct {
	int tls;
	char host[128];
	int port;
	char path[URL_MAX];
} url_t;

static int parse_url(const char *s, url_t *u) {
	int i = 0;
	if (starts_with_ci(s, "https://")) { u->tls = 1; u->port = 443; s += 8; }
	else if (starts_with_ci(s, "http://")) { u->tls = 0; u->port = 80; s += 7; }
	else return 0;
	while (*s && *s != '/' && *s != ':' && *s != '?' && i < (int)sizeof(u->host) - 1) u->host[i++] = *s++;
	u->host[i] = 0;
	if (!i) return 0;
	if (*s == ':') {
		u->port = 0;
		s++;
		while (*s >= '0' && *s <= '9') u->port = u->port * 10 + (*s++ - '0');
	}
	if (*s == '/') copy_str(u->path, s, sizeof u->path);
	else { u->path[0] = '/'; copy_str(u->path + 1, s, sizeof u->path - 1); }
	return 1;
}

// Collapse "/./" and "/x/../" in the path part of an absolute URL.
static void normalize_path(char *url) {
	char *p = strstr(url, "://"), *path, *q;
	char out[URL_MAX];
	int o = 0;
	if (!p) return;
	path = strchr(p + 3, '/');
	if (!path) return;
	q = strchr(path, '?');
	memcpy(out, url, path - url);
	o = path - url;
	while (*path && path != q) {
		if (path[0] == '/' && path[1] == '.' && (path[2] == '/' || path[2] == 0 || path + 2 == q)) { path += 2; continue; }
		if (path[0] == '/' && path[1] == '.' && path[2] == '.' && (path[3] == '/' || path[3] == 0 || path + 3 == q)) {
			path += 3;
			while (o > 0 && out[o - 1] != '/') o--;
			if (o > 0 && (p - url) + 3 < o) o--; // drop the slash too, keep host
			while (o > (p - url) + 3 && out[o - 1] != '/') o--;
			if (o <= (p - url) + 3) { out[o++] = '/'; }
			else o--;
			continue;
		}
		if (o < URL_MAX - 1) out[o++] = *path;
		path++;
	}
	while (*path && o < URL_MAX - 1) out[o++] = *path++;
	out[o] = 0;
	copy_str(url, out, URL_MAX);
}

// Resolve href against base into out (absolute, no fragment). Returns 0 if not followable.
static int resolve_url(const char *base, const char *href, char *out) {
	char tmp[URL_MAX];
	int i, o = 0;
	const char *p;
	while (*href == ' ' || *href == '\n' || *href == '\t' || *href == '\r') href++;
	if (!*href || *href == '#') return 0;
	if (starts_with_ci(href, "javascript:") || starts_with_ci(href, "mailto:") || starts_with_ci(href, "tel:") ||
		starts_with_ci(href, "data:") || starts_with_ci(href, "sms:"))
		return 0;
	if (starts_with_ci(href, "http://") || starts_with_ci(href, "https://") || starts_with_ci(href, "about:")) {
		copy_str(tmp, href, sizeof tmp);
	} else if (href[0] == '/' && href[1] == '/') {
		p = strstr(base, "://");
		if (!p) return 0;
		o = p - base + 1;
		memcpy(tmp, base, o);
		copy_str(tmp + o, href, sizeof tmp - o);
	} else if (href[0] == '/') {
		p = strstr(base, "://");
		if (!p) return 0;
		p = strchr(p + 3, '/');
		o = p ? p - base : (int)strlen(base);
		memcpy(tmp, base, o);
		copy_str(tmp + o, href, sizeof tmp - o);
	} else if (href[0] == '?') {
		p = strchr(base, '?');
		o = p ? p - base : (int)strlen(base);
		memcpy(tmp, base, o);
		copy_str(tmp + o, href, sizeof tmp - o);
	} else {
		const char *q = strchr(base, '?');
		p = strstr(base, "://");
		if (!p) return 0;
		o = q ? q - base : (int)strlen(base);
		while (o > (p - base) + 3 && base[o - 1] != '/') o--;
		if (o <= (p - base) + 3) { // base has no path
			o = strlen(base);
			memcpy(tmp, base, o);
			tmp[o++] = '/';
		} else memcpy(tmp, base, o);
		copy_str(tmp + o, href, sizeof tmp - o);
	}
	// strip fragment, encode spaces, drop control chars
	for (i = 0, o = 0; tmp[i] && tmp[i] != '#' && o < URL_MAX - 4; i++) {
		if (tmp[i] == ' ') { out[o++] = '%'; out[o++] = '2'; out[o++] = '0'; }
		else if ((unsigned char)tmp[i] >= 32) out[o++] = tmp[i];
	}
	out[o] = 0;
	if (!starts_with_ci(out, "about:")) normalize_path(out);
	// DuckDuckGo wraps results in a redirect page; jump straight to the target.
	if (strstr(out, "duckduckgo.com/l/") && (p = strstr(out, "uddg=")) != 0) {
		char enc[URL_MAX];
		for (i = 0, p += 5; *p && *p != '&' && i < URL_MAX - 1; i++) enc[i] = *p++;
		enc[i] = 0;
		url_decode(enc, tmp, sizeof tmp);
		if (starts_with_ci(tmp, "http")) copy_str(out, tmp, URL_MAX);
	}
	return 1;
}

/* ===================================================================== */
/*  Document building                                                    */
/* ===================================================================== */

static int link_open = -1;   // index into pool for the <a> being built
static int link_open_start;
static int span_open = -1;
static int pending_space, at_line_start = 1, blank_lines = 1, in_pre;

static void doc_reset(void) {
	doc_len = nlinks = nspans = pool_len = nlines = 0;
	top_line = 0;
	focus = -1;
	page_title[0] = refresh_url[0] = 0;
	truncated = 0;
	link_open = span_open = -1;
	pending_space = 0;
	at_line_start = 1;
	blank_lines = 1;
	in_pre = 0;
}

static void doc_put(VMWCHAR c) {
	if (doc_len >= DOC_MAX - 1) { truncated = 1; return; }
	doc[doc_len++] = c;
}

static void emit_newline(void) {
	pending_space = 0;
	if (!at_line_start) { doc_put('\n'); at_line_start = 1; blank_lines = 0; }
}

static void emit_break(void) { // paragraph: at most one blank line
	emit_newline();
	if (blank_lines < 1 && doc_len) { doc_put('\n'); blank_lines = 1; }
}

static void emit_char(VMWCHAR c) {
	if (!in_pre && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
		if (!at_line_start) pending_space = 1;
		return;
	}
	if (in_pre && c == '\n') { doc_put('\n'); at_line_start = 1; return; }
	if (in_pre && c == '\r') return;
	if (pending_space) { doc_put(' '); pending_space = 0; }
	if (c == '\t') c = ' ';
	doc_put(c);
	at_line_start = 0;
	blank_lines = 0;
}

static void emit_ascii(const char *s) { while (*s) emit_char((unsigned char)*s++); }

static int pool_add(const char *s) {
	int n = strlen(s) + 1, at = pool_len;
	if (pool_len + n > POOL_MAX) return -1;
	memcpy(pool + pool_len, s, n);
	pool_len += n;
	return at;
}

static void link_begin(const char *abs_url) {
	link_open = pool_add(abs_url);
	if (pending_space && !at_line_start) { doc_put(' '); pending_space = 0; }
	link_open_start = doc_len;
}

static void link_end(const char *fallback) {
	if (link_open < 0) return;
	if (doc_len == link_open_start && fallback) emit_ascii(fallback);
	while (link_open_start < doc_len && doc[link_open_start] == ' ') link_open_start++; // whitespace before the label
	if (doc_len > link_open_start && nlinks < LINK_MAX) {
		links[nlinks].start = link_open_start;
		links[nlinks].end = doc_len;
		links[nlinks].href = link_open;
		nlinks++;
	}
	link_open = -1;
}

/* ---------- HTML tokenizer ---------- */

enum { H_TEXT, H_TAG, H_COMMENT, H_RAW, H_ENTITY, H_PLAIN };
static int hstate;
static char tagbuf[1024];
static int taglen, tagquote;
static char entbuf[12];
static int entlen;
static char raw_end[12];   // "</script" etc.
static int raw_match;
static int skip_depth;     // inside <select>, <svg> ...
static int in_title;
static int title_len;
static char base_href[URL_MAX];

// charset decoding
static int cs_latin1;
static unsigned utf_cp;
static int utf_need;

static const struct { const char *name; unsigned short cp; } entities[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
	{"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA},
	{"Aacute", 0xC1}, {"Eacute", 0xC9}, {"Iacute", 0xCD}, {"Oacute", 0xD3}, {"Uacute", 0xDA},
	{"ntilde", 0xF1}, {"Ntilde", 0xD1}, {"uuml", 0xFC}, {"Uuml", 0xDC}, {"ccedil", 0xE7},
	{"agrave", 0xE0}, {"egrave", 0xE8}, {"ograve", 0xF2}, {"auml", 0xE4}, {"ouml", 0xF6},
	{"iexcl", 0xA1}, {"iquest", 0xBF}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"copy", 0xA9},
	{"reg", 0xAE}, {"deg", 0xB0}, {"middot", 0xB7}, {"euro", 0x20AC}, {"pound", 0xA3},
	{"ndash", '-'}, {"mdash", '-'}, {"hellip", 0x2026}, {"lsquo", '\''}, {"rsquo", '\''},
	{"ldquo", '"'}, {"rdquo", '"'}, {"bull", '*'}, {"times", 'x'}, {"ordm", 0xBA}, {"ordf", 0xAA},
	{0, 0}
};

static void html_text_char(unsigned cp);

static void html_reset(int plain) {
	hstate = plain ? H_PLAIN : H_TEXT;
	taglen = tagquote = entlen = raw_match = skip_depth = in_title = title_len = 0;
	base_href[0] = 0;
	cs_latin1 = 0;
	utf_need = 0;
	if (plain) in_pre = 1;
}

static void get_attr(const char *tag, const char *name, char *out, int size) {
	const char *p = tag;
	int n = strlen(name), i = 0;
	out[0] = 0;
	while ((p = strstr_ci(p, name)) != 0) {
		const char *q = p + n;
		if (p > tag && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r' || p[-1] == '/')) {
			while (*q == ' ') q++;
			if (*q == '=') {
				char quote = 0;
				q++;
				while (*q == ' ') q++;
				if (*q == '"' || *q == '\'') quote = *q++;
				while (*q && i < size - 1 && (quote ? *q != quote : (*q != ' ' && *q != '>' && *q != '\t' && *q != '\n')))
					out[i++] = *q++;
				out[i] = 0;
				return;
			}
		}
		p += n;
	}
}

static int tag_is(const char *name, const char *t) {
	while (*t) if (lower(*name++) != *t++) return 0;
	return *name == 0;
}

static void decode_attr_entities(char *s) {
	// attribute values carry &amp; in hrefs, &gt; &quot; in labels (ASCII results only)
	static const struct { const char *ent; char c; } map[] = {
		{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&#x27;", '\''}, {"&nbsp;", ' '}
	};
	char *r = s, *w = s;
	int i;
	while (*r) {
		if (*r == '&') {
			for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++) {
				int n = strlen(map[i].ent);
				if (starts_with_ci(r, map[i].ent)) { *w++ = map[i].c; r += n; break; }
			}
			if (i < (int)(sizeof map / sizeof map[0])) continue;
		}
		*w++ = *r++;
	}
	*w = 0;
}

static void handle_tag(void) {
	char name[16];
	char val[URL_MAX], abs[URL_MAX];
	int closing = 0, i = 0;
	const char *t = tagbuf;
	tagbuf[taglen] = 0;
	if (*t == '/') { closing = 1; t++; }
	while (*t && *t != ' ' && *t != '>' && *t != '/' && *t != '\t' && *t != '\n' && *t != '\r' && i < 15) name[i++] = lower(*t++);
	name[i] = 0;
	if (!name[0] || name[0] == '!' || name[0] == '?') return;

	if (skip_depth) {
		if (closing && (tag_is(name, "select") || tag_is(name, "svg") || tag_is(name, "template") || tag_is(name, "datalist"))) skip_depth--;
		else if (!closing && (tag_is(name, "select") || tag_is(name, "svg"))) skip_depth++;
		return;
	}

	if (!closing && (tag_is(name, "script") || tag_is(name, "style"))) {
		sprintf(raw_end, "</%s", name);
		raw_match = 0;
		hstate = H_RAW;
		return;
	}
	if (!closing && (tag_is(name, "select") || tag_is(name, "svg") || tag_is(name, "template") || tag_is(name, "datalist"))) {
		skip_depth = 1;
		return;
	}
	if (tag_is(name, "title")) { in_title = !closing; if (!closing) title_len = 0; return; }

	if (tag_is(name, "a")) {
		if (closing) { link_end("[link]"); return; }
		link_end("[link]");
		get_attr(tagbuf, "href", val, sizeof val);
		decode_attr_entities(val);
		if (val[0] && resolve_url(base_href[0] ? base_href : cur_url, val, abs)) link_begin(abs);
		return;
	}
	if (tag_is(name, "base") && !closing) {
		get_attr(tagbuf, "href", val, sizeof val);
		if (val[0]) resolve_url(cur_url, val, base_href);
		return;
	}
	if (tag_is(name, "meta") && !closing) {
		get_attr(tagbuf, "charset", val, sizeof val);
		if (!val[0]) {
			get_attr(tagbuf, "content", val, sizeof val);
			if (strstr_ci(tagbuf, "refresh")) {
				const char *u = strstr_ci(val, "url=");
				if (u) {
					u += 4;
					if (*u == '\'' || *u == '"') u++;
					copy_str(abs, u, sizeof abs);
					i = strlen(abs);
					if (i && (abs[i - 1] == '\'' || abs[i - 1] == '"')) abs[i - 1] = 0;
					resolve_url(cur_url, abs, refresh_url);
				}
				return;
			}
		}
		if (strstr_ci(val, "iso-8859") || strstr_ci(val, "windows-125") || strstr_ci(val, "latin")) cs_latin1 = 1;
		else if (strstr_ci(val, "utf-8")) cs_latin1 = 0;
		return;
	}
	if (tag_is(name, "img") && !closing) {
		get_attr(tagbuf, "alt", val, sizeof val);
		decode_attr_entities(val);
		if (val[0]) { emit_char('['); emit_ascii(val); emit_char(']'); }
		else if (link_open >= 0 && doc_len == link_open_start) emit_ascii("[img]");
		return;
	}
	if ((tag_is(name, "iframe") || tag_is(name, "frame")) && !closing) {
		get_attr(tagbuf, "src", val, sizeof val);
		if (val[0] && resolve_url(cur_url, val, abs)) {
			emit_newline();
			link_begin(abs);
			emit_ascii("[frame]");
			link_end(0);
			emit_newline();
		}
		return;
	}
	if (tag_is(name, "input") && !closing) {
		get_attr(tagbuf, "type", val, sizeof val);
		if (tag_is(val, "hidden")) return;
		if (tag_is(val, "submit") || tag_is(val, "button")) {
			get_attr(tagbuf, "value", val, sizeof val);
			decode_attr_entities(val);
			emit_char('['); emit_ascii(val[0] ? val : "OK"); emit_char(']');
		} else emit_ascii("[____]");
		return;
	}
	if (tag_is(name, "br")) { if (pending_space) pending_space = 0; doc_put('\n'); at_line_start = 1; return; }
	if (tag_is(name, "hr")) { emit_newline(); emit_ascii("----------"); emit_newline(); return; }
	if (tag_is(name, "pre")) { emit_break(); in_pre = !closing; return; }
	if (tag_is(name, "li")) {
		if (!closing) { emit_newline(); emit_ascii("- "); }
		return;
	}
	if (tag_is(name, "td") || tag_is(name, "th")) { if (!closing) pending_space = !at_line_start; return; }
	if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
		emit_break();
		if (!closing) { span_open = doc_len; }
		else if (span_open >= 0) {
			if (nspans < SPAN_MAX && doc_len > span_open) { spans[nspans].start = span_open; spans[nspans].end = doc_len; nspans++; }
			span_open = -1;
		}
		return;
	}
	if (tag_is(name, "p") || tag_is(name, "table") || tag_is(name, "ul") || tag_is(name, "ol") ||
		tag_is(name, "blockquote") || tag_is(name, "dl") || tag_is(name, "form") || tag_is(name, "figure") ||
		tag_is(name, "section") || tag_is(name, "article") || tag_is(name, "header") || tag_is(name, "footer") ||
		tag_is(name, "nav") || tag_is(name, "main") || tag_is(name, "aside")) {
		emit_break();
		return;
	}
	if (tag_is(name, "div") || tag_is(name, "tr") || tag_is(name, "dt") || tag_is(name, "dd") ||
		tag_is(name, "center") || tag_is(name, "address") || tag_is(name, "figcaption") || tag_is(name, "caption") ||
		tag_is(name, "option")) {
		emit_newline();
		return;
	}
}

static void flush_entity(void) {
	unsigned cp = 0;
	int i, ok = 0;
	entbuf[entlen] = 0;
	if (entbuf[0] == '#') {
		if (entbuf[1] == 'x' || entbuf[1] == 'X') { for (i = 2; entbuf[i]; i++) if (hexval(entbuf[i]) >= 0) cp = cp * 16 + hexval(entbuf[i]); ok = i > 2; }
		else { for (i = 1; entbuf[i]; i++) if (entbuf[i] >= '0' && entbuf[i] <= '9') cp = cp * 10 + (entbuf[i] - '0'); ok = i > 1; }
		if (cp == 0xA0) cp = ' ';
		if (cp == 0x2013 || cp == 0x2014) cp = '-';
		if (cp == 0x2018 || cp == 0x2019) cp = '\'';
		if (cp == 0x201C || cp == 0x201D) cp = '"';
		if (cp > 0xFFFF) cp = '?';
	} else {
		for (i = 0; entities[i].name; i++) if (!strcmp(entities[i].name, entbuf)) { cp = entities[i].cp; ok = 1; break; }
	}
	if (ok && cp) html_text_char(cp);
	else { html_text_char('&'); for (i = 0; entbuf[i]; i++) html_text_char((unsigned char)entbuf[i]); }
	entlen = 0;
}

static void html_text_char(unsigned cp) {
	if (cp == 0xA0) cp = ' ';
	if (in_title) {
		if (title_len < (int)sizeof(page_title) - 4) {
			// store title as UTF-8
			if (cp < 0x80) page_title[title_len++] = (char)cp;
			else if (cp < 0x800) { page_title[title_len++] = 0xC0 | (cp >> 6); page_title[title_len++] = 0x80 | (cp & 63); }
			else { page_title[title_len++] = 0xE0 | (cp >> 12); page_title[title_len++] = 0x80 | ((cp >> 6) & 63); page_title[title_len++] = 0x80 | (cp & 63); }
			page_title[title_len] = 0;
		}
		return;
	}
	if (skip_depth) return;
	emit_char((VMWCHAR)cp);
}

// One decoded code point of the response body.
static void html_feed_cp(unsigned cp) {
	switch (hstate) {
	case H_PLAIN:
		emit_char((VMWCHAR)cp);
		break;
	case H_TEXT:
		if (cp == '<') { hstate = H_TAG; taglen = 0; tagquote = 0; }
		else if (cp == '&') { hstate = H_ENTITY; entlen = 0; }
		else html_text_char(cp);
		break;
	case H_ENTITY:
		if (cp == ';') { flush_entity(); hstate = H_TEXT; }
		else if (((cp | 32) >= 'a' && (cp | 32) <= 'z') || (cp >= '0' && cp <= '9') || (cp == '#' && entlen == 0)) {
			if (entlen < (int)sizeof(entbuf) - 1) entbuf[entlen++] = (char)cp;
			else { flush_entity(); hstate = H_TEXT; }
		} else { flush_entity(); hstate = H_TEXT; html_feed_cp(cp); }
		break;
	case H_TAG:
		if (taglen == 3 && tagbuf[0] == '!' && tagbuf[1] == '-' && tagbuf[2] == '-') { hstate = H_COMMENT; raw_match = 0; }
		if (hstate == H_COMMENT) { html_feed_cp(cp); break; }
		if (tagquote) { if ((char)cp == tagquote) tagquote = 0; }
		else if ((cp == '"' || cp == '\'') && taglen && strchr(tagbuf, '=') ) { tagbuf[taglen] = 0; if (tagbuf[taglen - 1] == '=' || tagbuf[taglen - 1] == ' ') tagquote = (char)cp; }
		if (cp == '>' && !tagquote) { handle_tag(); if (hstate == H_TAG) hstate = H_TEXT; break; }
		if (taglen < (int)sizeof(tagbuf) - 1) tagbuf[taglen++] = cp < 0x80 ? (char)cp : '?';
		tagbuf[taglen] = 0;
		break;
	case H_COMMENT:
		// wait for "-->"
		if (cp == '-') raw_match = raw_match < 2 ? raw_match + 1 : 2;
		else if (cp == '>' && raw_match == 2) hstate = H_TEXT;
		else raw_match = 0;
		break;
	case H_RAW:
		if (lower(cp) == raw_end[raw_match]) {
			raw_match++;
			if (!raw_end[raw_match]) { hstate = H_TAG; taglen = 0; strcpy(tagbuf, raw_end + 1); taglen = strlen(tagbuf); }
		} else raw_match = (cp == '<') ? 1 : 0;
		break;
	}
}

static const unsigned short cp1252[32] = {
	0x20AC, '?', ',', 'f', '"', 0x2026, '+', '+', '^', '%', 'S', '<', 0x152, '?', 'Z', '?',
	'?', '\'', '\'', '"', '"', '*', '-', '-', '~', 0x2122, 's', '>', 0x153, '?', 'z', 'Y'
};

static void feed_latin1(unsigned char b) {
	if (b >= 0x80 && b < 0xA0) html_feed_cp(cp1252[b - 0x80]);
	else html_feed_cp(b);
}

// Raw body bytes: UTF-8 with Latin-1 fallback for stray bytes.
static void html_feed_byte(unsigned char b) {
	if (cs_latin1) { feed_latin1(b); return; }
	if (utf_need) {
		if ((b & 0xC0) == 0x80) {
			utf_cp = (utf_cp << 6) | (b & 63);
			if (--utf_need == 0) html_feed_cp(utf_cp > 0xFFFF ? '?' : utf_cp);
			return;
		}
		utf_need = 0;
		html_feed_cp('?');
	}
	if (b < 0x80) html_feed_cp(b);
	else if ((b & 0xE0) == 0xC0) { utf_cp = b & 31; utf_need = 1; }
	else if ((b & 0xF0) == 0xE0) { utf_cp = b & 15; utf_need = 2; }
	else if ((b & 0xF8) == 0xF0) { utf_cp = b & 7; utf_need = 3; }
	else feed_latin1(b);
}

/* ===================================================================== */
/*  Layout                                                               */
/* ===================================================================== */

static unsigned char cw_cache[256];
static int cw_ready;

static int char_w(VMWCHAR c) {
	VMWCHAR s[2];
	if (c < 256 && cw_cache[c]) return cw_cache[c];
	s[0] = c; s[1] = 0;
	{
		int w = vm_graphic_get_string_width(s);
		if (w <= 0) w = 6;
		if (c < 256) cw_cache[c] = (unsigned char)(w > 255 ? 255 : w);
		return w;
	}
}

static void layout(void) {
	int maxw = scr_w - 2 * MARGIN - 4; // leave room for scrollbar
	int i = 0, line_start = 0, w = 0, last_space = -1, w_at_space = 0;
	if (!cw_ready) { vm_graphic_set_font(VM_SMALL_FONT); cw_ready = 1; }
	nlines = 0;
	while (i < doc_len && nlines < LINE_MAX - 1) {
		VMWCHAR c = doc[i];
		if (c == '\n') {
			lines[nlines].start = line_start; lines[nlines].end = i; nlines++;
			line_start = i + 1; w = 0; last_space = -1; i++;
			continue;
		}
		w += char_w(c);
		if (c == ' ') { last_space = i; w_at_space = w; }
		if (w > maxw && i > line_start) {
			if (last_space > line_start) {
				lines[nlines].start = line_start; lines[nlines].end = last_space; nlines++;
				line_start = last_space + 1;
				w = w - w_at_space;
			} else {
				lines[nlines].start = line_start; lines[nlines].end = i; nlines++;
				line_start = i;
				w = char_w(c);
			}
			last_space = -1;
		}
		i++;
	}
	if (line_start < doc_len || nlines == 0) { lines[nlines].start = line_start; lines[nlines].end = doc_len; nlines++; }
}

static int visible_lines(void) { return BODY_H / lh; }

static int line_of(int pos) { // binary search
	int lo = 0, hi = nlines - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (lines[mid].start <= pos) lo = mid; else hi = mid - 1;
	}
	return lo;
}

static void clamp_top(void) {
	int maxtop = nlines - visible_lines();
	if (top_line > maxtop) top_line = maxtop;
	if (top_line < 0) top_line = 0;
}

static int link_visible(int i) {
	int l = line_of(links[i].start);
	return l >= top_line && l < top_line + visible_lines();
}

/* ===================================================================== */
/*  Built-in pages                                                       */
/* ===================================================================== */

static void page_link(const char *label, const char *url) {
	link_begin(url);
	emit_ascii(label);
	link_end(0);
	emit_newline();
}

static void build_home(void) {
	doc_reset();
	copy_str(cur_url, HOME_URL, sizeof cur_url);
	copy_str(page_title, "Nokia Web", sizeof page_title);
	span_open = doc_len; emit_ascii("Nokia Web"); spans[nspans].start = span_open; spans[nspans].end = doc_len; nspans++; span_open = -1;
	emit_break();
	emit_ascii("A tiny text browser for your Nokia 216. Pages are fetched directly by the phone, HTTPS included.");
	emit_break();
	page_link("> Search the web", "about:search");
	page_link("> Type an address", "about:go");
	emit_break();
	span_open = doc_len; emit_ascii("Light sites"); spans[nspans].start = span_open; spans[nspans].end = doc_len; nspans++; span_open = -1;
	emit_break();
	page_link("Wikipedia (ES)", "https://es.m.wikipedia.org/wiki/Especial:Aleatoria");
	page_link("68k.news - noticias", "http://68k.news/index.php?section=nation&loc=ES");
	page_link("El tiempo en Madrid", "https://wttr.in/Madrid?lang=es&format=%l:+%C+%t+(sensaci%C3%B3n+%f),+viento+%w,+humedad+%h,+lluvia+%p");
	page_link("Hacker News", "https://news.ycombinator.com/");
	page_link("NPR text", "https://text.npr.org/");
	page_link("CNN lite", "https://lite.cnn.com/");
	page_link("wiby.me - small web search", "http://wiby.me/");
	page_link("DuckDuckGo lite", "https://lite.duckduckgo.com/lite/");
	emit_break();
	emit_ascii("Keys: Up/Down links, Left/Right page, OK open, LSK menu, RSK back. 1 address, 3 search, 0 home.");
	layout();
	load_state = L_DONE;
	status_msg[0] = 0;
	dirty = 1;
}

static void build_message(const char *title, const char *msg) {
	doc_reset();
	copy_str(page_title, title, sizeof page_title);
	span_open = doc_len; emit_ascii(title); spans[nspans].start = span_open; spans[nspans].end = doc_len; nspans++; span_open = -1;
	emit_break();
	emit_ascii(msg);
	emit_break();
	emit_ascii("URL: ");
	emit_ascii(req_url);
	emit_break();
	page_link("> Try again", req_url);
	page_link("> Home", HOME_URL);
	layout();
	dirty = 1;
}

/* ===================================================================== */
/*  Networking (runs inside the worker coroutine)                        */
/* ===================================================================== */

static VMINT tcp_h = -1;
static volatile int tcp_connected, tcp_err;
static int net_tls;
static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static VMINT net_deadline;

// Lenient X.509 wrapper: accepts unknown roots when strict checking is off.
typedef struct { const br_x509_class *vtable; br_x509_minimal_context *inner; } x509_lax_t;
static x509_lax_t xlax;
static void lax_start_chain(const br_x509_class **c, const char *n) { x509_lax_t *x = (x509_lax_t *)c; x->inner->vtable->start_chain(&x->inner->vtable, n); }
static void lax_start_cert(const br_x509_class **c, uint32_t l) { x509_lax_t *x = (x509_lax_t *)c; x->inner->vtable->start_cert(&x->inner->vtable, l); }
static void lax_append(const br_x509_class **c, const unsigned char *b, size_t l) { x509_lax_t *x = (x509_lax_t *)c; x->inner->vtable->append(&x->inner->vtable, b, l); }
static void lax_end_cert(const br_x509_class **c) { x509_lax_t *x = (x509_lax_t *)c; x->inner->vtable->end_cert(&x->inner->vtable); }
static unsigned lax_end_chain(const br_x509_class **c) {
	x509_lax_t *x = (x509_lax_t *)c;
	unsigned r = x->inner->vtable->end_chain(&x->inner->vtable);
	return r == BR_ERR_X509_NOT_TRUSTED ? 0 : r;
}
static const br_x509_pkey *lax_get_pkey(const br_x509_class *const *c, unsigned *u) { const x509_lax_t *x = (const x509_lax_t *)c; return x->inner->vtable->get_pkey(&x->inner->vtable, u); }
static const br_x509_class lax_vtable = {
	sizeof(x509_lax_t), lax_start_chain, lax_start_cert, lax_append, lax_end_cert, lax_end_chain, lax_get_pkey
};

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
		if (tcp_err || cancel_req || timed_out()) return -1;
		thread_next();
	}
}

static int raw_write(void *ctx, const unsigned char *b, size_t n) {
	(void)ctx;
	for (;;) {
		int r = vm_tcp_write(tcp_h, (void *)b, n);
		if (r > 0) { net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS; return r; }
		if (tcp_err || cancel_req || timed_out()) return -1;
		thread_next();
	}
}

static unsigned long days_since_0(int y, int m, int d) {
	static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	unsigned long days;
	y--;
	days = 365UL * (y + 1) + y / 4 - y / 100 + y / 400; // days before Jan 1 of year y+1
	y++;
	days += cum[m - 1] + d - 1;
	if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
	return days;
}

// newlib's time() lands here; MRE has no syscall layer, so answer from the phone clock.
// <sys/time.h> clashes with vmsock.h, so mirror newlib's arm-none-eabi struct timeval.
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

/* Data account. VM_TCP_APN_CMNET is a China Mobile preset; on S30+ it points at the
   wrong profile and DNS fails. Use the phone's default internet APN instead (same
   approach as RDZDX/mre_https_get_gz, which works on Nokia 220/225). */
static int apn_mode = -1;
static char apn_desc[96];
static void net_close(void);

static void apn_setup(void) {
	vm_apn_info_ext apn;
	if (apn_mode >= 0) return;
	memset(&apn, 0, sizeof apn);
	if (vm_get_default_apn_info(&apn) == 0) {
		vm_dtacct_set(0, apn.apn_info_id);
		apn_mode = VM_APN_USER_DEFINE;
		apn.operator_name[sizeof apn.operator_name - 1] = 0;
		apn.apn_proxy[sizeof apn.apn_proxy - 1] = 0;
		sprintf(apn_desc, "data account %d (%s)%s%s", (int)apn.apn_info_id, apn.operator_name,
			apn.proxy_ip[0] ? ", proxy " : "", apn.proxy_ip[0] ? apn.proxy_ip : "");
	} else {
		apn_mode = VM_TCP_APN_CMNET;
		strcpy(apn_desc, "no default APN found, using CMNET");
	}
}

static volatile int dns_done;
static vm_soc_dns_result dns_res;
static VMINT dns_cb(vm_soc_dns_result *r) {
	if (r) dns_res = *r;
	dns_done = 1;
	return 0;
}

// Explicit lookup through the selected data account; fills ip as "a.b.c.d".
static int resolve_host(const char *host, char *ip) {
	VMINT r;
	unsigned char *a;
	memset(&dns_res, 0, sizeof dns_res);
	dns_done = 0;
	r = vm_soc_get_host_by_name(apn_mode, host, &dns_res, dns_cb);
	if (r == VM_E_SOC_WOULDBLOCK) {
		while (!dns_done) {
			if (cancel_req || timed_out()) return 0;
			thread_next();
		}
	} else if (r != VM_E_SOC_SUCCESS) return 0;
	if (dns_res.num <= 0) return 0;
	a = (unsigned char *)&dns_res.address[0]; // stored in network byte order
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
		if (cancel_req) return "Stopped.";
		if (timed_out()) return "Connection timed out. Check mobile data / APN.";
		thread_next();
	}
	return 0;
}

static const char *net_open(const url_t *u) {
	static char msg[200];
	const char *err;
	char ip[16];
	net_tls = u->tls;
	net_deadline = vm_get_tick_count() + NET_TIMEOUT_MS;
	apn_setup();
	sprintf(status_msg, "Connecting to %s...", u->host);
	dirty = 1;
	err = tcp_open(u->host, u->port);
	if (err && !strcmp(err, "dns")) {
		// the stack couldn't resolve by itself: look the name up explicitly, then connect by IP
		net_close();
		sprintf(status_msg, "Looking up %s...", u->host);
		dirty = 1;
		if (!resolve_host(u->host, ip)) {
			sprintf(msg, "Host not found (DNS failed). Using %s. Check that mobile data is on and the internet APN is set in the phone's settings.", apn_desc);
			return msg;
		}
		sprintf(status_msg, "Connecting to %s (%s)...", u->host, ip);
		dirty = 1;
		err = tcp_open(ip, u->port);
		if (err && !strcmp(err, "dns")) err = "Could not reach the server.";
	}
	if (err) {
		sprintf(msg, "%s Using %s.", err, apn_desc);
		return msg;
	}
	if (u->tls) {
		vm_time_t t;
		sprintf(status_msg, "Secure handshake with %s...", u->host);
		dirty = 1;
		br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
		if (!strict_certs) {
			xlax.vtable = &lax_vtable;
			xlax.inner = &xc;
			br_ssl_engine_set_x509(&sc.eng, &xlax.vtable);
		}
		vm_get_time(&t);
		if (t.year < 2026) { t.year = 2026; t.mon = 11; t.day = 1; t.hour = 12; t.min = t.sec = 0; } // clock never set
		br_x509_minimal_set_time(&xc, days_since_0(t.year, t.mon, t.day), t.hour * 3600UL + t.min * 60UL + t.sec);
		br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
		br_ssl_client_reset(&sc, u->host, 0);
		br_sslio_init(&ioc, &sc.eng, raw_read, 0, raw_write, 0);
	}
	return 0;
}

static void net_close(void) {
	if (tcp_h >= 0) vm_tcp_close(tcp_h);
	tcp_h = -1;
}

static int net_write(const char *s, int n) {
	if (net_tls) {
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
	if (net_tls) return br_sslio_read(&ioc, b, n);
	return raw_read(0, (unsigned char *)b, n);
}

static const char *tls_error_text(void) {
	static char msg[64];
	int err = br_ssl_engine_last_error(&sc.eng);
	switch (err) {
	case BR_ERR_X509_NOT_TRUSTED: return "Unknown certificate authority. You can turn off strict certificates in the menu (less safe).";
	case BR_ERR_X509_EXPIRED: return "Certificate expired or phone clock is wrong.";
	case BR_ERR_X509_BAD_SERVER_NAME: return "Certificate does not match the site name.";
	case 0: return "Connection closed.";
	case BR_ERR_RECV_FATAL_ALERT + 70: return "This site only accepts TLS 1.3, which the phone's TLS engine (1.2) can't speak.";
	case BR_ERR_RECV_FATAL_ALERT + 40: return "The site refused every cipher the phone offers (handshake failure).";
	}
	sprintf(msg, "Secure connection failed (TLS error %d).", err);
	return msg;
}

/* ---------- body decoding: identity / Content-Length / chunked ---------- */

static int chunked, chunk_left, chunk_state, body_left;

static void body_byte(unsigned char b) {
	bytes_in++;
	html_feed_byte(b);
}

// Returns 1 when the body is complete.
static int body_feed(const char *data, int n) {
	int i;
	for (i = 0; i < n; i++) {
		unsigned char b = (unsigned char)data[i];
		if (!chunked) {
			body_byte(b);
			if (body_left > 0 && --body_left == 0) return 1;
			continue;
		}
		switch (chunk_state) {
		case 0: // size line
			if (hexval(b) >= 0) chunk_left = chunk_left * 16 + hexval(b);
			else if (b == '\n') { if (chunk_left == 0) return 1; chunk_state = 2; }
			else chunk_state = 1; // extensions until LF
			break;
		case 1:
			if (b == '\n') { if (chunk_left == 0) return 1; chunk_state = 2; }
			break;
		case 2: // data
			body_byte(b);
			if (--chunk_left == 0) chunk_state = 3;
			break;
		case 3: // CRLF after data
			if (b == '\n') { chunk_state = 0; chunk_left = 0; }
			break;
		}
	}
	return 0;
}

/* ---------- one page load ---------- */

static void finish_page(void) {
	link_end(0);
	if (truncated) { emit_break(); emit_ascii("[Page truncated - too long for the phone]"); }
	if (!page_title[0]) {
		url_t u;
		if (parse_url(cur_url, &u)) copy_str(page_title, u.host, sizeof page_title);
	}
	layout();
	focus = -1;
	{
		int i;
		for (i = 0; i < nlinks; i++) if (link_visible(i)) { focus = i; break; }
	}
}

static void fetch(const char *start_url) {
	char url[URL_MAX], hdr[2048], chunk[512];
	char location[URL_MAX], ctype[96];
	int redirects, hlen, status, i;
	url_t u;
	const char *err;

	copy_str(url, start_url, sizeof url);
	for (redirects = 0; redirects <= MAX_REDIRECTS; redirects++) {
		if (!parse_url(url, &u)) { build_message("Bad address", "Only http:// and https:// addresses are supported."); load_state = L_ERROR; return; }
		bytes_in = 0;
		err = net_open(&u);
		if (err) { net_close(); build_message("Can't connect", err); load_state = L_ERROR; return; }

		sprintf(hdr,
			"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " UA "\r\n"
			"Accept: text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.1\r\n"
			"Accept-Language: es-ES,es;q=0.9,en;q=0.7\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
			u.path, u.host);
		sprintf(status_msg, "Requesting %s...", u.host);
		dirty = 1;
		if (net_write(hdr, strlen(hdr)) < 0) {
			net_close();
			build_message("Can't connect", net_tls ? tls_error_text() : "Could not send the request.");
			load_state = L_ERROR;
			return;
		}

		// headers, one line at a time: some sites send multi-KB headers (CSP),
		// so over-long lines are truncated instead of overflowing.
		location[0] = ctype[0] = 0;
		chunked = 0;
		body_left = -1;
		status = -1;
		for (;;) {
			char c;
			int got = 0;
			hlen = 0;
			for (;;) {
				int r = net_read(&c, 1);
				if (r <= 0) break;
				got = 1;
				if (c == '\n') break;
				if (c != '\r' && hlen < (int)sizeof(hdr) - 1) hdr[hlen++] = c;
			}
			hdr[hlen] = 0;
			if (!got) { status = -2; break; }
			if (status == -1) { // status line
				if (!starts_with_ci(hdr, "HTTP/") || hlen < 12) { status = -2; break; }
				status = 0;
				for (i = 9; hdr[i] >= '0' && hdr[i] <= '9'; i++) status = status * 10 + (hdr[i] - '0');
				continue;
			}
			if (!hlen) break; // blank line: end of headers
			{
				char *val = strchr(hdr, ':');
				if (!val) continue;
				val++;
				while (*val == ' ') val++;
				if (starts_with_ci(hdr, "Location:")) copy_str(location, val, sizeof location);
				else if (starts_with_ci(hdr, "Content-Type:")) copy_str(ctype, val, sizeof ctype);
				else if (starts_with_ci(hdr, "Transfer-Encoding:") && strstr_ci(val, "chunked")) chunked = 1;
				else if (starts_with_ci(hdr, "Content-Length:")) {
					body_left = 0;
					while (*val >= '0' && *val <= '9') body_left = body_left * 10 + (*val++ - '0');
				}
			}
		}
		if (status < 0) {
			net_close();
			build_message("No answer", net_tls ? tls_error_text() : "The server sent no valid response.");
			load_state = L_ERROR;
			return;
		}

		if (status >= 300 && status < 400 && location[0]) {
			char next_url[URL_MAX];
			net_close();
			if (!resolve_url(url, location, next_url)) break;
			copy_str(url, next_url, sizeof url);
			sprintf(status_msg, "Redirect %d...", redirects + 1);
			dirty = 1;
			continue;
		}

		if (ctype[0] && !strstr_ci(ctype, "text/") && !strstr_ci(ctype, "xhtml") && !strstr_ci(ctype, "xml")) {
			char msg[160];
			net_close();
			sprintf(msg, "This is not a web page (%s). Nokia Web can only show text and HTML.", ctype);
			build_message("Can't show file", msg);
			load_state = L_ERROR;
			return;
		}

		// body
		copy_str(cur_url, url, sizeof cur_url);
		doc_reset();
		html_reset(ctype[0] && strstr_ci(ctype, "text/plain") != 0);
		if (strstr_ci(ctype, "iso-8859") || strstr_ci(ctype, "windows-125")) cs_latin1 = 1;
		if (status >= 400) {
			char msg[40];
			sprintf(msg, "[HTTP error %d]", status);
			emit_ascii(msg);
			emit_break();
		}
		chunk_left = chunk_state = 0;
		{
			int done = (body_left == 0), since_layout = 0;
			while (!done && !cancel_req) {
				int r = net_read(chunk, sizeof chunk);
				if (r <= 0) break;
				done = body_feed(chunk, r);
				since_layout += r;
				if (since_layout > 6000) { // show the page while it streams in
					layout();
					since_layout = 0;
					dirty = 1;
				}
				sprintf(status_msg, "Loading %s  %d KB", u.host, bytes_in / 1024);
				dirty = 1;
				if (truncated) break;
			}
		}
		net_close();
		finish_page();
		load_state = L_DONE;
		status_msg[0] = 0;
		dirty = 1;
		seed_save();

		// tiny redirect pages: <meta http-equiv="refresh">
		if (refresh_url[0] && doc_len < 400 && redirects < MAX_REDIRECTS) {
			copy_str(url, refresh_url, sizeof url);
			continue;
		}
		return;
	}
	build_message("Too many redirects", "The site kept redirecting.");
	load_state = L_ERROR;
}

static void worker(void) {
	for (;;) {
		while (!req_pending) thread_next();
		req_pending = 0;
		cancel_req = 0;
		load_state = L_LOADING;
		fetch(req_url);
		if (cancel_req && load_state == L_LOADING) load_state = L_DONE;
		dirty = 1;
	}
}

/* ===================================================================== */
/*  Navigation                                                           */
/* ===================================================================== */

static void ask_url(void);
static void ask_search(void);

static void navigate(const char *url, int push) {
	if (!strcmp(url, "about:search")) { ask_search(); return; }
	if (!strcmp(url, "about:go")) { ask_url(); return; }
	if (push && cur_url[0] && strcmp(cur_url, url)) {
		if (hist_n == HIST_MAX) { memmove(history[0], history[1], (HIST_MAX - 1) * URL_MAX); hist_n--; }
		copy_str(history[hist_n++], cur_url, URL_MAX);
	}
	if (!strcmp(url, HOME_URL)) { build_home(); return; }
	if (load_state == L_LOADING) cancel_req = 1;
	copy_str(req_url, url, sizeof req_url);
	req_pending = 1;
	load_state = L_LOADING;
	status_msg[0] = 0;
	dirty = 1;
}

static void go_back(void) {
	if (load_state == L_LOADING) { cancel_req = 1; return; }
	if (!hist_n) return;
	hist_n--;
	navigate(history[hist_n], 0);
}

static VMWCHAR input_default[URL_MAX];
static int input_mode; // 0 url, 1 search

static void input_done(VMINT state, VMWSTR text) {
	char utf[URL_MAX], url[URL_MAX];
	if (state != VM_INPUT_OK || !text || !text[0]) return;
	ucs2_to_utf8(text, utf, sizeof utf);
	if (input_mode == 1) {
		char enc[URL_MAX];
		url_encode(utf, enc, sizeof enc - sizeof(SEARCH_URL));
		sprintf(url, SEARCH_URL "%s", enc);
	} else {
		char *s = utf;
		while (*s == ' ') s++;
		if (strstr(s, "://") || starts_with_ci(s, "about:")) copy_str(url, s, sizeof url);
		else { strcpy(url, "https://"); copy_str(url + 8, s, sizeof url - 8); }
	}
	navigate(url, 1);
}

static void ask_url(void) {
	input_mode = 0;
	vm_ascii_to_ucs2(input_default, sizeof input_default, (VMSTR)(starts_with_ci(cur_url, "about:") ? "" : cur_url));
	vm_input_text3(input_default, URL_MAX / 2 - 1, VM_INPUT_METHOD_URL, input_done);
}

static void ask_search(void) {
	input_mode = 1;
	input_default[0] = 0;
	vm_input_text3(input_default, 120, VM_INPUT_METHOD_ALPHABETIC_EXT, input_done);
}

static void focus_move(int dir) {
	int i;
	int vis = visible_lines();
	if (dir > 0) {
		for (i = focus + 1; i < nlinks; i++) {
			int l = line_of(links[i].start);
			if (l >= top_line + vis) break;
			if (l >= top_line) { focus = i; return; }
		}
		if (top_line + vis < nlines) {
			top_line += 3;
			clamp_top();
			if (focus >= 0 && !link_visible(focus)) focus = -1;
			for (i = (focus < 0 ? 0 : focus + 1); i < nlinks; i++) if (link_visible(i)) { if (focus < 0 || line_of(links[i].start) >= top_line + vis - 3) focus = i; break; }
		}
	} else {
		for (i = focus - 1; i >= 0; i--) {
			int l = line_of(links[i].start);
			if (l < top_line) break;
			if (l < top_line + vis) { focus = i; return; }
		}
		if (top_line > 0) {
			top_line -= 3;
			clamp_top();
			if (focus >= 0 && !link_visible(focus)) focus = -1;
			for (i = (focus < 0 ? nlinks - 1 : focus - 1); i >= 0; i--) if (link_visible(i)) { if (focus < 0 || line_of(links[i].start) < top_line + 3) focus = i; break; }
		}
	}
}

static void page_move(int dir) {
	int i;
	top_line += dir * (visible_lines() - 1);
	clamp_top();
	focus = -1;
	for (i = 0; i < nlinks; i++) if (link_visible(i)) { focus = i; break; }
}

/* ===================================================================== */
/*  Drawing                                                              */
/* ===================================================================== */

static void rect(int x, int y, int w, int h, VMUINT16 c) { vm_graphic_fill_rect(buf, x, y, w, h, c, c); }

static void text_utf8(int x, int y, const char *s, VMUINT16 color, int maxw) {
	VMWCHAR w[100];
	int n = 0, width = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && n < 98) {
		unsigned cp = *p++;
		if (cp >= 0xC0 && (*p & 0xC0) == 0x80) {
			if (cp < 0xE0) { cp = ((cp & 31) << 6) | (*p++ & 63); }
			else if ((p[1] & 0xC0) == 0x80) { cp = ((cp & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
		}
		if (cp == '\n' || cp == '\t') cp = ' ';
		width += char_w((VMWCHAR)cp);
		if (width > maxw) { if (n > 2) { w[n - 1] = '.'; w[n - 2] = '.'; } break; }
		w[n++] = (VMWCHAR)cp;
	}
	w[n] = 0;
	vm_graphic_textout(buf, x, y, w, n, color);
}

static int in_span(int pos) {
	int i;
	for (i = 0; i < nspans; i++) if (pos >= spans[i].start && pos < spans[i].end) return 1;
	return 0;
}

static void draw_line(int li, int y) {
	int pos = lines[li].start, end = lines[li].end, x = MARGIN;
	int li_link = 0;
	// find first link that might touch this line
	while (li_link < nlinks && links[li_link].end <= pos) li_link++;
	while (pos < end) {
		int run_end = end, is_link = -1, w = 0, k;
		VMUINT16 color;
		if (li_link < nlinks && links[li_link].start <= pos) { is_link = li_link; if (links[li_link].end < run_end) run_end = links[li_link].end; }
		else if (li_link < nlinks && links[li_link].start < run_end) run_end = links[li_link].start;
		// split at heading boundaries too
		{
			int hs = in_span(pos);
			for (k = pos + 1; k < run_end; k++) if (in_span(k) != hs) { run_end = k; break; }
			color = is_link >= 0 ? c_link : (hs ? c_head : c_text);
		}
		for (k = pos; k < run_end; k++) w += char_w(doc[k]);
		if (is_link >= 0 && is_link == focus) rect(x - 1, y - 1, w + 2, lh, c_focus_bg);
		vm_graphic_textout(buf, x, y, &doc[pos], run_end - pos, is_link == focus && is_link >= 0 ? c_bg : color);
		if (is_link >= 0 && is_link != focus) rect(x, y + fh, w, 1, c_link);
		x += w;
		pos = run_end;
		if (is_link >= 0 && pos >= links[is_link].end) li_link++;
	}
}

static void draw_menu(void) {
	static const char *items[] = {
		"1 Go to address", "2 Search", "3 Home", "4 Back", "5 Reload", "6 Certificates: ", "7 About", "8 Exit"
	};
	int n = 8, i, w = scr_w - 40, h = n * lh + 10, x = 20, y = scr_h - BAR_H - h - 4;
	rect(x - 2, y - 2, w + 4, h + 4, c_text);
	rect(x, y, w, h, c_bg);
	for (i = 0; i < n; i++) {
		char line[48];
		int yy = y + 5 + i * lh;
		strcpy(line, items[i]);
		if (i == 5) strcat(line, strict_certs ? "strict" : "relaxed");
		if (i == menu_sel) rect(x + 2, yy - 1, w - 4, lh, c_focus_bg);
		text_utf8(x + 6, yy, line, i == menu_sel ? c_bg : c_text, w - 12);
	}
}

static void draw(void) {
	int vis, i, y;
	if (!buf) return;
	dirty = 0;
	vm_graphic_set_font(VM_SMALL_FONT);
	rect(0, 0, scr_w, scr_h, c_bg);

	// page text
	vis = visible_lines();
	for (i = 0, y = BODY_Y + 2; i < vis && top_line + i < nlines; i++, y += lh) draw_line(top_line + i, y);

	// scrollbar
	if (nlines > vis) {
		int track = BODY_H - 4, th = track * vis / nlines, ty;
		if (th < 8) th = 8;
		ty = BODY_Y + 2 + (track - th) * top_line / (nlines - vis);
		rect(scr_w - 3, BODY_Y + 2, 2, track, c_dim);
		rect(scr_w - 4, ty, 4, th, c_text);
	}

	// title bar
	rect(0, 0, scr_w, BAR_H, c_bar);
	if (load_state == L_LOADING) {
		text_utf8(MARGIN, 2, status_msg[0] ? status_msg : "Loading...", c_bar_text, scr_w - 2 * MARGIN);
		rect(0, BAR_H - 2, (vm_get_tick_count() / 40) % scr_w, 2, c_link);
	} else {
		text_utf8(MARGIN, 2, page_title[0] ? page_title : cur_url, c_bar_text, scr_w - 2 * MARGIN);
	}

	// softkey bar
	rect(0, scr_h - BAR_H, scr_w, BAR_H, c_bar);
	text_utf8(MARGIN, scr_h - BAR_H + 2, menu_open ? "Select" : "Menu", c_bar_text, 80);
	{
		const char *mid = menu_open ? "" : (focus >= 0 && link_visible(focus)) ? "Open" : "";
		const char *right = menu_open ? "Close" : load_state == L_LOADING ? "Stop" : hist_n ? "Back" : "Exit";
		VMWCHAR w[16];
		vm_ascii_to_ucs2(w, sizeof w, (VMSTR)mid);
		vm_graphic_textout(buf, (scr_w - vm_graphic_get_string_width(w)) / 2, scr_h - BAR_H + 2, w, vm_wstrlen(w), c_bar_text);
		vm_ascii_to_ucs2(w, sizeof w, (VMSTR)right);
		vm_graphic_textout(buf, scr_w - MARGIN - vm_graphic_get_string_width(w), scr_h - BAR_H + 2, w, vm_wstrlen(w), c_bar_text);
	}

	// status line of focused link URL while idle
	if (!menu_open && load_state != L_LOADING && focus >= 0 && link_visible(focus) && !show_about) {
		rect(0, scr_h - 2 * BAR_H + 2, scr_w, BAR_H - 2, c_dim);
		text_utf8(MARGIN, scr_h - 2 * BAR_H + 3, pool + links[focus].href, c_bg, scr_w - 2 * MARGIN);
	}

	if (menu_open) draw_menu();
	vm_graphic_flush_layer(&layer, 1);
}

/* ===================================================================== */
/*  Events                                                               */
/* ===================================================================== */

static void tick(VMINT tid) {
	(void)tid;
	ent_tick();
	thread_next();  // let the network coroutine run
	if (dirty || load_state == L_LOADING) draw();
}

static void menu_pick(int i) {
	menu_open = 0;
	switch (i) {
	case 0: ask_url(); break;
	case 1: ask_search(); break;
	case 2: navigate(HOME_URL, 1); break;
	case 3: go_back(); break;
	case 4: if (cur_url[0]) navigate(cur_url, 0); break;
	case 5: strict_certs = !strict_certs; break;
	case 6:
		doc_reset();
		copy_str(page_title, "About Nokia Web", sizeof page_title);
		emit_ascii("Nokia Web 1.0 - text browser for MediaTek MRE phones, made for a Nokia 216.");
		emit_break();
		emit_ascii("HTTPS by BearSSL (MIT), coroutines by libco (ISC), TLS plumbing adapted from RDZDX/mre_https_get_gz (MIT). Root certificates from the Mozilla CA list.");
		emit_break();
		emit_ascii("Strict certificates: unknown authorities are rejected. Relaxed: they are accepted, which is less safe on public networks.");
		emit_break();
		page_link("> Home", HOME_URL);
		layout();
		break;
	case 7: vm_exit_app(); break;
	}
	dirty = 1;
}

static void handle_key(VMINT event, VMINT key) {
	ent_tick();
	ent_add(&key, sizeof key);
	if (event != VM_KEY_EVENT_DOWN && event != VM_KEY_EVENT_REPEAT) return;

	if (menu_open) {
		if (key == VM_KEY_UP) menu_sel = (menu_sel + 7) % 8;
		else if (key == VM_KEY_DOWN) menu_sel = (menu_sel + 1) % 8;
		else if (key == VM_KEY_OK || key == VM_KEY_LEFT_SOFTKEY) menu_pick(menu_sel);
		else if (key >= VM_KEY_NUM1 && key <= VM_KEY_NUM8) menu_pick(key - VM_KEY_NUM1);
		else if (key == VM_KEY_RIGHT_SOFTKEY) menu_open = 0;
		dirty = 1;
		return;
	}

	switch (key) {
	case VM_KEY_DOWN: focus_move(1); break;
	case VM_KEY_UP: focus_move(-1); break;
	case VM_KEY_RIGHT: case VM_KEY_NUM8: page_move(1); break;
	case VM_KEY_LEFT: case VM_KEY_NUM2: page_move(-1); break;
	case VM_KEY_OK: case VM_KEY_NUM5:
		if (event == VM_KEY_EVENT_DOWN && focus >= 0 && link_visible(focus)) navigate(pool + links[focus].href, 1);
		break;
	case VM_KEY_LEFT_SOFTKEY: if (event == VM_KEY_EVENT_DOWN) { menu_open = 1; menu_sel = 0; } break;
	case VM_KEY_RIGHT_SOFTKEY:
		if (event != VM_KEY_EVENT_DOWN) break;
		if (load_state == L_LOADING || hist_n) go_back();
		else vm_exit_app();
		break;
	case VM_KEY_NUM1: if (event == VM_KEY_EVENT_DOWN) ask_url(); break;
	case VM_KEY_NUM3: if (event == VM_KEY_EVENT_DOWN) ask_search(); break;
	case VM_KEY_NUM0: if (event == VM_KEY_EVENT_DOWN) navigate(HOME_URL, 1); break;
	case VM_KEY_STAR: top_line = 0; clamp_top(); page_move(0); break;
	case VM_KEY_POUND: top_line = nlines; clamp_top(); page_move(0); break;
	}
	dirty = 1;
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
			seed_load();
			thread_init();
			thread_create(0x10000, worker);
			build_home();
		} else layout();
		if (timer_id < 0) timer_id = vm_create_timer(60, tick);
		dirty = 1;
		draw();
		break;
	case VM_MSG_PAINT:
		dirty = 1;
		draw();
		break;
	case VM_MSG_INACTIVE:
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1;
		buf = 0;
		break;
	case VM_MSG_QUIT:
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
	c_text = VM_COLOR_888_TO_565(20, 20, 20);
	c_link = VM_COLOR_888_TO_565(0, 70, 190);
	c_focus_bg = VM_COLOR_888_TO_565(0, 70, 190);
	c_head = VM_COLOR_888_TO_565(150, 30, 30);
	c_bar = VM_COLOR_888_TO_565(30, 40, 60);
	c_bar_text = VM_COLOR_888_TO_565(240, 240, 240);
	c_dim = VM_COLOR_888_TO_565(110, 120, 140);
	c_err = VM_COLOR_888_TO_565(190, 0, 0);
	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_key);
}
