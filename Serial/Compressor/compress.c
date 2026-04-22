#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>

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
  FILE *file;
  unsigned char buffer;
  unsigned char bitCount;
  uint64_t bytesWritten;
} BitWriter;

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
Function that resizes a previously allocated memory block and verifies if the operation was successful.
Input:
    - ptr: pointer to the memory block that will be resized.
    - size: new number of bytes to allocate.
Output:
    - returns a pointer to the resized memory block.
*/

static void *reallocateMemory(void *ptr, size_t size) {
  void *tmp = realloc(ptr, size);
  if (tmp == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: memory couldn't be reallocated.|----\033[0m\n\n");
    free(ptr);
    exit(EXIT_FAILURE);
  }
  return tmp;
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
Function that compares two file entries using their relative paths.
Input:
    - a: pointer to the first file entry.
    - b: pointer to the second file entry.
Output:
    - returns a negative, zero or positive value according to strcmp rules.
*/

static int compareFileEntries(const void *a, const void *b) {
  const FileEntry *left = (const FileEntry *)a;
  const FileEntry *right = (const FileEntry *)b;
  return strcmp(left->relativePath, right->relativePath);
}

/*
Function that initializes the dynamic list of file entries.
Input:
    - list: pointer to the file list structure.
Output:
    - sets the initial values of the list fields.
*/

static void initList(FileList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

/*
Function that adds a new file entry to the dynamic list of files.
Input:
    - list: pointer to the file list structure.
    - relativePath: relative path of the file inside the input directory.
    - fullPath: full path of the file in the file system.
Output:
    - stores the new file entry in the list and updates its size if necessary.
*/

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

/*
Function that releases all dynamic memory used by the file list.
Input:
    - list: pointer to the file list structure.
Output:
    - frees every stored path and the dynamic array of file entries.
*/

static void freeList(FileList *list) {
  size_t i;

  for (i = 0; i < list->count; i++) {
    free(list->items[i].relativePath);
    free(list->items[i].fullPath);
  }

  free(list->items);
}

/*
Function that verifies if a file has .txt extension.
Input:
    - fileName: pointer to the file name.
Output:
    - returns 1 if the file ends in .txt, otherwise returns 0.
*/

static int hasTxtExtension(const char *fileName) {
  const char *dot = strrchr(fileName, '.');
  return dot != NULL && strcmp(dot, ".txt") == 0;
}

/*
Function that traverses a directory recursively and collects all valid text files.
Input:
    - baseDir: pointer to the base input directory.
    - relativeDir: pointer to the current relative subdirectory.
    - list: pointer to the file list structure.
Output:
    - fills the list with all .txt files found in the directory tree.
*/

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

/*
Function that starts the file collection process and sorts the resulting list.
Input:
    - directoryPath: pointer to the input directory path.
    - list: pointer to the file list structure.
Output:
    - stores the sorted list of files that will be compressed.
*/

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

/*
Function that counts the global frequency of every byte in all input files.
Input:
    - list: pointer to the file list structure.
    - freq: array that stores the frequency of each byte from 0 to 255.
Output:
    - fills the frequency array and stores the original size of each file.
*/

static void countFrequencies(FileList *list, uint32_t freq[ALPHABET_SIZE]) {
  unsigned char buffer[1024];
  size_t i;

  for (i = 0; i < ALPHABET_SIZE; i++) {
    freq[i] = 0;
  }

  for (i = 0; i < list->count; i++) {
    FILE *input = fopen(list->items[i].fullPath, "rb");
    size_t bytesRead;

    if (input == NULL) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
              list->items[i].fullPath);
      exit(EXIT_FAILURE);
    }

    list->items[i].originalSize = 0;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
      size_t j;
      for (j = 0; j < bytesRead; j++) {
        freq[buffer[j]]++;
      }
      list->items[i].originalSize += bytesRead;
    }

    fclose(input);
  }
}

/*
Function that calculates the compressed size of each file using the generated Huffman codes.
Input:
    - list: pointer to the file list structure.
    - codes: array of pointers to the Huffman codes for each byte.
Output:
    - stores the compressed size in bytes for each file entry.
*/

static void addCompressedSizes(FileList *list, char *codes[ALPHABET_SIZE]) {
  unsigned char buffer[1024];
  uint32_t codeLengths[ALPHABET_SIZE];
  size_t i;

  for (i = 0; i < ALPHABET_SIZE; i++) {
    codeLengths[i] = (codes[i] == NULL) ? 0u : (uint32_t)strlen(codes[i]);
  }

  for (i = 0; i < list->count; i++) {
    FILE *input = fopen(list->items[i].fullPath, "rb");
    uint64_t totalBits = 0;
    size_t bytesRead;

    if (input == NULL) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
              list->items[i].fullPath);
      exit(EXIT_FAILURE);
    }

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
      size_t j;
      for (j = 0; j < bytesRead; j++) {
        totalBits += codeLengths[buffer[j]];
      }
    }

    fclose(input);
    list->items[i].compressedSize = (totalBits + 7u) / 8u;
  }
}

/*
Function that writes a 32-bit unsigned integer in binary form to the output file.
Input:
    - output: pointer to the destination binary file.
    - value: unsigned 32-bit integer that will be written.
Output:
    - returns 1 if the 4 bytes were written successfully, otherwise returns 0.
*/

static int writeU32(FILE *output, uint32_t value) {
  unsigned char bytes[4];

  bytes[0] = (unsigned char)(value & 0xFFu);
  bytes[1] = (unsigned char)((value >> 8) & 0xFFu);
  bytes[2] = (unsigned char)((value >> 16) & 0xFFu);
  bytes[3] = (unsigned char)((value >> 24) & 0xFFu);

  return fwrite(bytes, 1, 4, output) == 4;
}

/*
Function that writes a 64-bit unsigned integer in binary form to the output file.
Input:
    - output: pointer to the destination binary file.
    - value: unsigned 64-bit integer that will be written.
Output:
    - returns 1 if the 8 bytes were written successfully, otherwise returns 0.
*/

static int writeU64(FILE *output, uint64_t value) {
  unsigned char bytes[8];
  int i;

  for (i = 0; i < 8; i++) {
    bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFFu);
  }

  return fwrite(bytes, 1, 8, output) == 8;
}

/*
Function that initializes the bit writer structure used to write compressed data.
Input:
    - writer: pointer to the bit writer structure.
    - file: pointer to the destination binary file.
Output:
    - sets the initial state of the bit writer.
*/

static void bitWriterInit(BitWriter *writer, FILE *file) {
  writer->file = file;
  writer->buffer = 0;
  writer->bitCount = 0;
  writer->bytesWritten = 0;
}

/*
Function that writes the bits of one Huffman code into the output stream.
Input:
    - writer: pointer to the bit writer structure.
    - code: pointer to the string that contains the Huffman code.
Output:
    - writes the code bit by bit into the output file buffer.
*/

static void bitWriterWriteCode(BitWriter *writer, const char *code) {
  size_t i;

  for (i = 0; code[i] != '\0'; i++) {
    writer->buffer = (unsigned char)(writer->buffer << 1);
    if (code[i] == '1') {
      writer->buffer |= 1u;
    }
    writer->bitCount++;

    if (writer->bitCount == 8) {
      fputc(writer->buffer, writer->file);
      writer->buffer = 0;
      writer->bitCount = 0;
      writer->bytesWritten++;
    }
  }
}

/*
Function that writes the last incomplete byte to the output file if there are pending bits.
Input:
    - writer: pointer to the bit writer structure.
Output:
    - flushes the remaining buffered bits and updates the number of written bytes.
*/

static void bitWriterFlush(BitWriter *writer) {
  if (writer->bitCount > 0) {
    writer->buffer = (unsigned char)(writer->buffer << (8 - writer->bitCount));
    fputc(writer->buffer, writer->file);
    writer->buffer = 0;
    writer->bitCount = 0;
    writer->bytesWritten++;
  }
}

/*
Function that writes the compressed data of one file into the archive.
Input:
    - output: pointer to the destination binary file.
    - file: pointer to the file entry that will be compressed and written.
    - codes: array of pointers to the Huffman codes for each byte.
Output:
    - writes the compressed file data to the archive and verifies the written size.
*/

static void writeCompressedFileData(FILE *output, const FileEntry *file, char *codes[ALPHABET_SIZE]) {
  unsigned char buffer[1024];
  FILE *input = fopen(file->fullPath, "rb");
  BitWriter writer;
  size_t bytesRead;

  if (input == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: couldn't open '%s'.|----\033[0m\n\n",
            file->fullPath);
    exit(EXIT_FAILURE);
  }

  bitWriterInit(&writer, output);

  while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
    size_t i;
    for (i = 0; i < bytesRead; i++) {
      bitWriterWriteCode(&writer, codes[buffer[i]]);
    }
  }

  bitWriterFlush(&writer);
  fclose(input);

  if (writer.bytesWritten != file->compressedSize) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: compressed size mismatch for '%s'.|----\033[0m\n\n",
            file->relativePath);
    exit(EXIT_FAILURE);
  }
}

/*
Function that creates the final binary archive and stores all metadata and compressed files.
Input:
    - outputPath: pointer to the name of the output binary file.
    - list: pointer to the list of file entries.
    - freq: array with the global frequency table used to rebuild the Huffman tree.
    - codes: array of pointers to the Huffman codes for each byte.
Output:
    - writes the complete compressed directory into one binary file.
*/

static void writeArchive(const char *outputPath,
                         const FileList *list,
                         const uint32_t freq[ALPHABET_SIZE],
                         char *codes[ALPHABET_SIZE]) {
  FILE *output = fopen(outputPath, "wb");
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

    if (!writeU32(output, pathLen) ||
        fwrite(file->relativePath, 1, pathLen, output) != pathLen ||
        !writeU64(output, file->originalSize) ||
        !writeU64(output, file->compressedSize)) {
      fclose(output);
      fprintf(stderr, "\033[41m\n----|ERROR: couldn't write file metadata.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (file->compressedSize > 0) {
      writeCompressedFileData(output, file, codes);
    }
  }

  fclose(output);
}

/*
Function that releases the dynamic memory used by the Huffman codes table.
Input:
    - codes: array of pointers to the Huffman codes.
Output:
    - frees every allocated Huffman code string.
*/

static void freeCodes(char *codes[ALPHABET_SIZE]) {
  size_t i;
  for (i = 0; i < ALPHABET_SIZE; i++) {
    free(codes[i]);
  }
}

int main(int argc, char *argv[]) {
  FileList list;
  HeapNode *root;
  char *codes[ALPHABET_SIZE] = {0};
  char path[ALPHABET_SIZE * 2];
  uint32_t freq[ALPHABET_SIZE];
  uint64_t totalOriginal = 0;
  uint64_t totalCompressed = 0;
  size_t i;
  
  // Time calculation variables
  
  struct timespec start, end;
  long seconds = 0;
  long nseconds = 0;
  double time = 0.0;

  if (argc != 3) {
    fprintf(stderr,
            "\033[41m\n----|Use: %s <directory> <output.bin>|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  initList(&list);
  collectFiles(argv[1], &list);
  countFrequencies(&list, freq);

  root = buildHuffmanTree(freq);
  buildCodes(root, path, 0, codes);

  addCompressedSizes(&list, codes);

  for (i = 0; i < list.count; i++) {
    totalOriginal += list.items[i].originalSize;
    totalCompressed += list.items[i].compressedSize;
  }

  writeArchive(argv[2], &list, freq, codes);

  clock_gettime(CLOCK_MONOTONIC, &end);
  
  seconds = end.tv_sec - start.tv_sec;
  nseconds = end.tv_nsec - start.tv_nsec;
  time = (seconds) * 1000L + (nseconds) / 1000000L;

  fprintf(stdout, "*** Directory %s compressed correctly ***\n", argv[1]);
  printf("| Files: %zu |\n", list.count);
  printf("| Original size: %llu bytes |\n", (unsigned long long)totalOriginal);
  printf("| Compressed size: %llu bytes |\n", (unsigned long long)totalCompressed);
  printf("| Time elapsed: %.6f ms |\n", time);

  freeCodes(codes);
  freeTree(root);
  freeList(&list);
  return EXIT_SUCCESS;
}
