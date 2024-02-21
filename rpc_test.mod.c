#include <linux/build-salt.h>
#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(.gnu.linkonce.this_module) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section(__versions) = {
	{ 0xf4fe1188, "module_layout" },
	{ 0x87b0ff51, "kmalloc_caches" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0x9875a8e1, "single_open" },
	{ 0x754d539c, "strlen" },
	{ 0x9e9202dd, "ib_dealloc_pd_user" },
	{ 0x3e230885, "single_release" },
	{ 0x22370a08, "remove_proc_entry" },
	{ 0x91fe6fb1, "rdma_destroy_id" },
	{ 0xfee7c6cb, "seq_read" },
	{ 0x58cb0daa, "pv_ops" },
	{ 0x81b523d5, "ib_destroy_cq_user" },
	{ 0xd9a5ea54, "__init_waitqueue_head" },
	{ 0x659c14b5, "ib_destroy_qp_user" },
	{ 0xc5850110, "printk" },
	{ 0x8ec90916, "module_put" },
	{ 0xdecd0b29, "__stack_chk_fail" },
	{ 0xac5fcec0, "in4_pton" },
	{ 0x2ea2c95c, "__x86_indirect_thunk_rax" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xf3e442c2, "rdma_disconnect" },
	{ 0xcbd4898c, "fortify_panic" },
	{ 0x5e122e26, "kmem_cache_alloc_trace" },
	{ 0xdbf17652, "_raw_spin_lock" },
	{ 0x3eeb2322, "__wake_up" },
	{ 0x9c401985, "ib_dereg_mr_user" },
	{ 0x1f433b7d, "seq_lseek" },
	{ 0x37a0cba, "kfree" },
	{ 0x69acdf38, "memcpy" },
	{ 0xc1b4cab8, "proc_create" },
	{ 0x362ef408, "_copy_from_user" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0xac36ff66, "try_module_get" },
};

MODULE_INFO(depends, "ib_core,rdma_cm");


MODULE_INFO(srcversion, "7AF697B9B7B97957828503C");
