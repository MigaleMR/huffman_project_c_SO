#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "huffman.h"

/* ------------------------------------------------------------------ */
/*  Tipos de datos                                                      */
/* ------------------------------------------------------------------ */

/* Lector de bits sobre un buffer en memoria. */
typedef struct {
  const unsigned char *data;
  uint64_t dataSize;
  uint64_t byteIndex;
  unsigned char buffer;
  unsigned char bitsLeft;
} MemoryBitReader;

/* Entrada cargada desde el archivo comprimido. */
typedef struct {
  char *relativePath;
  char *outputPath;
  unsigned char *compressedData;
  uint64_t originalSize;
  uint64_t compressedSize;
} ArchiveEntry;

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

/* Copia dinamica de una cadena. */
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
  int    needSlash = (lenA > 0 && a[lenA - 1] != '/');
  char  *result = (char *)allocateMemory(lenA + lenB + (needSlash ? 2 : 1));
  memcpy(result, a, lenA);
  if (needSlash) { result[lenA] = '/'; memcpy(result + lenA + 1, b, lenB + 1); }
  else { memcpy(result + lenA, b, lenB + 1); }
  return result;
}

/* ------------------------------------------------------------------ */
/*  Utilidades de directorio                                           */
/* ------------------------------------------------------------------ */

/* Genera el directorio de salida quitando la extension .bin. */
static char *makeOutputDirectoryFromBin(const char *archivePath) {
  size_t len = strlen(archivePath);
  char *result = duplicateMemory(archivePath);
  if (len >= 4 && strcmp(archivePath + len - 4, ".bin") == 0) {
    result[len - 4] = '\0';
  } else {
    char *tmp = realloc(result, len + 5);
    if (!tmp) { free(result); exit(EXIT_FAILURE); }
    result = tmp;
    memcpy(result + len, ".out", 5);
  }
  return result;
}

/* Crea el directorio si no existe. */
static void createDirectoryIfNeeded(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo crear el directorio '%s': %s|----\033[0m\n\n", path, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

/* Crea recursivamente todos los componentes de una ruta. */
static void ensureDirectoryTree(const char *directoryPath) {
  char *copy = duplicateMemory(directoryPath);
  char *p;
  if (copy[0] == '\0') { free(copy); return; }
  for (p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') { *p = '\0'; createDirectoryIfNeeded(copy); *p = '/'; }
  }
  createDirectoryIfNeeded(copy);
  free(copy);
}

/* Crea todos los directorios padre del archivo dado. */
static void ensureParentDirectories(const char *filePath) {
  char *copy = duplicateMemory(filePath);
  char *p;
  for (p = copy + 1; *p != '\0'; p++) {
    if (*p == '/') { *p = '\0'; createDirectoryIfNeeded(copy); *p = '/'; }
  }
  free(copy);
}

/* ------------------------------------------------------------------ */
/*  Validacion de ruta                                                 */
/* ------------------------------------------------------------------ */

/* Devuelve 1 si la ruta es insegura (absoluta, vacia, o contiene '..'). */
static int isUnsafeRelativePath(const char *path) {
  size_t i = 0, len = strlen(path);
  if (len == 0 || path[0] == '/') return 1;
  while (i < len) {
    size_t start = i, segLen;
    while (i < len && path[i] != '/') i++;
    segLen = i - start;
    if (segLen == 0) return 1;
    if (segLen == 1 && path[start] == '.') return 1;
    if (segLen == 2 && path[start] == '.' && path[start+1] == '.') return 1;
    if (i < len && path[i] == '/') i++;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Lectura binaria de enteros                                         */
/* ------------------------------------------------------------------ */

static void readExact(FILE *in, void *buf, size_t size, const char *what) {
  if (size > 0 && fread(buf, 1, size, in) != size) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo leer %s del archivo.|----\033[0m\n\n", what);
    exit(EXIT_FAILURE);
  }
}

static uint32_t readU32(FILE *in) {
  unsigned char b[4];
  readExact(in, b, 4, "entero de 32 bits");
  return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}

static uint64_t readU64(FILE *in) {
  unsigned char b[8]; uint64_t v=0; int i;
  readExact(in, b, 8, "entero de 64 bits");
  for (i=0;i<8;i++) v|=((uint64_t)b[i]<<(8*i));
  return v;
}

/* ------------------------------------------------------------------ */
/*  Lector de bits en memoria                                          */
/* ------------------------------------------------------------------ */

static void memoryBitReaderInit(MemoryBitReader *r, const unsigned char *data, uint64_t dataSize) {
  r->data=data; r->dataSize=dataSize; r->byteIndex=0; r->buffer=0; r->bitsLeft=0;
}

/* Lee el siguiente bit del buffer; devuelve -1 si no hay mas datos. */
static int memoryBitReaderReadBit(MemoryBitReader *r) {
  if (r->bitsLeft == 0) {
    if (r->byteIndex >= r->dataSize) return -1;
    r->buffer = r->data[r->byteIndex++]; r->bitsLeft = 8;
  }
  r->bitsLeft--;
  return (r->buffer >> r->bitsLeft) & 1;
}

/* ------------------------------------------------------------------ */
/*  Descompresion de un bloque                                         */
/*                                                                     */
/*  El arbol Huffman es de solo lectura (heredado via CoW del padre).  */
/*  compressedData es privado del hijo; no hay estado compartido       */
/*  mutable, por lo que no se necesita sincronizacion.                 */
/* ------------------------------------------------------------------ */

/*
 * Descomprime un bloque y escribe el resultado en el archivo de salida.
 */
static void decompressBlock(FILE               *output,
                            const HeapNode     *root,
                            uint64_t            originalSize,
                            uint64_t            compressedSize,
                            const unsigned char *compressedData,
                            const char         *relativePath) {
  MemoryBitReader reader;
  uint64_t        written = 0;

  if (originalSize == 0) return;

  memoryBitReaderInit(&reader, compressedData, compressedSize);

  while (written < originalSize) {
    const HeapNode *node = root;
    int bit;
    while (node->left != NULL) {
      bit = memoryBitReaderReadBit(&reader);
      if (bit < 0) {
        fprintf(stderr, "\033[41m\n----|ERROR: datos comprimidos incompletos en '%s'.|----\033[0m\n\n", relativePath);
        exit(EXIT_FAILURE);
      }
      node = (bit == 0) ? node->left : node->right;
    }
    fputc((unsigned char)node->symbol, output);
    written++;
  }
}

/* ------------------------------------------------------------------ */
/*  Lectura secuencial del archivo comprimido                          */
/* ------------------------------------------------------------------ */

/*
 * Lee todas las entradas del .bin y las carga en memoria.
 * Debe ejecutarse en un solo proceso (flujo secuencial del archivo).
 */
static ArchiveEntry *readArchiveEntries(FILE *input, uint32_t fileCount, const char *outputDir) {
  ArchiveEntry *entries = (ArchiveEntry *)allocateMemory((size_t)fileCount * sizeof(ArchiveEntry));
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    uint32_t pathLen = readU32(input);

    entries[i].relativePath = (char *)allocateMemory((size_t)pathLen + 1);
    entries[i].compressedData = NULL;

    readExact(input, entries[i].relativePath, pathLen, "ruta del archivo");
    entries[i].relativePath[pathLen] = '\0';

    if (isUnsafeRelativePath(entries[i].relativePath)) {
      fprintf(stderr, "\033[41m\n----|ERROR: ruta relativa invalida: '%s'|----\033[0m\n\n", entries[i].relativePath);
      exit(EXIT_FAILURE);
    }

    entries[i].originalSize   = readU64(input);
    entries[i].compressedSize = readU64(input);

    if (entries[i].compressedSize > 0) {
      if (entries[i].compressedSize > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "\033[41m\n----|ERROR: bloque comprimido demasiado grande.|----\033[0m\n\n");
        exit(EXIT_FAILURE);
      }
      entries[i].compressedData = (unsigned char *)allocateMemory((size_t)entries[i].compressedSize);
      readExact(input, entries[i].compressedData, (size_t)entries[i].compressedSize, "bloque comprimido");
    }

    entries[i].outputPath = joinPaths(outputDir, entries[i].relativePath);
  }

  return entries;
}

/* ------------------------------------------------------------------ */
/*  Creacion de directorios ANTES de bifurcar                          */
/* ------------------------------------------------------------------ */

/*
 * RCD1: el padre crea todos los directorios de forma secuencial antes
 * de cualquier fork(). Cuando los hijos arrancan, los directorios ya
 * existen y no hay condicion de carrera.
 */
static void createAllOutputDirectories(const ArchiveEntry *entries, uint32_t fileCount) {
  uint32_t i;
  for (i = 0; i < fileCount; i++) {
    ensureParentDirectories(entries[i].outputPath);
  }
}

/* ------------------------------------------------------------------ */
/*  DESCOMPRESION en paralelo con fork()                               */
/* ------------------------------------------------------------------ */

/*
 * Crea un hijo por entrada para descomprimir en paralelo.
 * Los directorios ya existen (RCD1); los hijos abren sus archivos directamente.
 * El arbol Huffman es compartido de solo lectura (CoW); no hay estado mutable compartido.
 */
static void decompressEntriesParallel(ArchiveEntry *entries, uint32_t fileCount, const HeapNode *root) {
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      fprintf(stderr, "\033[41m\n----|ERROR: fork fallo en la fase de descompresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO --- */
      FILE *output;

      output = fopen(entries[i].outputPath, "wb");
      if (output == NULL) {
        fprintf(stderr, "\033[41m\n----|ERROR: no se pudo crear '%s'.|----\033[0m\n\n", entries[i].outputPath);
        exit(EXIT_FAILURE);
      }

      /* RCD2: arbol de solo lectura + buffer privado = sin condicion de carrera. */
      decompressBlock(output, root, entries[i].originalSize, entries[i].compressedSize, entries[i].compressedData, entries[i].relativePath);

      fclose(output);
      exit(EXIT_SUCCESS);
    }
    /* El padre continua creando el siguiente hijo sin bloquearse. */
  }

  /* Barrera: el padre espera a todos los hijos; el codigo de salida indica exito o fallo. */
  for (i = 0; i < fileCount; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr, "\033[41m\n----|ERROR: proceso hijo fallo en la fase de descompresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Liberacion de memoria                                              */
/* ------------------------------------------------------------------ */

/* Libera toda la memoria dinamica del arreglo de entradas. */
static void freeArchiveEntries(ArchiveEntry *entries, uint32_t fileCount) {
  uint32_t i;
  for (i = 0; i < fileCount; i++) {
    free(entries[i].relativePath);
    free(entries[i].outputPath);
    free(entries[i].compressedData);
  }
  free(entries);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  FILE *input;
  HeapNode *root;
  char *outputDir;
  ArchiveEntry *entries;
  uint32_t fileCount;
  uint32_t freq[ALPHABET_SIZE];
  uint32_t i;
  struct timespec start, end;
  double elapsedMs;

  if (argc != 2) {
    fprintf(stderr, "\033[41m\n----|Uso: %s <entrada.bin>|----\033[0m\n\n", argv[0]);
    return EXIT_FAILURE;
  }

  input = fopen(argv[1], "rb");
  if (input == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n", argv[1]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  /* Leer cabecera y tabla de frecuencias (secuencial). */
  fileCount = readU32(input);
  for (i = 0; i < ALPHABET_SIZE; i++) freq[i] = readU32(input);

  /* Reconstruir arbol Huffman; sera de solo lectura en los hijos. */
  root = buildHuffmanTree(freq);

  /* Preparar directorio raiz de salida. */
  outputDir = makeOutputDirectoryFromBin(argv[1]);
  ensureDirectoryTree(outputDir);

  /* Cargar todas las entradas en memoria; los hijos las heredan via CoW. */
  entries = readArchiveEntries(input, fileCount, outputDir);
  fclose(input);

  /* RCD1: crear directorios antes del fork(). */
  createAllOutputDirectories(entries, fileCount);

  /* RCD2: descomprimir en paralelo. */
  decompressEntriesParallel(entries, fileCount, root);

  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsedMs = (end.tv_sec  - start.tv_sec)  * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1.0e6;

  freeArchiveEntries(entries, fileCount);
  free(outputDir);
  freeTree(root);

  fprintf(stdout, "*** Archivo %s descomprimido correctamente ***\n", argv[1]);
  printf("| Archivos: %u |\n", fileCount);
  printf("| Procesos: %u |\n", fileCount);
  printf("| Tiempo transcurrido: %.6f ms |\n", elapsedMs);

  return EXIT_SUCCESS;
}