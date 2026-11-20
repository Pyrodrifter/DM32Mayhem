// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signal generator (docs/spec/sstv.md's PCM path): tone, linear sweep, two-tone and noise as 8 kHz PCM for the FM
// transmitter. The board's hal_afsk_t.tx_pcm_start plays it; the APRS RF service keys and unkeys the radio.
#ifndef SIGGEN_H
#define SIGGEN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
	SIGGEN_TONE = 0,
	SIGGEN_SWEEP,
	SIGGEN_TWOTONE,
	SIGGEN_NOISE,
};

enum {
	SIGGEN_SINE = 0,
	SIGGEN_SQUARE,
	SIGGEN_TRIANGLE,
};

// f1 for the tone, sweep start and the first two-tone; f2 for the sweep end and the second two-tone. ms is the length.
bool siggen_begin(uint8_t mode, uint8_t wave, uint16_t f1, uint16_t f2, uint32_t ms);
// The HAL's PCM fill: full-scale 8 kHz samples, 0 at the end
size_t siggen_read(void *ctx, int16_t *out, size_t max);

#endif
