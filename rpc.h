#ifndef _RPC_H_
#define _RPC_H_

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/signal.h>
#include <linux/types.h>
#include <linux/proc_fs.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "bitmap.h"

#define RPC_MSG_BUF_NUM 4
#define PFX "rpc rdma "

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

enum rpc_channel_type {
	RPC_CH_RDMA = 1,
	// RPC_CH_SHMEM,
	// SOCKET,
	// SHMEM
};

struct rpc_ch_info {
	enum rpc_channel_type ch_type;
	void *ch_cb; // Channel control block.
	void *msgbuf_bitmap; // BIT_ARRAY*
	spinlock_t msgbuf_bitmap_lock;
};

/*
 *  Request: Client -> Server
 *  Response: Server -> Client
 */
struct rpc_req_param {
	struct rpc_ch_info *rpc_ch; // client's current rpc channel.
	char *data; // msg data to send.
	wait_queue_head_t *sem; /* address of client's semaphore address. (Required by RDMA channel)
		     * It is used to wait until server's response (ack) arrives.
		     */
};

struct rpc_resp_param {
	struct rpc_ch_info *rpc_ch; // server's current rpc channel.
	void *client_rpc_ch_addr; // address of client's rpc channel (rpc_ch). (Passed when requested.)
	char *data; // msg data to send.
	wait_queue_head_t *sem; // address of client's semaphore address. (Required by RDMA channel)
	int client_id; // client id (= cb id). To identify which client sent a message in SHMEM channel.
	int msgbuf_id; // msg buffer where client's msg arrived.
	uint64_t seqn; // seqn of the client's request.
};

// Call graph of callback functions:
// CQ event ->
// rpc_msg_handler_cb() (rpc layer) ->
// msg_handler() (user defined) ->

// Parameter for rpc layer's msg handler callback.
struct rpc_msg_handler_param {
	int msgbuf_id;
	struct rpc_ch_info *client_rpc_ch;
	struct msg_handler_param *param; // Passed to user's msg handler callback.
	void (*user_msg_handler_cb)(
		void *param); // user's msg handler callback func.
};

// Parameter for user's msg handler callback.
struct msg_handler_param {
	int client_id; // Used by SHMEM channel.
	int msgbuf_id;
	void *ch_cb;
	struct rpc_msg *msg;
};

struct __attribute__((packed)) rpc_msg_header {
	uint64_t seqn;
	struct rpc_ch_info *
		client_rpc_ch; // Client's address should be delivered through server's response.
	wait_queue_head_t *sem; // Client's semaphore address (Used by rdma channel).
};

// It stores identical data with struct rdma_msg but in little endian order.
struct __attribute__((packed)) rpc_msg {
	struct rpc_msg_header header;
	char data[]; // Flexible array.
};

int init_rpc_server(enum rpc_channel_type ch_type, char *target, int port,
		    int max_msgdata_size, void (*msg_handler)(void *data));
struct rpc_ch_info *init_rpc_client(enum rpc_channel_type ch_type, char *target,
				    int port, int max_msgdata_size,
				    void (*msg_handler)(void *data));

int send_rpc_msg_to_server(struct rpc_req_param *req_param);
void send_rpc_response_to_client(struct rpc_resp_param *resp_param);
void destroy_rpc_client(struct rpc_ch_info *rpc_ch);

uint64_t alloc_msgbuf_id(struct rpc_ch_info *rpc_ch);
void free_msgbuf_id(struct rpc_ch_info *rpc_ch, uint64_t bit_id);
void wait_rpc_shmem_response(struct rpc_ch_info *rpc_ch, int msgbuf_id,
			     int callback);
int get_max_msgdata_size(struct rpc_ch_info *rpc_ch);
#endif
