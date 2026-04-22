CC = gcc
CFLAGS = -Wall -Wextra -O2

TARGET = decompress_parallel
SOURCES = decompress.c huffman.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)
