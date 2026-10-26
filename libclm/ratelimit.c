// SPDX-License-Identifier: ISC
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "clm/ratelimit.h"
#include "banned.h"

struct clm_ratelimit {
	uint64_t rate;      /* tokens per second (0 = unlimited) */
	uint64_t burst;     /* max tokens */
	uint64_t tokens;    /* current tokens */
	uint64_t last_usec; /* last refill timestamp in usec */
};

static uint64_t
now_usec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

static void
refill(struct clm_ratelimit *rl)
{
	uint64_t now, elapsed, added, used;

	if (rl->rate == 0)
		return;

	now = now_usec();
	elapsed = now - rl->last_usec;
	if (elapsed == 0)
		return;

	/* rate * elapsed would overflow, or already dwarfs a full burst:
	 * the bucket is full and the exact count no longer matters. */
	if (elapsed >= UINT64_MAX / rl->rate) {
		rl->tokens = rl->burst;
		rl->last_usec = now;
		return;
	}

	/* tokens += rate * elapsed / 1e6. Integer division drops the
	 * sub-token remainder; advancing last_usec to now would discard that
	 * time, so a limiter polled faster than one token interval would
	 * never refill. Advance only by the time the whole tokens account
	 * for, carrying the remainder forward. */
	added = rl->rate * elapsed / 1000000ULL;
	if (added == 0)
		return; /* keep last_usec; let the remainder accumulate */
	if (added >= rl->burst - rl->tokens) {
		rl->tokens = rl->burst;
		rl->last_usec = now; /* full: no fractional credit to carry */
		return;
	}
	rl->tokens += added;
	used = added * 1000000ULL / rl->rate;
	rl->last_usec += used;
}

int
clm_ratelimit_new(struct clm_ratelimit **out, uint64_t rate, uint64_t burst)
{
	struct clm_ratelimit *rl = calloc(1, sizeof(*rl));
	if (rl == NULL)
		return -1;

	rl->rate = rate;
	rl->burst = burst ? burst : rate; /* default burst = 1 second */
	rl->tokens = rl->burst;           /* start full */
	rl->last_usec = now_usec();
	*out = rl;
	return 0;
}

void
clm_ratelimit_free(struct clm_ratelimit *rl)
{
	free(rl);
}

bool
clm_ratelimit_allow(struct clm_ratelimit *rl, size_t n)
{
	if (rl == NULL || rl->rate == 0)
		return true;

	refill(rl);

	if (rl->tokens >= n) {
		rl->tokens -= n;
		return true;
	}
	return false;
}

uint64_t
clm_ratelimit_delay(struct clm_ratelimit *rl, size_t n)
{
	uint64_t deficit;

	if (rl == NULL || rl->rate == 0)
		return 0;

	refill(rl);

	if (rl->tokens >= n)
		return 0;

	deficit = n - rl->tokens;
	/* usec = deficit * 1e6 / rate */
	return deficit * 1000000ULL / rl->rate;
}

void
clm_ratelimit_consume(struct clm_ratelimit *rl, size_t n)
{
	if (rl == NULL || rl->rate == 0)
		return;

	refill(rl);

	if (rl->tokens >= n)
		rl->tokens -= n;
	else
		rl->tokens = 0;
}

uint64_t
clm_ratelimit_tokens(struct clm_ratelimit *rl)
{
	if (rl == NULL || rl->rate == 0)
		return UINT64_MAX;

	refill(rl);
	return rl->tokens;
}

void
clm_ratelimit_set_rate(struct clm_ratelimit *rl, uint64_t rate)
{
	if (rl == NULL)
		return;
	refill(rl); /* settle tokens at old rate first */
	rl->rate = rate;
}

void
clm_ratelimit_set_burst(struct clm_ratelimit *rl, uint64_t burst)
{
	if (rl == NULL)
		return;
	rl->burst = burst;
	if (rl->tokens > burst)
		rl->tokens = burst;
}

uint64_t
clm_ratelimit_rate(struct clm_ratelimit *rl)
{
	return rl != NULL ? rl->rate : 0;
}

uint64_t
clm_ratelimit_burst(struct clm_ratelimit *rl)
{
	return rl != NULL ? rl->burst : 0;
}
