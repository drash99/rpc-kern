#include "rpc.h"

// TODO: Need to profile this lock contention.
uint64_t alloc_msgbuf_id(struct rpc_ch_info *rpc_ch)
{
	uint64_t bit_id;
	int ret;
 
	ret = 0;
	while (1) {
		spin_lock(&rpc_ch->msgbuf_bitmap_lock);
		ret = bit_array_find_first_clear_bit(rpc_ch->msgbuf_bitmap,
						     &bit_id);
		if (ret)
			bit_array_set_bit(rpc_ch->msgbuf_bitmap, bit_id);
		spin_unlock(&rpc_ch->msgbuf_bitmap_lock);

		if (ret)
			break;
		else
			printk(KERN_INFO "Failed to alloc a msgbuf id.");
	}

	// log_debug("[MSGBUF] alloc msgbuf=%lu", bit_id);

	return bit_id;
}

void free_msgbuf_id(struct rpc_ch_info *rpc_ch, uint64_t bit_id)
{
	// log_debug("[MSGBUF] free msgbuf=%lu", bit_id);
	spin_lock(&rpc_ch->msgbuf_bitmap_lock);
	bit_array_clear_bit(rpc_ch->msgbuf_bitmap, bit_id);
	spin_unlock(&rpc_ch->msgbuf_bitmap_lock);
}
