/*
 * xdp.c - 4.4 backport, trimmed to the generic-XDP consumer surface.
 *
 * The full v5.4 file also implements the mem-model allocator table
 * and the xdp_frame return path for native XDP drivers.  Those use
 * symbols 4.4 does not have (trace_mem_* events, page_frag_free,
 * page_to_virt, netdev_bpf prog_id/flags/extack) and nothing in this
 * tree reaches them: no driver implements ndo_bpf.  Kept: the rxq
 * registration core that net/core/dev.c calls.
 */
// SPDX-License-Identifier: GPL-2.0-only
/* net/core/xdp.c
 *
 * Copyright (c) 2017 Jesper Dangaard Brouer, Red Hat Inc.
 */
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/rhashtable.h>
#include <net/page_pool.h>

#include <net/xdp.h>
#include <net/xdp_priv.h> /* struct xdp_mem_allocator */
#include <trace/events/xdp.h>

#define REG_STATE_NEW		0x0
#define REG_STATE_REGISTERED	0x1
#define REG_STATE_UNREGISTERED	0x2
#define REG_STATE_UNUSED	0x3

/* This unregister operation will also cleanup and destroy the
 * allocator. The page_pool_free() operation is first called when it's
 * safe to remove, possibly deferred to a workqueue.
 */
void xdp_rxq_info_unreg(struct xdp_rxq_info *xdp_rxq)
{
	/* Simplify driver cleanup code paths, allow unreg "unused" */
	if (xdp_rxq->reg_state == REG_STATE_UNUSED)
		return;

	WARN(!(xdp_rxq->reg_state == REG_STATE_REGISTERED), "Driver BUG");

	/* 4.4: mem-model allocator not ported (no native XDP
	 * driver here); unregistering the rxq is enough. */

	xdp_rxq->reg_state = REG_STATE_UNREGISTERED;
	xdp_rxq->dev = NULL;

	/* Reset mem info to defaults */
	xdp_rxq->mem.id = 0;
	xdp_rxq->mem.type = 0;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_unreg);

static void xdp_rxq_info_init(struct xdp_rxq_info *xdp_rxq)
{
	memset(xdp_rxq, 0, sizeof(*xdp_rxq));
}

/* Returns 0 on success, negative on failure */
int xdp_rxq_info_reg(struct xdp_rxq_info *xdp_rxq,
		     struct net_device *dev, u32 queue_index)
{
	if (xdp_rxq->reg_state == REG_STATE_UNUSED) {
		WARN(1, "Driver promised not to register this");
		return -EINVAL;
	}

	if (xdp_rxq->reg_state == REG_STATE_REGISTERED) {
		WARN(1, "Missing unregister, handled but fix driver");
		xdp_rxq_info_unreg(xdp_rxq);
	}

	if (!dev) {
		WARN(1, "Missing net_device from driver");
		return -ENODEV;
	}

	/* State either UNREGISTERED or NEW */
	xdp_rxq_info_init(xdp_rxq);
	xdp_rxq->dev = dev;
	xdp_rxq->queue_index = queue_index;

	xdp_rxq->reg_state = REG_STATE_REGISTERED;
	return 0;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_reg);

void xdp_rxq_info_unused(struct xdp_rxq_info *xdp_rxq)
{
	xdp_rxq->reg_state = REG_STATE_UNUSED;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_unused);

bool xdp_rxq_info_is_reg(struct xdp_rxq_info *xdp_rxq)
{
	return (xdp_rxq->reg_state == REG_STATE_REGISTERED);
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_is_reg);
