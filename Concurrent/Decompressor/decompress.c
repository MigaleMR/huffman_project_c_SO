#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "huffman.h"

#ifdef _WIN32
#define mkdir(path, mode) _mkdir(path)
#include <direct.h>
#endif

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

typedef struct {
  ArchiveEntry *entry;
  const HeapNode *root;
  pthread_mutex_t *directoryMutex;
} DecompressTask;

static void *allocateMemory(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be allocated.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

static char *duplicateMemory(const char *text) {
  size_t len = strlen(text) + 1;
  char *copy = (char *)allocateMemory(len);
  memcpy(copy, text, len);
  return copy;
}

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

static void createDirectoryIfNeeded(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: directory couldn't be created '%s': %s|----\033[0m\n\n",
            path,
            strerror(errno));
    exit(EXIT_FAILURE);
  }
}

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

static void readExact(FILE *input, void *buffer, size_t size, const char *what) {
  if (size > 0 && fread(buffer, 1, size, input) != size) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: can not read %s from shared file.|----\033[0m\n\n",
            what);
    exit(EXIT_FAILURE);
  }
}

static uint32_t readU32(FILE *input) {
  unsigned char bytes[4];

  readExact(input, bytes, 4, "32 bits integer");
  return ((uint32_t)bytes[0]) |
         ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) |
         ((uint32_t)bytes[3] << 24);
}

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

static void memoryBitReaderInit(MemoryBitReader *reader,
                                const unsigned char *data,
                                uint64_t dataSize) {
  reader->data = data;
  reader->dataSize = dataSize;
  reader->byteIndex = 0;
  reader->buffer = 0;
  reader->bitsLeft = 0;
}

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

static void *decompressWorker(void *arg) {
  DecompressTask *task = (DecompressTask *)arg;
  FILE *output;

  pthread_mutex_lock(task->directoryMutex);
  ensureParentDirectories(task->entry->outputPath);
  pthread_mutex_unlock(task->directoryMutex);
  output = fopen(task->entry->outputPath, "wb");
  if (output == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't create '%s'.|----\033[0m\n\n",
            task->entry->outputPath);
    exit(EXIT_FAILURE);
  }

  decompressBlock(output,
                  task->root,
                  task->entry->originalSize,
                  task->entry->compressedSize,
                  task->entry->compressedData,
                  task->entry->relativePath);
  fclose(output);

  return NULL;
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

static void decompressEntriesParallel(ArchiveEntry *entries,
                                      uint32_t fileCount,
                                      const HeapNode *root) {
  size_t threadCount = fileCount;
  pthread_t *threads = (pthread_t *)allocateMemory(threadCount * sizeof(pthread_t));
  DecompressTask *tasks = (DecompressTask *)allocateMemory(threadCount * sizeof(DecompressTask));
  pthread_mutex_t directoryMutex;
  size_t i;

  if (pthread_mutex_init(&directoryMutex, NULL) != 0) {
    fprintf(stderr, "\033[41m\n----|ERROR: couldn't initialize directory mutex.|----\033[0m\n\n");
    free(tasks);
    free(threads);
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < threadCount; i++) {
    tasks[i].entry = &entries[i];
    tasks[i].root = root;
    tasks[i].directoryMutex = &directoryMutex;

    if (pthread_create(&threads[i], NULL, decompressWorker, &tasks[i]) != 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't create decompression thread.|----\033[0m\n\n");
      pthread_mutex_destroy(&directoryMutex);
      free(tasks);
      free(threads);
      exit(EXIT_FAILURE);
    }
  }

  for (i = 0; i < threadCount; i++) {
    pthread_join(threads[i], NULL);
  }

  pthread_mutex_destroy(&directoryMutex);
  free(tasks);
  free(threads);
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
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
#endif
}

static size_t parseThreadCount(const char *text) {
  char *endPtr = NULL;
  unsigned long value = strtoul(text, &endPtr, 10);

  if (text[0] == '\0' || endPtr == NULL || *endPtr != '\0' || value == 0) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: invalid thread count '%s'.|----\033[0m\n\n",
            text);
    exit(EXIT_FAILURE);
  }

  return (size_t)value;
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

  if (argc != 2 && argc != 3) {
    fprintf(stderr,
            "\033[41m\n----|Use: %s <input.bin> [threads]|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  if (argc == 3) {
    (void)parseThreadCount(argv[2]);
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
  decompressEntriesParallel(entries, fileCount, root);

  endMs = currentTimeMs();

  freeArchiveEntries(entries, fileCount);
  free(outputDir);
  freeTree(root);
  fclose(input);

  fprintf(stdout, "*** File %s decompressed correctly ***\n", argv[1]);
  printf("| Files: %u |\n", fileCount);
  printf("| Threads: %u |\n", fileCount);
  printf("| Time elapsed: %llu ms |\n", (unsigned long long)(endMs - startMs));

  return EXIT_SUCCESS;
}
