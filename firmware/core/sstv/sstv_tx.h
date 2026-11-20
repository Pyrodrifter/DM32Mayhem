// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SSTV transmit (docs/spec/sstv.md): a 320 x 256 palette picture sent as the Martin M1/M2 audio (VIS, sync, porch,
// G/B/R scans). The generator yields 8 kHz signed 16-bit PCM; the board's FM audio path plays it.
#ifndef SSTV_TX_H
#define SSTV_TX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_display.h"

// Choose the picture and mode. False when the mode is not one this transmitter makes or the picture is not 320 x 256.
bool sstv_tx_begin(const hal_image_t *image, uint8_t mode);
// How long the picture takes, ms (the VIS header and every line)
uint32_t sstv_tx_ms(uint8_t mode);
// The HAL's PCM fill (hal_afsk_t.tx_pcm_start): full-scale signed 16-bit, 8 kHz. Returns 0 at the end.
size_t sstv_tx_read(void *ctx, int16_t *out, size_t max);

#endif
