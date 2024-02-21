// #define _GNU_SOURCE
// #define _BSD_SOURCE
// #include <netinet/in.h>
// #include <infiniband/arch.h>
#include "rdma.h"
#include "rpc.h"

#define RDMA_SQ_DEPTH 16


// TODO: to be deleted.
#define RPING_MSG_FMT "rdma-ping-%d: "

static int errno;
static int cq_event_handler(struct rdma_ch_cb *cb);


/*
 * rping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

static int cma_event_handler(struct rdma_cm_id *cma_id,
			     struct rdma_cm_event *event)
{
	int ret = 0;
	struct rdma_ch_cb *cb = cma_id->context;

	printk(KERN_ERR PFX "cma_event type cma_id %p (%s)\n",
		   cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			printk(KERN_ERR PFX "rdma_resolve_route\n");
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
		printk(KERN_ERR PFX "child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		printk(KERN_ERR PFX "ESTABLISHED\n");

		/*
		 * Server will wake up when first RECV completes.
		 */
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
		printk(KERN_ERR PFX "cma event, error %d\n",
			 event->status);
		wake_up_interruptible(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		printk(KERN_ERR PFX "%s DISCONNECT EVENT...\n",
			cb->server ? "server" : "client");
		cb->state = DISCONNECTED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		ret = -1;
		break;

	default:
		printk(KERN_ERR PFX "unhandled event:, ignoring\n"
			);
		break;
	}

	return ret;
}




static struct rdma_ch_cb *clone_cb(struct rdma_ch_cb *listening_cb)
{
	struct rdma_ch_cb *cb;
	int i;

	cb = kzalloc(sizeof *cb, GFP_KERNEL);
	if (!cb)
		return NULL;

	*cb = *listening_cb; // shallow copy.
	cb->child_cm_id->context = cb;

	// Alloc new buf_ctxs.
	cb->buf_ctxs = kzalloc(cb->msgbuf_cnt * sizeof(struct msgbuf_ctx), GFP_KERNEL);
	if (!cb->buf_ctxs) {
		kfree(cb);
		return NULL;
	}

	// FIXME: Maybe we don't need it. Buffers will be allocated later.
	for (i = 0; i < cb->msgbuf_cnt; i++) {
		cb->buf_ctxs[i] = listening_cb->buf_ctxs[i]; //shallow copy.
	}

	printk(KERN_ERR PFX "Cloning CB:");
	printk(KERN_ERR PFX "parent_cb=%lx", listening_cb);
	printk(KERN_ERR PFX "parent cb->buf_ctxs=%lx", listening_cb->buf_ctxs);
	printk(KERN_ERR PFX "cloned_cb=%lx", cb);
	printk(KERN_ERR PFX "cloned cb->buf_ctxs=%lx", cb->buf_ctxs);

	return cb;
}

static int create_qp(struct rdma_ch_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));

	// TODO: Check configuration.
	init_attr.cap.max_send_wr = RDMA_SQ_DEPTH;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

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

static void cq_thread(struct ib_cq *cq, void *ctx)
{
	struct rdma_ch_cb *cb = ctx;
	int ret;

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret) {
			printk(KERN_ERR PFX "Failed to set notify!\n");
			return;
		}
		ret = cq_event_handler(cb);
		//ib_ack_cq_events(cb->cq, 1);
		if (ret)
			return;
}


static int setup_qp(struct rdma_ch_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};

	cb->pd = ib_alloc_pd(cm_id->device, 0);
	if (!cb->pd) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return errno;
	}
	printk(KERN_ERR PFX "created pd %p", cb->pd);

/*
	cb->channel = ib_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		printk(KERN_ERR PFX "ib_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	printk(KERN_ERR PFX "created channel %p", cb->channel);
*/ //channel?? 
	cb->cq = ib_create_cq(cm_id->device, cq_thread, NULL, cb,
			       &attr);
	if (!cb->cq) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	printk(KERN_ERR PFX "created cq %p\n", cb->cq);

	ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP); // Todo : IB_CQ_NEXT_COMP was originally 0
	if (ret) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = create_qp(cb);
	if (ret) {
		printk(KERN_ERR PFX "rdma_create_qp");
		goto err3;
	}
	printk(KERN_ERR PFX "created qp %p", cb->qp);
	return 0;

err3:
	ib_destroy_cq(cb->cq);
err2:
	//ib_destroy_comp_channel(cb->channel);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

static void setup_wr(struct rdma_ch_cb *cb)
{
	struct msgbuf_ctx *mb_ctx;
	int i;

	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];

		mb_ctx->recv_sgl.addr =
			(uint64_t)(unsigned long)mb_ctx->recv_buf;
		mb_ctx->recv_sgl.length = cb->msgbuf_size;
		mb_ctx->recv_sgl.lkey = mb_ctx->recv_mr->lkey;

		mb_ctx->rq_wr.wr_id = i; // msgbuf_id is stored to wr_id.
		mb_ctx->rq_wr.sg_list = &mb_ctx->recv_sgl;
		mb_ctx->rq_wr.num_sge = 1;

		mb_ctx->send_sgl.addr =
			(uint64_t)(unsigned long)mb_ctx->send_buf;
		mb_ctx->send_sgl.length = cb->msgbuf_size;
		mb_ctx->send_sgl.lkey = mb_ctx->send_mr->lkey;

		mb_ctx->sq_wr.wr_id = i;
		mb_ctx->sq_wr.opcode = IB_WR_SEND;
		mb_ctx->sq_wr.send_flags = IB_SEND_SIGNALED;
		mb_ctx->sq_wr.sg_list = &mb_ctx->send_sgl;
		mb_ctx->sq_wr.num_sge = 1;
	}
	// cb->rdma_sgl.addr = (uint64_t)(unsigned long)cb->rdma_buf;
	// cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
	// cb->rdma_sq_wr.send_flags = IB_SEND_SIGNALED;
	// cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
	// cb->rdma_sq_wr.num_sge = 1;
}

static inline int msgheader_size(void)
{
	struct rdma_msg msg;
	return (int)((uint64_t)&msg.data - (uint64_t)&msg);
}

static inline int msgdata_size(int msgbuf_size)
{
	return msgbuf_size - msgheader_size();
}

static inline int msgbuf_size(int msgdata_size)
{
	return msgdata_size + msgheader_size();
}

static inline int valid_msg_size(struct rdma_ch_cb *cb, char *data)
{
	return (cb->msgheader_size + sizeof *data) <= (uint64_t)cb->msgbuf_size;
}

static int alloc_msg_buffers(struct rdma_ch_cb *cb)
{
	struct msgbuf_ctx *mb_ctx;
	int i, ret;

	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];
		mb_ctx->recv_buf = kzalloc(1, cb->msgbuf_size); 
		// todo : handling alloc failure
		//mb_ctx->recv_buf = NULL;


		/*ret = posix_memalign((void **)&mb_ctx->recv_buf,
				     sysconf(_SC_PAGESIZE), cb->msgbuf_size);
		if (ret != 0) {
			// if (!mb_ctx->recv_buf) {
			printk(KERN_ERR PFX
				"Allocating message buffer (recv) failed. Error code=%d\n",
				ret);
			return -1;
		}*/

		printk(KERN_ERR PFX "alloc mb_ctx->recv_buf=%lx", mb_ctx->recv_buf);

		mb_ctx->send_buf = kzalloc(1, cb->msgbuf_size);
		/*mb_ctx->send_buf = NULL;
		ret = posix_memalign((void **)&mb_ctx->send_buf,
				     sysconf(_SC_PAGESIZE), cb->msgbuf_size);
		if (ret != 0) {
			// if (!mb_ctx->send_buf) {
			printk(KERN_ERR PFX
				"Allocating message buffer (send) failed. Error code=%d\n",
				ret);
			return -1;
		}*/

		printk(KERN_ERR PFX "alloc mb_ctx->send_buf=%lx", mb_ctx->send_buf);
	}

	return 0;
}

// TODO: Alloc RDMA buffer for Data Fetcher. Need to implement it the following code is just copied.
// static int alloc_rdma_buffer(struct rdma_ch_cb *cb)
// {
// 	cb->rdma_buf = malloc(cb->size);
// 	if (!cb->rdma_buf) {
// 		printk(KERN_ERR PFX "rdma_buf malloc failed\n");
// 		ret = -ENOMEM;
// 		goto err2;
// 	}

// 	// TODO: RDMA is used by DATA FETCHER.
// 	cb->rdma_mr =
// 		ib_reg_mr(cb->pd, cb->rdma_buf, cb->size,
// 			   IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ |
// 				   IB_ACCESS_REMOTE_WRITE);
// 	if (!cb->rdma_mr) {
// 		printk(KERN_ERR PFX "rdma_buf reg_mr failed\n");
// 		ret = errno;
// 		goto err3;
// 	}

// }

static int setup_buffers(struct rdma_ch_cb *cb)
{
	int ret, i;
	struct msgbuf_ctx *mb_ctx;

	printk(KERN_ERR PFX "setup_buffers called on cb %p", cb);

	// Malloc buffers.
	ret = alloc_msg_buffers(cb);
	if (ret) {
		printk(KERN_ERR  "Failed to alloc msg buffers.");
		goto err2;
	}

	// Register send/recv buffers to MR.
	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];

		mb_ctx->recv_mr =ib_dma_map_single(cb->pd->device, mb_ctx->recv_buf,
					     cb->msgbuf_size, DMA_BIDIRECTIONAL);
		if (!mb_ctx->recv_mr) {
			printk(KERN_ERR  "recv_buf reg_mr failed");
			ret = errno;
			goto err1;
		}

		mb_ctx->send_mr = ib_dma_map_single(cb->pd->device, mb_ctx->send_buf,
					     cb->msgbuf_size, DMA_BIDIRECTIONAL);
		if (!mb_ctx->send_mr) {
			printk(KERN_ERR  "send_buf reg_mr failed");
			ret = errno;
			goto err1;
		}
	}

	setup_wr(cb);
	printk(KERN_ERR PFX "allocated & registered buffers...");
	return 0;

// err5:
// 	kfree(cb->start_buf);
// err4:
// 	ib_dereg_mr(cb->rdma_mr);
// err3:
// 	kfree(cb->rdma_buf);
err1:
	// Deregister MRs.
	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];
		if (mb_ctx->recv_mr != NULL)
			ib_dereg_mr(mb_ctx->recv_mr);
		if (mb_ctx->send_mr != NULL)
			ib_dereg_mr(mb_ctx->send_mr);
	}

err2:
	// Free buffers.
	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];
		if (mb_ctx->recv_buf != NULL)
			kfree(mb_ctx->recv_buf);
		if (mb_ctx->send_buf != NULL)
			kfree(mb_ctx->send_buf);
	}
	return ret;
}

/**
 * @brief Copy the message and invoke a worker thread.
 * 
 * @param cb 
 * @param wc 
 * @return int 
 */
static int receive_msg(struct rdma_ch_cb *cb, struct ib_wc *wc)
{
	struct msgbuf_ctx *mb_ctx;
	struct rpc_msg_handler_param *rpc_param;
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	int msgbuf_id;
	int ret;

	// Check the size of received data.
	if (wc->byte_len != (uint32_t)cb->msgbuf_size) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	// These are kfreed in the handler callback function.
	rpc_param = kzalloc(sizeof *rpc_param, GFP_KERNEL);
	if (!rpc_param) {
		ret = -ENOMEM;
		goto err1;
	}
	param = kzalloc(sizeof *param, GFP_KERNEL);
	if (!param) {
		ret = -ENOMEM;
		goto err2;
	}
	msg = kzalloc(cb->msgbuf_size, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto err3;
	}

	msgbuf_id = wc->wr_id;

	mb_ctx = &cb->buf_ctxs[msgbuf_id];

	msg->header.seqn = ntohll(mb_ctx->recv_buf->seq_num);
	msg->header.sem = (wait_queue_head_t *)ntohll(mb_ctx->recv_buf->sem_addr);
	msg->header.client_rpc_ch =
		(struct rpc_ch_info *)ntohll(mb_ctx->recv_buf->rpc_ch_addr);
	// FIXME: Copy fixed size, currently.
	memcpy(&msg->data[0], &mb_ctx->recv_buf->data[0], cb->msgdata_size);

	param->msgbuf_id = msgbuf_id;
	param->ch_cb = (struct rdma_ch_cb *)cb;
	param->msg = msg;

	rpc_param->msgbuf_id = msgbuf_id;
	rpc_param->client_rpc_ch =
		(struct rpc_ch_info *)ntohll(mb_ctx->recv_buf->rpc_ch_addr);
	rpc_param->param = param;
	rpc_param->user_msg_handler_cb = cb->user_msg_handler_cb;

	printk(KERN_ERR PFX 
		"Received msgbuf_id=%d seqn=%lu data=%s rpc_ch_addr=0x%lx sem_addr=0x%lx",
		msgbuf_id, msg->header.seqn, msg->data,
		(uint64_t)rpc_param->client_rpc_ch, (uint64_t)msg->header.sem);

	// Execute RPC callback function in a worker thread.
	//thpool_add_work(cb->msg_handler_thpool, cb->rpc_msg_handler_cb, (void *)rpc_param);
	cb->rpc_msg_handler_cb(rpc_param);

	// cb->remote_rkey = be32toh(cb->recv_buf.rkey);
	// cb->remote_addr = ntohll(cb->recv_buf.buf);
	// cb->remote_len = be32toh(cb->recv_buf.size);
	// printk(KERN_ERR PFX "Received rkey %x addr %" PRIx64 " len %d from peer",
	// 	  cb->remote_rkey, cb->remote_addr, cb->remote_len);

	// Change server state.
	if (cb->state <= CONNECTED)
		cb->state = WORKING;

	return 0;
err3:
	kfree(param);
err2:
	kfree(rpc_param);
err1:
	return ret;
}

static int cq_event_handler(struct rdma_ch_cb *cb)
{
	struct ib_wc wc;
	const struct ib_recv_wr *bad_wr;
	int ret;
	int flushed = 0;

	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
		ret = 0;

		if (wc.status) {
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				flushed = 1;
				continue;
			}
			printk(KERN_ERR  "cq completion failed wc.status=%d",
				  wc.status);
			ret = -1;
			goto error;
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			printk(KERN_ERR PFX "send completion");
			break;

		case IB_WC_RDMA_WRITE:
			printk(KERN_ERR PFX "rdma write completion");
			// cb->state = RDMA_WRITE_COMPLETE;
			// sem_post(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			printk(KERN_ERR PFX "rdma read completion");
			// cb->state = RDMA_READ_COMPLETE;
			// sem_post(&cb->sem);
			break;

		case IB_WC_RECV:
			printk(KERN_ERR PFX "recv completion\n");
			ret = receive_msg(cb, &wc);
			if (ret) {
				printk(KERN_ERR  "recv wc error: ret=%d", ret);
				goto error;
			}

			ret = ib_post_recv(
				cb->qp, &cb->buf_ctxs[wc.wr_id].rq_wr,
				&bad_wr); // wc.wr_id stores msgbuf_id.
			if (ret) {
				printk(KERN_ERR  "post recv error: ret=%d", ret);
				goto error;
			}
			// sem_post(&cb->sem); // why commented?? 
			break;

		default:
			printk(KERN_ERR  "unknown!!!!! completion");
			ret = -1;
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR  "poll error %d", ret);
		goto error;
	}
	return flushed;

error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
	return ret;
}



static int do_accept(struct rdma_ch_cb *cb)
{
	int ret;

	printk(KERN_ERR PFX "accepting client connection request");

	ret = rdma_accept(cb->child_cm_id, NULL);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept\n");
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	return 0;
}

static void kfree_buffers(struct rdma_ch_cb *cb)
{
	int i;
	struct msgbuf_ctx *mb_ctx;

	printk(KERN_ERR PFX "kfree_buffers called on cb %p", cb);

	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];
		ib_dereg_mr(mb_ctx->recv_mr);
		printk(KERN_ERR PFX "kfree mb_ctx->recv_buf=%lx", mb_ctx->recv_buf);
		kfree(mb_ctx->recv_buf);

		ib_dereg_mr(mb_ctx->send_mr);
		printk(KERN_ERR PFX "kfree mb_ctx->send_buf=%lx", mb_ctx->send_buf);
		kfree(mb_ctx->send_buf);

		// ib_dereg_mr(cb->rdma_mr);
	}

	// kfree(cb->rdma_buf);
	// if (!cb->server) {
	// 	ib_dereg_mr(cb->start_mr);
	// 	kfree(cb->start_buf);
	// }
}

static void kfree_qp(struct rdma_ch_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	//ib_destroy_comp_channel(cb->channel);
	ib_dealloc_pd(cb->pd);
}

static void kfree_cb(struct rdma_ch_cb *cb)
{
	printk(KERN_ERR PFX "kfree cb->buf_ctxs=%lx", cb->buf_ctxs);
	kfree(cb->buf_ctxs);
	printk(KERN_ERR PFX "kfree cb=%lx", cb);
	kfree(cb);
}


static int bind_client(struct rdma_ch_cb *cb)
{
	int ret;

	if (cb->sin.ss_family == AF_INET)
		((struct sockaddr_in *)&cb->sin)->sin_port = cb->port;
	else
		((struct sockaddr_in6 *)&cb->sin)->sin6_port = cb->port;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&cb->sin,
				2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr\n");
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	printk(KERN_ERR PFX "rdma_resolve_addr - rdma_resolve_route successful");
	return 0;
}

static int connect_client(struct rdma_ch_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 7;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect\n");
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state != CONNECTED) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	printk(KERN_ERR PFX "rmda_connect successful");
	return 0;
}

// static void format_send(struct rdma_ch_cb *cb, char *buf, struct ib_mr *mr)
// {
// 	struct comm_rdma_mr_info *info = &cb->send_buf;

// 	info->buf = htons((uint64_t)(unsigned long)buf);
// 	info->rkey = htobe32(mr->rkey);
// 	info->size = htobe32(cb->size);

// 	printk(KERN_ERR PFX "RDMA addr %" PRIx64 " rkey %x len %d", ntohll(info->buf),
// 		  be32toh(info->rkey), be32toh(info->size));
// }

static inline uint64_t alloc_seqn(struct msgbuf_ctx *mb_ctx)
{
	uint64_t ret;

	//ret = atomic_fetch_add(&mb_ctx->seqn, 1);
	mb_ctx->seqn += 1;
	return mb_ctx->seqn;
}

int send_rdma_msg(struct rdma_ch_cb *cb, void *rpc_ch_addr, char *data,
		  wait_queue_head_t *sem, int msgbuf_id, uint64_t seqn)
{
	const struct ib_send_wr *bad_wr;
	struct rdma_msg *msg;
	struct msgbuf_ctx *mb_ctx;
	int ret;
	uint64_t new_seqn;
	// uint64_t data_size, remains;

	if (msgbuf_id >= cb->msgbuf_cnt) {
		printk(KERN_ERR  
			"msg buffer id(%d) exceeds total msg buffer count(%d).",
			msgbuf_id, cb->msgbuf_cnt);
		return 0;
	}

	mb_ctx = &cb->buf_ctxs[msgbuf_id];

	msg = mb_ctx->send_buf;
	if (!seqn) {
		new_seqn = alloc_seqn(mb_ctx);
		msg->seq_num = htons(new_seqn);
	} else {
		msg->seq_num = htons(seqn);
	}
	msg->sem_addr = htons((uint64_t)(unsigned long)sem);
	msg->rpc_ch_addr = htons((uint64_t)(unsigned long)rpc_ch_addr);

	// Copy and send fixed size, currently.
	// FIXME: Can we copy only the meaningful data? memset will be required.
	// memset(&msg->data[0], 0, cb->msgdata_size);
	memcpy(&msg->data[0], data, cb->msgdata_size);

	// No duplicate memset: (how to get data_size?)
	// memcpy(&msg->data[0], data, data_size);
	// remains = cb->msgdata_size - data_size;
	// memset(&msg->data[data_size], 0, remains);

	printk(KERN_INFO   
		"Sending RDMA msg: seqn=%lu sem_addr=%lx rpc_ch_addr=%lx data=\"%s\"",
		msg->seq_num, (uint64_t)sem, (uint64_t)rpc_ch_addr, msg->data);

	ret = ib_post_send(cb->qp, &mb_ctx->sq_wr, &bad_wr);
	printk(KERN_INFO   
		"Sending RDMA msg: seqn=%lu sem_addr=%lx rpc_ch_addr=%lx data=\"%s\"",
		msg->seq_num, (uint64_t)sem, (uint64_t)rpc_ch_addr, msg->data);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return 0;
	}

	// Do not wait completion or ack. FIXME: Do we need to wait completion?
	// A msg buffer is being allocated until the client receives a response (or an ack) from the server.

	return cb->msgbuf_size; // Send fixed size, currently.
}

static int run_client(struct rdma_ch_cb *cb)
{
	const struct ib_recv_wr *bad_wr;
	struct msgbuf_ctx *mb_ctx;
	int i, ret;

	ret = bind_client(cb);
	if (ret)
		return ret;

	ret = setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "setup_buffers failed: %d\n", ret);
		goto err1;
	}

	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];

		ret = ib_post_recv(cb->qp, &mb_ctx->rq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
			goto err2;
		}
	}


	ret = connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err3;
	}

	return 0;

err3:
	//pthread_join(cb->cqthread, NULL);
err2:
	kfree_buffers(cb);
err1:
	kfree_qp(cb);

	return ret;
}
static void fill_sockaddr(struct sockaddr_storage *sin, struct rdma_ch_cb *cb, char *addr, int port)
{
	memset(sin, 0, sizeof(*sin));

	struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
	sin4->sin_family = AF_INET;
	memcpy((void *)&sin4->sin_addr.s_addr, addr, 4);
	sin4->sin_port = port;
}
/**
 * @brief 
 * 
 * @param attr Attributes for an RDMA connection.
 * @return struct rdma_ch_cb* Returns rdma_ch_cb pointer. Return NULL on failure.
 */
struct rdma_ch_cb *init_rdma_ch(struct rdma_ch_attr *attr)
{
	struct rdma_ch_cb *cb;
	struct msgbuf_ctx *mb_ctx;
	int ret, i;

	cb = kzalloc(sizeof(struct rdma_ch_cb), GFP_KERNEL);
	if (!cb) {
		ret = -ENOMEM;
		goto out5;
	}

	cb->msgbuf_cnt = attr->msgbuf_cnt;
	cb->msgheader_size = msgheader_size();
	cb->msgdata_size = attr->msgdata_size;
	cb->msgbuf_size = msgbuf_size(attr->msgdata_size);
	cb->rpc_msg_handler_cb = attr->rpc_msg_handler_cb;
	cb->user_msg_handler_cb = attr->user_msg_handler_cb;

	cb->buf_ctxs = kzalloc(cb->msgbuf_cnt * sizeof(struct msgbuf_ctx), GFP_KERNEL);
	if (!cb->buf_ctxs) {
		ret = -ENOMEM;
		printk(KERN_ERR  "kzalloc failed.");
		goto out4;
	}

	// Some initialization.
	for (i = 0; i < cb->msgbuf_cnt; i++) {
		mb_ctx = &cb->buf_ctxs[i];
		mb_ctx->id = i; // Set ID.
		//atomic_init(&mb_ctx->seqn, 1); // Start from 1. 
		mb_ctx->seqn = 1; // ignore atomic 
		// because of the comment from seqn : TODO: It doesn't need to be atomic. Only one thread accesses it.
	}

	cb->server = attr->server;
	cb->state = IDLE;
	cb->size = cb->msgbuf_size; // msg buffer size.
	cb->sin.ss_family = AF_INET;
	cb->port = htons(attr->port);
	init_waitqueue_head(&cb->sem); // FIXME: Where is it used?

	if (!cb->server) {
		fill_sockaddr(&cb->sin, cb,attr->ip_addr, attr->port);
	}

	cb->cm_channel = NULL;// create_first_event_channel();
	if (!cb->cm_channel) {
		ret = errno;
		goto out3;
	}

	ret = rdma_create_id(&init_net, cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);

	if (ret) {
		printk(KERN_ERR PFX"rdma_create_id failed.\n");
		goto out2;
	}

	

	if (cb->server) {
		
		goto out1;
		
	} else
		run_client(cb);

	return cb;

	printk(KERN_ERR PFX"Destroy rdma connections.\n");
out1:
	rdma_destroy_id(cb->cm_id);
out2:
	//rdma_destroy_event_channel(cb->cm_channel);
out3:
	kfree(cb->buf_ctxs);
out4:
	kfree(cb);
out5:
	printk(KERN_ERR PFX"init_rdma_ch failed. ret=%d\n", ret);
	return NULL;
}

/**
 * @brief Called by client.
 * 
 * @param cb 
 */
void destroy_rdma_client(struct rdma_ch_cb *cb)
{
	if (cb->server) {
		printk(KERN_WARNING "destroy_rdma_ch() is for client.");
		return;
	}

	rdma_disconnect(cb->cm_id);
	//pthread_join(cb->cqthread, NULL);
	kfree_buffers(cb);
	kfree_qp(cb);

	rdma_destroy_id(cb->cm_id);
	//rdma_destroy_event_channel(cb->cm_channel);
	kfree(cb->buf_ctxs);
	kfree(cb);
}