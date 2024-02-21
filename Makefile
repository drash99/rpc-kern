KDIR = /lib/modules/`uname -r`/build
rpc_test-objs := rpc_client_test.o bitmap.o rdma.o rpc_client.o rpc_common.o
obj-m += rpc_test.o

all:
	make -C $(KDIR) M=`pwd` modules

install:
	make -C $(KDIR) M=`pwd` modules_install INSTALL_MOD_STRIP=1
	depmod -a

clean:
	make -C $(KDIR) M=`pwd` clean
