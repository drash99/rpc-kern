#include "test_global.h"
#include "rpc.h"
#include "rdma.h"


MODULE_AUTHOR("Seunghyun An");
MODULE_DESCRIPTION("RPC RDMA Kernel Client");
MODULE_LICENSE("GPL");

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
	wake_up_interruptible(sem);

	kfree(msg);
	kfree(param);
}

// Called in the requester thread.
void rpc_shmem_client_handler(void *arg)
{
	struct rpc_msg *msg;
	msg = (struct rpc_msg *)arg;

	printk(KERN_DEBUG "(shmem_ch_CLIENT_test) received: seqn=%lu data=%s",
		  msg->header.seqn, msg->data);
}

int test_main(void)
{
	char *data = "hello world";
	struct rpc_ch_info *rpc_cli_ch;
	int ret, msgbuf_id;
	enum rpc_channel_type ch_type;
	wait_queue_head_t sem;
	struct rpc_req_param req_param;

	char * g_ip_addr_res = kmalloc(16, GFP_KERNEL);
	in4_pton(g_ip_addr, -1, g_ip_addr_res, -1, NULL);

		printk(KERN_INFO "Channel type : RDMA");
		ch_type = RPC_CH_RDMA;
	switch (ch_type) {
	case RPC_CH_RDMA:
		rpc_cli_ch = init_rpc_client(RPC_CH_RDMA, g_ip_addr_res, g_port,
					     MAX_MSG_DATA_SIZE,
					     client_rdma_msg_handler);
		printk(KERN_INFO "Client is connected to server.");

		// Send a message.
		init_waitqueue_head(&sem);

		// Set param.
		req_param = (struct rpc_req_param){ .rpc_ch = rpc_cli_ch,
						    .data = data,
						    .sem = &sem };

		printk(KERN_INFO "Sending RPC message:%s", data);
		send_rpc_msg_to_server(&req_param);

		printk(KERN_INFO "Waiting server response.");
		//wait_event_interruptible(sem, 1);
		
		printk(KERN_INFO "Resume the main thread.");

		break;
	}

	if (rpc_cli_ch == NULL) {
		printk(KERN_ERR PFX "init_rpc_client failed.\n");
		ret = -1;
		goto out;
	}

	destroy_rpc_client(rpc_cli_ch);
	ret = 0;
out:
	
	//thpool_wait(handler_thpool);
	//thpool_destroy(handler_thpool);

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
	printk(KERN_INFO PFX "proc read called...\n");

	/*mutex_lock(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			seq_printk(KERN_ERR PFX seq,
			     "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
			     num++, cb->pd->device->name, cb->stats.send_bytes,
			     cb->stats.send_msgs, cb->stats.recv_bytes,
			     cb->stats.recv_msgs, cb->stats.write_bytes,
			     cb->stats.write_msgs,
			     cb->stats.read_bytes,
			     cb->stats.read_msgs);
		} else {
			seq_printk(KERN_ERR PFX seq, "%d listen\n", num++);
		}
	}
	mutex_unlock(&krping_mutex);*/
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
	printk(KERN_INFO PFX "proc write |%s|\n", cmd);
	
	rc = test_main();
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

static DEFINE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;

static int __init krping_init(void)
{
	printk("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krping_exit(void)
{
	printk("krping_exit\n");
	remove_proc_entry("krping", NULL);
}
module_init(krping_init);
module_exit(krping_exit);