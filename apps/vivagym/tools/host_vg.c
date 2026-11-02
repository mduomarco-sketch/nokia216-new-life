// Host test for vivagym.c: plain HTTP against tools/mock_server.py, MRE calls stubbed.
// Build (w64devkit gcc), from apps/vivagym (needs src/secrets.h, see secrets.example.h):
//   gcc -O1 -w -c tools/host_net.c -o host_net.o
//   gcc -O1 -w -I ../../sdk/include -I src/include -I src -D MRE -D __MRE_SDK__ -D VG_TLS=0 -D VG_HOST=\"127.0.0.1\" -D VG_PORT=8765
//       -o vgtest.exe tools/host_vg.c src/qr.c src/bearssl_impl.c tools/host_ui_stubs.c host_net.o -lws2_32 -ladvapi32
// Run: python tools/mock_server.py <password>   (other terminal), then vgtest.exe
#include <stdio.h>
#include <time.h>
#include "../src/vivagym.c"

void hn_init(void);
int hn_connect(const char *host, int port);
int hn_recv(int h, void *b, int n);
int hn_send(int h, const void *b, int n);
void hn_close(int h);

static void (*pending_cb)(VMINT, VMINT);
static VMINT pending_h = -1, pending_ev;
void thread_init(void) {}
void thread_create(unsigned int size, void (*entry)(void)) { (void)size; (void)entry; }
void thread_next(void) { if (pending_cb) { void (*cb)(VMINT, VMINT) = pending_cb; pending_cb = 0; cb(pending_h, pending_ev); } }
void thread_end(void) {}

VMINT vm_tcp_connect(const char *host, const VMINT port, const VMINT apn, void (*cb)(VMINT, VMINT)) {
	int r = hn_connect(host, port);
	(void)apn;
	pending_cb = cb;
	pending_h = 1;
	pending_ev = r == 1 ? VM_TCP_EVT_CONNECTED : r == -2 ? VM_TCP_EVT_HOST_NOT_FOUND : VM_TCP_EVT_PIPE_BROKEN;
	return 1;
}
VMINT vm_tcp_read(VMINT h, void *b, VMINT n) { int r = hn_recv(h, b, n); if (r <= 0) { tcp_err = 1; return -1; } return r; }
VMINT vm_tcp_write(VMINT h, void *b, VMINT n) { int r = hn_send(h, b, n); if (r <= 0) { tcp_err = 1; return -1; } return r; }
void vm_tcp_close(VMINT h) { hn_close(h); }
VMINT vm_get_default_apn_info(vm_apn_info_ext *a) { a->apn_info_id = 7; return 0; }
VMINT vm_dtacct_set(VMINT sim, VMUINT32 id) { (void)sim; (void)id; return 0; }
VMINT vm_soc_get_host_by_name(VMINT apn, const VMCHAR *host, vm_soc_dns_result *r, VMINT (*cb)(vm_soc_dns_result *)) {
	(void)apn; (void)host; (void)r; (void)cb;
	return VM_E_SOC_ERROR;
}

VMINT vm_get_tick_count(void) { return (VMINT)(clock() * 1000 / CLOCKS_PER_SEC); }
VMINT vm_get_time(vm_time_t *t) {
	time_t now = time(0);
	struct tm *g = gmtime(&now);
	t->year = g->tm_year + 1900; t->mon = g->tm_mon + 1; t->day = g->tm_mday;
	t->hour = g->tm_hour; t->min = g->tm_min; t->sec = g->tm_sec;
	return 0;
}
VMSTR vm_get_imei(void) { return "000000000000000"; }
VMINT vm_get_removable_driver(void) { return 'D'; }
VMINT vm_get_system_driver(void) { return 'C'; }

/* files: X:\name -> ./fs_X_name */
static FILE *fh_tab[8];
static void host_name(const VMWSTR w, char *out) {
	char a[64];
	int i;
	for (i = 0; w[i] && i < 63; i++) a[i] = (char)w[i];
	a[i] = 0;
	sprintf(out, "fs_%c_%s", a[0], a + 3);
}
VMFILE vm_file_open(const VMWSTR f, VMUINT m, VMUINT b) {
	char p[80];
	int i;
	(void)b;
	host_name(f, p);
	for (i = 1; i < 8 && fh_tab[i]; i++) {}
	if (i == 8) return -1;
	fh_tab[i] = fopen(p, m == MODE_READ ? "rb" : "wb");
	return fh_tab[i] ? i : -1;
}
void vm_file_close(VMFILE h) { fclose(fh_tab[h]); fh_tab[h] = 0; }
VMINT vm_file_read(VMFILE h, void *d, VMUINT l, VMUINT *n) { *n = fread(d, 1, l, fh_tab[h]); return 0; }
VMINT vm_file_write(VMFILE h, void *d, VMUINT l, VMUINT *n) { *n = fwrite(d, 1, l, fh_tab[h]); return 0; }
VMINT vm_file_delete(const VMWSTR f) { char p[80]; host_name(f, p); return remove(p); }

VMINT vm_ascii_to_ucs2(VMWSTR dst, VMINT size, VMSTR src) {
	int i = 0;
	while (src[i] && (i + 1) * 2 < size) { dst[i] = (unsigned char)src[i]; i++; }
	dst[i] = 0;
	return 0;
}
VMINT vm_wstrlen(VMWSTR s) { int n = 0; while (s[n]) n++; return n; }
VMINT vm_graphic_get_string_width(VMWSTR s) { return 6 * vm_wstrlen(s); }
void vm_graphic_set_font(font_size_t f) { (void)f; }

static const char *scr_name(int s) {
	static const char *n[] = {"START", "NEED_LOGIN", "BUSY", "QR", "ERROR", "ABOUT"};
	return n[s];
}
static void report(const char *label) {
	printf("[%s]\n   screen=%s qr=\"%s\" ver=%d\n   err=\"%s\" qr_err=\"%s\" access=%s refresh=%s\n", label, scr_name(screen),
		have_qr ? qr_text : "", have_qr ? qr.version : 0, err_msg, qr_err, cfg_access, cfg_refresh);
	fflush(stdout);
}

int main(int argc, char **argv) {
	hn_init();
	remove("fs_C_vivagym.cfg");
	remove("fs_D_vivagym.cfg");
	cfg_load();
	printf("default password compiled in: %s\n", cfg_pass[0] ? "yes" : "no");
	copy_str(cfg_email, argc > 1 ? argv[1] : "socio+test@example.es", sizeof cfg_email);
	job_qr(0); report("1 fresh login");
	strcpy(cfg_access, "EXPIRED"); job_qr(0); report("2 expired access -> refresh token");
	strcpy(cfg_access, "EXPIRED"); strcpy(cfg_refresh, "BAD"); job_qr(0); report("3 bad refresh -> password login");
	memset(cfg_email, 0, sizeof cfg_email); memset(cfg_pass, 0, sizeof cfg_pass); cfg_access[0] = cfg_refresh[0] = 0;
	cfg_load(); report("4 reloaded from settings file");
	printf("   email=%s pass_ok=%d\n", cfg_email, !strcmp(cfg_pass, VG_DEFAULT_PASSWORD));
	strcpy(cfg_pass, "wrong"); cfg_access[0] = cfg_refresh[0] = 0; have_qr = 0; job_qr(0); report("5 wrong password");
	{
		FILE *f = fopen("fs_C_vivagym.cfg", "rb");
		char b[200] = {0};
		if (f) { fread(b, 1, 120, f); fclose(f); }
		printf("settings file head (password must not be readable): %.60s\n", b);
	}
	return 0;
}

/* unused UI calls referenced by vivagym.c */
VMINT vm_input_text3(VMWSTR d, VMINT m, VMINT im, void (*cb)(VMINT, VMWSTR)) { (void)d; (void)m; (void)im; (void)cb; return -1; }
VMINT vm_input_set_editor_title(VMWSTR t) { (void)t; return 1; }
void vm_graphic_fill_rect(VMUINT8 *b, VMINT x, VMINT y, VMINT w, VMINT h, VMUINT16 l, VMUINT16 c) { (void)b; (void)x; (void)y; (void)w; (void)h; (void)l; (void)c; }
