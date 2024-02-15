/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006-2009 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "krping.h"
#include "getopt.h"

typedef uint64_t size_t;

static int debug = 0;
/*
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
MODULE_AUTHOR("Steve Wise");
MODULE_DESCRIPTION("RDMA ping server");
MODULE_LICENSE("Dual BSD/GPL");
*/
static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"server_inv", OPT_NOPARAM, 'I'},
 	{"wlat", OPT_NOPARAM, 'l'},
 	{"rlat", OPT_NOPARAM, 'L'},
 	{"bw", OPT_NOPARAM, 'B'},
 	{"duplex", OPT_NOPARAM, 'd'},
	{"tos", OPT_INT, 't'},
 	{"txdepth", OPT_INT, 'T'},
 	{"poll", OPT_NOPARAM, 'P'},
 	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
 	{"read_inv", OPT_NOPARAM, 'R'},
 	{"fr", OPT_NOPARAM, 'f'},
	{NULL, 0, 0}
};



static DEFINE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;





static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		if (!cb->server) {
			cb->state = CONNECTED;
		}
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

static int server_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	DEBUG_LOG("Received rkey %x addr %llx len %d from peer\n",
		  cb->remote_rkey, (unsigned long long)cb->remote_addr, 
		  cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	const struct ib_recv_wr *bad_wr;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
	if (cb->frtest) {
		printk(KERN_ERR PFX "cq completion event in frtest!\n");
		return;
	}
	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("send completion\n");
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n");
			cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("recv completion\n");
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
				ret = cb->server ? server_recv(cb, &wc) :
						   client_recv(cb, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

		wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "wait for CONNECTED state %d\n", 
				cb->state);
			return -1;
		}
	
	return 0;
}

static void krping_setup_wr(struct krping_cb *cb)
{
	cb->recv_sgl.addr = cb->recv_dma_addr;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->pd->local_dma_lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->pd->local_dma_lkey;

	cb->sq_wr.opcode = IB_WR_SEND;
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	/* 
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;
}

static int krping_setup_buffers(struct krping_cb *cb)
{
	int ret;

	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = ib_dma_map_single(cb->pd->device,
				   &cb->recv_buf, 
				   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = ib_dma_map_single(cb->pd->device,
					   &cb->send_buf, sizeof(cb->send_buf),
					   DMA_BIDIRECTIONAL);
	dma_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	cb->rdma_buf = ib_dma_alloc_coherent(cb->pd->device, cb->size,
					     &cb->rdma_dma_addr,
					     GFP_KERNEL);
	if (!cb->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf allocation failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	dma_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE)
				>> PAGE_SHIFT;
	cb->reg_mr = ib_alloc_mr(cb->pd,  IB_MR_TYPE_MEM_REG,
				 cb->page_list_len);
	if (IS_ERR(cb->reg_mr)) {
		ret = PTR_ERR(cb->reg_mr);
		DEBUG_LOG(PFX "recv_buf reg_mr failed %d\n", ret);
		goto bail;
	}
	DEBUG_LOG(PFX "reg rkey 0x%x page_list_len %u\n",
		cb->reg_mr->rkey, cb->page_list_len);


		cb->start_buf = ib_dma_alloc_coherent(cb->pd->device, cb->size,
						      &cb->start_dma_addr,
						      GFP_KERNEL);
		if (!cb->start_buf) {
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto bail;
		}
		dma_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);
	

	krping_setup_wr(cb);
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	return 0;
bail:
	if (cb->reg_mr && !IS_ERR(cb->reg_mr))
		ib_dereg_mr(cb->reg_mr);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_buf) {
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->rdma_buf,
				     cb->rdma_dma_addr);
	}
	if (cb->start_buf) {
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->start_buf,
				     cb->start_dma_addr);
	}
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr)
		ib_dereg_mr(cb->reg_mr);

	dma_unmap_single(cb->pd->device->dma_device,
			 dma_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 dma_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

	ib_dma_free_coherent(cb->pd->device, cb->size, cb->rdma_buf,
			     cb->rdma_dma_addr);

	if (cb->start_buf) {
		ib_dma_free_coherent(cb->pd->device, cb->size, cb->start_buf,
				     cb->start_dma_addr);
	}
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	
	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device, 0);
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * 2;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
			      cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret) {
			printk(KERN_ERR PFX "ib_create_cq failed\n");
			goto err2;
		}
	

	ret = krping_create_qp(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
static u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv)
{
	u32 rkey;
	const struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey;

	/*
	 * Update the reg key.
	 */
	ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
	cb->reg_mr_wr.key = cb->reg_mr->rkey;

	/*
	 * Update the reg WR with new buf info.
	 */
	if (buf == (u64)cb->start_dma_addr)
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
	else
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	sg_dma_address(&sg) = buf;
	sg_dma_len(&sg) = cb->size;

	ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	BUG_ON(ret <= 0 || ret > cb->page_list_len);

	DEBUG_LOG(PFX "post_inv = %d, reg_mr new rkey 0x%x pgsz %u len %lu"
		" iova_start %llx\n",
		post_inv,
		cb->reg_mr_wr.key,
		cb->reg_mr->page_size,
		(unsigned long)cb->reg_mr->length,
		(unsigned long long)cb->reg_mr->iova);

	if (post_inv)
		ret = ib_post_send(cb->qp, &cb->invalidate_wr, &bad_wr);
	else
		ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		cb->state = ERROR;
	}
	rkey = cb->reg_mr->rkey;
	return rkey;
}

static void krping_format_send(struct krping_cb *cb, u64 buf)
{
	struct krping_rdma_info *info = &cb->send_buf;
	u32 rkey;

	/*
	 * Client side will do reg or mw bind before
	 * advertising the rdma buffer.  Server side
	 * sends have no data.
	 */
		rkey = krping_rdma_rkey(cb, buf, !cb->server_invalidate);
		info->buf = htonll(buf);
		info->rkey = htonl(rkey);
		info->size = htonl(cb->size);
		DEBUG_LOG("RDMA addr %llx rkey %x len %d\n",
			  (unsigned long long)buf, rkey, cb->size);
	
}


static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;

	if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags) {
		printk(KERN_ERR PFX 
			"Fastreg not supported - device_cap_flags 0x%llx\n",
			(unsigned long long)dev->attrs.device_cap_flags);
		return 0;
	}
	DEBUG_LOG("Fastreg supported - device_cap_flags 0x%llx\n",
		(unsigned long long)dev->attrs.device_cap_flags);
	return 1;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	} else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
		if (cb->ip6_ndev_name[0] != 0) {
			struct net_device *ndev;

			ndev = __dev_get_by_name(&init_net, cb->ip6_ndev_name);
			if (ndev != NULL) {
				sin6->sin6_scope_id = ndev->ifindex;
				dev_put(ndev);
			}
		}
	}
}

static void krping_test_client(struct krping_cb *cb)
{
	int ping, start, cc, i, ret;
	const struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		krping_format_send(cb, cb->start_dma_addr);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "krping_format_send failed\n");
			break;
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}

		krping_format_send(cb, cb->rdma_dma_addr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}


		if (cb->verbose)
			printk(KERN_INFO PFX "ping data (64B max): |%.64s|\n",
				cb->rdma_buf);
#ifdef SLOW_KRPING
		wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
#endif
	}
}

/*
 * Manual qp flush test
 */
static void flush_qp(struct krping_cb *cb)
{
	struct ib_send_wr wr = { 0 };
	const struct ib_send_wr *bad;
	struct ib_recv_wr recv_wr = { 0 };
	const struct ib_recv_wr *recv_bad;
	struct ib_wc wc;
	int ret;
	int flushed = 0;
	int ccnt = 0;

	rdma_disconnect(cb->cm_id);
	DEBUG_LOG("disconnected!\n");

	wr.opcode = IB_WR_SEND;
	wr.wr_id = 0xdeadbeefcafebabe;
	ret = ib_post_send(cb->qp, &wr, &bad);
	if (ret) {
		printk(KERN_ERR PFX "%s post_send failed ret %d\n", __func__, ret);
		return;
	}

	recv_wr.wr_id = 0xcafebabedeadbeef;
	ret = ib_post_recv(cb->qp, &recv_wr, &recv_bad);
	if (ret) {
		printk(KERN_ERR PFX "%s post_recv failed ret %d\n", __func__, ret);
		return;
	}

	/* poll until the flush WRs complete */
	do {
		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0) {
			printk(KERN_ERR PFX "ib_poll_cq failed %d\n", ret);
			return;
		}
		if (ret == 0)
			continue;
		ccnt++;
		if (wc.wr_id == 0xdeadbeefcafebabe ||
		    wc.wr_id == 0xcafebabedeadbeef)
			flushed++;
	} while (flushed != 2);
	DEBUG_LOG("qp_flushed! ccnt %u\n", ccnt);
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device))
		return -EINVAL;

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static struct krping_cb * krping_init_client(struct krping_cb *cb) {
	
	const struct ib_recv_wr *bad_wr;
	int ret;

	/* set type of service, if any */
	if (cb->tos != 0)
		rdma_set_service_type(cb->cm_id, cb->tos);

	ret = krping_bind_client(cb);
	if (ret)
		goto err0;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}
	return cb;
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
err0:
	kfree(cb);
	return NULL;
}

static void krping_fin_client(struct krping_cb *cb) {
	krping_free_buffers(cb);
	krping_free_qp(cb);
}

static void krping_run_client(struct krping_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	int ret;

	/* set type of service, if any */
	if (cb->tos != 0)
		rdma_set_service_type(cb->cm_id, cb->tos);

	ret = krping_bind_client(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	krping_test_client(cb);
	rdma_disconnect(cb->cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
}

static void krping_test_rpc_client(struct krping_cb *cb)
{
	int ping, start, cc, i, ret;
	const struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		krping_format_send(cb, cb->start_dma_addr);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "krping_format_send failed\n");
			return;
		}
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			return;
		}

		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			return;
		}

		krping_format_send(cb, cb->rdma_dma_addr);
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			return;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			return;
		}

		if (cb->verbose)
			printk(KERN_INFO PFX "ping data (64B max): |%.64s|\n",
				cb->rdma_buf);
}

int krping_doit(char *cmd)
{
	struct krping_cb *cb;
	int op;
	int ret = 0;
	char *optarg;
	char *scope;
	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	mutex_lock(&krping_mutex);
	list_add_tail(&cb->list, &krping_cbs);
	mutex_unlock(&krping_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg,
			      &optint)) != 0) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			in4_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET;
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			DEBUG_LOG("port %d\n", (int)optint);
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = optint;
			if ((cb->size < 1) ||
			    (cb->size > RPING_BUFSIZE)) {
				printk(KERN_ERR PFX "Invalid size %d "
				       "(valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int)optint);
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				printk(KERN_ERR PFX "Invalid count %d\n",
					cb->count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		default:
			printk(KERN_ERR PFX "unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
	if (ret)
		goto out;

	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->server && cb->frtest) {
		printk(KERN_ERR PFX "must be client to run frtest\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(&init_net, krping_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	if (cb->server)
		krping_run_server(cb);
	else
		krping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	mutex_lock(&krping_mutex);
	list_del(&cb->list);
	mutex_unlock(&krping_mutex);
	kfree(cb);
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(struct seq_file *seq, void *v)
{
	struct krping_cb *cb;
	int num = 1;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	mutex_lock(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			seq_printf(seq,
			     "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
			     num++, cb->pd->device->name, cb->stats.send_bytes,
			     cb->stats.send_msgs, cb->stats.recv_bytes,
			     cb->stats.recv_msgs, cb->stats.write_bytes,
			     cb->stats.write_msgs,
			     cb->stats.read_bytes,
			     cb->stats.read_msgs);
		} else {
			seq_printf(seq, "%d listen\n", num++);
		}
	}
	mutex_unlock(&krping_mutex);
	module_put(THIS_MODULE);
	return 0;
}

/*
 * Write proc is used to start a ping client or server.
 */
static ssize_t krping_write_proc(struct file * file, const char __user * buffer,
		size_t count, loff_t *ppos)
{
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		kfree(cmd);
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	if (rc)
		return rc;
	else
		return (int) count;
}

static int krping_read_open(struct inode *inode, struct file *file)
{
        return single_open(file, krping_read_proc, inode->i_private);
}

static struct file_operations krping_ops = {
	.owner = THIS_MODULE,
	.open = krping_read_open,
	.read = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = krping_write_proc,
};

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);
}

module_init(krping_init);
module_exit(krping_exit);
