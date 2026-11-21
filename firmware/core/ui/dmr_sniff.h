// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The DMR sniffer's call tracker (CONFIG_DMR_SNIFF): fed a snapshot of the radio's DMR view on every UI tick, it
 * notices calls starting and ending, keeps a short log of the calls heard (newest first, one row per talker and
 * target, with a count) and formats a call as an NMEA-style line for the serial feed:
 *
 *   $PDMRS,<S|E>,<ts>,<cc>,<src>,<dst>,<G|P>,<flags>,<rssi>,<seconds>*hh
 *
 * S at a call's start, E at its end. flags: E encrypted, M emergency, L late entry (no header heard); rssi in dBm,
 * empty when unknown. No RTOS or hardware calls: the host tests run it (tests/host/dmrSniffTest.c).
 */
#ifndef DMR_SNIFF_H
#define DMR_SNIFF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMR_SNIFF_LOG_N		10
#define DMR_SNIFF_LINE_MAX	80

typedef struct {
	uint32_t src, dst;
	uint8_t cc, ts;
	bool group;		// group call, else private
	bool encrypted, emergency, late_entry;
	int16_t rssi_dbm;	// 0 = unknown
} dmr_sniff_call_t;

typedef struct {
	dmr_sniff_call_t call;	// as last heard
	uint32_t last_ms;	// when it was last heard
	uint32_t dur_ms;	// its last call's length
	uint16_t count;		// calls heard from this talker to this target
} dmr_sniff_entry_t;

// What an update saw (bits)
enum {
	DMR_SNIFF_EV_END	= 1u << 0,	// a call ended: dmr_sniff_t.ended, ended_ms
	DMR_SNIFF_EV_START	= 1u << 1,	// a call started: dmr_sniff_t.cur
};

typedef struct {
	dmr_sniff_entry_t log[DMR_SNIFF_LOG_N];	// newest first
	uint8_t n;
	uint32_t calls;			// calls heard since the last clear
	bool in_call;
	dmr_sniff_call_t cur;		// the call being heard (in_call)
	uint32_t cur_start_ms;
	dmr_sniff_call_t ended;		// the call the last DMR_SNIFF_EV_END ended
	uint32_t ended_ms;		// ... and its length
} dmr_sniff_t;

void dmr_sniff_init(dmr_sniff_t *s);
// One look at the radio: active = a call is being heard now, described by call (ignored when not active).
// Returns DMR_SNIFF_EV_* bits; a new call straight after another gives both (END first)
uint8_t dmr_sniff_update(dmr_sniff_t *s, bool active, const dmr_sniff_call_t *call, uint32_t now_ms);
// The call being heard for how long, 0 when none
uint32_t dmr_sniff_cur_ms(const dmr_sniff_t *s, uint32_t now_ms);
// The NMEA-style line (with \r\n) for a call: ev 'S' or 'E'. Returns its length
size_t dmr_sniff_line(char *buf, size_t len, char ev, const dmr_sniff_call_t *c, uint32_t dur_ms);

#endif
