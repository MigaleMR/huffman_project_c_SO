CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGET = compress_parallel
SOURCES = compress.c huffman.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)
