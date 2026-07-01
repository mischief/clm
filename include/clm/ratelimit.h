// SPDX-License-Identifier: ISC
#ifndef CLM_RATELIMIT_H
#define CLM_RATELIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Token bucket rate limiter.
 *
 * Tokens refill at `rate` per second, up to a maximum of `burst` tokens.
 * A request consumes N tokens. If insufficient tokens are available, the
 * caller can query the delay until enough accumulate.
 *
 * A rate of 0 means unlimited (all operations succeed immediately).
 */
struct clm_ratelimit;

/* Create a rate limiter. rate=tokens/sec, burst=max tokens (0 = unlimited). */
int clm_ratelimit_new(struct clm_ratelimit **out, uint64_t rate, uint64_t burst);
void clm_ratelimit_free(struct clm_ratelimit *rl);

/* Can n tokens be consumed right now? Consumes tokens if true. */
bool clm_ratelimit_allow(struct clm_ratelimit *rl, size_t n);

/* How many microseconds until n tokens become available? 0 = now. */
uint64_t clm_ratelimit_delay(struct clm_ratelimit *rl, size_t n);

/* Consume n tokens unconditionally (call after delay has elapsed). */
void clm_ratelimit_consume(struct clm_ratelimit *rl, size_t n);

/* Current available tokens. */
uint64_t clm_ratelimit_tokens(struct clm_ratelimit *rl);

/* Adjust parameters at runtime. */
void clm_ratelimit_set_rate(struct clm_ratelimit *rl, uint64_t rate);
void clm_ratelimit_set_burst(struct clm_ratelimit *rl, uint64_t burst);
uint64_t clm_ratelimit_rate(struct clm_ratelimit *rl);
uint64_t clm_ratelimit_burst(struct clm_ratelimit *rl);

#endif
