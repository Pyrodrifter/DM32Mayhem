// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SSTV receive (sstv_rx.h). The discriminator audio is treated as an FM signal: the instantaneous frequency is the
// SSTV tone. A 1900 Hz complex NCO, a one-pole low pass and the phase step give the tone per sample. The VIS header
// names the mode; each line's sync starts its timing, and a pixel is 1500..2300 Hz (black..white).
#include <math.h>
#include <string.h>
#include "sstv/sstv_rx.h"

// Tones in Hz
#define TONE_LEADER	1900.0f
#define TONE_BREAK	1200.0f
#define TONE_PORCH	1500.0f
#define TONE_BIT0	1100.0f
#define TONE_BIT1	1300.0f
#define PIXEL_LO	1500.0f
#define PIXEL_HI	2300.0f

// VIS timings, us
#define VIS_LEADER_US	300000u
#define VIS_BREAK_US	10000u
#define VIS_BIT_US	30000u
#define VIS_BITS	8u		// 7 data bits, then even parity

// The tone bins the Goertzel estimator measures, with the 1200 Hz sync and the VIS tones included
static const float sstv_gfreq[SSTV_GOERTZEL_BINS] = {
	1100.0f, 1200.0f, 1300.0f, 1500.0f, 1700.0f, 1900.0f, 2100.0f, 2300.0f
};

// A mode's line, us. Martin and Scottie send Y, Cr, Cb every line; the porch and the separators are 1500 Hz
typedef struct {
	uint8_t vis;
	uint8_t mode;
	uint16_t w, h;
	uint32_t sync, porch, sep, scan;	// us
} sstv_timing_t;

static const sstv_timing_t timings[] = {
	{ 44, SSTV_MODE_MARTIN_M1, 320, 256, 4862u, 572u, 572u, 146432u },
	{ 40, SSTV_MODE_MARTIN_M2, 320, 256, 4862u, 286u, 286u, 73216u },
	{ 60, SSTV_MODE_SCOTTIE_S1, 320, 256, 9000u, 1500u, 1500u, 138240u },
	{ 56, SSTV_MODE_SCOTTIE_S2, 320, 256, 9000u, 1500u, 1500u, 69120u },
};

enum { ST_WAIT_VIS, ST_BREAK, ST_LEADER2, ST_BITS, ST_STOP, ST_SYNC, ST_PORCH, ST_SCAN, ST_SEP };

static uint32_t us_samples(const sstv_rx_t *rx, uint32_t us)
{
	return (uint32_t)(((uint64_t)rx->rate * us) / 1000000u);
}

static uint8_t pixel_value(float hz)
{
	int v = (int)((hz - PIXEL_LO) * 255.0f / (PIXEL_HI - PIXEL_LO));
	if (v < 0) v = 0; else if (v > 255) v = 255;
	return (uint8_t)v;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static const sstv_timing_t *timing_for_mode(uint8_t mode)
{
	for (size_t i = 0; i < sizeof timings / sizeof timings[0]; i++) {
		if (timings[i].mode == mode) {
			return &timings[i];
		}
	}
	return NULL;
}

static const sstv_timing_t *timing_for_vis(uint8_t vis)
{
	for (size_t i = 0; i < sizeof timings / sizeof timings[0]; i++) {
		if (timings[i].vis == vis) {
			return &timings[i];
		}
	}
	return NULL;
}

static void finish_line(sstv_rx_t *rx)
{
	if (rx->pixel != NULL && (rx->have & 0x07) == 0x07) {
		for (uint16_t x = 0; x < SSTV_LINE_MAX; x++) {
			rx->pixel(rx->ctx, x, rx->line, rgb565(rx->rr[x], rx->gg[x], rx->bb[x]));
		}
	}
	if (rx->line_cb != NULL) {
		rx->line_cb(rx->ctx, rx->line, rx->lines);
	}
	rx->have = 0;
}

// The VIS named a mode: begin the first line, or wait for the next header
static void vis_done(sstv_rx_t *rx)
{
	const sstv_timing_t *t = timing_for_vis((uint8_t)(rx->vis_bits & 0x7F));
	if (t == NULL) {
		rx->state = ST_WAIT_VIS;
		rx->sample = 0;
		return;
	}
	rx->mode = t->mode;
	rx->lines = t->h;
	rx->line = 0;
	rx->have = 0;
	rx->state = ST_SYNC;
	rx->sync_n = 0;
	rx->sample = 0;
	if (rx->on_mode != NULL) {
		rx->on_mode(rx->ctx, t->vis, t->mode, t->w, t->h);
	}
}

static void feed_one(sstv_rx_t *rx, int16_t sample)
{
	// Goertzel tone estimate over about 2 ms (eight bins across the SSTV tones). The PortaPack Mayhem SSTV receiver
	// uses the same idea, and it is far steadier than counting zero crossings on the radio's 8 kHz FM audio.
	float sv = (float)sample / 32768.0f;
	for (int f = 0; f < SSTV_GOERTZEL_BINS; f++) {
		float q0 = rx->gcoef[f] * rx->gq1[f] - rx->gq2[f] + sv;
		rx->gq2[f] = rx->gq1[f];
		rx->gq1[f] = q0;
	}
	if (++rx->gcount >= rx->gn) {
		float mag[SSTV_GOERTZEL_BINS];
		int mi = 0;
		for (int f = 0; f < SSTV_GOERTZEL_BINS; f++) {
			mag[f] = rx->gq1[f] * rx->gq1[f] + rx->gq2[f] * rx->gq2[f] -
				 rx->gq1[f] * rx->gq2[f] * rx->gcoef[f];
			rx->gmag[f] = mag[f];
			rx->gq1[f] = 0;
			rx->gq2[f] = 0;
			if (mag[f] > mag[mi]) {
				mi = f;
			}
		}
		float hz = sstv_gfreq[mi];
		if (mi > 0 && mag[mi] > 0.0f) {	// interpolate with the neighbours: finer than the bin spacing
			float r = mag[mi - 1] / mag[mi];
			if (r > 0.2f) {
				hz -= (sstv_gfreq[mi] - sstv_gfreq[mi - 1]) * r * 0.5f;
			}
		}
		if (mi < SSTV_GOERTZEL_BINS - 1 && mag[mi] > 0.0f) {
			float r = mag[mi + 1] / mag[mi];
			if (r > 0.2f) {
				hz += (sstv_gfreq[mi + 1] - sstv_gfreq[mi]) * r * 0.5f;
			}
		}
		rx->hz = hz;
		rx->gcount = 0;
	}
	// A fast tone from the band-passed zero crossings: precise edges for the sync and the state machine. The
	// Goertzel above is steadier for the pixels but lags a block, which made every line start at a different x.
	float sv2 = (float)sample;
	rx->dc += 0.005f * (sv2 - rx->dc);
	float x = sv2 - rx->dc;
	float y = 0.2915f * x - 0.2915f * rx->bp_x2 - (-0.2225f) * rx->bp_y1 - 0.4169f * rx->bp_y2;
	rx->bp_x2 = rx->bp_x1;
	rx->bp_x1 = x;
	rx->bp_y2 = rx->bp_y1;
	rx->bp_y1 = y;
	const float th = 150.0f;
	bool cross = false;
	float frac = 0.0f;
	if (rx->zc_state <= 0 && y >= th) {
		float denom = y - rx->prev;
		frac = denom != 0.0f ? (th - rx->prev) / denom : 0.0f;
		rx->zc_state = 1;
		cross = true;
	} else if (rx->zc_state >= 0 && y <= -th) {
		float denom = y - rx->prev;
		frac = denom != 0.0f ? (-th - rx->prev) / denom : 0.0f;
		rx->zc_state = -1;
		cross = true;
	}
	if (cross) {
		if (frac < 0.0f) frac = 0.0f; else if (frac > 1.0f) frac = 1.0f;
		float pos = (float)(rx->total - 1u) + frac;
		if (rx->cross_ok) {
			float spacing = pos - rx->last_cross;
			if (spacing > 0.4f && spacing < 40.0f) {
				float f = (float)rx->rate / (2.0f * spacing);
				if (f > 300.0f && f < 3200.0f) {
					rx->hz_hist[2] = rx->hz_hist[1];
					rx->hz_hist[1] = rx->hz_hist[0];
					rx->hz_hist[0] = f;
					float a = rx->hz_hist[0], b = rx->hz_hist[1], c = rx->hz_hist[2];
					float lo = a < b ? a : b, hi = a < b ? b : a;
					rx->hz_fast = lo > c ? lo : (hi < c ? hi : c);	// median of three
				}
			}
		}
		rx->last_cross = pos;
		rx->cross_ok = true;
	}
	rx->prev = y;
	rx->prev_ok = true;
	rx->total++;

	rx->samples++;
	rx->sample++;

	// The sync/porch/VIS states use the fast edge; the pixel scan uses the steadier Goertzel
	float hz = rx->hz - rx->freq_offset;
	uint32_t n = rx->sample;

	switch (rx->state) {
	case ST_WAIT_VIS:
		if (hz > 1700.0f && hz < 2100.0f) {
			if (n >= us_samples(rx, 180000u)) {
				rx->state = ST_BREAK;
				rx->sample = 0;
			}
		} else {
			rx->sample = 0;
		}
		break;
	case ST_BREAK:
		// The break is the 1200 Hz pulse after the first leader. A longer leader (a sender's preamble) is fine:
		// stay here while the tone is still the leader, and go on the break.
		if (hz > 1000.0f && hz < 1400.0f) {
			rx->state = ST_LEADER2;
			rx->armed = false;
			rx->sample = 0;
		} else if (hz < 900.0f || n > us_samples(rx, 300000u)) {
			rx->state = ST_WAIT_VIS;	// a bit tone, or the break never came
			rx->sample = 0;
		}
		// else: the leader or the filter settling into the break, keep waiting
		break;
	case ST_LEADER2:
		// Wait for the second 1900 Hz leader. Once it has run 150 ms, the first VIS bit (1100/1300 Hz) begins when
		// the tone leaves the leader, so the bit timing starts there (the filter's few ms delay is inside a 30 ms bit).
		// A false break (a line sync heard while idle) must not hang here: give up after 600 ms.
		if (hz > 1700.0f && hz < 2100.0f) {
			if (n >= us_samples(rx, 150000u)) {
				rx->armed = true;
			}
		} else if (rx->armed && hz < 1500.0f) {
			rx->state = ST_BITS;
			rx->vis_bits = 0;
			rx->vis_count = 0;
			rx->vis_parity = 0;
			rx->bit_ones = 0;
			rx->sample = 0;
			rx->t0 = 0;
		} else if (n > us_samples(rx, 600000u)) {
			rx->state = ST_WAIT_VIS;
			rx->sample = 0;
		}
		break;
	case ST_BITS: {
		uint32_t per = us_samples(rx, VIS_BIT_US);
		if (per == 0) {
			rx->state = ST_WAIT_VIS;
			break;
		}
		// Integrate over the bit and decide on the majority: a single noisy sample is not trusted
		if (rx->vis_count < VIS_BITS && hz >= 1200.0f) {
			rx->bit_ones++;
		}
		if (n >= rx->t0 + per) {
			if (rx->vis_count < VIS_BITS) {
				uint8_t bit = rx->bit_ones > per / 2 ? 1u : 0u;
				if (rx->vis_count < 7) {
					rx->vis_bits |= (uint8_t)(bit << rx->vis_count);
					rx->vis_parity ^= bit;
				}
				rx->vis_count++;
				rx->bit_ones = 0;
			}
			rx->t0 += per;
			if (rx->vis_count >= VIS_BITS) {
				rx->state = ST_STOP;
				rx->sample = 0;
			}
		}
		break;
	}
	case ST_STOP:
		if (n >= us_samples(rx, VIS_BIT_US)) {
			vis_done(rx);
		}
		break;
	case ST_SYNC:
		// Lock on the end of the 1200 Hz sync pulse (the trailing edge), as the Mayhem receiver does: the porch and
		// the scan then start from a known point. The tone estimate lags a block, so the edge is seen a couple of ms
		// late; a fixed horizontal offset, not a drift.
		if (hz > 1050.0f && hz < 1350.0f) {
			if (rx->sync_n < 0xFFF0u) {
				rx->sync_n++;
				rx->sync_sum += rx->hz;
			}
		} else {
			if (rx->sync_n >= us_samples(rx, 1500u)) {
				float m = rx->sync_sum / (float)rx->sync_n;
				float off = m - 1200.0f;	// a constant tuning/tone offset shows here
				if (off > -250.0f && off < 250.0f) {
					rx->freq_offset = rx->freq_offset * 0.8f + off * 0.2f;
				}
				// Start the tone window at the line boundary, so every line is sampled at the same phase
				rx->gcount = 0;
				for (int f = 0; f < SSTV_GOERTZEL_BINS; f++) {
					rx->gq1[f] = 0;
					rx->gq2[f] = 0;
				}
				rx->state = ST_PORCH;
				rx->sample = 0;
				rx->t0 = 0;
				rx->seg = 0;
				rx->px = 0;
				rx->have = 0;
			}
			rx->sync_n = 0;
			rx->sync_sum = 0;
			if (rx->state == ST_SYNC && n > us_samples(rx, 60000u)) {
				rx->sample = 0;
				if (rx->forced == SSTV_MODE_NONE) {
					rx->state = ST_WAIT_VIS;	// no sync: back to looking for a header
				}
			}
		}
		break;
	case ST_PORCH: {
		const sstv_timing_t *m = timing_for_mode(rx->mode);
		if (m == NULL) {
			rx->state = ST_WAIT_VIS;
			break;
		}
		if (n >= us_samples(rx, m->porch)) {
			rx->state = ST_SCAN;
			rx->sample = 0;
			rx->px = 0;
			rx->t0 = 0;
		}
		break;
	}
	case ST_SCAN: {
		const sstv_timing_t *m = timing_for_mode(rx->mode);
		if (m == NULL) {
			rx->state = ST_WAIT_VIS;
			break;
		}
		uint32_t scan = us_samples(rx, m->scan);
		if (scan == 0) {
			rx->state = ST_WAIT_VIS;
			break;
		}
		while (rx->px < m->w) {
			uint32_t center = (uint32_t)(((uint64_t)rx->px * 2u + 1u) * scan / (2u * m->w));
			if (n < center) {
				break;
			}
			uint8_t v = pixel_value(hz);
			uint16_t x = rx->px;
			if (rx->seg == 0) {		// Martin and Scottie send G, then B, then R
				rx->gg[x] = v;
				rx->have |= 0x01;
			} else if (rx->seg == 1) {
				rx->bb[x] = v;
				rx->have |= 0x02;
			} else {
				rx->rr[x] = v;
				rx->have |= 0x04;
			}
			rx->px++;
		}
		if (n >= scan) {
			rx->state = ST_SEP;
			rx->sample = 0;
			rx->t0 = 0;
			rx->seg++;
		}
		break;
	}
	case ST_SEP: {
		const sstv_timing_t *m = timing_for_mode(rx->mode);
		if (m == NULL) {
			rx->state = ST_WAIT_VIS;
			break;
		}
		if (n >= us_samples(rx, m->sep)) {
			if (rx->seg >= 3) {
				finish_line(rx);
				rx->line++;
				if (rx->line >= rx->lines) {
					rx->frames++;
					if (rx->forced != SSTV_MODE_NONE) {
						const sstv_timing_t *t = timing_for_mode(rx->forced);
						rx->line = 0;
						if (t != NULL && rx->on_mode != NULL) {
							rx->on_mode(rx->ctx, t->vis, rx->forced, t->w, t->h);	// a clear image for the next
						}
						rx->state = ST_SYNC;
						rx->sync_n = 0;
					} else {
						if (rx->on_mode != NULL) {
							rx->on_mode(rx->ctx, 0, SSTV_MODE_NONE, 0, 0);
						}
						rx->mode = SSTV_MODE_NONE;
						rx->state = ST_WAIT_VIS;
					}
					rx->sample = 0;
				} else {
					rx->state = ST_SYNC;
					rx->sync_n = 0;
					rx->sample = 0;
				}
			} else {
				rx->state = ST_SCAN;
				rx->sample = 0;
				rx->px = 0;
				rx->t0 = 0;
			}
		}
		break;
	}
	default:
		rx->state = ST_WAIT_VIS;
		break;
	}
}

bool sstv_rx_begin(sstv_rx_t *rx, uint32_t rate, sstv_pixel_fn pixel, sstv_line_fn line, sstv_mode_fn on_mode,
		   void *ctx)
{
	if (rate < 4000u || rate > 48000u) {
		return false;
	}
	memset(rx, 0, sizeof(*rx));
	rx->rate = rate;
	rx->pixel = pixel;
	rx->line_cb = line;
	rx->on_mode = on_mode;
	rx->ctx = ctx;
	rx->state = ST_WAIT_VIS;
	for (int f = 0; f < SSTV_GOERTZEL_BINS; f++) {
		rx->gcoef[f] = 2.0f * (float)cos(2.0 * 3.141592653589793 * (double)sstv_gfreq[f] / (double)rate);
	}
	rx->gn = (uint8_t)(rate / 250u);	// about 4 ms: steadier on noisy FM audio
	if (rx->gn < 8) {
		rx->gn = 8;
	} else if (rx->gn > 64) {
		rx->gn = 64;
	}
	return true;
}

void sstv_rx_reset(sstv_rx_t *rx)
{
	sstv_rx_begin(rx, rx->rate, rx->pixel, rx->line_cb, rx->on_mode, rx->ctx);
}

void sstv_rx_force_mode(sstv_rx_t *rx, uint8_t mode)
{
	rx->forced = mode;
	rx->sample = 0;
	rx->line = 0;
	rx->have = 0;
	rx->seg = 0;
	rx->px = 0;
	if (mode == SSTV_MODE_NONE) {
		rx->mode = SSTV_MODE_NONE;
		rx->state = ST_WAIT_VIS;
		return;
	}
	const sstv_timing_t *t = timing_for_mode(mode);
	if (t == NULL) {
		rx->mode = SSTV_MODE_NONE;
		rx->state = ST_WAIT_VIS;
		return;
	}
	rx->mode = mode;
	rx->lines = t->h;
	rx->state = ST_SYNC;
	rx->sync_n = 0;
	if (rx->on_mode != NULL) {
		rx->on_mode(rx->ctx, t->vis, mode, t->w, t->h);
	}
}

void sstv_rx_feed(sstv_rx_t *rx, const int16_t *samples, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		feed_one(rx, samples[i]);
	}
}
