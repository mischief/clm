// SPDX-License-Identifier: ISC
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "clm/ratelimit.h"
#include "tap.h"

static void
sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

/*
 * Frequent polling must still refill. An interval too short to earn a whole
 * token must not discard its elapsed time, or a limiter polled faster than
 * one token interval never recovers. Drain, poll fast, expect accumulation.
 */
static int
refills_under_frequent_polls(void *arg)
{
	struct clm_ratelimit *rl = NULL;
	uint64_t t;
	int i;

	(void)arg;
	TAP_CHECK(clm_ratelimit_new(&rl, 10, 8) == 0, "limiter created");
	TAP_CHECK(clm_ratelimit_allow(rl, 8), "drain the full burst");
	TAP_CHECK(clm_ratelimit_tokens(rl) == 0, "bucket is empty");

	/* 30 polls of ~10ms = ~300ms; one token earns every 100ms at 10/s. */
	for (i = 0; i < 30; i++) {
		sleep_ms(10);
		(void)clm_ratelimit_tokens(rl); /* each call refills */
	}
	t = clm_ratelimit_tokens(rl);
	tap_diag(
	    "tokens after ~300ms of 10ms polls: %llu", (unsigned long long)t);
	TAP_CHECK(t >= 2, "bucket refilled despite frequent polling");
	clm_ratelimit_free(rl);
	return 0;
}

/* A single long wait refills proportionally and caps at burst. */
static int
refill_caps_at_burst(void *arg)
{
	struct clm_ratelimit *rl = NULL;

	(void)arg;
	TAP_CHECK(clm_ratelimit_new(&rl, 100, 4) == 0, "limiter created");
	TAP_CHECK(clm_ratelimit_allow(rl, 4), "drain");
	sleep_ms(200); /* 100/s * 0.2s = 20 tokens, capped at burst 4 */
	TAP_CHECK(clm_ratelimit_tokens(rl) == 4, "refill caps at burst");
	clm_ratelimit_free(rl);
	return 0;
}

/* Rate 0 means unlimited: everything is allowed with no delay. */
static int
unlimited_passes(void *arg)
{
	struct clm_ratelimit *rl = NULL;

	(void)arg;
	TAP_CHECK(clm_ratelimit_new(&rl, 0, 0) == 0, "unlimited created");
	TAP_CHECK(
	    clm_ratelimit_allow(rl, 1000000), "unlimited allows anything");
	TAP_CHECK(
	    clm_ratelimit_delay(rl, 1000000) == 0, "no delay when unlimited");
	clm_ratelimit_free(rl);
	return 0;
}

int
main(void)
{
	TAP_ADD(
	    "refills under frequent polls", refills_under_frequent_polls, NULL);
	TAP_ADD("refill caps at burst", refill_caps_at_burst, NULL);
	TAP_ADD("unlimited passes", unlimited_passes, NULL);
	return tap_run();
}
