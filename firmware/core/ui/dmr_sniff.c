// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR sniffer's call tracker and its serial line (dmr_sniff.h)
#include "dmr_sniff.h"

#include <string.h>
#include "fmt.h"

void dmr_sniff_init(dmr_sniff_t *s)
{
	memset(s, 0, sizeof *s);
}

static bool same_call(const dmr_sniff_call_t *a, const dmr_sniff_call_t *b)
{
	return a->src == b->src && a->dst == b->dst && a->group == b->group;
}

// One row per talker, target and slot: a talker heard again moves to the top
static void log_call(dmr_sniff_t *s, const dmr_sniff_call_t *c, uint32_t now_ms, uint32_t dur_ms, bool new_call)
{
	uint8_t i = 0;
	while (i < s->n && !(same_call(&s->log[i].call, c) && s->log[i].call.ts == c->ts)) {
		i++;
	}
	dmr_sniff_entry_t e;
	if (i < s->n) {
		e = s->log[i];
	} else {
		memset(&e, 0, sizeof e);
		if (s->n < DMR_SNIFF_LOG_N) {
			s->n++;
		}
		i = (uint8_t)(s->n - 1u);	// the oldest row goes when the log is full
	}
	memmove(&s->log[1], &s->log[0], (size_t)i * sizeof s->log[0]);
	e.call = *c;
	e.last_ms = now_ms;
	e.dur_ms = dur_ms;
	if (new_call && e.count < UINT16_MAX) {
		e.count++;
	}
	s->log[0] = e;
}

static void end_call(dmr_sniff_t *s, uint32_t now_ms)
{
	s->ended = s->cur;
	s->ended_ms = now_ms - s->cur_start_ms;
	log_call(s, &s->cur, now_ms, s->ended_ms, false);
	s->in_call = false;
}

uint8_t dmr_sniff_update(dmr_sniff_t *s, bool active, const dmr_sniff_call_t *call, uint32_t now_ms)
{
	uint8_t ev = 0;
	if (s->in_call && (!active || !same_call(&s->cur, call))) {
		end_call(s, now_ms);
		ev |= DMR_SNIFF_EV_END;
	}
	if (!active) {
		return ev;
	}
	if (!s->in_call) {
		s->in_call = true;
		s->cur = *call;
		s->cur_start_ms = now_ms;
		s->calls++;
		log_call(s, call, now_ms, 0, true);
		return (uint8_t)(ev | DMR_SNIFF_EV_START);
	}
	// The same call: what can change while it lasts (the header after a late entry, the signal, the color code
	// once a burst with it is decoded)
	int16_t rssi = call->rssi_dbm != 0 ? call->rssi_dbm : s->cur.rssi_dbm;
	s->cur = *call;
	s->cur.rssi_dbm = rssi;
	s->log[0].call = s->cur;
	s->log[0].last_ms = now_ms;
	s->log[0].dur_ms = now_ms - s->cur_start_ms;
	return ev;
}

uint32_t dmr_sniff_cur_ms(const dmr_sniff_t *s, uint32_t now_ms)
{
	return s->in_call ? now_ms - s->cur_start_ms : 0;
}

size_t dmr_sniff_line(char *buf, size_t len, char ev, const dmr_sniff_call_t *c, uint32_t dur_ms)
{
	char body[DMR_SNIFF_LINE_MAX];
	sbuf_t b;
	sb_init(&b, body, sizeof body);
	sb_str(&b, "PDMRS,");
	sb_chr(&b, ev);
	sb_chr(&b, ',');
	sb_u32(&b, c->ts, 1);
	sb_chr(&b, ',');
	sb_u32(&b, c->cc, 1);
	sb_chr(&b, ',');
	sb_u32(&b, c->src, 1);
	sb_chr(&b, ',');
	sb_u32(&b, c->dst, 1);
	sb_chr(&b, ',');
	sb_chr(&b, c->group ? 'G' : 'P');
	sb_chr(&b, ',');
	if (c->encrypted) {
		sb_chr(&b, 'E');
	}
	if (c->emergency) {
		sb_chr(&b, 'M');
	}
	if (c->late_entry) {
		sb_chr(&b, 'L');
	}
	sb_chr(&b, ',');
	if (c->rssi_dbm != 0) {
		sb_i32(&b, c->rssi_dbm);
	}
	sb_chr(&b, ',');
	sb_u32(&b, (dur_ms + 500u) / 1000u, 1);

	uint8_t sum = 0;
	for (const char *p = body; *p != '\0'; p++) {
		sum ^= (uint8_t)*p;
	}
	static const char hex[] = "0123456789ABCDEF";
	sb_init(&b, buf, len);
	sb_chr(&b, '$');
	sb_str(&b, body);
	sb_chr(&b, '*');
	sb_chr(&b, hex[sum >> 4]);
	sb_chr(&b, hex[sum & 0xFu]);
	sb_str(&b, "\r\n");
	return strlen(buf);
}
