#undef TRACE_SYSTEM
#define TRACE_SYSTEM xdp

#if !defined(_TRACE_XDP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_XDP_H

#include <linux/netdevice.h>
#include <linux/filter.h>
#include <linux/tracepoint.h>

#define __XDP_ACT_MAP(FN)	\
	FN(ABORTED)		\
	FN(DROP)		\
	FN(PASS)		\
	FN(TX)			\
	FN(REDIRECT)

#define __XDP_ACT_TP_FN(x)	\
	TRACE_DEFINE_ENUM(XDP_##x);
#define __XDP_ACT_SYM_FN(x)	\
	{ XDP_##x, #x },
#define __XDP_ACT_SYM_TAB	\
	__XDP_ACT_MAP(__XDP_ACT_SYM_FN) { -1, 0 }
__XDP_ACT_MAP(__XDP_ACT_TP_FN)

TRACE_EVENT(xdp_exception,

	TP_PROTO(const struct net_device *dev,
		 const struct bpf_prog *xdp, u32 act),

	TP_ARGS(dev, xdp, act),

	TP_STRUCT__entry(
		__string(name, dev->name)
		__array(u8, prog_tag, 8)
		__field(u32, act)
	),

	TP_fast_assign(
		BUILD_BUG_ON(sizeof(__entry->prog_tag) != sizeof(xdp->tag));
		memcpy(__entry->prog_tag, xdp->tag, sizeof(xdp->tag));
		__assign_str(name, dev->name);
		__entry->act = act;
	),

	TP_printk("prog=%s device=%s action=%s",
		  __print_hex_str(__entry->prog_tag, 8),
		  __get_str(name),
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB))
);

TRACE_EVENT(xdp_redirect,

	TP_PROTO(const struct net_device *from,
		 const struct net_device *to,
		 const struct bpf_prog *xdp, u32 act),

	TP_ARGS(from, to, xdp, act),

	TP_STRUCT__entry(
		__string(name_from, from->name)
		__string(name_to, to->name)
		__array(u8, prog_tag, 8)
		__field(u32, act)
	),

	TP_fast_assign(
		BUILD_BUG_ON(sizeof(__entry->prog_tag) != sizeof(xdp->tag));
		memcpy(__entry->prog_tag, xdp->tag, sizeof(xdp->tag));
		__assign_str(name_from, from->name);
		__assign_str(name_to, to->name);
		__entry->act = act;
	),

	TP_printk("prog=%s from=%s to=%s action=%s",
		  __print_hex_str(__entry->prog_tag, 8),
		  __get_str(name_from), __get_str(name_to),
		  __print_symbolic(__entry->act, __XDP_ACT_SYM_TAB))
);
TRACE_EVENT(mem_disconnect,

	TP_PROTO(const struct xdp_mem_allocator *xa,
		 bool safe_to_remove, bool force),

	TP_ARGS(xa, safe_to_remove, force),

	TP_STRUCT__entry(
		__field(const struct xdp_mem_allocator *,	xa)
		__field(u32,		mem_id)
		__field(u32,		mem_type)
		__field(const void *,	allocator)
		__field(bool,		safe_to_remove)
		__field(bool,		force)
		__field(int,		disconnect_cnt)
	),

	TP_fast_assign(
		__entry->xa		= xa;
		__entry->mem_id		= xa->mem.id;
		__entry->mem_type	= xa->mem.type;
		__entry->allocator	= xa->allocator;
		__entry->safe_to_remove	= safe_to_remove;
		__entry->force		= force;
		__entry->disconnect_cnt	= xa->disconnect_cnt;
	),

	TP_printk("mem_id=%d mem_type=%s allocator=%p"
		  " safe_to_remove=%s force=%s disconnect_cnt=%d",
		  __entry->mem_id,
		  __print_symbolic(__entry->mem_type, __MEM_TYPE_SYM_TAB),
		  __entry->allocator,
		  __entry->safe_to_remove ? "true" : "false",
		  __entry->force ? "true" : "false",
		  __entry->disconnect_cnt
	)
);

TRACE_EVENT(mem_connect,

	TP_PROTO(const struct xdp_mem_allocator *xa,
		 const struct xdp_rxq_info *rxq),

	TP_ARGS(xa, rxq),

	TP_STRUCT__entry(
		__field(const struct xdp_mem_allocator *,	xa)
		__field(u32,		mem_id)
		__field(u32,		mem_type)
		__field(const void *,	allocator)
		__field(const struct xdp_rxq_info *,		rxq)
		__field(int,		ifindex)
	),

	TP_fast_assign(
		__entry->xa		= xa;
		__entry->mem_id		= xa->mem.id;
		__entry->mem_type	= xa->mem.type;
		__entry->allocator	= xa->allocator;
		__entry->rxq		= rxq;
		__entry->ifindex	= rxq->dev->ifindex;
	),

	TP_printk("mem_id=%d mem_type=%s allocator=%p"
		  " ifindex=%d",
		  __entry->mem_id,
		  __print_symbolic(__entry->mem_type, __MEM_TYPE_SYM_TAB),
		  __entry->allocator,
		  __entry->ifindex
	)
);

TRACE_EVENT(mem_return_failed,

	TP_PROTO(const struct xdp_mem_info *mem,
		 const struct page *page),

	TP_ARGS(mem, page),

	TP_STRUCT__entry(
		__field(const struct page *,	page)
		__field(u32,		mem_id)
		__field(u32,		mem_type)
	),

	TP_fast_assign(
		__entry->page		= page;
		__entry->mem_id		= mem->id;
		__entry->mem_type	= mem->type;
	),

	TP_printk("mem_id=%d mem_type=%s page=%p",
		  __entry->mem_id,
		  __print_symbolic(__entry->mem_type, __MEM_TYPE_SYM_TAB),
		  __entry->page
	)
);

#endif /* _TRACE_XDP_H */

#include <trace/define_trace.h>
