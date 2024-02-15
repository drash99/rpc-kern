
#include "test_global.h" 
#include "rpc.h"

// Called in a worker thread.
void client_rdma_msg_handler(void *arg)
{
	struct msg_handler_param *param;
	struct rpc_msg *msg;
	wait_queue_head_t *sem;

	param = (struct msg_handler_param *)arg;
	msg = param->msg;

	sem = (wait_queue_head_t *)param->msg->header.sem;
	printk(KERN_ERR PFX
		"(rdma_ch_CLIENT_test) received: seqn=%lu sem_addr=%lx data=%s\n",
		msg->header.seqn, (uint64_t)msg->header.sem, msg->data);

	// post sema to resume the requesting thread.
	sem_post(sem);

	free(msg);
	free(param);
}


int notamainanymore(int argc, char **argv)
{
	char *data = "hello world";
	struct rpc_ch_info *rpc_cli_ch;
	int ret, msgbuf_id;
	enum rpc_channel_type ch_type;
	wait_queue_head_t sem;
	struct rpc_req_param req_param;

	// To get rid of unused parameter warning.
	argc = argc;
	argv = argv;

	if (argc < 2) {
		printf("Usage: %s [rdma|shmem]\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "rdma") == 0) {
		log_info("Channel type : RDMA");
		ch_type = RPC_CH_RDMA;
	} else {
		printf("Usage: %s [rdma|shmem]\n", argv[0]);
		return 1;
	}


	switch (ch_type) {
	case RPC_CH_RDMA:
		rpc_cli_ch = init_rpc_client(RPC_CH_RDMA, g_ip_addr, g_port,
					     MAX_MSG_DATA_SIZE,
					     client_rdma_msg_handler);
		log_info("Client is connected to server.");

		// Send a message.
		init_waitqueue_head(&sem);

		// Set param.
		req_param = (struct rpc_req_param){ .rpc_ch = rpc_cli_ch,
						    .data = data,
						    .sem = &sem };

		log_info("Sending RPC message:%s", data);
		send_rpc_msg_to_server(&req_param);

		log_info("Waiting server response.");
		sem_wait(&sem);
		log_info("Resume the main thread.");

		break;
	}

	if (rpc_cli_ch == NULL) {
		printf("init_rpc_client failed.\n");
		ret = -1;
		goto out;
	}

	destroy_rpc_client(rpc_cli_ch);
	ret = 0;
out:

	return ret;
}