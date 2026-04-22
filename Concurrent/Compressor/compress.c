#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "huffman.h"

typedef struct {
  char *relativePath;
  char *fullPath;
  uint64_t originalSize;
  uint64_t compressedSize;
} FileEntry;

typedef struct {
  FileEntry *items;
  size_t count;
  size_t capacity;
} FileList;

typedef struct {
  unsigned char *data;
  size_t size;
  size_t capacity;
  unsigned char buffer;
  unsigned char bitCount;
} MemoryBitWriter;

typedef struct {
  unsigned char *data;
  uint64_t size;
} CompressedFile;

typedef struct {
  FileEntry *file;
  uint32_t freq[ALPHABET_SIZE];
  uint32_t *globalFreq;
  pthread_mutex_t *freqMutex;
} FrequencyTask;

typedef struct {
  FileEntry *file;
  char **codes;
  CompressedFile *result;
} CompressTask;

static void *allocateMemory(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be allocated.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

static void *reallocateMemory(void *ptr, size_t size) {
  void *tmp = realloc(ptr, size);
  if (tmp == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be reallocated.|----\033[0m\n\n");
    free(ptr);
    exit(EXIT_FAILURE);
  }
  return tmp;
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

static int compareFileEntries(const void *a, const void *b) {
  const FileEntry *left = (const FileEntry *)a;
  const FileEntry *right = (const FileEntry *)b;
  return strcmp(left->relativePath, right->relativePath);
}

static void initList(FileList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

static void addToList(FileList *list, const char *relativePath, const char *fullPath) {
  FileEntry *entry;

  if (list->count == list->capacity) {
    size_t newCapacity = (list->capacity == 0) ? 16 : list->capacity * 2;
    list->items = (FileEntry *)reallocateMemory(list->items, newCapacity * sizeof(FileEntry));
    list->capacity = newCapacity;
  }

  entry = &list->items[list->count++];
  entry->relativePath = duplicateMemory(relativePath);
  entry->fullPath = duplicateMemory(fullPath);
  entry->originalSize = 0;
  entry->compressedSize = 0;
}

static void freeList(FileList *list) {
  size_t i;

  for (i = 0; i < list->count; i++) {
    free(list->items[i].relativePath);
    free(list->items[i].fullPath);
  }

  free(list->items);
}

static int hasTxtExtension(const char *fileName) {
  const char *dot = strrchr(fileName, '.');
  return dot != NULL && strcmp(dot, ".txt") == 0;
}

static void collectFilesRecursive(const char *baseDir, const char *relativeDir, FileList *list) {
  char *currentDir = (relativeDir[0] == '\0') ? duplicateMemory(baseDir) : joinPaths(baseDir, relativeDir);
  DIR *dir = opendir(currentDir);
  struct dirent *entry;

  if (dir == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: directory couldn't be opened '%s': %s|----\033[0m\n\n",
            currentDir,
            strerror(errno));
    free(currentDir);
    exit(EXIT_FAILURE);
  }

  while ((entry = readdir(dir)) != NULL) {
    char *relativePath;
    char *fullPath;
    struct stat st;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    relativePath = (relativeDir[0] == '\0') ? duplicateMemory(entry->d_name)
                                             : joinPaths(relativeDir, entry->d_name);
    fullPath = joinPaths(baseDir, relativePath);

    if (stat(fullPath, &st) != 0) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: couldn't apply stat to '%s': %s|----\033[0m\n\n",
              fullPath,
              strerror(errno));
      free(relativePath);
      free(fullPath);
      closedir(dir);
      free(currentDir);
      exit(EXIT_FAILURE);
    }

    if (S_ISDIR(st.st_mode)) {
      collectFilesRecursive(baseDir, relativePath, list);
    } else if (S_ISREG(st.st_mode) && hasTxtExtension(entry->d_name)) {
      addToList(list, relativePath, fullPath);
    }

    free(relativePath);
    free(fullPath);
  }

  closedir(dir);
  free(currentDir);
}

static void collectFiles(const char *directoryPath, FileList *list) {
  collectFilesRecursive(directoryPath, "", list);

  if (list->count == 0) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: files not found in '%s'.|----\033[0m\n\n",
            directoryPath);
    exit(EXIT_FAILURE);
  }

  qsort(list->items, list->count, sizeof(FileEntry), compareFileEntries);
}

static void memoryBitWriterInit(MemoryBitWriter *writer) {
  writer->data = NULL;
  writer->size = 0;
  writer->capacity = 0;
  writer->buffer = 0;
  writer->bitCount = 0;
}

static void memoryBitWriterAppendByte(MemoryBitWriter *writer, unsigned char value) {
  if (writer->size == writer->capacity) {
    size_t newCapacity = (writer->capacity == 0) ? 1024 : writer->capacity * 2;
    writer->data = (unsigned char *)reallocateMemory(writer->data, newCapacity);
    writer->capacity = newCapacity;
  }

  writer->data[writer->size++] = value;
}

static void memoryBitWriterWriteCode(MemoryBitWriter *writer, const char *code) {
  size_t i;

  for (i = 0; code[i] != '\0'; i++) {
    writer->buffer = (unsigned char)(writer->buffer << 1);
    if (code[i] == '1') {
      writer->buffer |= 1u;
    }
    writer->bitCount++;

    if (writer->bitCount == 8) {
      memoryBitWriterAppendByte(writer, writer->buffer);
      writer->buffer = 0;
      writer->bitCount = 0;
    }
  }
}

static void memoryBitWriterFlush(MemoryBitWriter *writer) {
  if (writer->bitCount > 0) {
    writer->buffer = (unsigned char)(writer->buffer << (8 - writer->bitCount));
    memoryBitWriterAppendByte(writer, writer->buffer);
    writer->buffer = 0;
    writer->bitCount = 0;
  }
}

static int writeU32(FILE *output, uint32_t value) {
  unsigned char bytes[4];

  bytes[0] = (unsigned char)(value & 0xFFu);
  bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
  bytes[2] = (unsigned char)((value >> 16) & 0xFFu);
  bytes[3] = (unsigned char)((value >> 24) & 0xFFu);

  return fwrite(bytes, 1, 4, output) == 4;
}

static int writeU64(FILE *output, uint64_t value) {
  unsigned char bytes[8];
  int i;

  for (i = 0; i < 8; i++) {
    bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFFu);
  }

  return fwrite(bytes, 1, 8, output) == 8;
}

static void compressFileToMemory(const FileEntry *file,
                                 char *codes[ALPHABET_SIZE],
                                 unsigned char **compressedData,
                                 uint64_t *compressedSize) {
  unsigned char buffer[4096];
  FILE *input = fopen(file->fullPath, "rb");
  MemoryBitWriter writer;
  size_t bytesRead;

  if (input == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
            file->fullPath);
    exit(EXIT_FAILURE);
  }

  memoryBitWriterInit(&writer);

  while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
    size_t i;
    for (i = 0; i < bytesRead; i++) {
      memoryBitWriterWriteCode(&writer, codes[buffer[i]]);
    }
  }

  memoryBitWriterFlush(&writer);
  fclose(input);

  *compressedData = writer.data;
  *compressedSize = (uint64_t)writer.size;
}

static void *frequencyWorker(void *arg) {
  FrequencyTask *task = (FrequencyTask *)arg;
  unsigned char buffer[4096];
  size_t i;
  FILE *input = fopen(task->file->fullPath, "rb");
  size_t bytesRead;

  for (i = 0; i < ALPHABET_SIZE; i++) {
    task->freq[i] = 0;
  }

  if (input == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
            task->file->fullPath);
    exit(EXIT_FAILURE);
  }

  task->file->originalSize = 0;
  while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
    size_t j;
    for (j = 0; j < bytesRead; j++) {
      task->freq[buffer[j]]++;
    }
    task->file->originalSize += bytesRead;
  }

  fclose(input);

  pthread_mutex_lock(task->freqMutex);
  for (i = 0; i < ALPHABET_SIZE; i++) {
    task->globalFreq[i] += task->freq[i];
  }
  pthread_mutex_unlock(task->freqMutex);

  return NULL;
}

static void countFrequenciesParallel(FileList *list, uint32_t freq[ALPHABET_SIZE]) {
  size_t threadCount = list->count;
  pthread_t *threads = (pthread_t *)allocateMemory(threadCount * sizeof(pthread_t));
  FrequencyTask *tasks = (FrequencyTask *)allocateMemory(threadCount * sizeof(FrequencyTask));
  pthread_mutex_t freqMutex;
  size_t i;

  for (i = 0; i < ALPHABET_SIZE; i++) {
    freq[i] = 0;
  }

  if (pthread_mutex_init(&freqMutex, NULL) != 0) {
    fprintf(stderr, "\033[41m\n----|ERROR: couldn't initialize frequency mutex.|----\033[0m\n\n");
    free(tasks);
    free(threads);
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < threadCount; i++) {
    tasks[i].file = &list->items[i];
    tasks[i].globalFreq = freq;
    tasks[i].freqMutex = &freqMutex;

    if (pthread_create(&threads[i], NULL, frequencyWorker, &tasks[i]) != 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't create frequency thread.|----\033[0m\n\n");
      pthread_mutex_destroy(&freqMutex);
      free(tasks);
      free(threads);
      exit(EXIT_FAILURE);
    }
  }

  for (i = 0; i < threadCount; i++) {
    pthread_join(threads[i], NULL);
  }

  pthread_mutex_destroy(&freqMutex);
  free(tasks);
  free(threads);
}

static void *compressWorker(void *arg) {
  CompressTask *task = (CompressTask *)arg;
  compressFileToMemory(task->file, task->codes, &task->result->data, &task->result->size);
  task->file->compressedSize = task->result->size;

  return NULL;
}

static void compressFilesParallel(FileList *list,
                                  char *codes[ALPHABET_SIZE],
                                  CompressedFile *results) {
  size_t threadCount = list->count;
  pthread_t *threads = (pthread_t *)allocateMemory(threadCount * sizeof(pthread_t));
  CompressTask *tasks = (CompressTask *)allocateMemory(threadCount * sizeof(CompressTask));
  size_t i;

  for (i = 0; i < list->count; i++) {
    results[i].data = NULL;
    results[i].size = 0;
  }

  for (i = 0; i < threadCount; i++) {
    tasks[i].file = &list->items[i];
    tasks[i].codes = codes;
    tasks[i].result = &results[i];

    if (pthread_create(&threads[i], NULL, compressWorker, &tasks[i]) != 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't create compression thread.|----\033[0m\n\n");
      free(tasks);
      free(threads);
      exit(EXIT_FAILURE);
    }
  }

  for (i = 0; i < threadCount; i++) {
    pthread_join(threads[i], NULL);
  }

  free(tasks);
  free(threads);
}

static uint64_t writeArchive(const char *outputPath,
                             FileList *list,
                             const uint32_t freq[ALPHABET_SIZE],
                             const CompressedFile *results) {
  FILE *output = fopen(outputPath, "wb");
  uint64_t totalCompressed = 0;
  size_t i;

  if (output == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't create '%s'.|----\033[0m\n\n",
            outputPath);
    exit(EXIT_FAILURE);
  }

  if (!writeU32(output, (uint32_t)list->count)) {
    fclose(output);
    fprintf(stderr, "\033[41m\n----|ERROR: couldn't write archive header.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < ALPHABET_SIZE; i++) {
    if (!writeU32(output, freq[i])) {
      fclose(output);
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't write frequency table.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  for (i = 0; i < list->count; i++) {
    const FileEntry *file = &list->items[i];
    uint32_t pathLen = (uint32_t)strlen(file->relativePath);

    totalCompressed += results[i].size;

    if (!writeU32(output, pathLen) ||
        fwrite(file->relativePath, 1, pathLen, output) != pathLen ||
        !writeU64(output, file->originalSize) ||
        !writeU64(output, results[i].size) ||
        (results[i].size > 0 &&
         fwrite(results[i].data, 1, (size_t)results[i].size, output) != (size_t)results[i].size)) {
      fclose(output);
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't write file metadata or data.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  fclose(output);
  return totalCompressed;
}

static void freeCodes(char *codes[ALPHABET_SIZE]) {
  size_t i;
  for (i = 0; i < ALPHABET_SIZE; i++) {
    free(codes[i]);
  }
}

static void freeCompressedFiles(CompressedFile *results, size_t count) {
  size_t i;

  for (i = 0; i < count; i++) {
    free(results[i].data);
  }

  free(results);
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
  FileList list;
  HeapNode *root;
  char *codes[ALPHABET_SIZE] = {0};
  char path[ALPHABET_SIZE * 2];
  uint32_t freq[ALPHABET_SIZE];
  uint64_t totalOriginal = 0;
  uint64_t totalCompressed = 0;
  uint64_t startMs;
  uint64_t endMs;
  CompressedFile *results;
  size_t i;

  if (argc != 3 && argc != 4) {
    fprintf(stderr,
            "\033[41m\n----|Use: %s <directory> <output.bin> [threads]|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  if (argc == 4) {
    (void)parseThreadCount(argv[3]);
  }

  startMs = currentTimeMs();

  initList(&list);
  collectFiles(argv[1], &list);
  countFrequenciesParallel(&list, freq);

  root = buildHuffmanTree(freq);
  buildCodes(root, path, 0, codes);

  for (i = 0; i < list.count; i++) {
    totalOriginal += list.items[i].originalSize;
  }

  results = (CompressedFile *)allocateMemory(list.count * sizeof(CompressedFile));
  compressFilesParallel(&list, codes, results);
  totalCompressed = writeArchive(argv[2], &list, freq, results);
  endMs = currentTimeMs();

  fprintf(stdout, "*** Directory %s compressed correctly ***\n", argv[1]);
  printf("| Files: %zu |\n", list.count);
  printf("| Original size: %llu bytes |\n", (unsigned long long)totalOriginal);
  printf("| Compressed size: %llu bytes |\n", (unsigned long long)totalCompressed);
  printf("| Threads: %zu |\n", list.count);
  printf("| Time elapsed: %llu ms |\n", (unsigned long long)(endMs - startMs));

  freeCompressedFiles(results, list.count);
  freeCodes(codes);
  freeTree(root);
  freeList(&list);
  return EXIT_SUCCESS;
}
