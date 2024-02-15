
#include "rpc.h"
//#include "bit_array.h"

/**
 * @brief Callback function of RPC layer. It frees RPC layer resources.
 * 
 * @param arg 
 */
static void client_rpc_rdma_msg_handler(void *arg)
{
	struct rpc_msg_handler_param *rpc_pa;
	rpc_pa = (struct rpc_msg_handler_param *)arg;

	// Free msg buffer bitmap.
	//free_msgbuf_id(rpc_pa->client_rpc_ch, rpc_pa->msgbuf_id);

	// Call user-defined callback.
	rpc_pa->user_msg_handler_cb((void *)rpc_pa->param);

	free(arg);
}



/**
 * @brief Initialize RPC client.
 * 
 * @param ch_type Channel type. For example, RDMA, shared memory, and so on.
 * @param target Server ip_addr for RDMA connection, cm_socket_name for SHMEM connection.
 * @param port 
 * @param max_msgdata_size The maximum size of a msg data in byte.
 * @param msg_handler Message handler callback function.
 * @param worker_thpool A worker thread pool that executes the handler callback function.
 * @return struct rpc_ch_info* RPC channel information. It is used to send a message to the counterpart.
 */
struct rpc_ch_info *init_rpc_client(enum rpc_channel_type ch_type, char *target,
				    int port, int max_msgdata_size,
				    void (*msg_handler)(void *data))
{
	struct krping_cb *rdma_cb;
	
	struct rpc_ch_info *rpc_ch;
	int is_server;

	rpc_ch = kzalloc(sizeof *rpc_ch, GFP_KERNEL);
	rpc_ch->ch_type = ch_type;

	//rpc_ch->msgbuf_bitmap = bit_array_create(RPC_MSG_BUF_NUM);
	init_waitqueue_head(&rpc_ch->msgbuf_bitmap_lock);



	// Print for test.
	//printf("Message buffer bitmaps: ");
	//bit_array_print(rpc_ch->msgbuf_bitmap, stdout);
	//fputc('\n', stdout);

	is_server = 0;

	switch (ch_type) {
	case RPC_CH_RDMA:
		rdma_cb->server = is_server;
		rdma_cb->count = RPC_MSG_BUF_NUM;
		rdma_cb->size = max_msgdata_size;
		in4_pton(target, -1, rdma_cb->addr, -1, NULL);
		rdma_cb->port = htons(port);
		rdma_cb->rpc_msg_handler_cb = client_rpc_rdma_msg_handler;
		rdma_cb->user_msg_handler_cb = msg_handler;
		rdma_cb = krping_init_client(rdma_cb);
		rpc_ch->ch_cb = rdma_cb;
		break;

	default:
		printk(KERN_ERR PFX "Invalid channel type for RPC.");
		goto err;
	}

	if (!rpc_ch->ch_cb) {
		goto err;
	}

	return rpc_ch;

err:
	kfree(rpc_ch);
	return NULL;
}

void destroy_rpc_client(struct rpc_ch_info *rpc_ch)
{
	switch (rpc_ch->ch_type) {
	case RPC_CH_RDMA:
		destroy_rdma_client((struct rdma_ch_cb *)rpc_ch->ch_cb);
		break;
	default:
		log_error("Invalid channel type for RPC.");
	}
	pthread_spin_destroy(&rpc_ch->msgbuf_bitmap_lock);
	//bit_array_free(rpc_ch->msgbuf_bitmap);
	kfree(rpc_ch);
}


int send_rdma_msg(struct krping_cb *cb, void *rpc_ch_addr, char *data,
		  sem_t *sem, int msgbuf_id, uint64_t seqn)
{
	struct ibv_send_wr *bad_wr;
	struct rdma_msg *msg;
	struct msgbuf_ctx *mb_ctx;
	int ret;
	uint64_t new_seqn;
	// uint64_t data_size, remains;

	if (msgbuf_id >= 1) {
		log_error(
			"msg buffer id(%d) exceeds total msg buffer count(%d).",
			msgbuf_id, 1);
		return 0;
	}

	//mb_ctx = &cb->buf_ctxs[msgbuf_id];

	msg = cb->send_buf;
	if (!seqn) {
		new_seqn = alloc_seqn(mb_ctx);
		msg->seq_num = htobe64(new_seqn);
	} else {
		msg->seq_num = htobe64(seqn);
	}
	msg->sem_addr = htobe64((uint64_t)(unsigned long)sem);
	msg->rpc_ch_addr = htobe64((uint64_t)(unsigned long)rpc_ch_addr);

	// Copy and send fixed size, currently.
	// FIXME: Can we copy only the meaningful data? memset will be required.
	// memset(&msg->data[0], 0, cb->msgdata_size);
	memcpy(&msg->data[0], data, cb->msgdata_size);

	// No duplicate memset: (how to get data_size?)
	// memcpy(&msg->data[0], data, data_size);
	// remains = cb->msgdata_size - data_size;
	// memset(&msg->data[data_size], 0, remains);

	log_info(
		"Sending RDMA msg: seqn=%lu sem_addr=%lx rpc_ch_addr=%lx data=\"%s\"",
		msg->seq_num, (uint64_t)sem, (uint64_t)rpc_ch_addr, msg->data);

	ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	log_info(
		"Sending RDMA msg: seqn=%lu sem_addr=%lx rpc_ch_addr=%lx data=\"%s\"",
		msg->seq_num, (uint64_t)sem, (uint64_t)rpc_ch_addr, msg->data);
	if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		return 0;
	}

	// Do not wait completion or ack. FIXME: Do we need to wait completion?
	// A msg buffer is being allocated until the client receives a response (or an ack) from the server.

	return cb->msgbuf_size; // Send fixed size, currently.
}

/**
 * @brief  Send an RPC message to server.
 * 
 * @param req_param Parameters required to send a message to server.
 * 
 * @return int msgbuf_id is returned. (Required by SHMEM channel).
 */
int send_rpc_msg_to_server(struct rpc_req_param *req_param)
{
	int msgbuf_id;
	struct rpc_ch_info *rpc_ch;

	rpc_ch = req_param->rpc_ch;

	// Alloc a message buffer id.
	msgbuf_id = alloc_msgbuf_id(rpc_ch);

	switch (rpc_ch->ch_type) {
	case RPC_CH_RDMA:
		send_rdma_msg((struct rdma_ch_cb *)rpc_ch->ch_cb, rpc_ch,
			      req_param->data, req_param->sem, msgbuf_id, 0);
		break;

	default:
		log_error("Invalid channel type for RPC.");
	}

	return msgbuf_id;
}