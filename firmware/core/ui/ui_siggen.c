// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signal generator (CONFIG_SIGGEN): tone, sweep, two-tone and noise through the FM transmitter. core/siggen makes
// the PCM; the APRS RF service keys the radio and board.afsk plays it.
#include "config.h"
#if CONFIG_SIGGEN
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "siggen/siggen.h"
#include "aprs/aprs.h"

#define SIGGEN_HZ	144500000u	// a 2 m test frequency; change it on the row

static uint32_t rf_hz = SIGGEN_HZ;
static uint8_t sg_mode = SIGGEN_TONE;
static uint8_t sg_wave = SIGGEN_SINE;
static uint16_t sg_f1 = 1000, sg_f2 = 3000;
static uint8_t len_idx = 2;		// 5 s
static uint8_t level = 80;

enum { EDIT_NONE, EDIT_RF, EDIT_T1, EDIT_T2 };
static uint8_t editing;
static char ed[UI_ENTRY_MAX + 2];
static uint8_t ed_len;

enum { SG_RF, SG_MODE, SG_WAVE, SG_T1, SG_T2, SG_LEN, SG_LEVEL, SG_SEND, SG_STATUS, SG_COUNT };

static const uint32_t lengths[] = { 1000u, 2000u, 5000u, 10000u, 30000u };
static const char *const length_names[] = { "1 s", "2 s", "5 s", "10 s", "30 s" };
static const uint8_t mode_order[] = { SIGGEN_TONE, SIGGEN_SWEEP, SIGGEN_TWOTONE, SIGGEN_NOISE };
static const uint8_t wave_order[] = { SIGGEN_SINE, SIGGEN_SQUARE, SIGGEN_TRIANGLE };

static const char *mode_name(uint8_t m)
{
	switch (m) {
	case SIGGEN_TONE: return "Tone";
	case SIGGEN_SWEEP: return "Sweep";
	case SIGGEN_TWOTONE: return "Two-tone";
	case SIGGEN_NOISE: return "Noise";
	default: return "Tone";
	}
}

static const char *wave_name(uint8_t w)
{
	switch (w) {
	case SIGGEN_SINE: return "Sine";
	case SIGGEN_SQUARE: return "Square";
	case SIGGEN_TRIANGLE: return "Triangle";
	default: return "Sine";
	}
}

static uint8_t next_in(const uint8_t *order, uint8_t n, uint8_t cur)
{
	uint8_t i = 0;
	while (i + 1 < n && order[i] != cur) {
		i++;
	}
	return order[(i + 1) % n];
}

static void rf_digits(uint32_t hz)
{
	ed_len = UI_ENTRY_MAX;
	uint32_t v = hz / 10u;
	for (int i = UI_ENTRY_MAX - 1; i >= 0; i--) {
		ed[i] = (char)('0' + v % 10u);
		v /= 10u;
	}
	ed[UI_ENTRY_MAX] = '\0';
}

static void tone_digits(uint16_t hz)
{
	char tmp[6];
	int n = 0;
	uint32_t v = hz;
	if (v == 0) {
		tmp[n++] = '0';
	} else {
		while (v != 0 && n < (int)sizeof tmp) {
			tmp[n++] = (char)('0' + v % 10u);
			v /= 10u;
		}
	}
	ed_len = (uint8_t)(n > 5 ? 5 : n);
	for (uint8_t i = 0; i < ed_len; i++) {
		ed[i] = tmp[n - 1 - i];
	}
	ed[ed_len] = '\0';
}

static uint32_t parse_digits(void)
{
	uint32_t v = 0;
	for (uint8_t i = 0; i < ed_len; i++) {
		v = v * 10u + (uint32_t)(ed[i] - '0');
	}
	return v;
}

static uint16_t sg_count(void) { return SG_COUNT; }

static void sg_item(uint16_t i, ui_item_t *it)
{
	item_num_set(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case SG_RF:
		str_copy(it->label, sizeof it->label, "Frequency");
		if (editing == EDIT_RF) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, ed_len != 0 ? ed : "type digits");
		} else {
			sb_freq(&b, rf_hz);
		}
		break;
	case SG_MODE:
		str_copy(it->label, sizeof it->label, "Mode");
		sb_str(&b, mode_name(sg_mode));
		break;
	case SG_WAVE:
		str_copy(it->label, sizeof it->label, "Wave");
		if (sg_mode != SIGGEN_TONE && sg_mode != SIGGEN_SWEEP) {
			it->flags |= UI_ITEM_DIM;
		}
		sb_str(&b, wave_name(sg_wave));
		break;
	case SG_T1:
		str_copy(it->label, sizeof it->label, "Tone 1");
		if (sg_mode == SIGGEN_NOISE) {
			it->flags |= UI_ITEM_DIM;
			sb_str(&b, "-");
		} else if (editing == EDIT_T1) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, ed_len != 0 ? ed : "type hz");
		} else {
			sb_u32(&b, sg_f1, 1);
			sb_str(&b, " Hz");
		}
		break;
	case SG_T2:
		str_copy(it->label, sizeof it->label, sg_mode == SIGGEN_SWEEP ? "Sweep to" : "Tone 2");
		if (sg_mode == SIGGEN_TONE || sg_mode == SIGGEN_NOISE) {
			it->flags |= UI_ITEM_DIM;
			sb_str(&b, "-");
		} else if (editing == EDIT_T2) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, ed_len != 0 ? ed : "type hz");
		} else {
			sb_u32(&b, sg_f2, 1);
			sb_str(&b, " Hz");
		}
		break;
	case SG_LEN:
		str_copy(it->label, sizeof it->label, sg_mode == SIGGEN_SWEEP ? "Sweep time" : "Length");
		sb_str(&b, length_names[len_idx]);
		break;
	case SG_LEVEL:
		str_copy(it->label, sizeof it->label, "Level");
		sb_u32(&b, level, 1);
		break;
	case SG_SEND:
		str_copy(it->label, sizeof it->label, "Send");
		sb_str(&b, ui_view()->state == RS_TX ? "Sending" : "Ready");
		break;
	case SG_STATUS:
		str_copy(it->label, sizeof it->label, "Status");
		sb_str(&b, ui_view()->state == RS_TX ? "Sending" : "Ready");
		break;
	default:
		break;
	}
}

static void sg_send(void)
{
	if (ui_view()->state == RS_TX) {
		ui_toast("Sending");
		return;
	}
	uint32_t ms = lengths[len_idx];
	if (!siggen_begin(sg_mode, sg_wave, sg_f1, sg_f2, ms)) {
		ui_toast("Bad settings");
		return;
	}
	if (aprs_queue_pcm(rf_hz, level, ms + 500u, siggen_read, NULL)) {
		ui_toast("Sending");
	} else {
		ui_toast(TR("Busy"));
	}
}

static void sg_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case SG_RF:
		editing = EDIT_RF;
		rf_digits(rf_hz);
		break;
	case SG_MODE:
		sg_mode = next_in(mode_order, (uint8_t)(sizeof mode_order / sizeof mode_order[0]), sg_mode);
		break;
	case SG_WAVE:
		if (sg_mode == SIGGEN_TONE || sg_mode == SIGGEN_SWEEP) {
			sg_wave = next_in(wave_order, (uint8_t)(sizeof wave_order / sizeof wave_order[0]), sg_wave);
		}
		break;
	case SG_T1:
		if (sg_mode != SIGGEN_NOISE) {
			editing = EDIT_T1;
			tone_digits(sg_f1);
		}
		break;
	case SG_T2:
		if (sg_mode == SIGGEN_SWEEP || sg_mode == SIGGEN_TWOTONE) {
			editing = EDIT_T2;
			tone_digits(sg_f2);
		}
		break;
	case SG_LEN:
		len_idx = (uint8_t)((len_idx + 1u) % (sizeof lengths / sizeof lengths[0]));
		break;
	case SG_LEVEL:
		level = (uint8_t)(level >= 100u ? 10u : level + 10u);
		break;
	case SG_SEND:
		sg_send();
		break;
	default:
		break;
	}
	ui_dirty();
}

static bool sg_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	bool repeat = ev->gesture == GESTURE_REPEAT;
	int d = ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;

	if (editing != EDIT_NONE) {
		if (d >= 0 && (press || repeat)) {
			(void)ui_freq_entry_digit(ed, &ed_len, (char)('0' + d));
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_BACK && press) {
			if (ed_len != 0) {
				ed[--ed_len] = '\0';
			} else {
				editing = EDIT_NONE;
			}
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_SELECT && press) {
			if (editing == EDIT_RF) {
				uint32_t f = ui_freq_entry_hz(ed, ed_len);
				if (f != 0) {
					rf_hz = f;
				}
			} else {
				uint32_t v = parse_digits();
				if (v < 20u) {
					v = 20u;
				} else if (v > 3000u) {
					v = 3000u;
				}
				if (editing == EDIT_T1) {
					sg_f1 = (uint16_t)v;
				} else {
					sg_f2 = (uint16_t)v;
				}
			}
			editing = EDIT_NONE;
			ui_dirty();
			return true;
		}
		return true;
	}
	return false;
}

static void sg_enter(void)
{
	editing = EDIT_NONE;
	ui_dirty();
}

const ui_scr_def_t ui_siggen_screen = {
	.name = "siggen", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Signal"),
	.hint = TRN("OK change  Back"), .count = sg_count, .item = sg_item, .select = sg_select,
	.key = sg_key, .enter = sg_enter,
};
#endif
