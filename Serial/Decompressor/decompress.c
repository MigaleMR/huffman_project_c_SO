#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#include "huffman.h"

typedef struct {
  FILE *file;
  unsigned char buffer;
  unsigned char bitsLeft;
  uint64_t bytesRemaining;
} BitReader;

/*
Function that allocates dynamic memory and verifies if the allocation was successful.
Input:
    - size: number of bytes to allocate.
Output:
    - returns a pointer to the allocated memory block.
*/

static void *allocateMemory(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be allocated.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

/*
Function that creates a dynamic copy of a string.
Input:
    - text: pointer to the original string.
Output:
    - returns a pointer to the copied string.
*/

static char *duplicateMemory(const char *text) {
  size_t len = strlen(text) + 1;
  char *copy = (char *)allocateMemory(len);
  memcpy(copy, text, len);
  return copy;
}

/*
Function that joins two path fragments into a single valid path.
Input:
    - a: pointer to the first part of the path.
    - b: pointer to the second part of the path.
Output:
    - returns a pointer to the new complete path.
*/

static char *joinPaths(const char *a, const char *b) {
  size_t lenA = strlen(a);
  size_t lenB = strlen(b);
  int needSlash = (lenA > 0 && a[lenA - 1] != '/');
  char *result = (char *)allocateMemory(lenA + lenB + (needSlash ? 2 : 1));

  memcpy(result, a, lenA);
  if (needSlash) {
    result[lenA] = '/';
    memcpy(result + lenA + 1, b, lenB + 1);
  } else {
    memcpy(result + lenA, b, lenB + 1);
  }

  return result;
}

/*
Function that generates the output directory name from the compressed .bin file name.
Input:
    - archivePath: path of the compressed binary file.
Output:
    - returns a pointer to the generated output directory path.
*/

static char *makeOutputDirectoryFromBin(const char *archivePath) {
  size_t len = strlen(archivePath);
  char *result = duplicateMemory(archivePath);

  if (len >= 4 && strcmp(archivePath + len - 4, ".bin") == 0) {
    result[len - 4] = '\0';
  } else {
    char *tmp = realloc(result, len + 5);
    if (tmp == NULL) {
      free(result);
      fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be reallocated.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
    result = tmp;
    memcpy(result + len, ".out", 5);
  }

  return result;
}

/*
Function that creates a directory if it does not already exist.
Input:
    - path: path of the directory to create.
Output:
    - creates the directory in the file system when necessary.
*/

static void createDirectoryIfNeeded(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: directory couldn't be created '%s': %s|----\033[0m\n\n",
            path,
            strerror(errno));
    exit(EXIT_FAILURE);
  }
}

/*
Function that ensures the complete directory tree exists for the output root directory.
Input:
    - directoryPath: path of the output directory tree.
Output:
    - creates all missing directories in the specified path.
*/

static void ensureDirectoryTree(const char *directoryPath) {
  char *copy = duplicateMemory(directoryPath);
  char *p;

  if (copy[0] == '\0') {
    free(copy);
    return;
  }

  for (p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      createDirectoryIfNeeded(copy);
      *p = '/';
    }
  }

  createDirectoryIfNeeded(copy);
  free(copy);
}

/*
Function that ensures all parent directories of a file path exist.
Input:
    - filePath: full path of the output file.
Output:
    - creates the missing parent directories required for the file.
*/

static void ensureParentDirectories(const char *filePath) {
  char *copy = duplicateMemory(filePath);
  char *p;

  for (p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      createDirectoryIfNeeded(copy);
      *p = '/';
    }
  }

  free(copy);
}

/*
Function that validates whether a relative path is unsafe for extraction.
Input:
    - path: relative path stored in the compressed archive.
Output:
    - returns 1 if the path is unsafe, otherwise returns 0.
*/

static int isUnsafeRelativePath(const char *path) {
  size_t i = 0;
  size_t len = strlen(path);

  if (len == 0 || path[0] == '/') {
    return 1;
  }

  while (i < len) {
    size_t start = i;
    size_t segmentLen;

    while (i < len && path[i] != '/') {
      i++;
    }

    segmentLen = i - start;
    if (segmentLen == 0) {
      return 1;
    }

    if (segmentLen == 1 && path[start] == '.') {
      return 1;
    }

    if (segmentLen == 2 && path[start] == '.' && path[start + 1] == '.') {
      return 1;
    }

    if (i < len && path[i] == '/') {
      i++;
    }
  }

  return 0;
}

/*
Function that reads an exact number of bytes from the compressed file.
Input:
    - input: pointer to the compressed input file.
    - buffer: pointer to the destination memory buffer.
    - size: number of bytes to read.
    - what: text description of the data being read.
Output:
    - stores the requested bytes in the destination buffer.
*/

static void readExact(FILE *input, void *buffer, size_t size, const char *what) {
  if (size > 0 && fread(buffer, 1, size, input) != size) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: can not read %s from shared file.|----\033[0m\n\n",
            what);
    exit(EXIT_FAILURE);
  }
}

/*
Function that reads a 32-bit unsigned integer from the compressed file.
Input:
    - input: pointer to the compressed input file.
Output:
    - returns the 32-bit unsigned integer that was read.
*/

static uint32_t readU32(FILE *input) {
  unsigned char bytes[4];

  readExact(input, bytes, 4, "32 bits integer");
  return ((uint32_t)bytes[0]) |
         ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) |
         ((uint32_t)bytes[3] << 24);
}

/*
Function that reads a 64-bit unsigned integer from the compressed file.
Input:
    - input: pointer to the compressed input file.
Output:
    - returns the 64-bit unsigned integer that was read.
*/

static uint64_t readU64(FILE *input) {
  unsigned char bytes[8];
  uint64_t value = 0;
  int i;

  readExact(input, bytes, 8, "64 bits integer");
  for (i = 0; i < 8; i++) {
    value |= ((uint64_t)bytes[i] << (8 * i));
  }

  return value;
}

/*
Function that initializes the bit reader structure used to read compressed data.
Input:
    - reader: pointer to the bit reader structure.
    - file: pointer to the compressed input file.
    - byteCount: number of compressed bytes available for the current block.
Output:
    - sets the initial state of the bit reader.
*/

static void bitReaderInit(BitReader *reader, FILE *file, uint64_t byteCount) {
  reader->file = file;
  reader->buffer = 0;
  reader->bitsLeft = 0;
  reader->bytesRemaining = byteCount;
}

/*
Function that reads one bit from the compressed input stream.
Input:
    - reader: pointer to the bit reader structure.
Output:
    - returns the next bit read from the input stream, or -1 if no more bits are available.
*/

static int bitReaderReadBit(BitReader *reader) {
  if (reader->bitsLeft == 0) {
    int value;

    if (reader->bytesRemaining == 0) {
      return -1;
    }

    value = fgetc(reader->file);
    if (value == EOF) {
      return -1;
    }

    reader->buffer = (unsigned char)value;
    reader->bitsLeft = 8;
    reader->bytesRemaining--;
  }

  reader->bitsLeft--;
  return (reader->buffer >> reader->bitsLeft) & 1;
}

/*
Function that consumes any remaining unread bytes of the current compressed block.
Input:
    - reader: pointer to the bit reader structure.
Output:
    - discards the remaining bytes of the current block and resets the pending bit count.
*/

static void bitReaderConsumeRemaining(BitReader *reader) {
  while (reader->bytesRemaining > 0) {
    if (fgetc(reader->file) == EOF) {
      break;
    }
    reader->bytesRemaining--;
  }
  reader->bitsLeft = 0;
}

/*
Function that decompresses one compressed file block using the Huffman tree.
Input:
    - input: pointer to the compressed input file.
    - output: pointer to the decompressed output file.
    - root: pointer to the root of the Huffman tree.
    - originalSize: number of bytes expected in the decompressed file.
    - compressedSize: number of bytes occupied by the compressed block.
    - relativePath: relative path of the file being decompressed.
Output:
    - writes the decompressed file contents to the output file.
*/

static void decompressBlock(FILE *input,
                            FILE *output,
                            const HeapNode *root,
                            uint64_t originalSize,
                            uint64_t compressedSize,
                            const char *relativePath) {
  BitReader reader;
  uint64_t written = 0;

  if (originalSize == 0) {
    bitReaderInit(&reader, input, compressedSize);
    bitReaderConsumeRemaining(&reader);
    return;
  }

  bitReaderInit(&reader, input, compressedSize);

  while (written < originalSize) {
    const HeapNode *current = root;

    while (!huffmanIsLeaf(current)) {
      int bit = bitReaderReadBit(&reader);
      if (bit < 0) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: unexpected end of compressed block '%s'.|----\033[0m\n\n",
                relativePath);
        exit(EXIT_FAILURE);
      }

      current = (bit == 0) ? huffmanGetLeft(current) : huffmanGetRight(current);
      if (current == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: invalid Huffman tree path for '%s'.|----\033[0m\n\n",
                relativePath);
        exit(EXIT_FAILURE);
      }
    }

    fputc(huffmanGetCharacter(current), output);
    written++;
  }

  bitReaderConsumeRemaining(&reader);
}

int main(int argc, char *argv[]) {
  FILE *input;
  HeapNode *root;
  char *outputDir;
  uint32_t fileCount;
  uint32_t freq[ALPHABET_SIZE];
  uint32_t i;
  
  // Time calculation variables
  
  struct timespec start, end;
  long seconds = 0;
  long nseconds = 0;
  double time = 0.0;

  if (argc != 2) {
    fprintf(stderr,
            "\033[41m\n----|Use: %s <input.bin>|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  input = fopen(argv[1], "rb");
  if (input == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
            argv[1]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  fileCount = readU32(input);
  for (i = 0; i < ALPHABET_SIZE; i++) {
    freq[i] = readU32(input);
  }

  root = buildHuffmanTree(freq);

  outputDir = makeOutputDirectoryFromBin(argv[1]);
  ensureDirectoryTree(outputDir);

  for (i = 0; i < fileCount; i++) {
    uint32_t pathLen = readU32(input);
    char *relativePath = (char *)allocateMemory((size_t)pathLen + 1);
    char *outputPath;
    uint64_t originalSize;
    uint64_t compressedSize;
    FILE *output;

    readExact(input, relativePath, pathLen, "file path");
    relativePath[pathLen] = '\0';

    if (isUnsafeRelativePath(relativePath)) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: invalid relative route: '%s'|----\033[0m\n\n",
              relativePath);
      free(relativePath);
      free(outputDir);
      freeTree(root);
      fclose(input);
      return EXIT_FAILURE;
    }

    originalSize = readU64(input);
    compressedSize = readU64(input);

    outputPath = joinPaths(outputDir, relativePath);
    ensureParentDirectories(outputPath);

    output = fopen(outputPath, "wb");
    if (output == NULL) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: couldn't create '%s'.|----\033[0m\n\n",
              outputPath);
      free(outputPath);
      free(relativePath);
      free(outputDir);
      freeTree(root);
      fclose(input);
      return EXIT_FAILURE;
    }

    decompressBlock(input, output, root, originalSize, compressedSize, relativePath);
    fclose(output);

    free(outputPath);
    free(relativePath);
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  
  seconds = end.tv_sec - start.tv_sec;
  nseconds = end.tv_nsec - start.tv_nsec;
  time = (seconds) * 1000L + (nseconds) / 1000000L;

  free(outputDir);
  freeTree(root);
  fclose(input);

  fprintf(stdout, "*** File %s decompressed correctly ***\n", argv[1]);
  printf("| Time elapsed: %.6f ms |\n", time);

  return EXIT_SUCCESS;
}
