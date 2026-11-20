// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SSTV transmit (sstv_tx.h). The picture is a 4 bpp palette image in flash; the generator walks the Martin line at
// 8 kHz and the board's PCM path sends it through the FM modulator. Integer only.
#include "config.h"
#if CONFIG_SSTV_TX
#include "sstv/sstv_tx.h"
#include "sstv/sstv_rx.h"
#include "aprs/afsk.h"

#define US_PER_SAMPLE	125u		// 8 kHz
#define VIS_LEADER_US	300000u
#define VIS_BREAK_US	10000u
#define VIS_BIT_US	30000u
#define VIS_STOP_US	30000u

#define TONE_LEADER	1900u
#define TONE_SYNC	1200u
#define TONE_PORCH	1500u
#define TONE_BIT0	1100u
#define TONE_BIT1	1300u
#define PIXEL_LO	1500u
#define PIXEL_HI	2300u
#define PHASE_INC	536871u		// 2^32 / 8000

typedef struct {
	uint8_t vis;
	uint8_t mode;
	uint16_t w, h;
	uint32_t sync, porch, sep, scan;	// us
} sstv_tx_mode_t;

static const sstv_tx_mode_t modes[] = {
	{ 44, SSTV_MODE_MARTIN_M1, 320, 256, 4862u, 572u, 572u, 146432u },
	{ 40, SSTV_MODE_MARTIN_M2, 320, 256, 4862u, 286u, 286u, 73216u },
};

enum { ST_LEADER1, ST_BREAK, ST_LEADER2, ST_BITS, ST_STOP, ST_SYNC, ST_PORCH, ST_SCAN, ST_SEP, ST_DONE };

static struct {
	const hal_image_t *img;
	const sstv_tx_mode_t *m;
	uint8_t state, vis, bit, seg;
	uint16_t line;
	uint32_t elapsed, phase;
} tx;

static const sstv_tx_mode_t *mode_for(uint8_t mode)
{
	for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
		if (modes[i].mode == mode) {
			return &modes[i];
		}
	}
	return NULL;
}

bool sstv_tx_begin(const hal_image_t *image, uint8_t mode)
{
	const sstv_tx_mode_t *m = mode_for(mode);
	if (image == NULL || m == NULL || image->width != m->w || image->height != m->h) {
		return false;
	}
	tx.img = image;
	tx.m = m;
	tx.state = ST_LEADER1;
	tx.elapsed = 0;
	tx.phase = 0;
	tx.line = 0;
	tx.seg = 0;
	tx.bit = 0;
	uint8_t parity = 0;
	for (int i = 0; i < 7; i++) {
		parity ^= (uint8_t)((m->vis >> i) & 1u);
	}
	tx.vis = (uint8_t)((m->vis & 0x7Fu) | (uint8_t)(parity << 7));
	return true;
}

uint32_t sstv_tx_ms(uint8_t mode)
{
	const sstv_tx_mode_t *m = mode_for(mode);
	if (m == NULL) {
		return 0;
	}
	uint64_t us = VIS_LEADER_US + VIS_BREAK_US + VIS_LEADER_US + 8u * VIS_BIT_US + VIS_STOP_US;
	us += (uint64_t)m->h * (m->sync + m->porch + 3u * (m->scan + m->sep));
	return (uint32_t)((us + 999u) / 1000u);
}

// Martin sends green, then blue, then red; 1500 Hz is 0, 2300 Hz is 255
static uint8_t component(uint16_t x, uint16_t y, uint8_t seg)
{
	const hal_image_t *im = tx.img;
	uint8_t pair = im->bits[y * ((im->width + 1u) / 2u) + (x >> 1)];
	uint8_t idx = (x & 1u) ? (pair & 0x0Fu) : (pair >> 4);
	uint16_t c = im->palette[idx];
	if (seg == 0) {
		uint8_t g = (uint8_t)((c >> 5) & 0x3Fu);
		return (uint8_t)((g * 255u + 31u) / 63u);
	}
	if (seg == 1) {
		uint8_t b = (uint8_t)(c & 0x1Fu);
		return (uint8_t)((b * 255u + 15u) / 31u);
	}
	uint8_t r = (uint8_t)((c >> 11) & 0x1Fu);
	return (uint8_t)((r * 255u + 15u) / 31u);
}

static uint32_t state_duration(void)
{
	switch (tx.state) {
	case ST_LEADER1:
	case ST_LEADER2: return VIS_LEADER_US;
	case ST_BREAK: return VIS_BREAK_US;
	case ST_BITS: return VIS_BIT_US;
	case ST_STOP: return VIS_STOP_US;
	case ST_SYNC: return tx.m->sync;
	case ST_PORCH: return tx.m->porch;
	case ST_SCAN: return tx.m->scan;
	case ST_SEP: return tx.m->sep;
	default: return 0;
	}
}

static uint16_t state_tone(void)
{
	switch (tx.state) {
	case ST_LEADER1:
	case ST_LEADER2: return TONE_LEADER;
	case ST_BREAK:
	case ST_STOP:
	case ST_SYNC: return TONE_SYNC;
	case ST_PORCH:
	case ST_SEP: return TONE_PORCH;
	case ST_BITS: return ((tx.vis >> tx.bit) & 1u) ? TONE_BIT1 : TONE_BIT0;
	case ST_SCAN: {
		uint32_t x = (tx.elapsed * tx.m->w) / tx.m->scan;
		if (x >= tx.m->w) {
			x = tx.m->w - 1u;
		}
		uint8_t v = component((uint16_t)x, tx.line, tx.seg);
		return (uint16_t)(PIXEL_LO + (v * (PIXEL_HI - PIXEL_LO)) / 255u);
	}
	default:
		return TONE_SYNC;
	}
}

static void next_state(void)
{
	switch (tx.state) {
	case ST_LEADER1: tx.state = ST_BREAK; break;
	case ST_BREAK: tx.state = ST_LEADER2; break;
	case ST_LEADER2: tx.state = ST_BITS; tx.bit = 0; break;
	case ST_BITS:
		if (++tx.bit >= 8u) {
			tx.state = ST_STOP;
			tx.bit = 0;
		}
		break;
	case ST_STOP:
		tx.state = ST_SYNC;
		tx.line = 0;
		tx.seg = 0;
		break;
	case ST_SYNC: tx.state = ST_PORCH; break;
	case ST_PORCH: tx.state = ST_SCAN; tx.seg = 0; break;
	case ST_SCAN: tx.seg++; tx.state = ST_SEP; break;
	case ST_SEP:
		if (tx.seg >= 3u) {
			tx.line++;
			tx.seg = 0;
			tx.state = (tx.line >= tx.m->h) ? ST_DONE : ST_SYNC;
		} else {
			tx.state = ST_SCAN;
		}
		break;
	default: tx.state = ST_DONE; break;
	}
}

static void advance(void)
{
	while (tx.state != ST_DONE) {
		uint32_t d = state_duration();
		if (tx.elapsed < d) {
			return;
		}
		tx.elapsed -= d;
		next_state();
	}
}

size_t sstv_tx_read(void *ctx, int16_t *out, size_t max)
{
	(void)ctx;
	size_t n = 0;
	while (n < max && tx.state != ST_DONE) {
		uint16_t f = state_tone();
		out[n++] = afsk_sine(tx.phase);
		tx.phase += (uint32_t)f * PHASE_INC;
		tx.elapsed += US_PER_SAMPLE;
		advance();
	}
	return n;
}
#endif
