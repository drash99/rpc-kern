#ifndef _KRPING_H
#define _KRPING_H
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

#define PFX "krping: "


/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr6=2001:db8:0:f101::1,validate > /proc/krping
 *
 * krping "ping/pong" loop:
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

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV,
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct krping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

struct krping_stats {
	unsigned long long send_bytes;
	unsigned long long send_msgs;
	unsigned long long recv_bytes;
	unsigned long long recv_msgs;
	unsigned long long write_bytes;
	unsigned long long write_msgs;
	unsigned long long read_bytes;
	unsigned long long read_msgs;
};
/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 128*1024
#define RPING_SQ_DEPTH 64

#define DEBUG_LOG if (debug) printk

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))


/*
 * Control block struct.
 */
struct krping_cb {
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_reg_wr reg_mr_wr;
	struct ib_send_wr invalidate_wr;
	struct ib_mr *reg_mr;
	int server_invalidate;
	int read_inv;
	u8 key;

	struct ib_recv_wr rq_wr;	/* recv work request record */
	struct ib_sge recv_sgl;		/* recv single SGE */
	struct krping_rdma_info recv_buf __aligned(16);	/* malloc'd buffer */
	u64 recv_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(recv_mapping);

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct krping_rdma_info send_buf __aligned(16); /* single send buf */
	u64 send_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(send_mapping);

	struct ib_rdma_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(rdma_mapping);
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	u64  start_dma_addr;
	DEFINE_DMA_UNMAP_ADDR(start_mapping);
	struct ib_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char ip6_ndev_name[128];	/* IPv6 netdev name */
	char *addr_str;			/* dst addr string */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int poll;			/* poll or block for rlat test */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;		/* use 0 for lkey */
	int frtest;			/* reg test */
	int tos;			/* type of service */
    
	void (*rpc_msg_handler_cb)(
		void *rpc_param); // rpc layer callback function.
	void (*user_msg_handler_cb)(void *param); // user callback function.

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;
};

static int krping_bind_client(struct krping_cb *cb);
static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id);
static int krping_setup_buffers(struct krping_cb *cb);
static int krping_connect_client(struct krping_cb *cb);

static void krping_free_buffers(struct krping_cb *cb);

static void krping_free_qp(struct krping_cb *cb);
static struct krping_cb * krping_init_client(struct krping_cb *cb);
static void krping_fin_client(struct krping_cb *cb);



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

// Message format including header.
// The size of message (headers + data buffer) == cb->msgbuf_size.
struct __attribute__((__packed__)) rdma_msg {
	__be64 seq_num; // sequence number.
	__be64 rpc_ch_addr; // Client's rpc_ch_info address.
	__be64 sem_addr; // Client's semaphore address.
	char data[]; // Data. Flexible array.
};

#endif