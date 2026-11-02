// Minimal QR Code encoder for the VivaGym check-in app.
// Byte mode, error correction level M, versions 1-10 (up to 213 bytes).
#pragma once

#define QR_MAX_VERSION 10
#define QR_MAX_SIZE (17 + 4 * QR_MAX_VERSION)

typedef struct {
	int version;
	int size;   // modules per side
	int mask;
	unsigned char m[QR_MAX_SIZE][QR_MAX_SIZE]; // [y][x], 1 = dark
} qr_t;

// Encodes len bytes of data. force_mask = -1 picks the best mask (normal use),
// 0..7 forces one (tests). Returns the version, or 0 if the data is too long.
int qr_encode(const unsigned char *data, int len, int force_mask, qr_t *out);
