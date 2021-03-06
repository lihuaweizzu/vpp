/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vnet/vnet.h>
#include <vppinfra/vec.h>
#include <vppinfra/error.h>
#include <vppinfra/format.h>
#include <vppinfra/xxhash.h>

#include <vnet/ethernet/ethernet.h>
#include <dpdk/device/dpdk.h>
#include <vnet/classify/vnet_classify.h>
#include <vnet/mpls/packet.h>
#include <vnet/handoff.h>
#include <vnet/devices/devices.h>
#include <vnet/feature/feature.h>

#include <dpdk/device/dpdk_priv.h>

#ifndef CLIB_MULTIARCH_VARIANT
static char *dpdk_error_strings[] = {
#define _(n,s) s,
  foreach_dpdk_error
#undef _
};
#endif

always_inline u32
dpdk_rx_next_from_etype (struct rte_mbuf *mb)
{
  ethernet_header_t *h = rte_pktmbuf_mtod (mb, ethernet_header_t *);
  if (PREDICT_TRUE (h->type == clib_host_to_net_u16 (ETHERNET_TYPE_IP4)))
    {
      if (PREDICT_TRUE ((mb->ol_flags & PKT_RX_IP_CKSUM_GOOD) != 0))
	return VNET_DEVICE_INPUT_NEXT_IP4_NCS_INPUT;
      else
	return VNET_DEVICE_INPUT_NEXT_IP4_INPUT;
    }
  else if (PREDICT_TRUE (h->type == clib_host_to_net_u16 (ETHERNET_TYPE_IP6)))
    return VNET_DEVICE_INPUT_NEXT_IP6_INPUT;
  else
    if (PREDICT_TRUE (h->type == clib_host_to_net_u16 (ETHERNET_TYPE_MPLS)))
    return VNET_DEVICE_INPUT_NEXT_MPLS_INPUT;
  else
    return VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;
}

always_inline void
dpdk_rx_error_from_mb (struct rte_mbuf *mb, u32 * next, u8 * error)
{
  if (mb->ol_flags & PKT_RX_IP_CKSUM_BAD)
    {
      *error = DPDK_ERROR_IP_CHECKSUM_ERROR;
      *next = VNET_DEVICE_INPUT_NEXT_DROP;
    }
  else
    *error = DPDK_ERROR_NONE;
}

static_always_inline void
dpdk_add_trace (vlib_main_t * vm, vlib_node_runtime_t * node, u32 next,
		dpdk_device_t * xd, u16 queue_id,
		vlib_buffer_t * b, struct rte_mbuf *mb)
{
  vlib_trace_buffer (vm, node, next, b, /* follow_chain */ 0);

  dpdk_rx_dma_trace_t *t0 = vlib_add_trace (vm, node, b, sizeof t0[0]);
  t0->queue_index = queue_id;
  t0->device_index = xd->device_index;
  t0->buffer_index = vlib_get_buffer_index (vm, b);

  clib_memcpy (&t0->mb, mb, sizeof t0->mb);
  clib_memcpy (&t0->buffer, b, sizeof b[0] - sizeof b->pre_data);
  clib_memcpy (t0->buffer.pre_data, b->data, sizeof t0->buffer.pre_data);
  clib_memcpy (&t0->data, mb->buf_addr + mb->data_off, sizeof t0->data);
}

static inline u32
dpdk_rx_burst (dpdk_main_t * dm, dpdk_device_t * xd, u16 queue_id)
{
  u32 n_buffers;
  u32 n_left;
  u32 n_this_chunk;

  n_left = VLIB_FRAME_SIZE;
  n_buffers = 0;

  if (PREDICT_TRUE (xd->flags & DPDK_DEVICE_FLAG_PMD))
    {
      while (n_left)
	{
	  n_this_chunk = rte_eth_rx_burst (xd->device_index, queue_id,
					   xd->rx_vectors[queue_id] +
					   n_buffers, n_left);
	  n_buffers += n_this_chunk;
	  n_left -= n_this_chunk;

	  /* Empirically, DPDK r1.8 produces vectors w/ 32 or fewer elts */
	  if (n_this_chunk < 32)
	    break;
	}
    }
  else
    {
      ASSERT (0);
    }

  return n_buffers;
}

static_always_inline void
dpdk_process_subseq_segs (vlib_main_t * vm, vlib_buffer_t * b,
			  struct rte_mbuf *mb, vlib_buffer_free_list_t * fl)
{
  u8 nb_seg = 1;
  struct rte_mbuf *mb_seg = 0;
  vlib_buffer_t *b_seg, *b_chain = 0;
  mb_seg = mb->next;
  b_chain = b;

  if (mb->nb_segs < 2)
    return;

  b->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;
  b->total_length_not_including_first_buffer = 0;

  while (nb_seg < mb->nb_segs)
    {
      ASSERT (mb_seg != 0);

      b_seg = vlib_buffer_from_rte_mbuf (mb_seg);
      vlib_buffer_init_for_free_list (b_seg, fl);

      ASSERT ((b_seg->flags & VLIB_BUFFER_NEXT_PRESENT) == 0);
      ASSERT (b_seg->current_data == 0);

      /*
       * The driver (e.g. virtio) may not put the packet data at the start
       * of the segment, so don't assume b_seg->current_data == 0 is correct.
       */
      b_seg->current_data =
	(mb_seg->buf_addr + mb_seg->data_off) - (void *) b_seg->data;

      b_seg->current_length = mb_seg->data_len;
      b->total_length_not_including_first_buffer += mb_seg->data_len;

      b_chain->flags |= VLIB_BUFFER_NEXT_PRESENT;
      b_chain->next_buffer = vlib_get_buffer_index (vm, b_seg);

      b_chain = b_seg;
      mb_seg = mb_seg->next;
      nb_seg++;
    }
}

static_always_inline void
dpdk_prefetch_buffer (struct rte_mbuf *mb)
{
  vlib_buffer_t *b = vlib_buffer_from_rte_mbuf (mb);
  CLIB_PREFETCH (mb, CLIB_CACHE_LINE_BYTES, LOAD);
  CLIB_PREFETCH (b, CLIB_CACHE_LINE_BYTES, STORE);
}

static_always_inline void
dpdk_prefetch_ethertype (struct rte_mbuf *mb)
{
  CLIB_PREFETCH (mb->buf_addr + mb->data_off +
		 STRUCT_OFFSET_OF (ethernet_header_t, type),
		 CLIB_CACHE_LINE_BYTES, LOAD);
}

/*
 * This function is used when there are no worker threads.
 * The main thread performs IO and forwards the packets.
 */
static_always_inline u32
dpdk_device_input (dpdk_main_t * dm, dpdk_device_t * xd,
		   vlib_node_runtime_t * node, u32 thread_index, u16 queue_id,
		   int maybe_multiseg, u32 n_trace)
{
  u32 n_buffers;
  u32 next_index = VNET_DEVICE_INPUT_NEXT_ETHERNET_INPUT;
  u32 n_left_to_next, *to_next;
  u32 mb_index;
  vlib_main_t *vm = vlib_get_main ();
  uword n_rx_bytes = 0;
  vlib_buffer_free_list_t *fl;
  vlib_buffer_t *bt = vec_elt_at_index (dm->buffer_templates, thread_index);

  if ((xd->flags & DPDK_DEVICE_FLAG_ADMIN_UP) == 0)
    return 0;

  n_buffers = dpdk_rx_burst (dm, xd, queue_id);

  if (n_buffers == 0)
    {
      return 0;
    }

  fl = vlib_buffer_get_free_list (vm, VLIB_BUFFER_DEFAULT_FREE_LIST_INDEX);

  /* Update buffer template */
  vnet_buffer (bt)->sw_if_index[VLIB_RX] = xd->vlib_sw_if_index;
  bt->error = node->errors[DPDK_ERROR_NONE];
  /* as DPDK is allocating empty buffers from mempool provided before interface
     start for each queue, it is safe to store this in the template */
  bt->buffer_pool_index = xd->buffer_pool_for_queue[queue_id];

  mb_index = 0;

  while (n_buffers > 0)
    {
      vlib_buffer_t *b0, *b1, *b2, *b3;
      u32 bi0, next0;
      u32 bi1, next1;
      u32 bi2, next2;
      u32 bi3, next3;
      u8 error0, error1, error2, error3;
      i16 offset0, offset1, offset2, offset3;
      u64 or_ol_flags;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_buffers >= 12 && n_left_to_next >= 4)
	{
	  struct rte_mbuf *mb0, *mb1, *mb2, *mb3;

	  /* prefetches are interleaved with the rest of the code to reduce
	     pressure on L1 cache */
	  dpdk_prefetch_buffer (xd->rx_vectors[queue_id][mb_index + 8]);
	  dpdk_prefetch_ethertype (xd->rx_vectors[queue_id][mb_index + 4]);

	  mb0 = xd->rx_vectors[queue_id][mb_index];
	  mb1 = xd->rx_vectors[queue_id][mb_index + 1];
	  mb2 = xd->rx_vectors[queue_id][mb_index + 2];
	  mb3 = xd->rx_vectors[queue_id][mb_index + 3];

	  ASSERT (mb0);
	  ASSERT (mb1);
	  ASSERT (mb2);
	  ASSERT (mb3);

	  if (maybe_multiseg)
	    {
	      if (PREDICT_FALSE (mb0->nb_segs > 1))
		dpdk_prefetch_buffer (mb0->next);
	      if (PREDICT_FALSE (mb1->nb_segs > 1))
		dpdk_prefetch_buffer (mb1->next);
	      if (PREDICT_FALSE (mb2->nb_segs > 1))
		dpdk_prefetch_buffer (mb2->next);
	      if (PREDICT_FALSE (mb3->nb_segs > 1))
		dpdk_prefetch_buffer (mb3->next);
	    }

	  b0 = vlib_buffer_from_rte_mbuf (mb0);
	  b1 = vlib_buffer_from_rte_mbuf (mb1);
	  b2 = vlib_buffer_from_rte_mbuf (mb2);
	  b3 = vlib_buffer_from_rte_mbuf (mb3);

	  dpdk_prefetch_buffer (xd->rx_vectors[queue_id][mb_index + 9]);
	  dpdk_prefetch_ethertype (xd->rx_vectors[queue_id][mb_index + 5]);

	  clib_memcpy64_x4 (b0, b1, b2, b3, bt);

	  dpdk_prefetch_buffer (xd->rx_vectors[queue_id][mb_index + 10]);
	  dpdk_prefetch_ethertype (xd->rx_vectors[queue_id][mb_index + 6]);

	  bi0 = vlib_get_buffer_index (vm, b0);
	  bi1 = vlib_get_buffer_index (vm, b1);
	  bi2 = vlib_get_buffer_index (vm, b2);
	  bi3 = vlib_get_buffer_index (vm, b3);

	  to_next[0] = bi0;
	  to_next[1] = bi1;
	  to_next[2] = bi2;
	  to_next[3] = bi3;
	  to_next += 4;
	  n_left_to_next -= 4;

	  if (PREDICT_FALSE (xd->per_interface_next_index != ~0))
	    {
	      next0 = next1 = next2 = next3 = xd->per_interface_next_index;
	    }
	  else
	    {
	      next0 = dpdk_rx_next_from_etype (mb0);
	      next1 = dpdk_rx_next_from_etype (mb1);
	      next2 = dpdk_rx_next_from_etype (mb2);
	      next3 = dpdk_rx_next_from_etype (mb3);

	      or_ol_flags = (mb0->ol_flags | mb1->ol_flags |
			     mb2->ol_flags | mb3->ol_flags);
	      if (PREDICT_FALSE (or_ol_flags & PKT_RX_IP_CKSUM_BAD))
		{
		  dpdk_rx_error_from_mb (mb0, &next0, &error0);
		  dpdk_rx_error_from_mb (mb1, &next1, &error1);
		  dpdk_rx_error_from_mb (mb2, &next2, &error2);
		  dpdk_rx_error_from_mb (mb3, &next3, &error3);
		  b0->error = node->errors[error0];
		  b1->error = node->errors[error1];
		  b2->error = node->errors[error2];
		  b3->error = node->errors[error3];
		}
	    }

	  dpdk_prefetch_buffer (xd->rx_vectors[queue_id][mb_index + 11]);
	  dpdk_prefetch_ethertype (xd->rx_vectors[queue_id][mb_index + 7]);

	  offset0 = device_input_next_node_advance[next0];
	  b0->current_data = mb0->data_off + offset0 - RTE_PKTMBUF_HEADROOM;
	  b0->flags |= device_input_next_node_flags[next0];
	  vnet_buffer (b0)->l3_hdr_offset = b0->current_data;
	  vnet_buffer (b0)->l2_hdr_offset =
	    mb0->data_off - RTE_PKTMBUF_HEADROOM;
	  b0->current_length = mb0->data_len - offset0;
	  n_rx_bytes += mb0->pkt_len;

	  offset1 = device_input_next_node_advance[next1];
	  b1->current_data = mb1->data_off + offset1 - RTE_PKTMBUF_HEADROOM;
	  b1->flags |= device_input_next_node_flags[next1];
	  vnet_buffer (b1)->l3_hdr_offset = b1->current_data;
	  vnet_buffer (b1)->l2_hdr_offset =
	    mb1->data_off - RTE_PKTMBUF_HEADROOM;
	  b1->current_length = mb1->data_len - offset1;
	  n_rx_bytes += mb1->pkt_len;

	  offset2 = device_input_next_node_advance[next2];
	  b2->current_data = mb2->data_off + offset2 - RTE_PKTMBUF_HEADROOM;
	  b2->flags |= device_input_next_node_flags[next2];
	  vnet_buffer (b2)->l3_hdr_offset = b2->current_data;
	  vnet_buffer (b2)->l2_hdr_offset =
	    mb2->data_off - RTE_PKTMBUF_HEADROOM;
	  b2->current_length = mb2->data_len - offset2;
	  n_rx_bytes += mb2->pkt_len;

	  offset3 = device_input_next_node_advance[next3];
	  b3->current_data = mb3->data_off + offset3 - RTE_PKTMBUF_HEADROOM;
	  b3->flags |= device_input_next_node_flags[next3];
	  vnet_buffer (b3)->l3_hdr_offset = b3->current_data;
	  vnet_buffer (b3)->l2_hdr_offset =
	    mb3->data_off - RTE_PKTMBUF_HEADROOM;
	  b3->current_length = mb3->data_len - offset3;
	  n_rx_bytes += mb3->pkt_len;


	  /* Process subsequent segments of multi-segment packets */
	  if (maybe_multiseg)
	    {
	      dpdk_process_subseq_segs (vm, b0, mb0, fl);
	      dpdk_process_subseq_segs (vm, b1, mb1, fl);
	      dpdk_process_subseq_segs (vm, b2, mb2, fl);
	      dpdk_process_subseq_segs (vm, b3, mb3, fl);
	    }

	  /*
	   * Turn this on if you run into
	   * "bad monkey" contexts, and you want to know exactly
	   * which nodes they've visited... See main.c...
	   */
	  VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b0);
	  VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b1);
	  VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b2);
	  VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b3);

	  /* Do we have any driver RX features configured on the interface? */
	  vnet_feature_start_device_input_x4 (xd->vlib_sw_if_index,
					      &next0, &next1, &next2, &next3,
					      b0, b1, b2, b3);

	  vlib_validate_buffer_enqueue_x4 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, bi1, bi2, bi3,
					   next0, next1, next2, next3);
	  n_buffers -= 4;
	  mb_index += 4;

	  if (n_trace)
	    {
	      dpdk_add_trace (vm, node, next0, xd, queue_id, b0, mb0);
	      n_trace--;
	    }
	  if (n_trace)
	    {
	      dpdk_add_trace (vm, node, next1, xd, queue_id, b1, mb1);
	      n_trace--;
	    }
	  if (n_trace)
	    {
	      dpdk_add_trace (vm, node, next2, xd, queue_id, b2, mb2);
	      n_trace--;
	    }
	  if (n_trace)
	    {
	      dpdk_add_trace (vm, node, next3, xd, queue_id, b3, mb3);
	      n_trace--;
	    }
	}
      while (n_buffers > 0 && n_left_to_next > 0)
	{
	  struct rte_mbuf *mb0 = xd->rx_vectors[queue_id][mb_index];

	  if (PREDICT_TRUE (n_buffers > 3))
	    {
	      dpdk_prefetch_buffer (xd->rx_vectors[queue_id][mb_index + 2]);
	      dpdk_prefetch_ethertype (xd->rx_vectors[queue_id]
				       [mb_index + 1]);
	    }

	  ASSERT (mb0);

	  b0 = vlib_buffer_from_rte_mbuf (mb0);

	  /* Prefetch one next segment if it exists. */
	  if (PREDICT_FALSE (mb0->nb_segs > 1))
	    dpdk_prefetch_buffer (mb0->next);

	  clib_memcpy (b0, bt, CLIB_CACHE_LINE_BYTES);

	  bi0 = vlib_get_buffer_index (vm, b0);

	  to_next[0] = bi0;
	  to_next++;
	  n_left_to_next--;

	  if (PREDICT_FALSE (xd->per_interface_next_index != ~0))
	    next0 = xd->per_interface_next_index;
	  else
	    {
	      next0 = dpdk_rx_next_from_etype (mb0);

	      dpdk_rx_error_from_mb (mb0, &next0, &error0);
	      b0->error = node->errors[error0];
	    }

	  offset0 = device_input_next_node_advance[next0];
	  b0->current_data = mb0->data_off + offset0 - RTE_PKTMBUF_HEADROOM;
	  b0->flags |= device_input_next_node_flags[next0];
	  vnet_buffer (b0)->l3_hdr_offset = b0->current_data;
	  vnet_buffer (b0)->l2_hdr_offset =
	    mb0->data_off - RTE_PKTMBUF_HEADROOM;
	  b0->current_length = mb0->data_len - offset0;
	  n_rx_bytes += mb0->pkt_len;

	  /* Process subsequent segments of multi-segment packets */
	  dpdk_process_subseq_segs (vm, b0, mb0, fl);

	  /*
	   * Turn this on if you run into
	   * "bad monkey" contexts, and you want to know exactly
	   * which nodes they've visited... See main.c...
	   */
	  VLIB_BUFFER_TRACE_TRAJECTORY_INIT (b0);

	  /* Do we have any driver RX features configured on the interface? */
	  vnet_feature_start_device_input_x1 (xd->vlib_sw_if_index, &next0,
					      b0);

	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, next0);
	  n_buffers--;
	  mb_index++;

	  if (n_trace)
	    {
	      dpdk_add_trace (vm, node, next0, xd, queue_id, b0, mb0);
	      n_trace--;
	    }
	}
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_increment_combined_counter
    (vnet_get_main ()->interface_main.combined_sw_if_counters
     + VNET_INTERFACE_COUNTER_RX,
     thread_index, xd->vlib_sw_if_index, mb_index, n_rx_bytes);

  vnet_device_increment_rx_packets (thread_index, mb_index);

  return mb_index;
}

static_always_inline u32
dpdk_device_input_mseg (dpdk_main_t * dm, dpdk_device_t * xd,
			vlib_node_runtime_t * node, u32 thread_index,
			u16 queue_id, u32 n_trace)
{
  if (xd->flags & DPDK_DEVICE_FLAG_MAYBE_MULTISEG)
    return dpdk_device_input (dm, xd, node, thread_index, queue_id,
			      /* maybe_multiseg */ 1, n_trace);
  else
    return dpdk_device_input (dm, xd, node, thread_index, queue_id,
			      /* maybe_multiseg */ 0, n_trace);
}

static inline void
poll_rate_limit (dpdk_main_t * dm)
{
  /* Limit the poll rate by sleeping for N msec between polls */
  if (PREDICT_FALSE (dm->poll_sleep_usec != 0))
    {
      struct timespec ts, tsrem;

      ts.tv_sec = 0;
      ts.tv_nsec = 1000 * dm->poll_sleep_usec;

      while (nanosleep (&ts, &tsrem) < 0)
	{
	  ts = tsrem;
	}
    }
}

/** \brief Main DPDK input node
    @node dpdk-input

    This is the main DPDK input node: across each assigned interface,
    call rte_eth_rx_burst(...) or similar to obtain a vector of
    packets to process. Handle early packet discard. Derive @c
    vlib_buffer_t metadata from <code>struct rte_mbuf</code> metadata,
    Depending on the resulting metadata: adjust <code>b->current_data,
    b->current_length </code> and dispatch directly to
    ip4-input-no-checksum, or ip6-input. Trace the packet if required.

    @param vm   vlib_main_t corresponding to the current thread
    @param node vlib_node_runtime_t
    @param f    vlib_frame_t input-node, not used.

    @par Graph mechanics: buffer metadata, next index usage

    @em Uses:
    - <code>struct rte_mbuf mb->ol_flags</code>
        - PKT_RX_IP_CKSUM_BAD
    - <code> RTE_ETH_IS_xxx_HDR(mb->packet_type) </code>
        - packet classification result

    @em Sets:
    - <code>b->error</code> if the packet is to be dropped immediately
    - <code>b->current_data, b->current_length</code>
        - adjusted as needed to skip the L2 header in  direct-dispatch cases
    - <code>vnet_buffer(b)->sw_if_index[VLIB_RX]</code>
        - rx interface sw_if_index
    - <code>vnet_buffer(b)->sw_if_index[VLIB_TX] = ~0</code>
        - required by ipX-lookup
    - <code>b->flags</code>
        - to indicate multi-segment pkts (VLIB_BUFFER_NEXT_PRESENT), etc.

    <em>Next Nodes:</em>
    - Static arcs to: error-drop, ethernet-input,
      ip4-input-no-checksum, ip6-input, mpls-input
    - per-interface redirection, controlled by
      <code>xd->per_interface_next_index</code>
*/

uword
CLIB_MULTIARCH_FN (dpdk_input) (vlib_main_t * vm, vlib_node_runtime_t * node,
				vlib_frame_t * f)
{
  dpdk_main_t *dm = &dpdk_main;
  dpdk_device_t *xd;
  uword n_rx_packets = 0;
  vnet_device_input_runtime_t *rt = (void *) node->runtime_data;
  vnet_device_and_queue_t *dq;
  u32 thread_index = node->thread_index;

  /*
   * Poll all devices on this cpu for input/interrupts.
   */
  /* *INDENT-OFF* */
  foreach_device_and_queue (dq, rt->devices_and_queues)
    {
      xd = vec_elt_at_index(dm->devices, dq->dev_instance);
      if (PREDICT_FALSE (xd->flags & DPDK_DEVICE_FLAG_BOND_SLAVE))
	continue; 	/* Do not poll slave to a bonded interface */
      u32 n_trace = vlib_get_trace_count (vm, node);
      if (PREDICT_TRUE(n_trace == 0))
        n_rx_packets += dpdk_device_input_mseg (dm, xd, node, thread_index,
						dq->queue_id, 0);
      else
        {
          u32 n_tr_packets = dpdk_device_input_mseg (dm, xd, node, thread_index,
						     dq->queue_id, n_trace);
          n_rx_packets += n_tr_packets;
          vlib_set_trace_count (vm, node,
				n_trace - clib_min(n_trace, n_tr_packets));
        }
    }
  /* *INDENT-ON* */

  poll_rate_limit (dm);

  return n_rx_packets;
}

#ifndef CLIB_MULTIARCH_VARIANT
/* *INDENT-OFF* */
VLIB_REGISTER_NODE (dpdk_input_node) = {
  .function = dpdk_input,
  .type = VLIB_NODE_TYPE_INPUT,
  .name = "dpdk-input",
  .sibling_of = "device-input",

  /* Will be enabled if/when hardware is detected. */
  .state = VLIB_NODE_STATE_DISABLED,

  .format_buffer = format_ethernet_header_with_length,
  .format_trace = format_dpdk_rx_dma_trace,

  .n_errors = DPDK_N_ERROR,
  .error_strings = dpdk_error_strings,
};
/* *INDENT-ON* */

vlib_node_function_t __clib_weak dpdk_input_avx512;
vlib_node_function_t __clib_weak dpdk_input_avx2;

#if __x86_64__
static void __clib_constructor
dpdk_input_multiarch_select (void)
{
  if (dpdk_input_avx512 && clib_cpu_supports_avx512f ())
    dpdk_input_node.function = dpdk_input_avx512;
  else if (dpdk_input_avx2 && clib_cpu_supports_avx2 ())
    dpdk_input_node.function = dpdk_input_avx2;
}
#endif
#endif

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
