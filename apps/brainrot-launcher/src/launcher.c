// Brainrot Launcher - app grid with fan-art character icons for Nokia S30+ (MRE).
// Lists X:\Apps\*.vxp, gives each app a character, OK launches it.
// D-pad / 2468 move, OK/5 open, RSK exit.
#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "vmtimer.h"
#include "vmpromng.h"
#include <string.h>
#include "sprites.h"

#define MAX_APPS 40
#define COLS 3
#define CELL_W 80
#define CELL_H 90
#define ICON_SCALE 2
#define ICON_PX (SPRITE_N * ICON_SCALE)
#define TOP_H 24
#define BOT_H 22

typedef struct {
	char name[40];        // file name without .vxp
	VMWCHAR path[100];    // full UCS-2 path
	int sprite;
} app_t;

static app_t apps[MAX_APPS];
static int napps, sel, top_row;
static int scr_w = 240, scr_h = 320;
static VMINT layer = -1;
static VMUINT8 *buf;
static VMINT timer_id = -1;
static int frame, splash = 1;
static VMUINT16 c_bg, c_bg2, c_sel, c_text, c_dim, c_bar;

/* ---------- app list ---------- */

static const struct { const char *file, *label; int sprite; } known[] = {
	{"peanut", "Game Boy", 0},
	{"peanut_500k", "Game Boy lite", 6},
	{"peanut_2500k", "Game Boy Color", 5},
	{"NokiaWeb", "Nokia Web", 2},
	{"TelnetVXP", "Telnet", 4},
	{"Snake", "Snake", 8},
	{"GravityDefied", "Gravity", 1},
	{"mtmine64", "MTMine", 9},
	{"2048", "2048", 3},
	{"Calculator", "Calculadora", 7},
	{"AerialFlappers", "Flappy", 10},
	{"rirc", "IRC chat", 1},
	{"FTP", "FTP", 4},
	{"Qrsp", "QR scan", 7},
	{"Photop", "Camara", 3},
	{"VxpMic", "Micro", 5},
	{"BadVXP", "Bad Apple", 6},
	{"ring", "Ring", 10},
	{0, 0, 0}
};

static int eq_ci(const char *a, const char *b) {
	while (*a && *b) {
		char x = *a++, y = *b++;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return *a == *b;
}

static const char *label_of(const app_t *a) {
	int i;
	for (i = 0; known[i].file; i++) if (eq_ci(known[i].file, a->name)) return known[i].label;
	return a->name;
}

static void add_app(const VMWCHAR *found, const char *dir) {
	app_t *a = &apps[napps];
	char ascii[160];
	const char *base;
	int i, n, h = 0;
	vm_ucs2_to_ascii(ascii, sizeof ascii, (VMWSTR)found);
	base = strrchr(ascii, '\\');
	base = base ? base + 1 : ascii;
	n = strlen(base);
	if (n < 5 || !eq_ci(base + n - 4, ".vxp")) return;
	if (eq_ci(base, "Launcher.vxp")) return; // don't list ourselves
	for (i = 0; i < n - 4 && i < (int)sizeof(a->name) - 1; i++) a->name[i] = base[i];
	a->name[i] = 0;
	{
		char full[200];
		if (strchr(ascii, ':')) strcpy(full, ascii); // some firmwares return the full path
		else { strcpy(full, dir); strcat(full, base); }
		vm_ascii_to_ucs2(a->path, sizeof a->path, full);
	}
	a->sprite = -1;
	for (i = 0; known[i].file; i++) if (eq_ci(known[i].file, a->name)) { a->sprite = known[i].sprite; break; }
	if (a->sprite < 0) {
		for (i = 0; a->name[i]; i++) h = h * 31 + a->name[i];
		a->sprite = (h & 0x7fffffff) % SPRITE_COUNT;
	}
	napps++;
}

static void sort_apps(void) {
	int i, j;
	for (i = 1; i < napps; i++) {
		app_t t = apps[i];
		for (j = i; j > 0 && strcmp(label_of(&apps[j - 1]), label_of(&t)) > 0; j--) apps[j] = apps[j - 1];
		apps[j] = t;
	}
}

static void scan_apps(void) {
	char dir[16], pattern[24];
	VMWCHAR wpat[24];
	struct vm_fileinfo_t info;
	VMINT h, drive = vm_get_removable_driver();
	napps = 0;
	if (drive < 0) drive = vm_get_system_driver();
	sprintf(dir, "%c:\\Apps\\", (char)drive);
	sprintf(pattern, "%s*.vxp", dir);
	vm_ascii_to_ucs2(wpat, sizeof wpat, pattern);
	h = vm_find_first(wpat, &info);
	if (h < 0) return;
	do {
		if (napps >= MAX_APPS) break;
		add_app(info.filename, dir);
	} while (vm_find_next(h, &info) == 0);
	vm_find_close(h);
	sort_apps();
}

/* ---------- drawing ---------- */

static void rect(int x, int y, int w, int h, VMUINT16 c) { vm_graphic_fill_rect(buf, x, y, w, h, c, c); }

static void draw_sprite(const sprite_t *s, int x0, int y0, int scale) {
	VMUINT16 *fb = (VMUINT16 *)buf;
	int x, y, i, j;
	for (y = 0; y < SPRITE_N; y++) {
		for (x = 0; x < SPRITE_N; x++) {
			int b = s->px[(y * SPRITE_N + x) >> 1];
			int idx = (x & 1) ? (b & 15) : (b >> 4);
			if (!idx) continue;
			for (j = 0; j < scale; j++) {
				int py = y0 + y * scale + j;
				if (py < TOP_H || py >= scr_h - BOT_H) continue;
				for (i = 0; i < scale; i++) {
					int px = x0 + x * scale + i;
					if (px >= 0 && px < scr_w) fb[py * scr_w + px] = s->pal[idx];
				}
			}
		}
	}
}

static void text_center(int cx, int y, const char *s, VMUINT16 color, int maxw) {
	VMWCHAR w[48];
	int n;
	vm_ascii_to_ucs2(w, sizeof w, (VMSTR)s);
	n = vm_wstrlen(w);
	while (n > 1 && vm_graphic_get_string_width(w) > maxw) { w[--n] = 0; if (n > 2) { w[n - 1] = '.'; } }
	vm_graphic_textout(buf, cx - vm_graphic_get_string_width(w) / 2, y, w, vm_wstrlen(w), color);
}

static int visible_rows(void) { return (scr_h - TOP_H - BOT_H) / CELL_H; }

static void draw_splash(void) {
	int i;
	rect(0, 0, scr_w, scr_h, c_bg);
	for (i = 0; i < scr_h; i += 8) rect(0, i, scr_w, 4, c_bg2);
	draw_sprite(&sprites[0], (scr_w - SPRITE_N * 4) / 2, 40 + ((frame / 3) % 2) * 4, 4);
	vm_graphic_set_font(VM_LARGE_FONT);
	text_center(scr_w / 2, 200, "TUNG TUNG TUNG", c_sel, scr_w);
	text_center(scr_w / 2, 226, "SAHUR", c_sel, scr_w);
	vm_graphic_set_font(VM_SMALL_FONT);
	text_center(scr_w / 2, 266, "Brainrot Launcher", c_text, scr_w);
	text_center(scr_w / 2, 290, "press any key", c_dim, scr_w);
}

static void draw(void) {
	int r, c, rows, fh;
	if (!buf) return;
	vm_graphic_set_font(VM_SMALL_FONT);
	if (splash) { draw_splash(); vm_graphic_flush_layer(&layer, 1); return; }
	fh = vm_graphic_get_character_height();
	rect(0, 0, scr_w, scr_h, c_bg);
	for (r = TOP_H; r < scr_h; r += 16) rect(0, r, scr_w, 1, c_bg2);

	rows = visible_rows();
	for (r = 0; r < rows; r++) {
		for (c = 0; c < COLS; c++) {
			int i = (top_row + r) * COLS + c;
			int x = c * CELL_W, y = TOP_H + 4 + r * CELL_H, bounce = 0;
			if (i >= napps) break;
			if (i == sel) {
				vm_graphic_fill_rect(buf, x + 2, y - 2, CELL_W - 4, CELL_H - 2, c_sel, c_sel);
				bounce = (frame / 2) % 2 ? -2 : 0;
			}
			draw_sprite(&sprites[apps[i].sprite], x + (CELL_W - ICON_PX) / 2, y + bounce, ICON_SCALE);
			text_center(x + CELL_W / 2, y + ICON_PX + 2, label_of(&apps[i]), i == sel ? c_bg : c_text, CELL_W - 4);
		}
	}

	// header: character of the selected app
	rect(0, 0, scr_w, TOP_H, c_bar);
	if (napps) text_center(scr_w / 2, (TOP_H - fh) / 2, sprites[apps[sel].sprite].name, c_sel, scr_w - 8);
	else text_center(scr_w / 2, (TOP_H - fh) / 2, "BRAINROT LAUNCHER", c_sel, scr_w - 8);

	// footer
	rect(0, scr_h - BOT_H, scr_w, BOT_H, c_bar);
	text_center(scr_w / 2, scr_h - BOT_H + (BOT_H - fh) / 2, napps ? "Abrir" : "", c_text, 80);
	text_center(scr_w - 24, scr_h - BOT_H + (BOT_H - fh) / 2, "Salir", c_text, 48);
	if (!napps) text_center(scr_w / 2, 140, "No apps in \\Apps", c_text, scr_w);

	// scroll hint
	if ((napps + COLS - 1) / COLS > rows) {
		int total = (napps + COLS - 1) / COLS, track = scr_h - TOP_H - BOT_H - 8;
		int th = track * rows / total, ty = TOP_H + 4 + (track - th) * top_row / (total - rows);
		rect(scr_w - 3, ty, 2, th, c_dim);
	}
	vm_graphic_flush_layer(&layer, 1);
}

/* ---------- input ---------- */

static void move(int d) {
	int rows = visible_rows(), row;
	if (!napps) return;
	sel += d;
	if (sel < 0) sel = 0;
	if (sel >= napps) sel = napps - 1;
	row = sel / COLS;
	if (row < top_row) top_row = row;
	if (row >= top_row + rows) top_row = row - rows + 1;
}

static void launch(void) {
	if (!napps) return;
	vm_start_app(apps[sel].path, 0, 1); // exit first so the app gets all the memory
}

static void handle_key(VMINT event, VMINT key) {
	if (event != VM_KEY_EVENT_DOWN && event != VM_KEY_EVENT_REPEAT) return;
	if (splash) { splash = 0; draw(); return; }
	switch (key) {
	case VM_KEY_LEFT: case VM_KEY_NUM4: move(-1); break;
	case VM_KEY_RIGHT: case VM_KEY_NUM6: move(1); break;
	case VM_KEY_UP: case VM_KEY_NUM2: move(-COLS); break;
	case VM_KEY_DOWN: case VM_KEY_NUM8: move(COLS); break;
	case VM_KEY_OK: case VM_KEY_NUM5: if (event == VM_KEY_EVENT_DOWN) launch(); return;
	case VM_KEY_RIGHT_SOFTKEY: vm_exit_app(); return;
	}
	draw();
}

static void tick(VMINT tid) {
	(void)tid;
	frame++;
	if (splash && frame > 12) splash = 0;
	draw();
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
		if (message == VM_MSG_CREATE) scan_apps();
		if (timer_id < 0) timer_id = vm_create_timer(150, tick);
		draw();
		break;
	case VM_MSG_PAINT:
		draw();
		break;
	case VM_MSG_INACTIVE:
	case VM_MSG_QUIT:
		if (timer_id >= 0) vm_delete_timer(timer_id);
		timer_id = -1;
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1;
		buf = 0;
		break;
	}
}

void vm_main(void) {
	c_bg = VM_COLOR_888_TO_565(38, 24, 58);
	c_bg2 = VM_COLOR_888_TO_565(48, 32, 72);
	c_sel = VM_COLOR_888_TO_565(255, 214, 60);
	c_text = VM_COLOR_888_TO_565(245, 240, 255);
	c_dim = VM_COLOR_888_TO_565(150, 130, 190);
	c_bar = VM_COLOR_888_TO_565(20, 12, 34);
	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_key);
}
