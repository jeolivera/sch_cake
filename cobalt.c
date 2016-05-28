/*
 * COBALT - Codel-BLUE Alternate AQM algorithm.
 *
 *  Copyright (C) 2011-2012 Kathleen Nichols <nichols@pollere.com>
 *  Copyright (C) 2011-2012 Van Jacobson <van@pollere.net>
 *  Copyright (C) 2012 Eric Dumazet <edumazet@google.com>
 *  Copyright (C) 2016 Michael D. Taht <dave.taht@bufferbloat.net>
 *  Copyright (c) 2015-2016 Jonathan Morton <chromatix99@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the authors may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include <linux/version.h>
#include <linux/types.h>
#include <linux/ktime.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>
#include <net/inet_ecn.h>
#include <linux/reciprocal_div.h>
#include <linux/random.h>

/*
 * COBALT operates the Codel and BLUE algorithms in parallel, in order
 * to obtain the best features of each.  Codel is excellent on flows
 * which respond to congestion signals in a TCP-like way.  BLUE is far
 * more effective on unresponsive flows.
 */

#if KERNEL_VERSION(3, 18, 0) > LINUX_VERSION_CODE
#include "codel5_compat.h"
#endif

typedef u64 cobalt_time_t;
typedef s64 cobalt_tdiff_t;

#define MS2TIME(a) (a * (u64) NSEC_PER_MSEC)
#define US2TIME(a) (a * (u64) NSEC_PER_USEC)

struct cobalt_skb_cb {
	cobalt_time_t enqueue_time;
};

static struct cobalt_skb_cb *get_cobalt_cb(const struct sk_buff *skb)
{
	qdisc_cb_private_validate(skb, sizeof(struct cobalt_skb_cb));
	return (struct cobalt_skb_cb *)qdisc_skb_cb(skb)->data;
}

static cobalt_time_t cobalt_get_enqueue_time(const struct sk_buff *skb)
{
	return get_cobalt_cb(skb)->enqueue_time;
}

/**
 * struct cobalt_params - contains codel and blue parameters
 * @interval:	codel initial drop rate
 * @target:     maximum persistent sojourn time & blue update rate
 * @threshold:	tolerance for product of sojourn time and time above target
 * @p_inc:      increment of blue drop probability
 * @p_dec:      decrement of blue drop probability
 */
struct cobalt_params {
	cobalt_time_t	interval;
	cobalt_time_t	target;
	cobalt_time_t	threshold;
	u32          	p_inc;
	u32          	p_dec;
};

/**
 * struct cobalt_vars - contains codel and blue variables
 * @count:		  dropping frequency
 * @rec_inv_sqrt: reciprocal value of sqrt(count) >> 1
 * @drop_next:    time to drop next packet, or when we dropped last
 * @drop_count:	  temp count of dropped packets in dequeue()
 * @ecn_mark:     number of packets we ECN marked instead of dropping
 * @p_drop:       BLUE drop probability
 * @dropping:     set if in dropping state
 */
struct cobalt_vars {
	u32		count;
	u32		rec_inv_sqrt;
	cobalt_time_t	drop_next;
	cobalt_time_t	blue_timer;
	u32     p_drop;
	bool	dropping;
	bool    ecn_marked;
};
#define REC_INV_SQRT_BITS (8 * sizeof(u32))
#define REC_INV_SQRT_SHIFT (32 - REC_INV_SQRT_BITS)
#define REC_INV_SQRT_CACHE (16)

static u32 cobalt_rec_inv_sqrt_cache[REC_INV_SQRT_CACHE] = {0};

/*
 * http://en.wikipedia.org/wiki/Methods_of_computing_square_roots#Iterative_methods_for_reciprocal_square_roots
 * new_invsqrt = (invsqrt / 2) * (3 - count * invsqrt^2)
 *
 * Here, invsqrt is a fixed point number (< 1.0), 32bit mantissa, aka Q0.32
 */
static void cobalt_Newton_step(struct cobalt_vars *vars)
{
	if (vars->count < REC_INV_SQRT_CACHE &&
	   likely(cobalt_rec_inv_sqrt_cache[vars->count])) {
		vars->rec_inv_sqrt = cobalt_rec_inv_sqrt_cache[vars->count];
	} else {
		u32 invsqrt = ((u32)vars->rec_inv_sqrt) << REC_INV_SQRT_SHIFT;
		u32 invsqrt2 = ((u64)invsqrt * invsqrt) >> 32;
		u64 val = (3LL << 32) - ((u64)vars->count * invsqrt2);

		val >>= 2; /* avoid overflow in following multiply */
		val = (val * invsqrt) >> (32 - 2 + 1);

		vars->rec_inv_sqrt = val >> REC_INV_SQRT_SHIFT;
	}
}

static void cobalt_cache_init(void)
{
	struct cobalt_vars v;

	cobalt_vars_init(&v);
	v.rec_inv_sqrt = ~0U >> REC_INV_SQRT_SHIFT;
	cobalt_rec_inv_sqrt_cache[0] = v.rec_inv_sqrt;

	for (v.count = 1; v.count < REC_INV_SQRT_CACHE; v.count++) {
		cobalt_Newton_step(&v);
		cobalt_Newton_step(&v);
		cobalt_Newton_step(&v);
		cobalt_Newton_step(&v);

		cobalt_rec_inv_sqrt_cache[v.count] = v.rec_inv_sqrt;
	}
}

void cobalt_vars_init(struct cobalt_vars *vars)
{
	memset(vars, 0, sizeof(*vars));

	if(!cobalt_rev_inv_sqrt_cache[0]) {
		cobalt_cache_init();
		cobalt_rev_inv_sqrt_cache[0] = ~0;
	}
}

/*
 * CoDel control_law is t + interval/sqrt(count)
 * We maintain in rec_inv_sqrt the reciprocal value of sqrt(count) to avoid
 * both sqrt() and divide operation.
 */
static cobalt_time_t cobalt_control_law(cobalt_time_t t,
				      cobalt_time_t interval,
				      u32 rec_inv_sqrt)
{
	return t + reciprocal_scale(interval, rec_inv_sqrt <<
				    REC_INV_SQRT_SHIFT);
}

/* Call this when a packet had to be dropped due to queue overflow. */
void cobalt_queue_full(struct cobalt_vars *vars, struct cobalt_params *p, cobalt_time_t now)
{
	if((now - vars->blue_timer) > p->target) {
		vars->p_drop += p->p_inc;
		if(vars->p_drop < p->p_inc)
			vars->p_drop = ~0;
		vars->blue_timer = now;
	}
	vars->dropping = true;
	vars->drop_next = now;
	if(!vars->count)
		vars->count = 1;
}

/* Call this when the queue was serviced but turned out to be empty. */
void cobalt_queue_empty(struct cobalt_vars *vars, struct cobalt_params *p, cobalt_time_t now)
{
	if((now - vars->blue_timer) > p->target) {
		if(vars->p_drop < p->p_dec)
			vars->p_drop = 0;
		else
			vars->p_drop -= p->p_dec;
		vars->blue_timer = now;
	}
	vars->dropping = false;
}

/* Call this with a freshly dequeued packet for possible congestion marking.
 * Returns true as an instruction to drop the packet, false for delivery.
 */
bool cobalt_should_drop(struct cobalt_vars *vars,
	struct cobalt_params *p,
	cobalt_time_t now,
	struct sk_buff *skb)
{
	bool drop = false;

	/* Simplified Codel implementation */
	cobalt_tdiff_t sojourn  = now - cobalt_get_enqueue_time(skb);
	cobalt_tdiff_t schedule = now - vars->drop_next;
	bool over_target = sojourn > p->target;
	bool next_due    = vars->count && schedule >= 0;

	vars->ecn_marked = false;

	if(over_target) {
		if(!vars->dropping) {
			vars->dropping = true;
			vars->drop_next = now + p->interval;
		}
		if(!vars->count)
			vars->count = 1;
	} else if(vars->dropping) {
		vars->dropping = false;
	}

	if(next_due && vars->dropping) {
		/* Use ECN mark if possible, otherwise drop */
		drop = !(vars->ecn_marked = INET_ECN_set_ce(skb));

		vars->count++;
		if(!vars->count)
			vars->count--;
		cobalt_Newton_step(vars);
		vars->drop_next = cobalt_control_law(vars->drop_next, p->interval, vars->rec_inv_sqrt);
	} else {
		while(next_due) {
			vars->count--;
			cobalt_Newton_step(vars);
			vars->drop_next = cobalt_control_law(vars->drop_next, p->interval, vars->rec_inv_sqrt);
			schedule = now - vars->drop_next;
			next_due = vars->count && schedule >= 0;
		}
	}

	/* Simple BLUE implementation.  Lack of ECN is deliberate. */
	if(vars->p_drop)
		drop |= (prandom_u32() < vars->p_drop);

	return drop;
}