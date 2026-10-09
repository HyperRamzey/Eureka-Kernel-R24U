/*
 *  drivers/net/veth.c
 *
 *  Copyright (C) 2007 OpenVZ http://openvz.org, SWsoft Inc
 *
 * Author: Pavel Emelianov <xemul@openvz.org>
 * Ethtool interface from: Eric W. Biederman <ebiederm@xmission.com>
 *
 */

#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/ethtool.h>
#include <linux/etherdevice.h>
#include <net/xdp.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <net/page_pool.h>
#include <trace/events/xdp.h>
#include <linux/u64_stats_sync.h>

#include <net/rtnetlink.h>
#include <net/dst.h>
#include <net/xfrm.h>
#include <linux/veth.h>
#include <linux/module.h>

#define DRV_NAME	"veth"
#define DRV_VERSION	"1.0"

#define MIN_MTU 68		/* Min L3 MTU */
#define MAX_MTU 65535		/* Max L3 MTU (arbitrary) */

struct pcpu_vstats {
	u64			packets;
	u64			bytes;
	struct u64_stats_sync	syncp;
};

#define VETH_XDP_HEADROOM	256	/* v5.4 value: room for an xdp_frame */

/* One rxq per veth side.  4.4 veth is single-queue, and the XDP
 * path is synchronous (upstream defers through a ptr_ring + NAPI),
 * so the upstream veth_rq keeps only its file-local state here.
 */
struct veth_rq {
	struct page_pool	*pool;		/* page_pool consumer */
	struct xdp_rxq_info	xdp_rxq;
	struct xdp_mem_info	xdp_mem;	/* registered mem id+type */
	struct bpf_prog __rcu	*xdp_prog;
};

struct veth_priv {
	struct net_device __rcu	*peer;
	atomic64_t		dropped;
	struct bpf_prog		*_xdp_prog;
	struct veth_rq		rq;
};

/*
 * ethtool interface
 */

static struct {
	const char string[ETH_GSTRING_LEN];
} ethtool_stats_keys[] = {
	{ "peer_ifindex" },
};

static int veth_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	cmd->supported		= 0;
	cmd->advertising	= 0;
	ethtool_cmd_speed_set(cmd, SPEED_10000);
	cmd->duplex		= DUPLEX_FULL;
	cmd->port		= PORT_TP;
	cmd->phy_address	= 0;
	cmd->transceiver	= XCVR_INTERNAL;
	cmd->autoneg		= AUTONEG_DISABLE;
	cmd->maxtxpkt		= 0;
	cmd->maxrxpkt		= 0;
	return 0;
}

static void veth_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, DRV_VERSION, sizeof(info->version));
}

static void veth_get_strings(struct net_device *dev, u32 stringset, u8 *buf)
{
	switch(stringset) {
	case ETH_SS_STATS:
		memcpy(buf, &ethtool_stats_keys, sizeof(ethtool_stats_keys));
		break;
	}
}

static int veth_get_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return ARRAY_SIZE(ethtool_stats_keys);
	default:
		return -EOPNOTSUPP;
	}
}

static void veth_get_ethtool_stats(struct net_device *dev,
		struct ethtool_stats *stats, u64 *data)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer = rtnl_dereference(priv->peer);

	data[0] = peer ? peer->ifindex : 0;
}

static const struct ethtool_ops veth_ethtool_ops = {
	.get_settings		= veth_get_settings,
	.get_drvinfo		= veth_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_strings		= veth_get_strings,
	.get_sset_count		= veth_get_sset_count,
	.get_ethtool_stats	= veth_get_ethtool_stats,
};

static struct sk_buff *veth_build_skb(void *head, int headroom, int len,
				      int buflen)
{
	struct sk_buff *skb;

	if (!buflen) {
		buflen = SKB_DATA_ALIGN(headroom + len) +
			 SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	}
	skb = build_skb(head, buflen);
	if (!skb)
		return NULL;

	skb_reserve(skb, headroom);
	skb_put(skb, len);

	return skb;
}

/* Run the receiving device's XDP program on a forwarded skb.
 *
 * Ported from v5.4's veth_xdp_rcv_skb() minus its ptr_ring/NAPI batching:
 * the packet is processed inline, in the transmitter's xmit context.
 * 4.4 adaptations are marked inline.
 */
static int veth_xdp_rcv_skb(struct net_device *dev, struct veth_rq *rq,
		    struct sk_buff *skb)
{
	u32 pktlen, headroom, act;
	void *orig_data, *orig_data_end;
	struct bpf_prog *xdp_prog;
	int mac_len, delta, off;
	struct xdp_buff xdp;

	skb_orphan(skb);

	rcu_read_lock();
	xdp_prog = rcu_dereference(rq->xdp_prog);
	if (unlikely(!xdp_prog)) {
		rcu_read_unlock();
		return dev_forward_skb(dev, skb);
	}

	mac_len = skb->data - skb_mac_header(skb);
	pktlen = skb->len + mac_len;
	headroom = skb_headroom(skb) - mac_len;

	if (skb_shared(skb) || skb_head_is_locked(skb) ||
	    skb_is_nonlinear(skb) || headroom < XDP_PACKET_HEADROOM) {
		struct sk_buff *nskb;
		struct page *page;
		void *head, *start;

		if (pktlen > PAGE_SIZE - VETH_XDP_HEADROOM -
			      SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
			goto drop;

		/* page_pool consumer: v5.4 takes this page from alloc_page();
		 * here it comes from (and on failure returns to) the per-rq
		 * page_pool registered at attach time.
		 */
		page = page_pool_alloc_pages(rq->pool, GFP_ATOMIC | __GFP_NOWARN);
		if (!page)
			goto drop;

		head = page_address(page);
		start = head + VETH_XDP_HEADROOM;
		if (skb_copy_bits(skb, -mac_len, start, pktlen)) {
			page_pool_put_page(rq->pool, page, false);
			goto drop;
		}

		nskb = veth_build_skb(head, VETH_XDP_HEADROOM + mac_len,
				      skb->len, PAGE_SIZE);
		if (!nskb) {
			page_pool_put_page(rq->pool, page, false);
			goto drop;
		}

		/* 4.4 has no skb_copy_header()/skb_headers_offset_update() -
		 * carry the metadata the stack cares about explicitly.
		 */
		nskb->priority = skb->priority;
		nskb->mark = skb->mark;
		skb_copy_hash(nskb, skb);
		consume_skb(skb);
		skb = nskb;
	}

	xdp.data_hard_start = skb->head;
	xdp.data = skb_mac_header(skb);
	xdp.data_end = xdp.data + pktlen;
	xdp.data_meta = xdp.data;
	xdp.rxq = &rq->xdp_rxq;
	orig_data = xdp.data;
	orig_data_end = xdp.data_end;

	act = bpf_prog_run_xdp(xdp_prog, &xdp);

	switch (act) {
	case XDP_PASS:
		break;
	case XDP_TX:
		/* skb-based TX, same helper as the generic-XDP path */
		generic_xdp_tx(skb, xdp_prog);
		goto consumed;
	case XDP_REDIRECT:
		if (xdp_do_generic_redirect(dev, skb, &xdp, xdp_prog)) {
			/* redirect failed: the helper only reports it */
			kfree_skb(skb);
		}
		goto consumed;
	default:
		bpf_warn_invalid_xdp_action(act);
		/* fall through */
	case XDP_ABORTED:
		trace_xdp_exception(dev, xdp_prog, act);
		/* fall through */
	case XDP_DROP:
		goto drop;
	}
	rcu_read_unlock();

	delta = orig_data - xdp.data;
	off = mac_len + delta;
	if (off > 0)
		__skb_push(skb, off);
	else if (off < 0)
		__skb_pull(skb, -off);
	skb->mac_header -= delta;
	off = xdp.data_end - orig_data_end;
	if (off != 0)
		__skb_put(skb, off);
	skb->protocol = eth_type_trans(skb, dev);
	/* 4.4 adaptation: no skb_metadata_set()/meta_len, so data_meta is not
	 * propagated on PASS (same as the generic-XDP commit).
	 *
	 * Upstream returns the skb to its NAPI; this synchronous graft owns
	 * delivery, so deliver here.  Returning NET_RX_SUCCESS without
	 * delivering silently ate packets (seen on device).
	 */
	return dev_forward_skb(dev, skb);
drop:
	rcu_read_unlock();
	kfree_skb(skb);
	return NET_RX_DROP;
consumed:
	rcu_read_unlock();
	return NET_RX_DROP;
}

static int veth_xdp_rcv(struct net_device *dev, struct veth_rq *rq,
		    struct sk_buff *skb)
{
	if (!rcu_access_pointer(rq->xdp_prog))
		return dev_forward_skb(dev, skb);

	return veth_xdp_rcv_skb(dev, rq, skb);
}

static int veth_enable_xdp(struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct veth_rq *rq = &priv->rq;
	static const struct page_pool_params pp_params = {
		.pool_size = 256,
		.nid = NUMA_NO_NODE,
	};
	int err;

	if (rq->pool)
		return 0;

	/* page_pool consumer: one order-0 pool per rxq, registered as the
	 * rxq's XDP memory allocator (fires trace_mem_connect).
	 */
	rq->pool = page_pool_create(&pp_params);
	if (IS_ERR(rq->pool))
		return PTR_ERR(rq->pool);

	err = xdp_rxq_info_reg(&rq->xdp_rxq, dev, 0);
	if (err)
		goto err_pool;

	err = xdp_rxq_info_reg_mem_model(&rq->xdp_rxq, MEM_TYPE_PAGE_POOL,
				       rq->pool);
	if (err)
		goto err_rxq;

	/* Cache the registered mem info so the rx path can stamp it onto
	 * frames and xdp_return_buff()/xdp_return_frame() recycle into this

	 * pool instead of freeing to the buddy allocator.
	 */
	rq->xdp_mem = rq->xdp_rxq.mem;
	pr_info("veth: XDP attached on %s: page_pool=%p mem_id=%u "
		"type=%d (pool-backed copy path + frame recycling)\n",
		dev->name, rq->pool, rq->xdp_rxq.mem.id,
		rq->xdp_rxq.mem.type);

	rcu_assign_pointer(rq->xdp_prog, priv->_xdp_prog);
	return 0;

err_rxq:
	xdp_rxq_info_unreg(&rq->xdp_rxq);
err_pool:
	page_pool_free(rq->pool);
	rq->pool = NULL;
	return err;
}

static void veth_disable_xdp(struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct veth_rq *rq = &priv->rq;

	rcu_assign_pointer(rq->xdp_prog, NULL);
	/* wait for in-flight RX to stop touching the pool before teardown */
	synchronize_rcu();
	pr_info("veth: XDP detached on %s: page_pool torn down\n",
		dev->name);

	xdp_rxq_info_unreg_mem_model(&rq->xdp_rxq);	/* fires mem_disconnect */
	xdp_rxq_info_unreg(&rq->xdp_rxq);
	page_pool_free(rq->pool);
	rq->pool = NULL;
}

static int veth_xdp_set(struct net_device *dev, struct bpf_prog *prog)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct bpf_prog *old_prog;
	struct net_device *peer;
	unsigned int max_mtu;
	int err;

	old_prog = priv->_xdp_prog;
	priv->_xdp_prog = prog;
	peer = rtnl_dereference(priv->peer);

	if (prog) {
		if (!peer) {
			err = -ENOTCONN;
			goto err;
		}

		max_mtu = PAGE_SIZE - VETH_XDP_HEADROOM -
			  peer->hard_header_len -	
			  SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
		if (peer->mtu > max_mtu) {			err = -ERANGE;
			goto err;
		}

		err = veth_enable_xdp(dev);
		if (err)
			goto err;

		if (!old_prog)
			peer->hw_features &= ~NETIF_F_GSO_SOFTWARE;
	}

	if (old_prog) {
		if (!prog)
			veth_disable_xdp(dev);

		if (peer)
			peer->hw_features |= NETIF_F_GSO_SOFTWARE;

		bpf_prog_put(old_prog);
	}

	if ((!!old_prog ^ !!prog) && peer)
		netdev_update_features(peer);

	return 0;
err:
	priv->_xdp_prog = old_prog;

	return err;
}

static u32 veth_xdp_query(struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	const struct bpf_prog *xdp_prog;

	xdp_prog = priv->_xdp_prog;
	if (xdp_prog)
		return xdp_prog->aux->id;

	return 0;
}

static int veth_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return veth_xdp_set(dev, xdp->prog);
	case XDP_QUERY_PROG:
		xdp->prog_id = veth_xdp_query(dev);
		return 0;
	default:
		return -EINVAL;
	}
}

static netdev_tx_t veth_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct veth_priv *rcv_priv;
	int rc;

	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *rcv;
	int length = skb->len;

	rcu_read_lock();
	rcv = rcu_dereference(priv->peer);
	if (unlikely(!rcv) || !pskb_may_pull(skb, ETH_HLEN)) {
		kfree_skb(skb);
		goto drop;
	}

	rcv_priv = netdev_priv(rcv);
	if (rcu_access_pointer(rcv_priv->rq.xdp_prog)) {
		/* Native XDP on the receiving veth: hand it the packet with
		 * skb->dev already the receiver, exactly what
		 * dev_forward_skb() sets before netif_rx().
		 */
		skb->dev = rcv;
		rc = veth_xdp_rcv(rcv, &rcv_priv->rq, skb);
	} else {
		rc = dev_forward_skb(rcv, skb);
	}

	if (likely(rc == NET_RX_SUCCESS)) {
		struct pcpu_vstats *stats = this_cpu_ptr(dev->vstats);

		u64_stats_update_begin(&stats->syncp);
		stats->bytes += length;
		stats->packets++;
		u64_stats_update_end(&stats->syncp);
	} else {
drop:
		atomic64_inc(&priv->dropped);
	}
	rcu_read_unlock();
	return NETDEV_TX_OK;
}

/*
 * general routines
 */

static u64 veth_stats_one(struct pcpu_vstats *result, struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	int cpu;

	result->packets = 0;
	result->bytes = 0;
	for_each_possible_cpu(cpu) {
		struct pcpu_vstats *stats = per_cpu_ptr(dev->vstats, cpu);
		u64 packets, bytes;
		unsigned int start;

		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			packets = stats->packets;
			bytes = stats->bytes;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));
		result->packets += packets;
		result->bytes += bytes;
	}
	return atomic64_read(&priv->dropped);
}

static struct rtnl_link_stats64 *veth_get_stats64(struct net_device *dev,
						  struct rtnl_link_stats64 *tot)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer;
	struct pcpu_vstats one;

	tot->tx_dropped = veth_stats_one(&one, dev);
	tot->tx_bytes = one.bytes;
	tot->tx_packets = one.packets;

	rcu_read_lock();
	peer = rcu_dereference(priv->peer);
	if (peer) {
		tot->rx_dropped = veth_stats_one(&one, peer);
		tot->rx_bytes = one.bytes;
		tot->rx_packets = one.packets;
	}
	rcu_read_unlock();

	return tot;
}

/* fake multicast ability */
static void veth_set_multicast_list(struct net_device *dev)
{
}

static int veth_open(struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer = rtnl_dereference(priv->peer);

	if (!peer)
		return -ENOTCONN;

	if (peer->flags & IFF_UP) {
		netif_carrier_on(dev);
		netif_carrier_on(peer);
	}
	return 0;
}

static int veth_close(struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer = rtnl_dereference(priv->peer);

	netif_carrier_off(dev);
	if (peer)
		netif_carrier_off(peer);

	return 0;
}

static int is_valid_veth_mtu(int new_mtu)
{
	return new_mtu >= MIN_MTU && new_mtu <= MAX_MTU;
}

static int veth_change_mtu(struct net_device *dev, int new_mtu)
{
	if (!is_valid_veth_mtu(new_mtu))
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

static int veth_dev_init(struct net_device *dev)
{
	dev->vstats = netdev_alloc_pcpu_stats(struct pcpu_vstats);
	if (!dev->vstats)
		return -ENOMEM;
	return 0;
}

static void veth_dev_free(struct net_device *dev)
{
	free_percpu(dev->vstats);
	free_netdev(dev);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void veth_poll_controller(struct net_device *dev)
{
	/* veth only receives frames when its peer sends one
	 * Since it's a synchronous operation, we are guaranteed
	 * never to have pending data when we poll for it so
	 * there is nothing to do here.
	 *
	 * We need this though so netpoll recognizes us as an interface that
	 * supports polling, which enables bridge devices in virt setups to
	 * still use netconsole
	 */
}
#endif	/* CONFIG_NET_POLL_CONTROLLER */

static int veth_get_iflink(const struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer;
	int iflink;

	rcu_read_lock();
	peer = rcu_dereference(priv->peer);
	iflink = peer ? peer->ifindex : 0;
	rcu_read_unlock();

	return iflink;
}

static const struct net_device_ops veth_netdev_ops = {
	.ndo_init            = veth_dev_init,
	.ndo_open            = veth_open,
	.ndo_stop            = veth_close,
	.ndo_start_xmit      = veth_xmit,
	.ndo_change_mtu      = veth_change_mtu,
	.ndo_get_stats64     = veth_get_stats64,
	.ndo_set_rx_mode     = veth_set_multicast_list,
	.ndo_set_mac_address = eth_mac_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= veth_poll_controller,
#endif
	.ndo_get_iflink		= veth_get_iflink,
	.ndo_features_check	= passthru_features_check,
	.ndo_bpf		= veth_xdp,
};

#define VETH_FEATURES (NETIF_F_SG | NETIF_F_FRAGLIST | NETIF_F_ALL_TSO |    \
		       NETIF_F_HW_CSUM | NETIF_F_RXCSUM | NETIF_F_HIGHDMA | \
		       NETIF_F_GSO_GRE | NETIF_F_GSO_UDP_TUNNEL |	    \
		       NETIF_F_GSO_IPIP | NETIF_F_GSO_SIT | NETIF_F_UFO	|   \
		       NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX | \
		       NETIF_F_HW_VLAN_STAG_TX | NETIF_F_HW_VLAN_STAG_RX )

static void veth_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->priv_flags &= ~IFF_TX_SKB_SHARING;
	dev->priv_flags |= IFF_LIVE_ADDR_CHANGE;
	dev->priv_flags |= IFF_NO_QUEUE;

	dev->netdev_ops = &veth_netdev_ops;
	dev->ethtool_ops = &veth_ethtool_ops;
	dev->features |= NETIF_F_LLTX;
	dev->features |= VETH_FEATURES;
	dev->vlan_features = dev->features &
			     ~(NETIF_F_HW_VLAN_CTAG_TX |
			       NETIF_F_HW_VLAN_STAG_TX |
			       NETIF_F_HW_VLAN_CTAG_RX |
			       NETIF_F_HW_VLAN_STAG_RX);
	dev->destructor = veth_dev_free;

	dev->hw_features = VETH_FEATURES;
	dev->hw_enc_features = VETH_FEATURES;
}

/*
 * netlink interface
 */

static int veth_validate(struct nlattr *tb[], struct nlattr *data[])
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}
	if (tb[IFLA_MTU]) {
		if (!is_valid_veth_mtu(nla_get_u32(tb[IFLA_MTU])))
			return -EINVAL;
	}
	return 0;
}

static struct rtnl_link_ops veth_link_ops;

static int veth_newlink(struct net *src_net, struct net_device *dev,
			 struct nlattr *tb[], struct nlattr *data[])
{
	int err;
	struct net_device *peer;
	struct veth_priv *priv;
	char ifname[IFNAMSIZ];
	struct nlattr *peer_tb[IFLA_MAX + 1], **tbp;
	unsigned char name_assign_type;
	struct ifinfomsg *ifmp;
	struct net *net;

	/*
	 * create and register peer first
	 */
	if (data != NULL && data[VETH_INFO_PEER] != NULL) {
		struct nlattr *nla_peer;

		nla_peer = data[VETH_INFO_PEER];
		ifmp = nla_data(nla_peer);
		err = rtnl_nla_parse_ifla(peer_tb,
					  nla_data(nla_peer) + sizeof(struct ifinfomsg),
					  nla_len(nla_peer) - sizeof(struct ifinfomsg));
		if (err < 0)
			return err;

		err = veth_validate(peer_tb, NULL);
		if (err < 0)
			return err;

		tbp = peer_tb;
	} else {
		ifmp = NULL;
		tbp = tb;
	}

	if (tbp[IFLA_IFNAME]) {
		nla_strlcpy(ifname, tbp[IFLA_IFNAME], IFNAMSIZ);
		name_assign_type = NET_NAME_USER;
	} else {
		snprintf(ifname, IFNAMSIZ, DRV_NAME "%%d");
		name_assign_type = NET_NAME_ENUM;
	}

	net = rtnl_link_get_net(src_net, tbp);
	if (IS_ERR(net))
		return PTR_ERR(net);

	peer = rtnl_create_link(net, ifname, name_assign_type,
				&veth_link_ops, tbp);
	if (IS_ERR(peer)) {
		put_net(net);
		return PTR_ERR(peer);
	}

	if (tbp[IFLA_ADDRESS] == NULL)
		eth_hw_addr_random(peer);

	if (ifmp && (dev->ifindex != 0))
		peer->ifindex = ifmp->ifi_index;

	peer->gso_max_size = dev->gso_max_size;
	peer->gso_max_segs = dev->gso_max_segs;

	err = register_netdevice(peer);
	put_net(net);
	net = NULL;
	if (err < 0)
		goto err_register_peer;

	netif_carrier_off(peer);

	err = rtnl_configure_link(peer, ifmp);
	if (err < 0)
		goto err_configure_peer;

	/*
	 * register dev last
	 *
	 * note, that since we've registered new device the dev's name
	 * should be re-allocated
	 */

	if (tb[IFLA_ADDRESS] == NULL)
		eth_hw_addr_random(dev);

	if (tb[IFLA_IFNAME])
		nla_strlcpy(dev->name, tb[IFLA_IFNAME], IFNAMSIZ);
	else
		snprintf(dev->name, IFNAMSIZ, DRV_NAME "%%d");

	err = register_netdevice(dev);
	if (err < 0)
		goto err_register_dev;

	netif_carrier_off(dev);

	/*
	 * tie the deviced together
	 */

	priv = netdev_priv(dev);
	rcu_assign_pointer(priv->peer, peer);

	priv = netdev_priv(peer);
	rcu_assign_pointer(priv->peer, dev);
	return 0;

err_register_dev:
	/* nothing to do */
err_configure_peer:
	unregister_netdevice(peer);
	return err;

err_register_peer:
	free_netdev(peer);
	return err;
}

static void veth_dellink(struct net_device *dev, struct list_head *head)
{
	struct veth_priv *priv;
	struct net_device *peer;

	priv = netdev_priv(dev);
	peer = rtnl_dereference(priv->peer);

	/* Note : dellink() is called from default_device_exit_batch(),
	 * before a rcu_synchronize() point. The devices are guaranteed
	 * not being freed before one RCU grace period.
	 */
	RCU_INIT_POINTER(priv->peer, NULL);
	unregister_netdevice_queue(dev, head);

	if (peer) {
		priv = netdev_priv(peer);
		RCU_INIT_POINTER(priv->peer, NULL);
		unregister_netdevice_queue(peer, head);
	}
}

static const struct nla_policy veth_policy[VETH_INFO_MAX + 1] = {
	[VETH_INFO_PEER]	= { .len = sizeof(struct ifinfomsg) },
};

static struct net *veth_get_link_net(const struct net_device *dev)
{
	struct veth_priv *priv = netdev_priv(dev);
	struct net_device *peer = rtnl_dereference(priv->peer);

	return peer ? dev_net(peer) : dev_net(dev);
}

static struct rtnl_link_ops veth_link_ops = {
	.kind		= DRV_NAME,
	.priv_size	= sizeof(struct veth_priv),
	.setup		= veth_setup,
	.validate	= veth_validate,
	.newlink	= veth_newlink,
	.dellink	= veth_dellink,
	.policy		= veth_policy,
	.maxtype	= VETH_INFO_MAX,
	.get_link_net	= veth_get_link_net,
};

/*
 * init/fini
 */

static __init int veth_init(void)
{
	return rtnl_link_register(&veth_link_ops);
}

static __exit void veth_exit(void)
{
	rtnl_link_unregister(&veth_link_ops);
}

module_init(veth_init);
module_exit(veth_exit);

MODULE_DESCRIPTION("Virtual Ethernet Tunnel");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS_RTNL_LINK(DRV_NAME);
