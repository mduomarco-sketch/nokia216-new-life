// Snake for Nokia S30+ (MediaTek MRE), classic green-LCD style.
// D-pad or 2/4/6/8 steer, OK/5 start+pause, right softkey quits.
#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "vmtimer.h"
#include <string.h>

#define CELL 8
#define COLS 28
#define ROWS 33
#define FIELD_X ((240 - COLS * CELL) / 2)
#define FIELD_Y 44
#define MAX_LEN (COLS * ROWS)

#define START_DELAY 170
#define MIN_DELAY 60
#define DELAY_STEP 4

static VMUINT16 c_bg, c_ink, c_dim;

enum { TITLE, PLAY, PAUSE, OVER };
enum { UP, RIGHT, DOWN, LEFT };

static int scr_w = 240, scr_h = 320;
static VMINT layer = -1;
static VMUINT8 *buf = 0;
static VMINT timer_id = -1;
static int delay = START_DELAY;

static int state = TITLE;
static unsigned char occupied[COLS * ROWS];
static unsigned char body_x[MAX_LEN], body_y[MAX_LEN];
static int head, len;               // ring buffer: head is newest segment
static int dir, queued[2], nqueued; // queue lets fast double-taps turn twice
static int food_x, food_y;
static int score, hiscore;
static int new_best;
static unsigned int rng = 1;

/* ---------- helpers ---------- */

static unsigned int rnd(void) {
	rng = rng * 1103515245u + 12345u;
	return rng >> 16;
}

static void itoa_simple(int v, char *out) {
	char tmp[12];
	int n = 0, i = 0;
	if (v == 0) tmp[n++] = '0';
	while (v > 0) { tmp[n++] = '0' + v % 10; v /= 10; }
	while (n) out[i++] = tmp[--n];
	out[i] = 0;
}

static void text(int x, int y, const char *s, VMUINT16 color) {
	VMWCHAR w[64];
	vm_ascii_to_ucs2(w, sizeof(w), (VMSTR)s);
	vm_graphic_textout(buf, x, y, w, vm_wstrlen(w), color);
}

static void text_center(int y, const char *s, VMUINT16 color) {
	VMWCHAR w[64];
	vm_ascii_to_ucs2(w, sizeof(w), (VMSTR)s);
	vm_graphic_textout(buf, (scr_w - vm_graphic_get_string_width(w)) / 2, y, w, vm_wstrlen(w), color);
}

static void rect(int x, int y, int w, int h, VMUINT16 color) {
	vm_graphic_fill_rect(buf, x, y, w, h, color, color);
}

/* ---------- high score on the memory card ---------- */

static void hiscore_path(VMWCHAR *out, int size) {
	char path[16] = "X:\\snake.hi";
	VMINT drive = vm_get_removable_driver();
	if (drive < 0) { out[0] = 0; return; }
	path[0] = (char)drive;
	vm_ascii_to_ucs2(out, size, path);
}

static void load_hiscore(void) {
	VMWCHAR path[32];
	VMFILE f;
	VMUINT n = 0;
	int v = 0;
	hiscore_path(path, sizeof(path));
	if (!path[0]) return;
	f = vm_file_open(path, MODE_READ, TRUE);
	if (f < 0) return;
	if (vm_file_read(f, &v, sizeof(v), &n) >= 0 && n == sizeof(v) && v > 0 && v < 100000)
		hiscore = v;
	vm_file_close(f);
}

static void save_hiscore(void) {
	VMWCHAR path[32];
	VMFILE f;
	VMUINT n = 0;
	hiscore_path(path, sizeof(path));
	if (!path[0]) return;
	f = vm_file_open(path, MODE_CREATE_ALWAYS_WRITE, TRUE);
	if (f < 0) return;
	vm_file_write(f, &hiscore, sizeof(hiscore), &n);
	vm_file_close(f);
}

/* ---------- game logic ---------- */

static void place_food(void) {
	int free_cells = COLS * ROWS - len, pick, i;
	if (free_cells <= 0) return;
	pick = rnd() % free_cells;
	for (i = 0; i < COLS * ROWS; i++) {
		if (occupied[i]) continue;
		if (pick-- == 0) { food_x = i % COLS; food_y = i / COLS; return; }
	}
}

static void set_speed(int ms);

static void new_game(void) {
	int i;
	memset(occupied, 0, sizeof(occupied));
	rng ^= (unsigned int)vm_get_tick_count();
	len = 4;
	head = len - 1;
	for (i = 0; i < len; i++) {
		body_x[i] = COLS / 2 - (len - 1) + i;
		body_y[i] = ROWS / 2;
		occupied[body_y[i] * COLS + body_x[i]] = 1;
	}
	dir = RIGHT;
	nqueued = 0;
	score = 0;
	new_best = 0;
	place_food();
	state = PLAY;
	set_speed(START_DELAY);
}

static void game_over(void) {
	state = OVER;
	if (score > hiscore) {
		hiscore = score;
		new_best = 1;
		save_hiscore();
	}
}

static void step(void) {
	int hx = body_x[head], hy = body_y[head];
	int tail = (head - len + 1 + MAX_LEN) % MAX_LEN;
	int grow;

	if (nqueued) {
		dir = queued[0];
		queued[0] = queued[1];
		nqueued--;
	}
	switch (dir) {
	case UP: hy--; break;
	case DOWN: hy++; break;
	case LEFT: hx--; break;
	case RIGHT: hx++; break;
	}
	if (hx < 0 || hy < 0 || hx >= COLS || hy >= ROWS) { game_over(); return; }

	grow = (hx == food_x && hy == food_y);
	// The tail moves out of the way this tick, so stepping onto it is legal.
	if (!grow) occupied[body_y[tail] * COLS + body_x[tail]] = 0;
	if (occupied[hy * COLS + hx]) { game_over(); return; }

	head = (head + 1) % MAX_LEN;
	body_x[head] = hx;
	body_y[head] = hy;
	occupied[hy * COLS + hx] = 1;

	if (grow) {
		len++;
		score += 1 + (START_DELAY - delay) / 20; // faster snake, juicier food
		if (len >= MAX_LEN) { game_over(); return; }
		place_food();
		if (delay > MIN_DELAY) set_speed(delay - DELAY_STEP);
	}
}

static void turn(int d) {
	int last = nqueued ? queued[nqueued - 1] : dir;
	if (nqueued == 2 || d == last || (d + 2) % 4 == last) return;
	queued[nqueued++] = d;
}

/* ---------- drawing ---------- */

static void draw_cell(int cx, int cy, VMUINT16 color) {
	rect(FIELD_X + cx * CELL, FIELD_Y + cy * CELL, CELL - 1, CELL - 1, color);
}

static void draw_food(void) {
	int x = FIELD_X + food_x * CELL, y = FIELD_Y + food_y * CELL;
	// little diamond, like the 3310 one
	rect(x + 3, y, 1, 7, c_ink);
	rect(x + 2, y + 1, 3, 5, c_ink);
	rect(x + 1, y + 2, 5, 3, c_ink);
	rect(x, y + 3, 7, 1, c_ink);
}

static void draw_snake(void) {
	int i, idx;
	for (i = 0; i < len; i++) {
		idx = (head - i + MAX_LEN) % MAX_LEN;
		draw_cell(body_x[idx], body_y[idx], c_ink);
	}
	// eyes on the head
	idx = head;
	rect(FIELD_X + body_x[idx] * CELL + 2, FIELD_Y + body_y[idx] * CELL + 2, 3, 3, c_bg);
}

static void draw_hud(void) {
	char num[12], line[32];
	vm_graphic_set_font(VM_MEDIUM_FONT);
	itoa_simple(score, num);
	text(8, 10, num, c_ink);
	strcpy(line, "HI ");
	itoa_simple(hiscore, num);
	strcat(line, num);
	{
		VMWCHAR w[32];
		vm_ascii_to_ucs2(w, sizeof(w), line);
		vm_graphic_textout(buf, scr_w - 8 - vm_graphic_get_string_width(w), 10, w, vm_wstrlen(w), c_ink);
	}
}

static void draw_border(void) {
	int x0 = FIELD_X - 3, y0 = FIELD_Y - 3;
	int w = COLS * CELL + 5, h = ROWS * CELL + 5;
	rect(x0, y0, w, 2, c_ink);
	rect(x0, y0 + h - 2, w, 2, c_ink);
	rect(x0, y0, 2, h, c_ink);
	rect(x0 + w - 2, y0, 2, h, c_ink);
}

static void panel(int y, int h) {
	rect(24, y, scr_w - 48, h, c_bg);
	rect(24, y, scr_w - 48, 2, c_ink);
	rect(24, y + h - 2, scr_w - 48, 2, c_ink);
	rect(24, y, 2, h, c_ink);
	rect(scr_w - 26, y, 2, h, c_ink);
}

static void draw_title(void) {
	int x, y;
	char num[12], line[32];
	// big pixel "snake" wiggling across the top as decoration
	for (x = 0; x < 20; x++) {
		y = (x / 4) % 2 ? 1 : 0;
		rect(40 + x * 8, 70 + y * 8, 7, 7, c_ink);
	}
	rect(40 + 19 * 8 + 8, 70 + 8 * ((19 / 4) % 2), 7, 7, c_dim);
	vm_graphic_set_font(VM_LARGE_FONT);
	text_center(110, "SNAKE", c_ink);
	vm_graphic_set_font(VM_MEDIUM_FONT);
	text_center(150, "made for your Nokia 216", c_dim);
	strcpy(line, "Best: ");
	itoa_simple(hiscore, num);
	strcat(line, num);
	text_center(185, line, c_ink);
	vm_graphic_set_font(VM_SMALL_FONT);
	text_center(225, "Steer: D-pad or 2 4 6 8", c_ink);
	text_center(243, "Pause: OK or 5", c_ink);
	text_center(261, "Quit: right softkey", c_ink);
	vm_graphic_set_font(VM_MEDIUM_FONT);
	if ((vm_get_tick_count() / 500) % 2)
		text_center(290, "Press OK to start", c_ink);
}

static void draw(void) {
	char num[12], line[32];
	if (!buf) return;
	rect(0, 0, scr_w, scr_h, c_bg);
	if (state == TITLE) {
		draw_title();
	} else {
		draw_hud();
		draw_border();
		draw_food();
		draw_snake();
		if (state == PAUSE) {
			panel(130, 60);
			vm_graphic_set_font(VM_LARGE_FONT);
			text_center(140, "PAUSED", c_ink);
			vm_graphic_set_font(VM_SMALL_FONT);
			text_center(168, "OK to continue", c_ink);
		} else if (state == OVER) {
			panel(110, 110);
			vm_graphic_set_font(VM_LARGE_FONT);
			text_center(120, "GAME OVER", c_ink);
			vm_graphic_set_font(VM_MEDIUM_FONT);
			strcpy(line, "Score: ");
			itoa_simple(score, num);
			strcat(line, num);
			text_center(152, line, c_ink);
			if (new_best) text_center(172, "New high score!", c_ink);
			vm_graphic_set_font(VM_SMALL_FONT);
			text_center(196, "OK: again   RSK: quit", c_ink);
		}
	}
	vm_graphic_flush_layer(&layer, 1);
}

/* ---------- timer & events ---------- */

static void tick(VMINT tid) {
	if (state == PLAY) step();
	draw();
}

static void set_speed(int ms) {
	delay = ms;
	if (timer_id >= 0) vm_delete_timer(timer_id);
	timer_id = vm_create_timer(state == PLAY ? delay : 250, tick);
}

static void stop_timer(void) {
	if (timer_id >= 0) vm_delete_timer(timer_id);
	timer_id = -1;
}

static void handle_key(VMINT event, VMINT key) {
	if (event != VM_KEY_EVENT_DOWN) return;

	if (key == VM_KEY_RIGHT_SOFTKEY) {
		if (state == PLAY) { state = PAUSE; set_speed(delay); draw(); return; }
		vm_exit_app();
		return;
	}

	switch (state) {
	case TITLE:
	case OVER:
		if (key == VM_KEY_OK || key == VM_KEY_NUM5) new_game();
		break;
	case PAUSE:
		if (key == VM_KEY_OK || key == VM_KEY_NUM5) { state = PLAY; set_speed(delay); }
		break;
	case PLAY:
		switch (key) {
		case VM_KEY_UP: case VM_KEY_NUM2: turn(UP); break;
		case VM_KEY_DOWN: case VM_KEY_NUM8: turn(DOWN); break;
		case VM_KEY_LEFT: case VM_KEY_NUM4: turn(LEFT); break;
		case VM_KEY_RIGHT: case VM_KEY_NUM6: turn(RIGHT); break;
		case VM_KEY_OK: case VM_KEY_NUM5:
			state = PAUSE;
			set_speed(delay);
			break;
		}
		break;
	}
	draw();
}

static void handle_sysevt(VMINT message, VMINT param) {
	switch (message) {
	case VM_MSG_CREATE:
	case VM_MSG_ACTIVE:
		scr_w = vm_graphic_get_screen_width();
		scr_h = vm_graphic_get_screen_height();
		layer = vm_graphic_create_layer(0, 0, scr_w, scr_h, -1);
		buf = vm_graphic_get_layer_buffer(layer);
		vm_graphic_set_clip(0, 0, scr_w, scr_h);
		vm_switch_power_saving_mode(turn_off_mode); // keep backlight on while playing
		if (message == VM_MSG_CREATE) load_hiscore();
		set_speed(delay);
		draw();
		break;
	case VM_MSG_PAINT:
		draw();
		break;
	case VM_MSG_INACTIVE: // incoming call, etc.
		if (state == PLAY) state = PAUSE;
		/* fall through */
	case VM_MSG_QUIT:
		stop_timer();
		vm_switch_power_saving_mode(turn_on_mode);
		if (layer >= 0) vm_graphic_delete_layer(layer);
		layer = -1;
		buf = 0;
		break;
	}
}

void vm_main(void) {
	c_bg = VM_COLOR_888_TO_565(199, 240, 216);
	c_ink = VM_COLOR_888_TO_565(67, 82, 61);
	c_dim = VM_COLOR_888_TO_565(120, 150, 110);
	rng = (unsigned int)vm_get_tick_count() | 1;
	vm_reg_sysevt_callback(handle_sysevt);
	vm_reg_keyboard_callback(handle_key);
}
