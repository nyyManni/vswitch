obj-m += vswitch.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

build:
	rsync -av . linux-tester:vswitch
	ssh linux-tester 'cd vswitch && make'
