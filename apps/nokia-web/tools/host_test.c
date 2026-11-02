// Host-side test harness: compiles src/web.c for Windows with the MRE calls stubbed
// (Winsock for TCP, wall clock for time), then fetches real URLs and dumps the
// text + links the phone would show. Build with w64devkit gcc (see tools/host_test.sh).
#include <stdio.h>
#include <time.h>

#include "../src/web.c"

void hn_init(void);
int hn_connect(const char *host, int port);
int hn_recv(int h, void *b, int n);
int hn_send(int h, const void *b, int n);
void hn_close(int h);

/* ---- thread.h ---- */
static void (*pending_cb)(VMINT, VMINT);
static VMINT pending_h = -1, pending_ev;
void thread_init(void) {}
void thread_create(unsigned int size, void (*entry)(void)) { (void)size; (void)entry; }
void thread_next(void) {
	if (pending_cb) { void (*cb)(VMINT, VMINT) = pending_cb; pending_cb = 0; cb(pending_h, pending_ev); }
}
void thread_end(void) {}

/* ---- sockets ---- */
VMINT vm_tcp_connect(const char *host, const VMINT port, const VMINT apn, void (*cb)(VMINT, VMINT)) {
	int r = hn_connect(host, port);
	(void)apn;
	pending_cb = cb;
	pending_h = 1;
	pending_ev = r == 1 ? VM_TCP_EVT_CONNECTED : r == -2 ? VM_TCP_EVT_HOST_NOT_FOUND : VM_TCP_EVT_PIPE_BROKEN;
	return 1;
}
VMINT vm_tcp_read(VMINT h, void *b, VMINT n) {
	int r = hn_recv(h, b, n);
	if (r <= 0) { tcp_err = 1; return -1; }
	return r;
}
VMINT vm_tcp_write(VMINT h, void *b, VMINT n) {
	int r = hn_send(h, b, n);
	if (r <= 0) { tcp_err = 1; return -1; }
	return r;
}
void vm_tcp_close(VMINT h) { hn_close(h); }
VMINT vm_get_default_apn_info(vm_apn_info_ext *a) { a->apn_info_id = 7; strcpy(a->operator_name, "host"); a->proxy_ip[0] = 0; return 0; }
VMINT vm_dtacct_set(VMINT sim, VMUINT32 id) { (void)sim; (void)id; return 0; }
VMINT vm_soc_get_host_by_name(VMINT apn, const VMCHAR *host, vm_soc_dns_result *r, VMINT (*cb)(vm_soc_dns_result *)) {
	(void)apn; (void)host; (void)r; (void)cb;
	return VM_E_SOC_ERROR;
}

/* ---- system ---- */
VMINT vm_get_tick_count(void) { return (VMINT)(clock() * 1000 / CLOCKS_PER_SEC); }
VMINT vm_get_time(vm_time_t *t) {
	time_t now = time(0);
	struct tm *g = gmtime(&now);
	t->year = g->tm_year + 1900; t->mon = g->tm_mon + 1; t->day = g->tm_mday;
	t->hour = g->tm_hour; t->min = g->tm_min; t->sec = g->tm_sec;
	return 0;
}
VMSTR vm_get_imei(void) { return "000000000000000"; }
VMINT vm_get_removable_driver(void) { return -1; }
VMINT vm_get_system_driver(void) { return 'C'; }
VMFILE vm_file_open(const VMWSTR f, VMUINT m, VMUINT b) { (void)f; (void)m; (void)b; return -1; }
void vm_file_close(VMFILE h) { (void)h; }
VMINT vm_file_read(VMFILE h, void *d, VMUINT l, VMUINT *n) { (void)h; (void)d; (void)l; *n = 0; return -1; }
VMINT vm_file_write(VMFILE h, void *d, VMUINT l, VMUINT *n) { (void)h; (void)d; (void)l; *n = 0; return -1; }
VMINT vm_ascii_to_ucs2(VMWSTR dst, VMINT size, VMSTR src) {
	int i = 0;
	while (src[i] && (i + 1) * 2 < size) { dst[i] = (unsigned char)src[i]; i++; }
	dst[i] = 0;
	return 0;
}
VMINT vm_wstrlen(VMWSTR s) { int n = 0; while (s[n]) n++; return n; }
VMINT vm_graphic_get_string_width(VMWSTR s) { return 6 * vm_wstrlen(s); }
void vm_graphic_set_font(font_size_t f) { (void)f; }

/* ---- UI calls the harness never exercises ---- */
VMINT vm_input_text3(VMWSTR d, VMINT m, VMINT im, void (*cb)(VMINT, VMWSTR)) { (void)d; (void)m; (void)im; (void)cb; return -1; }
void vm_graphic_fill_rect(VMUINT8 *b, VMINT x, VMINT y, VMINT w, VMINT h, VMUINT16 l, VMUINT16 c) { (void)b; (void)x; (void)y; (void)w; (void)h; (void)l; (void)c; }
void vm_graphic_textout(VMUINT8 *b, VMINT x, VMINT y, VMWSTR s, VMINT n, VMUINT16 c) { (void)b; (void)x; (void)y; (void)s; (void)n; (void)c; }
VMINT vm_graphic_flush_layer(VMINT *h, VMINT n) { (void)h; (void)n; return 0; }
VMINT vm_graphic_get_screen_width(void) { return 240; }
VMINT vm_graphic_get_screen_height(void) { return 320; }
VMINT vm_graphic_create_layer(VMINT x, VMINT y, VMINT w, VMINT h, VMINT t) { (void)x; (void)y; (void)w; (void)h; (void)t; return 0; }
VMUINT8 *vm_graphic_get_layer_buffer(VMINT h) { (void)h; return 0; }
VMINT vm_graphic_delete_layer(VMINT h) { (void)h; return 0; }
void vm_graphic_set_clip(VMINT a, VMINT b, VMINT c, VMINT d) { (void)a; (void)b; (void)c; (void)d; }
VMINT vm_graphic_get_character_height(void) { return 13; }
VMINT vm_create_timer(VMUINT32 ms, VM_TIMERPROC_T p) { (void)ms; (void)p; return 0; }
VMINT vm_delete_timer(VMINT t) { (void)t; return 0; }
void vm_reg_sysevt_callback(void (*f)(VMINT, VMINT)) { (void)f; }
void vm_reg_keyboard_callback(vm_key_handler_t f) { (void)f; }
void vm_exit_app(void) {}

/* ---- dump ---- */
static void put_utf8(unsigned c) {
	if (c < 0x80) putchar(c);
	else if (c < 0x800) { putchar(0xC0 | (c >> 6)); putchar(0x80 | (c & 63)); }
	else { putchar(0xE0 | (c >> 12)); putchar(0x80 | ((c >> 6) & 63)); putchar(0x80 | (c & 63)); }
}

int main(int argc, char **argv) {
	int a, i, maxlines = argc > 2 ? 60 : 1000000;
	hn_init();
	strict_certs = 1;
	scr_w = 240; scr_h = 320; lh = 16; fh = 13;
	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--relaxed")) { strict_certs = 0; continue; }
		copy_str(req_url, argv[a], sizeof req_url);
		cur_url[0] = 0;
		load_state = L_LOADING;
		fetch(req_url);
		printf("=== %s\n", argv[a]);
		printf("state=%s final=%s title=\"%s\" chars=%d lines=%d links=%d bytes=%d%s\n",
			load_state == L_DONE ? "DONE" : "ERROR", cur_url, page_title, doc_len, nlines, nlinks, bytes_in, truncated ? " TRUNCATED" : "");
		for (i = 0; i < nlines && i < maxlines; i++) {
			int k;
			printf("| ");
			for (k = lines[i].start; k < lines[i].end; k++) put_utf8(doc[k]);
			printf("\n");
		}
		for (i = 0; i < nlinks && i < 15; i++) {
			int k;
			printf("  link %2d: \"", i);
			for (k = links[i].start; k < links[i].end && k < links[i].start + 30; k++) put_utf8(doc[k]);
			printf("\" -> %s\n", pool + links[i].href);
		}
	}
	return 0;
}
