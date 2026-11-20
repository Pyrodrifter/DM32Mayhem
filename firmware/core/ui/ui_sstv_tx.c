// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SSTV transmit (CONFIG_SSTV_TX): pick the picture and Martin mode, preview it, set the frequency and level, send. The
// picture generator is core/sstv/sstv_tx.c; the APRS RF service keys the radio and board.afsk plays the PCM.
#include "config.h"
#if CONFIG_SSTV_TX
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "sstv/sstv_tx.h"
#include "sstv/sstv_rx.h"
#include "sstv/sstv_image.h"
#include "aprs/aprs.h"

#define SSTV_TX_HZ	144500000u	// the 2 m SSTV calling frequency

static uint32_t tx_hz = SSTV_TX_HZ;
static uint8_t tx_mode = SSTV_MODE_MARTIN_M1;
static uint8_t tx_level = 80;
static bool editing;
static char ed[UI_ENTRY_MAX + 1];
static uint8_t ed_len;

static const uint8_t mode_order[] = { SSTV_MODE_MARTIN_M1, SSTV_MODE_MARTIN_M2 };

enum { TX_ROW_FREQ, TX_ROW_MODE, TX_ROW_IMAGE, TX_ROW_PREVIEW, TX_ROW_LEVEL, TX_ROW_SEND, TX_ROW_STATUS,
       TX_ROW_COUNT };

static const char *mode_name(uint8_t m)
{
	switch (m) {
	case SSTV_MODE_MARTIN_M1: return "Martin M1";
	case SSTV_MODE_MARTIN_M2: return "Martin M2";
	default: return "Martin M1";
	}
}

static void tx_freq_digits(uint32_t hz)
{
	ed_len = UI_ENTRY_MAX;
	uint32_t v = hz / 10u;
	for (int i = UI_ENTRY_MAX - 1; i >= 0; i--) {
		ed[i] = (char)('0' + v % 10u);
		v /= 10u;
	}
	ed[UI_ENTRY_MAX] = '\0';
}

static uint16_t sstv_tx_count(void) { return TX_ROW_COUNT; }

static void sstv_tx_item(uint16_t i, ui_item_t *it)
{
	item_num_set(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case TX_ROW_FREQ:
		str_copy(it->label, sizeof it->label, "Frequency");
		if (editing) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, ed_len != 0 ? ed : "type digits");
		} else {
			sb_freq(&b, tx_hz);
		}
		break;
	case TX_ROW_MODE:
		str_copy(it->label, sizeof it->label, "Mode");
		sb_str(&b, mode_name(tx_mode));
		break;
	case TX_ROW_IMAGE:
		str_copy(it->label, sizeof it->label, "Image");
		sb_str(&b, "DM32 poster");
		break;
	case TX_ROW_PREVIEW:
		str_copy(it->label, sizeof it->label, "Preview");
		sb_str(&b, "Show");
		break;
	case TX_ROW_LEVEL:
		str_copy(it->label, sizeof it->label, "Level");
		sb_u32(&b, tx_level, 1);
		break;
	case TX_ROW_SEND:
		str_copy(it->label, sizeof it->label, "Send picture");
		sb_str(&b, ui_view()->state == RS_TX ? "Sending" : "Ready");
		break;
	case TX_ROW_STATUS:
		str_copy(it->label, sizeof it->label, "Status");
		sb_str(&b, ui_view()->state == RS_TX ? "Sending" : "Ready");
		break;
	default:
		break;
	}
}

static void sstv_send(void)
{
	if (ui_view()->state == RS_TX) {
		ui_toast("Sending");
		return;
	}
	if (!sstv_tx_begin(&sstv_image_dm32mayhem, tx_mode)) {
		ui_toast("No picture");
		return;
	}
	uint32_t ms = sstv_tx_ms(tx_mode);
	if (aprs_queue_sstv(tx_hz, tx_level, ms + 3000u)) {
		ui_toast("Sending");
	} else {
		ui_toast(TR("Busy"));
	}
}

static void sstv_tx_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case TX_ROW_FREQ:
		editing = true;
		tx_freq_digits(tx_hz);
		break;
	case TX_ROW_MODE: {
		const int n = (int)(sizeof mode_order / sizeof mode_order[0]);
		int k = 0;
		while (k < n && mode_order[k] != tx_mode) {
			k++;
		}
		tx_mode = mode_order[(k + 1) % n];
		break;
	}
	case TX_ROW_IMAGE:
		ui_toast("Only one picture");
		break;
	case TX_ROW_PREVIEW:
		ui_push(SCR_SSTV_PREVIEW, 0);
		break;
	case TX_ROW_LEVEL:
		tx_level = (uint8_t)(tx_level >= 100u ? 10u : tx_level + 10u);
		break;
	case TX_ROW_SEND:
		sstv_send();
		break;
	default:
		break;
	}
	ui_dirty();
}

static bool sstv_tx_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	bool repeat = ev->gesture == GESTURE_REPEAT;
	int d = ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;

	if (editing) {
		if (d >= 0 && (press || repeat)) {
			(void)ui_freq_entry_digit(ed, &ed_len, (char)('0' + d));
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_BACK && press) {
			if (ed_len != 0) {
				ed[--ed_len] = '\0';
			} else {
				editing = false;
			}
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_SELECT && press) {
			uint32_t f = ui_freq_entry_hz(ed, ed_len);
			if (f != 0) {
				editing = false;
				tx_hz = f;
			}
			ui_dirty();
			return true;
		}
		return true;
	}
	return false;
}

static void sstv_tx_enter(void)
{
	editing = false;
	ui_dirty();
}

const ui_scr_def_t ui_sstv_tx_screen = {
	.name = "sstv-tx", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("SSTV TX"),
	.hint = TRN("OK change  Back"), .count = sstv_tx_count, .item = sstv_tx_item, .select = sstv_tx_select,
	.key = sstv_tx_key, .enter = sstv_tx_enter,
};

// The picture as it will go out, scaled to 240 x 192 (the receive screen's layout)
static void preview_draw(void)
{
	gfx_fill(0, 0, gfx_width(), gfx_height(), ui_col.bg);
	const hal_image_t *im = &sstv_image_dm32mayhem;
	int top = 66;
	int w = 240, h = 192;
	if (w > gfx_width()) {
		w = gfx_width();
	}
	if (top + h > gfx_height()) {
		h = gfx_height() - top;
	}
	for (int y = 0; y < h; y++) {
		int sy = (int)((uint32_t)y * im->height / (uint32_t)h);
		for (int x = 0; x < w; x++) {
			int sx = (int)((uint32_t)x * im->width / (uint32_t)w);
			uint8_t pair = im->bits[sy * ((im->width + 1u) / 2u) + (sx >> 1)];
			uint8_t idx = (sx & 1) ? (pair & 0x0Fu) : (pair >> 4);
			gfx_pixel(x, top + y, im->palette[idx]);
		}
	}
	(void)ui_draw_top(ui_view(), &ui_st, "SSTV TX", mode_name(tx_mode));
	ui_draw_bottom(&ui_st, TR("Back"));
	ui_draw_banner(&ui_st);
}

const ui_scr_def_t ui_sstv_preview_screen = {
	.name = "sstv-preview", .flags = 0, .prefix = TRN("SSTV TX"), .hint = TRN("Back"), .draw = preview_draw,
};
#endif
