// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR sniffer's call tracker (core/ui/dmr_sniff.c): calls starting and ending, the log's order and counts, and
// the serial line with its checksum
#include <stdio.h>
#include <string.h>
#include "ui/dmr_sniff.h"

const char *tr(const char *en);
const char *tr(const char *en) { return en; }	// fmt.c's ages; no catalogs here

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static dmr_sniff_call_t mk(uint32_t src, uint32_t dst, uint8_t ts, uint8_t cc)
{
	dmr_sniff_call_t c;
	memset(&c, 0, sizeof c);
	c.src = src;
	c.dst = dst;
	c.ts = ts;
	c.cc = cc;
	c.group = true;
	return c;
}

static void test_start_end(void)
{
	dmr_sniff_t s;
	dmr_sniff_init(&s);
	dmr_sniff_call_t a = mk(3101234, 91, 1, 1);
	CHECK(dmr_sniff_update(&s, false, NULL, 0) == 0);
	CHECK(dmr_sniff_update(&s, true, &a, 1000) == DMR_SNIFF_EV_START);
	CHECK(s.in_call && s.calls == 1 && s.n == 1 && s.log[0].count == 1);
	// Late entry first, then the signal and the header: the same call, updated
	dmr_sniff_call_t a2 = a;
	a2.rssi_dbm = -87;
	CHECK(dmr_sniff_update(&s, true, &a2, 1500) == 0);
	a2.rssi_dbm = 0;	// a tick without a new reading keeps the last one
	CHECK(dmr_sniff_update(&s, true, &a2, 2000) == 0);
	CHECK(s.cur.rssi_dbm == -87 && s.log[0].call.rssi_dbm == -87);
	CHECK(dmr_sniff_cur_ms(&s, 3000) == 2000);
	CHECK(dmr_sniff_update(&s, false, NULL, 4000) == DMR_SNIFF_EV_END);
	CHECK(!s.in_call && s.ended.src == 3101234 && s.ended_ms == 3000 && s.log[0].dur_ms == 3000);
	CHECK(dmr_sniff_cur_ms(&s, 5000) == 0);
	CHECK(dmr_sniff_update(&s, false, NULL, 5000) == 0);
}

static void test_back_to_back(void)
{
	dmr_sniff_t s;
	dmr_sniff_init(&s);
	dmr_sniff_call_t a = mk(3101234, 91, 1, 1), b = mk(3105555, 91, 1, 1);
	(void)dmr_sniff_update(&s, true, &a, 0);
	// Another talker with no idle look between: the first ends, the second starts
	CHECK(dmr_sniff_update(&s, true, &b, 2000) == (DMR_SNIFF_EV_END | DMR_SNIFF_EV_START));
	CHECK(s.ended.src == 3101234 && s.cur.src == 3105555 && s.calls == 2 && s.n == 2);
	CHECK(s.log[0].call.src == 3105555 && s.log[1].call.src == 3101234);
	// The first talker again: one row, counted twice, back on top
	(void)dmr_sniff_update(&s, false, NULL, 3000);
	(void)dmr_sniff_update(&s, true, &a, 4000);
	CHECK(s.n == 2 && s.log[0].call.src == 3101234 && s.log[0].count == 2 && s.log[1].count == 1);
	// The same talker on the other slot is a row of its own
	(void)dmr_sniff_update(&s, false, NULL, 5000);
	dmr_sniff_call_t a_ts2 = mk(3101234, 91, 2, 1);
	(void)dmr_sniff_update(&s, true, &a_ts2, 6000);
	CHECK(s.n == 3 && s.log[0].call.ts == 2);
}

static void test_log_full(void)
{
	dmr_sniff_t s;
	dmr_sniff_init(&s);
	for (uint32_t i = 0; i < DMR_SNIFF_LOG_N + 3u; i++) {
		dmr_sniff_call_t c = mk(3100000u + i, 9, 1, 1);
		(void)dmr_sniff_update(&s, true, &c, i * 1000u);
		(void)dmr_sniff_update(&s, false, NULL, i * 1000u + 500u);
	}
	CHECK(s.n == DMR_SNIFF_LOG_N && s.calls == DMR_SNIFF_LOG_N + 3u);
	CHECK(s.log[0].call.src == 3100000u + DMR_SNIFF_LOG_N + 2u);	// newest first
	CHECK(s.log[DMR_SNIFF_LOG_N - 1u].call.src == 3100003u);	// the three oldest were dropped
}

static void test_line(void)
{
	char buf[DMR_SNIFF_LINE_MAX + 8];
	dmr_sniff_call_t c = mk(3101234, 91, 1, 3);
	c.rssi_dbm = -87;
	size_t n = dmr_sniff_line(buf, sizeof buf, 'E', &c, 12400);
	CHECK(n == strlen(buf));
	CHECK(strncmp(buf, "$PDMRS,E,1,3,3101234,91,G,,-87,12*", 34) == 0);
	// The checksum: XOR of everything between $ and *
	uint8_t sum = 0;
	for (const char *p = buf + 1; *p != '*'; p++) {
		sum ^= (uint8_t)*p;
	}
	char want[8];
	snprintf(want, sizeof want, "*%02X\r\n", sum);
	CHECK(strcmp(strchr(buf, '*'), want) == 0);

	dmr_sniff_call_t p = mk(3101234, 3105555, 2, 0);
	p.group = false;
	p.encrypted = true;
	p.emergency = true;
	p.late_entry = true;
	(void)dmr_sniff_line(buf, sizeof buf, 'S', &p, 0);
	CHECK(strncmp(buf, "$PDMRS,S,2,0,3101234,3105555,P,EML,,0*", 38) == 0);
	// A short buffer is cut, never overrun
	char small[12];
	memset(small, 'x', sizeof small);
	(void)dmr_sniff_line(small, 8, 'S', &c, 0);
	CHECK(strlen(small) == 7 && small[8] == 'x');
}

int main(void)
{
	test_start_end();
	test_back_to_back();
	test_log_full();
	test_line();
	printf("# dmrSniffTest: %d checks, %s\n", checks, failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
