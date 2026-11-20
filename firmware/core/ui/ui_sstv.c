// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SSTV receive (CONFIG_SSTV_RX, docs/spec/sstv.md): the FM receive audio goes to the decoder in the UI tick, like the
// RTTY/ACARS receivers, and each pixel is drawn straight into the frame buffer as it arrives (no image buffer, so the
// ~15 KB free RAM is enough). 320 x 256 scales to 240 x 192 on the DM32.
#include "config.h"
#if CONFIG_SSTV_RX
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "board.h"
#include "radio/radio.h"
#include "sstv/sstv_rx.h"
#include "debuglink/dlog.h"
#include "FreeRTOS.h"
#include "task.h"

#define SSTV_HZ		144500000u	// the 2 m SSTV calling frequency; the decoder takes any FM audio
#define SSTV_IMG_TOP	66
#define SSTV_IMG_W	240
#define SSTV_IMG_H	192

static sstv_rx_t rx;
static bool active, starting;
static uint32_t hz = SSTV_HZ, start_ms;
static uint8_t mode, lines_done;
static uint16_t lines_total;
static bool dirty, needs_clear = true;
static const char *sstv_err;
static uint8_t forced = SSTV_MODE_MARTIN_M1;	// the mode chosen in the UI (SSTV_MODE_NONE = VIS auto)
static const uint8_t mode_order[] = { SSTV_MODE_NONE, SSTV_MODE_MARTIN_M1, SSTV_MODE_MARTIN_M2,
				      SSTV_MODE_SCOTTIE_S1, SSTV_MODE_SCOTTIE_S2 };
static bool editing;			// the frequency editor owns the keys while set
static uint8_t row;			// the selected line: 0 Freq, 1 Mode
static bool mode_edit;			// the mode line is being changed
static uint8_t mode_sel;
static char ed[UI_ENTRY_MAX + 1];
static uint8_t ed_len;
static uint32_t dbg_ms;		// the 1 Hz diagnostic log
static uint32_t draw_ms;		// the last screen refresh (throttled: a flush is slow)
static int32_t dbg_peak;


static const char *mode_name(uint8_t m)
{
	switch (m) {
	case SSTV_MODE_MARTIN_M1: return "Martin M1";
	case SSTV_MODE_MARTIN_M2: return "Martin M2";
	case SSTV_MODE_SCOTTIE_S1: return "Scottie S1";
	case SSTV_MODE_SCOTTIE_S2: return "Scottie S2";
	default: return "SSTV";
	}
}

static void on_pixel(void *ctx, uint16_t x, uint16_t y, uint16_t color)
{
	(void)ctx;
	int sx = (int)((uint32_t)x * SSTV_IMG_W / SSTV_LINE_MAX);
	int sy = (int)((uint32_t)y * SSTV_IMG_H / SSTV_LINES_MAX);
	gfx_pixel(sx, SSTV_IMG_TOP + sy, color);
}

static void on_line(void *ctx, uint16_t line, uint16_t lines)
{
	(void)ctx; (void)lines;
	lines_done = (uint8_t)(line + 1);
	dirty = true;
}

static void on_mode(void *ctx, uint8_t vis, uint8_t m, uint16_t w, uint16_t h)
{
	(void)ctx; (void)vis; (void)w;
	if (m != SSTV_MODE_NONE) {
		mode = m;
		lines_done = 0;
		lines_total = h;
		gfx_fill(0, SSTV_IMG_TOP, SSTV_IMG_W, SSTV_IMG_H, RGB565(0, 0, 0));	// a new picture
	}
	dirty = true;
}

static void sstv_stop(void)
{
	if (active) {
		if (board.afsk != NULL && board.afsk->rx_stop != NULL) {
			board.afsk->rx_stop();
		}
		active = false;
	}
}

// Drain the receive ring into the decoder (UI tick)
static void sstv_pump(void)
{
	if (!active || board.afsk == NULL || board.afsk->rx_read == NULL) {
		return;
	}
	int16_t buf[160];
	uint16_t n;
	for (int budget = 8; budget > 0 &&
	     (n = board.afsk->rx_read(buf, (uint16_t)(sizeof buf / sizeof buf[0]))) != 0; budget--) {
		sstv_rx_feed(&rx, buf, n);
	}
}

// Once the radio task reports VFO FM (the app queued it on entry), open the receive audio. Opening it before the
// mode changes is what returned "audio busy": the DMR glue's FM-RX hook is not free while the old channel is up.
static void sstv_open(void)
{
	if (board.afsk == NULL || board.afsk->rx_start == NULL || board.afsk->rx_read == NULL) {
		sstv_err = TR("No audio path");
		starting = false;
		return;
	}
	if (!board.afsk->rx_start()) {
		return;	// the ring is held a poll by the APRS: sstv_tick retries until the timeout
	}
	sstv_rx_begin(&rx, board.afsk->rx_rate_hz != 0 ? board.afsk->rx_rate_hz : 8000u,
		      on_pixel, on_line, on_mode, NULL);
	sstv_rx_force_mode(&rx, forced);
	active = true;
	starting = false;
}

static bool sstv_tick(uint32_t now)
{
	if (starting) {
		const radio_view_t *v = ui_view();
		if (v != NULL && v->vfo && v->ch.mode == RADIO_MODE_FM && v->state != RS_TX) {
			sstv_open();
			if (active) {
				ui_dirty();
				return true;
			}
		}
		if (start_ms == 0) {
			start_ms = now;
		} else if (now - start_ms > 2000u) {
			if (sstv_err == NULL) {
				sstv_err = TR("Audio busy");
			}
			starting = false;
		}
		ui_dirty();
		return true;
	}
	sstv_pump();
	if (now - dbg_ms >= 1000u) {
		dbg_ms = now;
		dlog("sstv n=%u st=%u hz=%d off=%d mode=%u", (unsigned)rx.samples, rx.state, (int)rx.hz,
		     (int)rx.freq_offset, mode);
		dbg_peak = 0;
	}
	if ((dirty && now - draw_ms >= 150u) || now - draw_ms >= 1000u) {
		dirty = false;
		draw_ms = now;
		return true;
	}
	return false;
}

static void sstv_draw(void)
{
	// The picture lives in the frame buffer, drawn as it decodes: clear only the strip and the footer, never the image
	if (needs_clear) {
		gfx_fill(0, SSTV_IMG_TOP, SSTV_IMG_W, SSTV_IMG_H, RGB565(0, 0, 0));
		needs_clear = false;
	}
	gfx_fill(0, 0, gfx_width(), SSTV_IMG_TOP, ui_col.bg);
	gfx_fill(0, SSTV_IMG_TOP + SSTV_IMG_H, gfx_width(), gfx_height() - (SSTV_IMG_TOP + SSTV_IMG_H), ui_col.bg);

	// Header: what it is doing (a line progress, Tuning or Waiting)
	char sub[48];
	sbuf_t b;
	sb_init(&b, sub, sizeof sub);
	if (sstv_err != NULL) {
		sb_str(&b, sstv_err);
	} else if (mode != SSTV_MODE_NONE) {
		sb_u32(&b, lines_done, 1);
		sb_chr(&b, '/');
		sb_u32(&b, lines_total, 1);
	} else if (starting) {
		sb_str(&b, TR("Tuning"));
	} else {
		sb_str(&b, TR("Waiting"));
	}
	int y = ui_draw_top(ui_view(), &ui_st, "SSTV RX", sub) + 1;

	// The frequency and the chosen mode, each on its own line; the selected one is highlighted
	const font_t *f = ui_layout.small;
	char line[40];
	sbuf_t lb;
	bool sel_freq = !mode_edit && row == 0;
	sb_init(&lb, line, sizeof line);
	sb_str(&lb, "Freq ");
	if (editing) {
		sb_str(&lb, ed_len != 0 ? ed : TR("type digits"));
	} else {
		sb_freq(&lb, hz);
	}
	gfx_text(f, 0, y, line, sel_freq || editing ? ui_col.rx : ui_col.fg);
	bool sel_mode = mode_edit || row != 0;
	sb_init(&lb, line, sizeof line);
	sb_str(&lb, "Mode ");
	if (mode_edit) {
		sb_str(&lb, mode_sel == SSTV_MODE_NONE ? "Auto" : mode_name(mode_sel));
	} else {
		sb_str(&lb, forced == SSTV_MODE_NONE ? "Auto" : mode_name(forced));
	}
	gfx_text(f, 0, y + f->h + 1, line, sel_mode ? ui_col.rx : ui_col.fg);

	if (editing) {
		ui_draw_bottom(&ui_st, TR("OK set  Back"));
	} else if (mode_edit) {
		ui_draw_bottom(&ui_st, TR("Up Down  OK set  Back"));
	} else {
		ui_draw_bottom(&ui_st, TR("Up Down  OK edit  Back"));
	}
	ui_draw_banner(&ui_st);
}

static void sstv_set_hz(uint32_t f)
{
	hz = f;
	radio_req_vfo_set(hz);		// retune now; the decoder keeps listening to the FM audio
	ui_dirty();
}

static bool sstv_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	bool repeat = ev->gesture == GESTURE_REPEAT;
	int d = ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;

	// Typing a frequency: digits build it, Back deletes (and leaves once empty), OK applies it
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
				sstv_set_hz(f);
			}
			ui_dirty();
			return true;
		}
		return true;
	}

	// Changing the mode: Up/Down cycles the choices, OK applies, Back cancels
	if (mode_edit) {
		if ((ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) && (press || repeat)) {
			const int n = (int)(sizeof mode_order / sizeof mode_order[0]);
			int i = 0;
			while (i < n && mode_order[i] != mode_sel) {
				i++;
			}
			i = (i + (ev->action == ACT_NAV_UP ? 1 : n - 1)) % n;
			mode_sel = mode_order[i];
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_SELECT && press) {
			forced = mode_sel;
			mode_edit = false;
			if (active) {
				sstv_rx_force_mode(&rx, forced);
			}
			ui_dirty();
			return true;
		}
		if (ev->action == ACT_BACK && press) {
			mode_edit = false;
			ui_dirty();
			return true;
		}
		return true;
	}

	if (!press && !repeat) {
		return false;
	}
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN) {
		row ^= 1u;		// Fref <-> Mode
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (row == 0) {
			editing = true;
			ed_len = 0;
			ed[0] = '\0';
		} else {
			mode_edit = true;
			mode_sel = forced;
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void sstv_enter(void)
{
	needs_clear = true;
	sstv_err = NULL;
	active = false;
	starting = true;
	start_ms = 0;
	mode = SSTV_MODE_NONE;
	lines_done = 0;
	lines_total = 0;
	editing = false;
	mode_edit = false;
	row = 0;
	ed_len = 0;
	ed[0] = '\0';
	dbg_ms = 0;
	draw_ms = 0;
	dbg_peak = 0;
	dirty = false;
	radio_req_vfo(true);
	radio_req_vfo_set(hz);
	radio_req_mode(RADIO_MODE_FM);
	ui_dirty();
}

static void sstv_leave(void)
{
	sstv_stop();
	starting = false;
	editing = false;
}

const ui_scr_def_t ui_sstv_screen = {
	.name = "sstv", .flags = SF_FIELD, .hint = "Back stop", .key = sstv_key, .draw = sstv_draw,
	.enter = sstv_enter, .leave = sstv_leave, .tick = sstv_tick,
};
#endif
