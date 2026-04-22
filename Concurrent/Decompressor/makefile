CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

all: decompress_pthread

decompress_pthread: decompress.c huffman.c
	$(CC) $(CFLAGS) -o decompress_pthread decompress.c huffman.c

clean:
	rm -f decompress_pthread
