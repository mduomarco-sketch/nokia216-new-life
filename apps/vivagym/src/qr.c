// Minimal QR Code encoder (ISO/IEC 18004): byte mode, ECC level M, versions 1-10.
// Structure follows Project Nayuki's QR Code generator (MIT), trimmed to what the
// check-in app needs and written without dynamic memory for MRE.
#include "qr.h"
#include <string.h>

// ECC level M, index = version
static const unsigned char ECC_PER_BLOCK[QR_MAX_VERSION + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const unsigned char NUM_BLOCKS[QR_MAX_VERSION + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
#define FORMAT_ECC_M 0

static unsigned char isfn[QR_MAX_SIZE][QR_MAX_SIZE];
static unsigned char codewords[400];     // final interleaved stream (v10 = 346)
static unsigned char dat[400];           // data codewords before ECC
static unsigned char blocks[8][200];

static int raw_modules(int ver) {
	int r = (16 * ver + 128) * ver + 64;
	if (ver >= 2) {
		int na = ver / 7 + 2;
		r -= (25 * na - 10) * na - 55;
		if (ver >= 7) r -= 36;
	}
	return r;
}

static int data_codewords(int ver) {
	return raw_modules(ver) / 8 - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver];
}

/* ---------- Reed-Solomon over GF(2^8), poly 0x11D ---------- */

static unsigned char gf_mul(unsigned char x, unsigned char y) {
	int z = 0, i;
	for (i = 7; i >= 0; i--) {
		z = (z << 1) ^ ((z >> 7) * 0x11D);
		z ^= ((y >> i) & 1) * x;
	}
	return (unsigned char)z;
}

static void rs_divisor(int degree, unsigned char *res) {
	unsigned char root = 1;
	int i, j;
	memset(res, 0, degree);
	res[degree - 1] = 1;
	for (i = 0; i < degree; i++) {
		for (j = 0; j < degree; j++) {
			res[j] = gf_mul(res[j], root);
			if (j + 1 < degree) res[j] ^= res[j + 1];
		}
		root = gf_mul(root, 0x02);
	}
}

static void rs_remainder(const unsigned char *data, int len, const unsigned char *div, int degree, unsigned char *res) {
	int i, j;
	memset(res, 0, degree);
	for (i = 0; i < len; i++) {
		unsigned char f = data[i] ^ res[0];
		memmove(res, res + 1, degree - 1);
		res[degree - 1] = 0;
		for (j = 0; j < degree; j++) res[j] ^= gf_mul(div[j], f);
	}
}

/* ---------- module helpers ---------- */

static qr_t *Q;

static void setfn(int x, int y, int dark) {
	Q->m[y][x] = (unsigned char)(dark != 0);
	isfn[y][x] = 1;
}

static void finder(int cx, int cy) {
	int dx, dy;
	for (dy = -4; dy <= 4; dy++)
		for (dx = -4; dx <= 4; dx++) {
			int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, d = ax > ay ? ax : ay;
			int x = cx + dx, y = cy + dy;
			if (x >= 0 && x < Q->size && y >= 0 && y < Q->size) setfn(x, y, d != 2 && d != 4);
		}
}

static void alignment(int cx, int cy) {
	int dx, dy;
	for (dy = -2; dy <= 2; dy++)
		for (dx = -2; dx <= 2; dx++) {
			int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
			setfn(cx + dx, cy + dy, (ax > ay ? ax : ay) != 1);
		}
}

static void draw_format(int mask) {
	int data = (FORMAT_ECC_M << 3) | mask, rem = data, bits, i, s = Q->size;
	for (i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	bits = ((data << 10) | rem) ^ 0x5412;
	for (i = 0; i <= 5; i++) setfn(8, i, (bits >> i) & 1);
	setfn(8, 7, (bits >> 6) & 1);
	setfn(8, 8, (bits >> 7) & 1);
	setfn(7, 8, (bits >> 8) & 1);
	for (i = 9; i < 15; i++) setfn(14 - i, 8, (bits >> i) & 1);
	for (i = 0; i < 8; i++) setfn(s - 1 - i, 8, (bits >> i) & 1);
	for (i = 8; i < 15; i++) setfn(8, s - 15 + i, (bits >> i) & 1);
	setfn(8, s - 8, 1);
}

static void draw_version(void) {
	int rem = Q->version, bits, i;
	if (Q->version < 7) return;
	for (i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
	bits = (Q->version << 12) | rem;
	for (i = 0; i < 18; i++) {
		int bit = (bits >> i) & 1, a = Q->size - 11 + i % 3, b = i / 3;
		setfn(a, b, bit);
		setfn(b, a, bit);
	}
}

static void draw_function_patterns(void) {
	int s = Q->size, i, j, n = 0, pos[7];
	for (i = 0; i < s; i++) { setfn(6, i, i % 2 == 0); setfn(i, 6, i % 2 == 0); }
	finder(3, 3);
	finder(s - 4, 3);
	finder(3, s - 4);
	if (Q->version > 1) {
		int na = Q->version / 7 + 2;
		int step = (Q->version * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
		int p = s - 7;
		n = na;
		pos[0] = 6;
		for (i = na - 1; i >= 1; i--, p -= step) pos[i] = p;
		for (i = 0; i < n; i++)
			for (j = 0; j < n; j++) {
				if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
				alignment(pos[i], pos[j]);
			}
	}
	draw_format(0); // reserve; real bits drawn after masking
	draw_version();
}

static void draw_codewords(const unsigned char *cw, int len) {
	int s = Q->size, i = 0, right, vert, j;
	for (right = s - 1; right >= 1; right -= 2) {
		if (right == 6) right = 5;
		for (vert = 0; vert < s; vert++)
			for (j = 0; j < 2; j++) {
				int x = right - j, up = ((right + 1) & 2) == 0, y = up ? s - 1 - vert : vert;
				if (!isfn[y][x] && i < len * 8) {
					Q->m[y][x] = (cw[i >> 3] >> (7 - (i & 7))) & 1;
					i++;
				}
			}
	}
}

static void apply_mask(int mask) {
	int x, y, s = Q->size;
	for (y = 0; y < s; y++)
		for (x = 0; x < s; x++) {
			int inv;
			if (isfn[y][x]) continue;
			switch (mask) {
			case 0: inv = (x + y) % 2 == 0; break;
			case 1: inv = y % 2 == 0; break;
			case 2: inv = x % 3 == 0; break;
			case 3: inv = (x + y) % 3 == 0; break;
			case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
			case 5: inv = x * y % 2 + x * y % 3 == 0; break;
			case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
			default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
			}
			Q->m[y][x] ^= (unsigned char)inv;
		}
}

/* ---------- penalty (mask choice) ---------- */

static int module_at(int x, int y, int transpose) { return transpose ? Q->m[x][y] : Q->m[y][x]; }

static int penalty(void) {
	int s = Q->size, p = 0, x, y, t, dark = 0, total, k;
	for (t = 0; t < 2; t++)
		for (y = 0; y < s; y++) {
			int run = 1;
			for (x = 1; x <= s; x++) {
				if (x < s && module_at(x, y, t) == module_at(x - 1, y, t)) { run++; continue; }
				if (run >= 5) p += 3 + (run - 5);
				run = 1;
			}
			// finder-like 1:1:3:1:1 with 4 light modules on one side
			for (x = 0; x + 10 < s; x++) {
				static const unsigned char a[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
				int ok1 = 1, ok2 = 1, i;
				for (i = 0; i < 11; i++) {
					if (module_at(x + i, y, t) != a[i]) ok1 = 0;
					if (module_at(x + i, y, t) != a[10 - i]) ok2 = 0;
				}
				p += 40 * (ok1 + ok2);
			}
		}
	for (y = 0; y + 1 < s; y++)
		for (x = 0; x + 1 < s; x++) {
			int c = Q->m[y][x];
			if (c == Q->m[y][x + 1] && c == Q->m[y + 1][x] && c == Q->m[y + 1][x + 1]) p += 3;
		}
	for (y = 0; y < s; y++) for (x = 0; x < s; x++) dark += Q->m[y][x];
	total = s * s;
	k = ((dark * 20 - total * 10 < 0 ? total * 10 - dark * 20 : dark * 20 - total * 10) + total - 1) / total - 1;
	p += k * 10;
	return p;
}

/* ---------- encode ---------- */

int qr_encode(const unsigned char *data, int len, int force_mask, qr_t *out) {
	int ver, cap = 0, bits = 0, i, j, k;
	int nb, ecc, raw, nshort, shortlen, ndc;
	unsigned char div[32], *b;

	for (ver = 1; ver <= QR_MAX_VERSION; ver++) {
		int need = 4 + (ver <= 9 ? 8 : 16) + 8 * len;
		cap = data_codewords(ver) * 8;
		if (need <= cap) break;
	}
	if (ver > QR_MAX_VERSION || len <= 0) return 0;

	/* bit stream */
	ndc = data_codewords(ver);
	memset(dat, 0, sizeof dat);
#define PUT(v, n) do { int q_; for (q_ = (n) - 1; q_ >= 0; q_--) { if (((v) >> q_) & 1) dat[bits >> 3] |= 0x80 >> (bits & 7); bits++; } } while (0)
	PUT(4, 4);
	PUT(len, ver <= 9 ? 8 : 16);
	for (i = 0; i < len; i++) PUT(data[i], 8);
	k = cap - bits; if (k > 4) k = 4;
	bits += k;                      // terminator (zeros)
	bits = (bits + 7) & ~7;         // byte align
	for (i = bits / 8, j = 0; i < ndc; i++, j ^= 1) dat[i] = j ? 0x11 : 0xEC;
#undef PUT

	/* ECC blocks + interleave */
	nb = NUM_BLOCKS[ver];
	ecc = ECC_PER_BLOCK[ver];
	raw = raw_modules(ver) / 8;
	nshort = nb - raw % nb;
	shortlen = raw / nb;
	rs_divisor(ecc, div);
	for (i = 0, k = 0; i < nb; i++) {
		int dl = shortlen - ecc + (i < nshort ? 0 : 1);
		b = blocks[i];
		memcpy(b, dat + k, dl);
		k += dl;
		rs_remainder(b, dl, div, ecc, b + shortlen + 1 - ecc);
		// short blocks: data [0,dl), unused slot at dl (skipped when interleaving), ECC at [shortlen+1-ecc, shortlen+1)
		if (i < nshort) b[dl] = 0;
	}
	for (i = 0, k = 0; i <= shortlen; i++)
		for (j = 0; j < nb; j++)
			if (i != shortlen - ecc || j >= nshort) codewords[k++] = blocks[j][i];

	/* modules */
	Q = out;
	memset(out, 0, sizeof *out);
	memset(isfn, 0, sizeof isfn);
	out->version = ver;
	out->size = ver * 4 + 17;
	draw_function_patterns();
	draw_codewords(codewords, raw);

	if (force_mask < 0) {
		int best = -1, bestp = 0x7FFFFFFF, m;
		for (m = 0; m < 8; m++) {
			int p;
			apply_mask(m);
			draw_format(m);
			p = penalty();
			if (p < bestp) { bestp = p; best = m; }
			apply_mask(m); // undo (XOR)
		}
		force_mask = best;
	}
	apply_mask(force_mask);
	draw_format(force_mask);
	out->mask = force_mask;
	return ver;
}
