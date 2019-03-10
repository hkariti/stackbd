
.PHONY: all

all: stackbd_util read_block_log

stackbd_util: stackbd_util.o ../common/stackbd.h
	gcc -o stackbd_util stackbd_util.c

read_block_log: read_block_log.o

clean:
	rm stackbd_util.o stackbd_util
