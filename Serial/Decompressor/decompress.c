#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "huffman.h"

typedef struct {
  const unsigned char *data;
  uint64_t dataSize;
  uint64_t byteIndex;
  unsigned char buffer;
  unsigned char bitsLeft;
} MemoryBitReader;

typedef struct {
  char *relativePath;
  char *outputPath;
  unsigned char *compressedData;
  uint64_t originalSize;
  uint64_t compressedSize;
} ArchiveEntry;

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

static void memoryBitReaderInit(MemoryBitReader *reader,
                                const unsigned char *data,
                                uint64_t dataSize) {
  reader->data = data;
  reader->dataSize = dataSize;
  reader->byteIndex = 0;
  reader->buffer = 0;
  reader->bitsLeft = 0;
}

/*
Function that reads one bit from the compressed input stream.
Input:
    - reader: pointer to the bit reader structure.
Output:
    - returns the next bit read from the input stream, or -1 if no more bits are available.
*/

static int memoryBitReaderReadBit(MemoryBitReader *reader) {
  if (reader->bitsLeft == 0) {
    if (reader->byteIndex >= reader->dataSize) {
      return -1;
    }

    reader->buffer = reader->data[reader->byteIndex++];
    reader->bitsLeft = 8;
  }

  reader->bitsLeft--;
  return (reader->buffer >> reader->bitsLeft) & 1;
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

static void decompressBlock(FILE *output,
                            const HeapNode *root,
                            uint64_t originalSize,
                            uint64_t compressedSize,
                            const unsigned char *compressedData,
                            const char *relativePath) {
  MemoryBitReader reader;
  uint64_t written = 0;

  if (originalSize == 0) {
    return;
  }

  memoryBitReaderInit(&reader, compressedData, compressedSize);

  while (written < originalSize) {
    const HeapNode *current = root;

    while (!huffmanIsLeaf(current)) {
      int bit = memoryBitReaderReadBit(&reader);
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
}

static ArchiveEntry *readArchiveEntries(FILE *input, uint32_t fileCount, const char *outputDir) {
  ArchiveEntry *entries = (ArchiveEntry *)allocateMemory((size_t)fileCount * sizeof(ArchiveEntry));
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    uint32_t pathLen = readU32(input);
    entries[i].relativePath = (char *)allocateMemory((size_t)pathLen + 1);
    entries[i].compressedData = NULL;

    readExact(input, entries[i].relativePath, pathLen, "file path");
    entries[i].relativePath[pathLen] = '\0';

    if (isUnsafeRelativePath(entries[i].relativePath)) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: invalid relative route: '%s'|----\033[0m\n\n",
              entries[i].relativePath);
      exit(EXIT_FAILURE);
    }

    entries[i].originalSize = readU64(input);
    entries[i].compressedSize = readU64(input);

    if (entries[i].compressedSize > 0) {
      if (entries[i].compressedSize > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "\033[41m\n----|ERROR: compressed block too large.|----\033[0m\n\n");
        exit(EXIT_FAILURE);
      }

      entries[i].compressedData = (unsigned char *)allocateMemory((size_t)entries[i].compressedSize);
      readExact(input,
                entries[i].compressedData,
                (size_t)entries[i].compressedSize,
                "compressed block");
    }

    entries[i].outputPath = joinPaths(outputDir, entries[i].relativePath);
  }

  return entries;
}

static void decompressEntriesSequential(ArchiveEntry *entries,
                                        uint32_t fileCount,
                                        const HeapNode *root) {
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    FILE *output;

    ensureParentDirectories(entries[i].outputPath);
    output = fopen(entries[i].outputPath, "wb");
    if (output == NULL) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: couldn't create '%s'.|----\033[0m\n\n",
              entries[i].outputPath);
      exit(EXIT_FAILURE);
    }

    decompressBlock(output,
                    root,
                    entries[i].originalSize,
                    entries[i].compressedSize,
                    entries[i].compressedData,
                    entries[i].relativePath);
    fclose(output);
  }
}

static void freeArchiveEntries(ArchiveEntry *entries, uint32_t fileCount) {
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    free(entries[i].relativePath);
    free(entries[i].outputPath);
    free(entries[i].compressedData);
  }

  free(entries);
}

static uint64_t currentTimeMs(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency;
  LARGE_INTEGER counter;

  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart * 1000ull) / frequency.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
#endif
}

int main(int argc, char *argv[]) {
  FILE *input;
  HeapNode *root;
  char *outputDir;
  ArchiveEntry *entries;
  uint32_t fileCount;
  uint32_t freq[ALPHABET_SIZE];
  uint64_t startMs;
  uint64_t endMs;
  uint32_t i;

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

  startMs = currentTimeMs();

  fileCount = readU32(input);
  for (i = 0; i < ALPHABET_SIZE; i++) {
    freq[i] = readU32(input);
  }

  root = buildHuffmanTree(freq);

  outputDir = makeOutputDirectoryFromBin(argv[1]);
  ensureDirectoryTree(outputDir);
  entries = readArchiveEntries(input, fileCount, outputDir);
  decompressEntriesSequential(entries, fileCount, root);

  endMs = currentTimeMs();

  freeArchiveEntries(entries, fileCount);
  free(outputDir);
  freeTree(root);
  fclose(input);

  fprintf(stdout, "*** File %s decompressed correctly ***\n", argv[1]);
  printf("| Files: %u |\n", fileCount);
  printf("| Threads: 1 |\n");
  printf("| Time elapsed: %llu ms |\n", (unsigned long long)(endMs - startMs));

  return EXIT_SUCCESS;
}
