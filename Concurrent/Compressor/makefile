CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

all: compress_pthread

compress_pthread: compress.c huffman.c
	$(CC) $(CFLAGS) -o compress_pthread compress.c huffman.c

clean:
	rm -f compress_pthread
