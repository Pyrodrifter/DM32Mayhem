// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The DMR sniffer app (CONFIG_DMR_SNIFF, SCR_DMR_SNIFF): tunes the VFO to a frequency in DMR mode and latches hear all
 * on any color code (radio_req_dmr_hear_any), so every call on the chosen timeslot is reported: talker, target, group
 * or private, color code, signal, encrypted / emergency / late entry. Receive only: it never keys the radio, and with
 * no vocoder nothing is played. Calls go into a short log (dmr_sniff.c) and, with Serial on, out of the programming
 * cable as $PDMRS lines (dmr_sniff.h), the way the GPS screen sends its NMEA.
 */
#include "config.h"

#if CONFIG_DMR_SNIFF

#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "ui_dmr_sniff.h"
#include "dmr_sniff.h"
#include "fmt.h"
#include "i18n.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "hostlink/hostlink.h"

enum { SN_ROW_FREQ, SN_ROW_TS, SN_ROW_CC, SN_ROW_CALLS, SN_ROW_SERIAL, SN_ROW_CLEAR, SN_ROW_COUNT };

#define SN_ANY_CC	0xFFu
#define SN_LOG_SHOWN	8u

static dmr_sniff_t sn;
static uint32_t sn_hz = 441000000u;	// the US DMR simplex calling frequency
static uint8_t sn_ts = 1;
static uint8_t sn_cc = SN_ANY_CC;
static bool sn_serial = true;
static bool sn_on;			// the radio is set up for us (enter, until leave)
static bool sn_new;
static uint32_t sn_check_ms;
static int8_t sn_edit = -1;
static char sn_digits[UI_ENTRY_MAX + 1];
static uint8_t sn_len;
static char sn_text[560];		// the body: shared by the main screen and the log, only one is on top

static void sn_tune(void)
{
	radio_req_vfo(true);
	radio_req_vfo_set(sn_hz);
	radio_req_mode(RADIO_MODE_DMR);
	radio_req_dmr_timeslot(sn_ts);
	if (sn_cc == SN_ANY_CC) {
		radio_req_dmr_hear_any(true);
	} else {
		radio_req_dmr_color_code(sn_cc);
		radio_req_dmr_hear_all(true);
	}
	sn_check_ms = hal_millis() + 1500u;	// give the radio time before checking it took
	sn_new = true;
}

static void sn_enter(void)
{
	sn_edit = -1;
	if (!sn_on) {
		sn_on = true;
		sn_tune();
	}
}

static void sn_leave(void)
{
	if (!sn_on) {
		return;
	}
	sn_on = false;
	if (sn.in_call) {
		(void)dmr_sniff_update(&sn, false, NULL, hal_millis());
	}
	radio_req_dmr_hear_all(false);
	radio_req_mode(RADIO_MODE_FM);		// the VFO back as the other apps expect it
}

static void sn_send(char ev, const dmr_sniff_call_t *c, uint32_t dur_ms)
{
	if (!sn_serial) {
		return;
	}
	char line[DMR_SNIFF_LINE_MAX + 8];
	size_t n = dmr_sniff_line(line, sizeof line, ev, c, dur_ms);
	(void)hostlink_write((const uint8_t *)line, n);
}

// The radio's settings drift back (another request, a retune): put ours back, at most once a second
static void sn_check(const radio_view_t *v, uint32_t now)
{
	if ((int32_t)(now - sn_check_ms) < 0) {
		return;
	}
	sn_check_ms = now + 1000u;
	if (!v->vfo || v->ch.mode != RADIO_MODE_DMR || v->ch.rx_hz != sn_hz) {
		sn_tune();
		return;
	}
	if (!v->dmr.active) {
		return;
	}
	if (v->dmr.ts != sn_ts) {
		radio_req_dmr_timeslot(sn_ts);
	}
	if (sn_cc == SN_ANY_CC) {
		if (!v->dmr.any_cc) {
			radio_req_dmr_hear_any(true);
		}
	} else if (!v->dmr.hear_all_latch || v->dmr.any_cc || (!sn.in_call && v->dmr.cc != sn_cc)) {
		radio_req_dmr_color_code(sn_cc);
		radio_req_dmr_hear_all(true);
	}
}

static bool sn_tick(uint32_t now)
{
	if (!sn_on) {
		return false;
	}
	const radio_view_t *v = ui_view();
	sn_check(v, now);
	bool active = v->dmr.active && (v->dmr.state == RDMR_RX || v->dmr.state == RDMR_RX_OTHER);
	dmr_sniff_call_t c;
	memset(&c, 0, sizeof c);
	if (active) {
		c.src = v->dmr.call.src;
		c.dst = v->dmr.call.dst;
		c.group = v->dmr.call.group;
		c.encrypted = v->dmr.call.encrypted;
		c.emergency = v->dmr.call.emergency;
		c.late_entry = v->dmr.late_entry;
		c.cc = v->dmr.cc;
		c.ts = v->dmr.ts;
		c.rssi_dbm = v->dmr.rssi_dbm;
	}
	uint8_t ev = dmr_sniff_update(&sn, active, &c, now);
	if (ev & DMR_SNIFF_EV_END) {
		sn_send('E', &sn.ended, sn.ended_ms);
	}
	if (ev & DMR_SNIFF_EV_START) {
		sn_send('S', &sn.cur, 0);
	}
	// A call's clock and signal change every second; otherwise only when something happened
	static uint32_t last_s;
	uint32_t s = now / 1000u;
	bool redraw = ev != 0 || sn_new || (sn.in_call && s != last_s);
	last_s = s;
	sn_new = false;
	return redraw;
}

// "91" for a talkgroup, the ID for a private call's target
static void sb_target(sbuf_t *b, const dmr_sniff_call_t *c)
{
	sb_str(b, c->group ? "TG " : "ID ");
	sb_u32(b, c->dst, 1);
}

static void sb_flags(sbuf_t *b, const dmr_sniff_call_t *c)
{
	if (c->encrypted) {
		sb_str(b, " ENC");
	}
	if (c->emergency) {
		sb_str(b, " EMRG");
	}
	if (c->late_entry) {
		sb_str(b, " late");
	}
}

static const char *sn_body(void)
{
	sbuf_t b;
	sb_init(&b, sn_text, sizeof sn_text);
	const radio_view_t *v = ui_view();
	uint32_t now = hal_millis();
	if (sn.in_call) {
		const dmr_sniff_call_t *c = &sn.cur;
		sb_u32(&b, c->src, 1);
		if (v->dmr.call.src == c->src && v->dmr.call.src_name[0] != '\0') {
			sb_chr(&b, ' ');
			sb_str(&b, v->dmr.call.src_name);
		}
		sb_str(&b, "\n> ");
		sb_target(&b, c);
		if (v->dmr.call.dst == c->dst && v->dmr.call.dst_name[0] != '\0') {
			sb_chr(&b, ' ');
			sb_str(&b, v->dmr.call.dst_name);
		}
		sb_str(&b, "\nTS");
		sb_u32(&b, c->ts, 1);
		sb_str(&b, " CC");
		sb_u32(&b, c->cc, 1);
		if (c->rssi_dbm != 0) {
			sb_chr(&b, ' ');
			sb_i32(&b, c->rssi_dbm);
			sb_str(&b, " dBm");
		}
		sb_chr(&b, ' ');
		sb_mmss(&b, dmr_sniff_cur_ms(&sn, now) / 1000u);
		sb_flags(&b, c);
		return sn_text;
	}
	if (!v->dmr.active) {
		sb_str(&b, "Starting DMR receive ");
		sb_freq(&b, sn_hz);
		return sn_text;
	}
	sb_str(&b, "Listening ");
	sb_freq(&b, sn_hz);
	if (sn.n != 0) {
		const dmr_sniff_entry_t *e = &sn.log[0];
		sb_str(&b, "\nLast: ");
		sb_u32(&b, e->call.src, 1);
		sb_str(&b, " > ");
		sb_target(&b, &e->call);
		sb_chr(&b, ' ');
		sb_age_long(&b, now - e->last_ms);
	}
	return sn_text;
}

static uint16_t sn_count(void) { return SN_ROW_COUNT; }

static void sn_freq_digits(uint32_t hz)
{
	sn_len = UI_ENTRY_MAX;
	uint32_t v = hz / 10u;
	for (int i = UI_ENTRY_MAX - 1; i >= 0; i--) {
		sn_digits[i] = (char)('0' + v % 10u);
		v /= 10u;
	}
	sn_digits[UI_ENTRY_MAX] = '\0';
}

static void sn_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case SN_ROW_FREQ:
		str_copy(it->label, sizeof it->label, "Frequency");
		if (sn_edit == (int8_t)i) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, sn_len != 0 ? sn_digits : "type digits");
		} else {
			sb_freq(&b, sn_hz);
		}
		break;
	case SN_ROW_TS:
		str_copy(it->label, sizeof it->label, "Timeslot");
		sb_u32(&b, sn_ts, 1);
		break;
	case SN_ROW_CC:
		str_copy(it->label, sizeof it->label, "Color code");
		if (sn_cc == SN_ANY_CC) {
			sb_str(&b, "Any");
		} else {
			sb_u32(&b, sn_cc, 1);
		}
		break;
	case SN_ROW_CALLS:
		str_copy(it->label, sizeof it->label, "Calls heard");
		sb_u32(&b, sn.calls, 1);
		break;
	case SN_ROW_SERIAL:
		str_copy(it->label, sizeof it->label, "Serial $PDMRS");
		sb_str(&b, sn_serial ? "On" : "Off");
		break;
	case SN_ROW_CLEAR:
		str_copy(it->label, sizeof it->label, "Clear log");
		break;
	default:
		break;
	}
}

static void sn_select(uint16_t i, bool hold)
{
	switch (i) {
	case SN_ROW_FREQ:
		sn_edit = (int8_t)i;
		sn_freq_digits(sn_hz);
		break;
	case SN_ROW_TS:
		sn_ts = sn_ts == 1 ? 2 : 1;
		radio_req_dmr_timeslot(sn_ts);
		sn_new = true;
		break;
	case SN_ROW_CC:
		// Any, 0..15; a long OK goes back to Any
		sn_cc = hold || sn_cc == 15u ? SN_ANY_CC : sn_cc == SN_ANY_CC ? 0u : (uint8_t)(sn_cc + 1u);
		sn_tune();
		break;
	case SN_ROW_CALLS:
		ui_push(SCR_DMR_SNIFF_LOG, 0);
		break;
	case SN_ROW_SERIAL:
		sn_serial = !sn_serial;
		break;
	case SN_ROW_CLEAR:
		dmr_sniff_init(&sn);
		ui_toast("Cleared");
		break;
	default:
		break;
	}
	ui_dirty();
}

static bool sn_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	if (sn_edit < 0) {
		return false;		// the list's keys; Back leaves the screen
	}
	int d = ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;
	if (d >= 0 && (press || ev->gesture == GESTURE_LONG)) {
		if (sn_len >= UI_ENTRY_MAX) {
			sn_len = 0;	// the first digit replaces the value shown
		}
		(void)ui_freq_entry_digit(sn_digits, &sn_len, (char)('0' + d));
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && press) {
		if (sn_len != 0) {
			sn_digits[--sn_len] = '\0';
		} else {
			sn_edit = -1;
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		uint32_t hz = ui_freq_entry_hz(sn_digits, sn_len);	// 0 with a toast when it isn't a receive frequency
		if (hz != 0) {
			sn_hz = hz;
			sn_tune();
		}
		sn_edit = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && press) {
		sn_edit = -1;
		ui_dirty();
	}
	return true;			// while a value is being typed it owns the keys
}

// The log, newest first: one line per talker and target
static const char *sn_log_body(void)
{
	sbuf_t b;
	sb_init(&b, sn_text, sizeof sn_text);
	if (sn.n == 0) {
		sb_str(&b, "No calls yet");
		return sn_text;
	}
	uint32_t now = hal_millis();
	for (uint8_t i = 0; i < sn.n && i < SN_LOG_SHOWN; i++) {
		const dmr_sniff_entry_t *e = &sn.log[i];
		if (i != 0) {
			sb_chr(&b, '\n');
		}
		sb_u32(&b, e->call.src, 1);
		sb_chr(&b, '>');
		sb_str(&b, e->call.group ? "TG" : "");
		sb_u32(&b, e->call.dst, 1);
		sb_str(&b, " TS");
		sb_u32(&b, e->call.ts, 1);
		sb_str(&b, " CC");
		sb_u32(&b, e->call.cc, 1);
		if (e->count > 1) {
			sb_str(&b, " x");
			sb_u32(&b, e->count, 1);
		}
		sb_chr(&b, ' ');
		sb_age_long(&b, now - e->last_ms);
		sb_flags(&b, &e->call);
	}
	return sn_text;
}

static uint16_t sn_log_count(void) { return 0; }

static bool sn_log_key(const input_event_t *ev)
{
	if (ev->gesture == GESTURE_PRESS && (ev->action == ACT_SELECT || ev->action == ACT_CONTEXT)) {
		dmr_sniff_init(&sn);
		ui_toast("Cleared");
		ui_dirty();
		return true;
	}
	return false;			// Back pops
}

const ui_scr_def_t dmr_sniff_screen = {
	.name = "dmr-sniff", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("DMR Sniffer"),
	.hint = TRN("OK change  Back"),
	.count = sn_count, .item = sn_item, .select = sn_select, .key = sn_key, .enter = sn_enter, .leave = sn_leave,
	.tick = sn_tick, .body = sn_body,
};

const ui_scr_def_t dmr_sniff_log_screen = {
	.name = "dmr-sniff-log", .flags = SF_LIST | SF_FIELD, .prefix = TRN("DMR Calls"),
	.hint = TRN("OK clear  Back"),
	.count = sn_log_count, .key = sn_log_key, .tick = sn_tick, .body = sn_log_body,
};

#endif	// CONFIG_DMR_SNIFF
