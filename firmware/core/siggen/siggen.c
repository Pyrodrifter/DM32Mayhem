// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signal generator (siggen.h). Integer only; the FM audio path is 8 kHz.
#include "config.h"
#if CONFIG_SIGGEN
#include "siggen/siggen.h"
#include "aprs/afsk.h"

#define RATE		8000u
#define PHASE_INC	536871u		// 2^32 / 8000
#define F_MIN		20u
#define F_MAX		3000u

static struct {
	uint8_t mode, wave;
	uint16_t f1, f2;
	uint32_t total, sample;
	uint32_t p1, p2;
	uint32_t rng;
} sg;

static uint16_t clamp_f(uint32_t f)
{
	return (uint16_t)(f < F_MIN ? F_MIN : f > F_MAX ? F_MAX : f);
}

bool siggen_begin(uint8_t mode, uint8_t wave, uint16_t f1, uint16_t f2, uint32_t ms)
{
	if (ms == 0 || mode > SIGGEN_NOISE || wave > SIGGEN_TRIANGLE) {
		return false;
	}
	sg.mode = mode;
	sg.wave = wave;
	sg.f1 = clamp_f(f1);
	sg.f2 = clamp_f(f2);
	sg.total = ms * (RATE / 1000u);
	sg.sample = 0;
	sg.p1 = 0;
	sg.p2 = 0;
	sg.rng = 0x2545F491u;
	return sg.total != 0;
}

static int16_t wave_sample(uint32_t phase)
{
	if (sg.wave == SIGGEN_SINE) {
		return afsk_sine(phase);
	}
	if (sg.wave == SIGGEN_SQUARE) {
		return (phase & 0x80000000u) ? 32767 : -32767;
	}
	int32_t p = (int32_t)(phase >> 16);	// 0..65535: -full, +full, -full
	int32_t t = p < 32768 ? (p * 2 - 32768) : (98303 - p * 2);
	return (int16_t)t;
}

size_t siggen_read(void *ctx, int16_t *out, size_t max)
{
	(void)ctx;
	size_t n = 0;
	while (n < max && sg.sample < sg.total) {
		int16_t v;
		switch (sg.mode) {
		case SIGGEN_TONE:
			v = wave_sample(sg.p1);
			sg.p1 += (uint32_t)sg.f1 * PHASE_INC;
			break;
		case SIGGEN_SWEEP: {
			int32_t f = (int32_t)sg.f1 +
				(int32_t)((int32_t)sg.f2 - (int32_t)sg.f1) * (int32_t)sg.sample / (int32_t)sg.total;
			v = wave_sample(sg.p1);
			sg.p1 += (uint32_t)f * PHASE_INC;
			break;
		}
		case SIGGEN_TWOTONE:
			v = (int16_t)(((int32_t)afsk_sine(sg.p1) + afsk_sine(sg.p2)) / 2);
			sg.p1 += (uint32_t)sg.f1 * PHASE_INC;
			sg.p2 += (uint32_t)sg.f2 * PHASE_INC;
			break;
		default:
			sg.rng ^= sg.rng << 13;
			sg.rng ^= sg.rng >> 17;
			sg.rng ^= sg.rng << 5;
			v = (int16_t)(sg.rng >> 16);
			break;
		}
		out[n++] = v;
		sg.sample++;
	}
	return n;
}
#endif
