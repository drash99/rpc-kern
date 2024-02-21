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
	{ 0x3e230885, "single_release" },
	{ 0xfee7c6cb, "seq_read" },
	{ 0x1f433b7d, "seq_lseek" },
	{ 0x22370a08, "remove_proc_entry" },
	{ 0xc1b4cab8, "proc_create" },
	{ 0x362ef408, "_copy_from_user" },
	{ 0x88db9f48, "__check_object_size" },
	{ 0xeb233a45, "__kmalloc" },
	{ 0xdecd0b29, "__stack_chk_fail" },
	{ 0xd9a5ea54, "__init_waitqueue_head" },
	{ 0xac5fcec0, "in4_pton" },
	{ 0x5e122e26, "kmem_cache_alloc_trace" },
	{ 0x87b0ff51, "kmalloc_caches" },
	{ 0x8ec90916, "module_put" },
	{ 0xac36ff66, "try_module_get" },
	{ 0x9875a8e1, "single_open" },
	{ 0x37a0cba, "kfree" },
	{ 0x3eeb2322, "__wake_up" },
	{ 0xc5850110, "printk" },
	{ 0xbdfb6dbb, "__fentry__" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "9122957000C6CC14558275F");
