#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "huffman.h"

/* ------------------------------------------------------------------ */
/*  Tipos de datos                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
  char    *relativePath;
  char    *fullPath;
  uint64_t originalSize;
  uint64_t compressedSize;
} FileEntry;

typedef struct {
  FileEntry *items;
  size_t     count;
  size_t     capacity;
} FileList;

typedef struct {
  FILE          *file;
  unsigned char  buffer;
  unsigned char  bitCount;
  uint64_t       bytesWritten;
} BitWriter;

/* Resultado que cada hijo escribe en su slot exclusivo de mmap. */
typedef struct {
  uint32_t freq[ALPHABET_SIZE];
  uint64_t originalSize;
  uint64_t compressedSize;
} SharedResult;

/* ------------------------------------------------------------------ */
/*  Utilidades de memoria                                              */
/* ------------------------------------------------------------------ */

/* Reserva memoria; termina si falla. */
static void *allocateMemory(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo asignar memoria.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

/* Redimensiona bloque; termina si falla. */
static void *reallocateMemory(void *ptr, size_t size) {
  void *tmp = realloc(ptr, size);
  if (tmp == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo reasignar memoria.|----\033[0m\n\n");
    free(ptr);
    exit(EXIT_FAILURE);
  }
  return tmp;
}

/* Copia dinamica de cadena. */
static char *duplicateMemory(const char *text) {
  size_t len  = strlen(text) + 1;
  char *copy = (char *)allocateMemory(len);
  memcpy(copy, text, len);
  return copy;
}

/* Une dos fragmentos de ruta con '/' si hace falta. */
static char *joinPaths(const char *a, const char *b) {
  size_t lenA = strlen(a);
  size_t lenB = strlen(b);
  int needSlash = (lenA > 0 && a[lenA - 1] != '/');
  char *result = (char *)allocateMemory(lenA + lenB + (needSlash ? 2 : 1));
  memcpy(result, a, lenA);
  if (needSlash) { result[lenA] = '/'; memcpy(result + lenA + 1, b, lenB + 1); }
  else { memcpy(result + lenA, b, lenB + 1); }
  return result;
}

/* Compara dos FileEntry por ruta relativa. */
static int compareFileEntries(const void *a, const void *b) {
  return strcmp(((const FileEntry *)a)->relativePath, ((const FileEntry *)b)->relativePath);
}

/* ------------------------------------------------------------------ */
/*  Lista de archivos                                                  */
/* ------------------------------------------------------------------ */

static void initList(FileList *list) {
  list->items = NULL; list->count = 0; list->capacity = 0;
}

/* Agrega una entrada; duplica capacidad si es necesario. */
static void addToList(FileList *list, const char *relativePath, const char *fullPath) {
  FileEntry *entry;
  if (list->count == list->capacity) {
    size_t newCap = (list->capacity == 0) ? 16 : list->capacity * 2;
    list->items = (FileEntry *)reallocateMemory(list->items, newCap * sizeof(FileEntry));
    list->capacity = newCap;
  }
  entry = &list->items[list->count++];
  entry->relativePath = duplicateMemory(relativePath);
  entry->fullPath = duplicateMemory(fullPath);
  entry->originalSize = 0;
  entry->compressedSize = 0;
}

/* Libera toda la memoria de la lista. */
static void freeList(FileList *list) {
  size_t i;
  for (i = 0; i < list->count; i++) {
    free(list->items[i].relativePath);
    free(list->items[i].fullPath);
  }
  free(list->items);
}

/* ------------------------------------------------------------------ */
/*  Recorrido de directorio                                            */
/* ------------------------------------------------------------------ */

/* Devuelve 1 si el archivo tiene extension .txt. */
static int hasTxtExtension(const char *fileName) {
  const char *dot = strrchr(fileName, '.');
  return dot != NULL && strcmp(dot, ".txt") == 0;
}

/* Recorre el directorio recursivamente y recopila archivos .txt. */
static void collectFilesRecursive(const char *baseDir, const char *relativeDir, FileList *list) {
  char *currentDir = (relativeDir[0] == '\0') ? duplicateMemory(baseDir) : joinPaths(baseDir, relativeDir);
  DIR *dir = opendir(currentDir);
  struct dirent *entry;

  if (dir == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo abrir el directorio '%s': %s|----\033[0m\n\n", currentDir, strerror(errno));
    free(currentDir);
    exit(EXIT_FAILURE);
  }

  while ((entry = readdir(dir)) != NULL) {
    char *relativePath;
    char *fullPath;
    struct stat st;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    relativePath = (relativeDir[0] == '\0') ? duplicateMemory(entry->d_name) : joinPaths(relativeDir, entry->d_name);
    fullPath = joinPaths(baseDir, relativePath);

    if (stat(fullPath, &st) != 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: no se pudo aplicar stat a '%s': %s|----\033[0m\n\n", fullPath, strerror(errno));
      free(relativePath); free(fullPath);
      closedir(dir); free(currentDir);
      exit(EXIT_FAILURE);
    }

    if (S_ISDIR(st.st_mode)) collectFilesRecursive(baseDir, relativePath, list);
    else if (S_ISREG(st.st_mode) && hasTxtExtension(entry->d_name))
            addToList(list, relativePath, fullPath);

    free(relativePath);
    free(fullPath);
  }
  closedir(dir);
  free(currentDir);
}

/* Recopila todos los .txt del directorio y ordena la lista. */
static void collectFiles(const char *directoryPath, FileList *list) {
  collectFilesRecursive(directoryPath, "", list);
  if (list->count == 0) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se encontraron archivos en '%s'.|----\033[0m\n\n", directoryPath);
    exit(EXIT_FAILURE);
  }
  qsort(list->items, list->count, sizeof(FileEntry), compareFileEntries);
}

/* ------------------------------------------------------------------ */
/*  Escritor de bits sobre FILE*                                       */
/* ------------------------------------------------------------------ */

static void bitWriterInit(BitWriter *w, FILE *f) {
  w->file = f; w->buffer = 0; w->bitCount = 0; w->bytesWritten = 0;
}

/* Escribe los bits de un codigo Huffman en el flujo de salida. */
static void bitWriterWriteCode(BitWriter *w, const char *code) {
  size_t i;
  for (i = 0; code[i] != '\0'; i++) {
    w->buffer = (unsigned char)(w->buffer << 1);
    if (code[i] == '1') w->buffer |= 1u;
    if (++w->bitCount == 8) {
      fputc(w->buffer, w->file);
      w->buffer = 0; w->bitCount = 0; w->bytesWritten++;
    }
  }
}

/* Vacia los bits pendientes completando el ultimo byte con ceros. */
static void bitWriterFlush(BitWriter *w) {
  if (w->bitCount > 0) {
    w->buffer = (unsigned char)(w->buffer << (8 - w->bitCount));
    fputc(w->buffer, w->file);
    w->buffer = 0; w->bitCount = 0; w->bytesWritten++;
  }
}

/* ------------------------------------------------------------------ */
/*  Escritura binaria de enteros                                       */
/* ------------------------------------------------------------------ */

static int writeU32(FILE *out, uint32_t v) {
  unsigned char b[4];
  b[0]=(unsigned char)(v&0xFF); b[1]=(unsigned char)(v>>8&0xFF);
  b[2]=(unsigned char)(v>>16&0xFF); b[3]=(unsigned char)(v>>24&0xFF);
  return fwrite(b, 1, 4, out) == 4;
}

static int writeU64(FILE *out, uint64_t v) {
  unsigned char b[8]; int i;
  for (i=0;i<8;i++) b[i]=(unsigned char)(v>>(8*i)&0xFF);
  return fwrite(b, 1, 8, out) == 8;
}

/* ------------------------------------------------------------------ */
/*  FASE 1 - Conteo de frecuencias en paralelo                        */
/* ------------------------------------------------------------------ */

/*
 * Lanza un hijo por archivo para contar frecuencias de bytes.
 * Cada hijo escribe en shared[i] (slot exclusivo).
 * El padre fusiona los resultados tras esperar a todos los hijos.
 */
static void countFrequenciesParallel(FileList *list, uint32_t freq[ALPHABET_SIZE], SharedResult *shared) {
  size_t i;
  pid_t  pid;

  for (i = 0; i < ALPHABET_SIZE; i++) freq[i] = 0;

  for (i = 0; i < list->count; i++) {
    pid = fork();
    if (pid < 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: fork fallo en la fase de frecuencias.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO --- */
      unsigned char buffer[4096];
      size_t j;
      FILE *input;
      size_t bytesRead;

      for (j = 0; j < ALPHABET_SIZE; j++) shared[i].freq[j] = 0;
      shared[i].originalSize = 0;

      input = fopen(list->items[i].fullPath, "rb");
      if (input == NULL) {
        fprintf(stderr, "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n", list->items[i].fullPath);
        exit(EXIT_FAILURE);
      }

      /* RC1: cada hijo escribe solo en shared[i], sin conflicto con otros hijos. */
      while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        for (j = 0; j < bytesRead; j++) shared[i].freq[buffer[j]]++;
        shared[i].originalSize += (uint64_t)bytesRead;
      }

      fclose(input);
      exit(EXIT_SUCCESS);
    }
    /* El padre sigue creando hijos sin bloquearse. */
  }

  /* Barrera: el padre espera a todos los hijos antes de leer shared[]. */
  for (i = 0; i < list->count; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr, "\033[41m\n----|ERROR: proceso hijo fallo en la fase de frecuencias.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  /* RC2: fusion secuencial; todos los hijos ya terminaron. */
  for (i = 0; i < list->count; i++) {
    size_t j;
    for (j = 0; j < ALPHABET_SIZE; j++) freq[j] += shared[i].freq[j];
    list->items[i].originalSize = shared[i].originalSize;
  }
}

/* ------------------------------------------------------------------ */
/*  FASE 2 - Compresion en paralelo con archivos temporales           */
/* ------------------------------------------------------------------ */

/*
 * Lanza un hijo por archivo para comprimir con los codigos Huffman.
 * Cada hijo escribe su resultado en /tmp/huff_<ppid>_<i>.tmp y
 * reporta el tamaño en shared[i].compressedSize.
 * El padre ensambla el archivo final tras esperar a todos los hijos.
 */
static void compressFilesParallel(FileList *list, char *codes[ALPHABET_SIZE], SharedResult *shared, pid_t mainPid) {
  size_t i;
  pid_t  pid;

  for (i = 0; i < list->count; i++) {
    pid = fork();
    if (pid < 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: fork fallo en la fase de compresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO --- */
      char          tmpPath[256];
      FILE         *input;
      FILE         *output;
      unsigned char buffer[4096];
      BitWriter     writer;
      size_t        bytesRead;
      size_t        j;

      /* Nombre unico por indice para evitar colisiones en /tmp. */
      snprintf(tmpPath, sizeof(tmpPath), "/tmp/huff_%d_%zu.tmp", (int)mainPid, i);

      input = fopen(list->items[i].fullPath, "rb");
      if (input == NULL) {
        fprintf(stderr, "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n", list->items[i].fullPath);
        exit(EXIT_FAILURE);
      }

      output = fopen(tmpPath, "wb");
      if (output == NULL) {
        fprintf(stderr, "\033[41m\n----|ERROR: no se pudo crear el temporal '%s'.|----\033[0m\n\n", tmpPath);
        fclose(input);
        exit(EXIT_FAILURE);
      }

      /* RC4: cada hijo escribe en su propio FILE* y shared[i]; codes[] es de solo lectura. */
      bitWriterInit(&writer, output);
      while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        for (j = 0; j < bytesRead; j++)
          bitWriterWriteCode(&writer, codes[buffer[j]]);
      }
      bitWriterFlush(&writer);

      fclose(input);
      fclose(output);

      /* Reportar tamaño al padre a traves del slot exclusivo en mmap. */
      shared[i].compressedSize = writer.bytesWritten;

      exit(EXIT_SUCCESS);
    }
    /* El padre continua creando el siguiente hijo sin bloquearse. */
  }

  /* Barrera: el padre espera a todos los hijos antes de leer los tamaños. */
  for (i = 0; i < list->count; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr, "\033[41m\n----|ERROR: proceso hijo fallo en la fase de compresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  /* Todos los hijos terminaron; leer tamaños del mmap. */
  for (i = 0; i < list->count; i++) {
    list->items[i].compressedSize = shared[i].compressedSize;
  }
}

/* ------------------------------------------------------------------ */
/*  Escritura del archivo final                                        */
/* ------------------------------------------------------------------ */

/*
 * RC5: ensamblado secuencial inevitable.
 * La cabecera, tabla de frecuencias y bloques de datos deben ir
 * en orden fijo en el .bin, por lo que no puede paralelizarse.
 */
static void writeArchive(const char *outputPath, const FileList *list, const uint32_t freq[ALPHABET_SIZE], pid_t mainPid) {
  FILE  *output;
  size_t i;

  output = fopen(outputPath, "wb");
  if (output == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo crear '%s'.|----\033[0m\n\n", outputPath);
    exit(EXIT_FAILURE);
  }

  if (!writeU32(output, (uint32_t)list->count)) {
    fclose(output);
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo escribir la cabecera.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }

  for (i = 0; i < ALPHABET_SIZE; i++) {
    if (!writeU32(output, freq[i])) {
      fclose(output);
      fprintf(stderr, "\033[41m\n----|ERROR: no se pudo escribir la tabla de frecuencias.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  for (i = 0; i < list->count; i++) {
    const FileEntry *file    = &list->items[i];
    uint32_t         pathLen = (uint32_t)strlen(file->relativePath);

    if (!writeU32(output, pathLen) ||
        fwrite(file->relativePath, 1, pathLen, output) != pathLen ||
        !writeU64(output, file->originalSize) ||
        !writeU64(output, file->compressedSize)) {
      fclose(output);
      fprintf(stderr,
              "\033[41m\n----|ERROR: no se pudo escribir metadatos de '%s'.|----\033[0m\n\n",
              file->relativePath);
      exit(EXIT_FAILURE);
    }

    /* Copiar datos del temporal al archivo final y eliminar el temporal. */
    if (file->compressedSize > 0) {
      char          tmpPath[256];
      FILE         *tmpFile;
      unsigned char copyBuf[4096];
      size_t        bytesRead;

      snprintf(tmpPath, sizeof(tmpPath), "/tmp/huff_%d_%zu.tmp", (int)mainPid, i);
      tmpFile = fopen(tmpPath, "rb");
      if (tmpFile == NULL) {
        fclose(output);
        fprintf(stderr, "\033[41m\n----|ERROR: no se pudo abrir el temporal '%s'.|----\033[0m\n\n", tmpPath);
        exit(EXIT_FAILURE);
      }
      while ((bytesRead = fread(copyBuf, 1, sizeof(copyBuf), tmpFile)) > 0) {
        if (fwrite(copyBuf, 1, bytesRead, output) != bytesRead) {
          fclose(tmpFile); fclose(output);
          fprintf(stderr, "\033[41m\n----|ERROR: error al copiar datos de '%s'.|----\033[0m\n\n", file->relativePath);
          exit(EXIT_FAILURE);
        }
      }
      fclose(tmpFile);
      remove(tmpPath);
    }
  }

  fclose(output);
}

/* ------------------------------------------------------------------ */
/*  Liberacion de memoria                                              */
/* ------------------------------------------------------------------ */

static void freeCodes(char *codes[ALPHABET_SIZE]) {
  size_t i;
  for (i = 0; i < ALPHABET_SIZE; i++) free(codes[i]);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  FileList list;
  HeapNode *root;
  char *codes[ALPHABET_SIZE];
  char path[ALPHABET_SIZE * 2];
  uint32_t freq[ALPHABET_SIZE];
  size_t i;
  uint64_t totalOriginal = 0;
  uint64_t totalCompressed = 0;
  pid_t mainPid = getpid();
  SharedResult *shared;
  struct timespec start, end;
  double elapsedMs;

  for (i = 0; i < ALPHABET_SIZE; i++) codes[i] = NULL;

  if (argc != 3) {
    fprintf(stderr, "\033[41m\n----|Uso: %s <directorio> <salida.bin>|----\033[0m\n\n", argv[0]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  /* Recopilar lista de archivos. */
  initList(&list);
  collectFiles(argv[1], &list);

  /*
   * mmap compartido entre padre e hijos.
   * MAP_SHARED: escrituras de hijos visibles al padre.
   * MAP_ANONYMOUS: sin archivo de respaldo en disco.
   */
  shared = (SharedResult *)mmap(NULL, list.count * sizeof(SharedResult), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED) {
    fprintf(stderr, "\033[41m\n----|ERROR: mmap fallo para la memoria compartida.|----\033[0m\n\n");
    freeList(&list);
    return EXIT_FAILURE;
  }

  /* Fase 1: hijos cuentan frecuencias (RC1); padre fusiona (RC2). */
  countFrequenciesParallel(&list, freq, shared);

  for (i = 0; i < list.count; i++) totalOriginal += list.items[i].originalSize;

  /* RC3: el arbol Huffman necesita las frecuencias completas; no puede solaparse. */
  root = buildHuffmanTree(freq);
  buildCodes(root, path, 0, codes);

  /* Fase 2: hijos comprimen (RC4); padre ensambla el .bin (RC5). */
  compressFilesParallel(&list, codes, shared, mainPid);

  for (i = 0; i < list.count; i++) totalCompressed += list.items[i].compressedSize;

  munmap(shared, list.count * sizeof(SharedResult));

  /* RC5: ensamblado secuencial del .bin final. */
  writeArchive(argv[2], &list, freq, mainPid);

  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsedMs = (end.tv_sec  - start.tv_sec)  * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1.0e6;

  fprintf(stdout, "*** Directorio %s comprimido correctamente ***\n", argv[1]);
  printf("| Archivos: %zu |\n", list.count);
  printf("| Tamaño original: %llu bytes |\n", (unsigned long long)totalOriginal);
  printf("| Tamaño comprimido: %llu bytes |\n", (unsigned long long)totalCompressed);
  printf("| Procesos: %zu |\n", list.count);
  printf("| Tiempo transcurrido: %.6f ms |\n", elapsedMs);

  freeCodes(codes);
  freeTree(root);
  freeList(&list);
  return EXIT_SUCCESS;
}