/*
 * decompress.c - Descompresor Huffman paralelo (basado en procesos)
 *
 * Estrategia de paralelismo:
 *
 *   Fase de lectura (secuencial, un solo proceso padre)
 *     El archivo comprimido se lee de forma secuencial por el padre.
 *     Todos los bloques comprimidos se cargan en memoria antes de
 *     paralelizar. Esta fase no se puede paralelizar porque cada
 *     bloque comienza justo despues del anterior en el flujo binario.
 *
 *   Fase de descompresion (paralela)
 *     Tras cargar todos los metadatos y datos comprimidos, el padre
 *     crea un proceso hijo por entrada del archivo. Gracias a fork(),
 *     cada hijo recibe una instantanea copy-on-write de la memoria
 *     del padre y tiene acceso inmediato a:
 *       - El arbol Huffman (solo lectura, sin mutex)
 *       - Su propia entrada ArchiveEntry (datos, tamaños y rutas)
 *     Cada hijo descomprime su entrada de forma independiente y
 *     escribe un archivo de salida.
 *
 *     Directorios de salida
 *       Dos hijos pueden intentar crear el mismo subdirectorio al
 *       mismo tiempo. Esto es seguro porque createDirectoryIfNeeded()
 *       tolera EEXIST, asi que no se necesita sincronizacion extra.
 *
 *   Comparacion con version concurrente (hilos):
 *     pthread_create / pthread_join  ->  fork / waitpid
 *     acceso directo a memoria       ->  instantanea copy-on-write del fork
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "huffman.h"

/* ------------------------------------------------------------------ */
/*  Tipos de datos                                                      */
/* ------------------------------------------------------------------ */

/* Lector de bits sobre un buffer en memoria */
typedef struct {
  const unsigned char *data;
  uint64_t             dataSize;
  uint64_t             byteIndex;
  unsigned char        buffer;
  unsigned char        bitsLeft;
} MemoryBitReader;

/* Una entrada cargada desde el archivo comprimido */
typedef struct {
  char          *relativePath;
  char          *outputPath;
  unsigned char *compressedData;
  uint64_t       originalSize;
  uint64_t       compressedSize;
} ArchiveEntry;

/* ------------------------------------------------------------------ */
/*  Utilidades de memoria                                               */
/* ------------------------------------------------------------------ */

/*
 * Reserva memoria dinamica y verifica que la asignacion sea exitosa.
 * Entrada : numero de bytes a reservar.
 * Salida  : puntero al bloque reservado.
 */
static void *allocateMemory(size_t size) {
  void *ptr = malloc(size);
  if (ptr == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo asignar memoria.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

/*
 * Crea una copia dinamica de una cadena de texto.
 * Entrada : puntero a la cadena original.
 * Salida  : puntero a la copia.
 */
static char *duplicateMemory(const char *text) {
  size_t  len  = strlen(text) + 1;
  char   *copy = (char *)allocateMemory(len);
  memcpy(copy, text, len);
  return copy;
}

/*
 * Une dos fragmentos de ruta en una sola ruta valida.
 * Entrada : primera parte (a) y segunda parte (b) de la ruta.
 * Salida  : puntero a la ruta completa resultante.
 */
static char *joinPaths(const char *a, const char *b) {
  size_t lenA      = strlen(a);
  size_t lenB      = strlen(b);
  int    needSlash = (lenA > 0 && a[lenA - 1] != '/');
  char  *result    = (char *)allocateMemory(lenA + lenB + (needSlash ? 2 : 1));

  memcpy(result, a, lenA);
  if (needSlash) {
    result[lenA] = '/';
    memcpy(result + lenA + 1, b, lenB + 1);
  } else {
    memcpy(result + lenA, b, lenB + 1);
  }
  return result;
}

/* ------------------------------------------------------------------ */
/*  Utilidades de directorio                                            */
/* ------------------------------------------------------------------ */

/*
 * Genera el nombre del directorio de salida a partir del archivo .bin.
 * Entrada : ruta del archivo comprimido.
 * Salida  : puntero a la ruta del directorio de salida generado.
 */
static char *makeOutputDirectoryFromBin(const char *archivePath) {
  size_t  len    = strlen(archivePath);
  char   *result = duplicateMemory(archivePath);

  if (len >= 4 && strcmp(archivePath + len - 4, ".bin") == 0) {
    result[len - 4] = '\0';
  } else {
    char *tmp = realloc(result, len + 5);
    if (tmp == NULL) {
      free(result);
      fprintf(stderr, "\033[41m\n----|ERROR: no se pudo reasignar memoria.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
    result = tmp;
    memcpy(result + len, ".out", 5);
  }
  return result;
}

/*
 * Crea un directorio si no existe; ignora el error EEXIST.
 * Entrada : ruta del directorio a crear.
 * Salida  : directorio creado en el sistema de archivos si no existia.
 */
static void createDirectoryIfNeeded(const char *path) {
  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo crear el directorio '%s': %s|----\033[0m\n\n",
            path, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

/*
 * Crea todos los directorios de una ruta que no existan.
 * Entrada : ruta del directorio raiz de salida.
 * Salida  : arbol de directorios creado completamente.
 */
static void ensureDirectoryTree(const char *directoryPath) {
  char *copy = duplicateMemory(directoryPath);
  char *p;

  if (copy[0] == '\0') { free(copy); return; }

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
 * Crea los directorios padre de un archivo si no existen.
 * REGION CRITICA: el llamador debe sostener el mutex antes de invocar
 * esta funcion para evitar condiciones de carrera entre hijos.
 * Entrada : ruta completa del archivo de salida.
 * Salida  : directorios padre creados.
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

/* ------------------------------------------------------------------ */
/*  Validacion de ruta                                                  */
/* ------------------------------------------------------------------ */

/*
 * Verifica si una ruta relativa es insegura para extraccion.
 * Entrada : ruta relativa almacenada en el archivo comprimido.
 * Salida  : 1 si la ruta es insegura, 0 en caso contrario.
 */
static int isUnsafeRelativePath(const char *path) {
  size_t i   = 0;
  size_t len = strlen(path);

  if (len == 0 || path[0] == '/') return 1;

  while (i < len) {
    size_t start      = i;
    size_t segmentLen;
    while (i < len && path[i] != '/') i++;
    segmentLen = i - start;
    if (segmentLen == 0) return 1;
    if (segmentLen == 1 && path[start] == '.')  return 1;
    if (segmentLen == 2 && path[start] == '.' && path[start + 1] == '.') return 1;
    if (i < len && path[i] == '/') i++;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Lectura binaria de enteros                                          */
/* ------------------------------------------------------------------ */

/*
 * Lee exactamente 'size' bytes del archivo de entrada.
 * Entrada : archivo de entrada, buffer destino, cantidad de bytes y descripcion.
 * Salida  : bytes almacenados en el buffer; termina el proceso si falla.
 */
static void readExact(FILE *input, void *buffer, size_t size, const char *what) {
  if (size > 0 && fread(buffer, 1, size, input) != size) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo leer %s del archivo.|----\033[0m\n\n",
            what);
    exit(EXIT_FAILURE);
  }
}

/*
 * Lee un entero de 32 bits en formato little-endian.
 * Entrada : archivo de entrada.
 * Salida  : valor entero leido.
 */
static uint32_t readU32(FILE *input) {
  unsigned char bytes[4];
  readExact(input, bytes, 4, "entero de 32 bits");
  return ((uint32_t)bytes[0])        |
         ((uint32_t)bytes[1] <<  8)  |
         ((uint32_t)bytes[2] << 16)  |
         ((uint32_t)bytes[3] << 24);
}

/*
 * Lee un entero de 64 bits en formato little-endian.
 * Entrada : archivo de entrada.
 * Salida  : valor entero leido.
 */
static uint64_t readU64(FILE *input) {
  unsigned char bytes[8];
  uint64_t      value = 0;
  int           i;
  readExact(input, bytes, 8, "entero de 64 bits");
  for (i = 0; i < 8; i++) value |= ((uint64_t)bytes[i] << (8 * i));
  return value;
}

/* ------------------------------------------------------------------ */
/*  Lector de bits en memoria                                           */
/* ------------------------------------------------------------------ */

/*
 * Inicializa el lector de bits sobre un buffer en memoria.
 * Entrada : puntero al lector, datos y tamaño del buffer.
 * Salida  : estado inicial del lector.
 */
static void memoryBitReaderInit(MemoryBitReader    *reader,
                                const unsigned char *data,
                                uint64_t             dataSize) {
  reader->data      = data;
  reader->dataSize  = dataSize;
  reader->byteIndex = 0;
  reader->buffer    = 0;
  reader->bitsLeft  = 0;
}

/*
 * Lee el siguiente bit del buffer en memoria.
 * Entrada : puntero al lector.
 * Salida  : bit leido (0 o 1), o -1 si no hay mas datos.
 */
static int memoryBitReaderReadBit(MemoryBitReader *reader) {
  if (reader->bitsLeft == 0) {
    if (reader->byteIndex >= reader->dataSize) return -1;
    reader->buffer   = reader->data[reader->byteIndex++];
    reader->bitsLeft = 8;
  }
  reader->bitsLeft--;
  return (reader->buffer >> reader->bitsLeft) & 1;
}

/* ------------------------------------------------------------------ */
/*  Descompresion de un bloque (sin estado compartido, sin mutex)      */
/* ------------------------------------------------------------------ */

/*
 * Descomprime un bloque comprimido usando el arbol Huffman.
 * Opera solo sobre datos propios del hijo; no requiere mutex.
 * Entrada : archivo de salida, arbol Huffman, tamaños, datos y ruta.
 * Salida  : bytes originales escritos en el archivo de salida.
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
    const HeapNode *current = root;

    while (!huffmanIsLeaf(current)) {
      int bit = memoryBitReaderReadBit(&reader);
      if (bit < 0) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: fin inesperado del bloque comprimido '%s'.|----\033[0m\n\n",
                relativePath);
        exit(EXIT_FAILURE);
      }
      current = (bit == 0) ? huffmanGetLeft(current) : huffmanGetRight(current);
      if (current == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: ruta invalida en el arbol Huffman para '%s'.|----\033[0m\n\n",
                relativePath);
        exit(EXIT_FAILURE);
      }
    }

    fputc(huffmanGetCharacter(current), output);
    written++;
  }
}

/* ------------------------------------------------------------------ */
/*  Lectura secuencial del archivo comprimido                          */
/* ------------------------------------------------------------------ */

/*
 * Lee todas las entradas del archivo comprimido y las carga en memoria.
 * Debe ejecutarse en un solo proceso (la posicion del cursor es secuencial).
 * Entrada : archivo de entrada, cantidad de archivos y directorio de salida.
 * Salida  : arreglo de entradas con datos y rutas listas para descomprimir.
 */
static ArchiveEntry *readArchiveEntries(FILE       *input,
                                        uint32_t    fileCount,
                                        const char *outputDir) {
  ArchiveEntry *entries = (ArchiveEntry *)allocateMemory(
                            (size_t)fileCount * sizeof(ArchiveEntry));
  uint32_t i;

  for (i = 0; i < fileCount; i++) {
    uint32_t pathLen = readU32(input);

    entries[i].relativePath   = (char *)allocateMemory((size_t)pathLen + 1);
    entries[i].compressedData = NULL;

    readExact(input, entries[i].relativePath, pathLen, "ruta del archivo");
    entries[i].relativePath[pathLen] = '\0';

    if (isUnsafeRelativePath(entries[i].relativePath)) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: ruta relativa invalida: '%s'|----\033[0m\n\n",
              entries[i].relativePath);
      exit(EXIT_FAILURE);
    }

    entries[i].originalSize   = readU64(input);
    entries[i].compressedSize = readU64(input);

    if (entries[i].compressedSize > 0) {
      if (entries[i].compressedSize > (uint64_t)SIZE_MAX) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: bloque comprimido demasiado grande.|----\033[0m\n\n");
        exit(EXIT_FAILURE);
      }
      entries[i].compressedData =
          (unsigned char *)allocateMemory((size_t)entries[i].compressedSize);
      readExact(input, entries[i].compressedData,
                (size_t)entries[i].compressedSize, "bloque comprimido");
    }

    entries[i].outputPath = joinPaths(outputDir, entries[i].relativePath);
  }

  return entries;
}

/* ------------------------------------------------------------------ */
/*  DESCOMPRESION PARALELA con fork()                                   */
/*                                                                      */
/*  Cada hijo recibe una instantanea copy-on-write de la memoria del   */
/*  padre (arbol Huffman y buffers de datos comprimidos).              */
/*  decompressBlock() solo lee datos propios del hijo.                 */
/* ------------------------------------------------------------------ */

static void decompressEntriesParallel(ArchiveEntry   *entries,
                                      uint32_t        fileCount,
                                      const HeapNode *root) {
  uint32_t i;
  pid_t    pid;

  /* Crear un proceso hijo por entrada del archivo */
  for (i = 0; i < fileCount; i++) {
    pid = fork();

    if (pid < 0) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: fork fallo en la fase de descompresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO ---------------------------------------------------- */
      FILE *output;
      ensureParentDirectories(entries[i].outputPath);

      output = fopen(entries[i].outputPath, "wb");
      if (output == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: no se pudo crear '%s'.|----\033[0m\n\n",
                entries[i].outputPath);
        exit(EXIT_FAILURE);
      }

      /* Descomprimir; solo datos propios del hijo, sin mutex necesario */
      decompressBlock(output,
                      root,
                      entries[i].originalSize,
                      entries[i].compressedSize,
                      entries[i].compressedData,
                      entries[i].relativePath);
      fclose(output);

      exit(EXIT_SUCCESS);
    }
    /* El padre continua creando hijos */
  }

  /* Padre espera a que todos los hijos terminen */
  for (i = 0; i < fileCount; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: proceso hijo fallo en la fase de descompresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Liberacion de memoria                                               */
/* ------------------------------------------------------------------ */

/*
 * Libera toda la memoria dinamica del arreglo de entradas del archivo.
 * Entrada : arreglo de entradas y cantidad de entradas.
 * Salida  : rutas, datos comprimidos y el arreglo liberados.
 */
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
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  FILE          *input;
  HeapNode      *root;
  char          *outputDir;
  ArchiveEntry  *entries;
  uint32_t       fileCount;
  uint32_t       freq[ALPHABET_SIZE];
  uint32_t       i;
  struct timespec start, end;
  double          elapsedMs;

  if (argc != 2) {
    fprintf(stderr,
            "\033[41m\n----|Uso: %s <entrada.bin>|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  input = fopen(argv[1], "rb");
  if (input == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n",
            argv[1]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  /* Leer cabecera (secuencial) */
  fileCount = readU32(input);
  for (i = 0; i < ALPHABET_SIZE; i++) freq[i] = readU32(input);

  /* Reconstruir arbol Huffman (secuencial) */
  root = buildHuffmanTree(freq);

  /* Preparar directorio de salida */
  outputDir = makeOutputDirectoryFromBin(argv[1]);
  ensureDirectoryTree(outputDir);

  /* Cargar todos los datos comprimidos en memoria (secuencial) */
  entries = readArchiveEntries(input, fileCount, outputDir);
  fclose(input);

  /* Descomprimir todas las entradas en paralelo */
  decompressEntriesParallel(entries, fileCount, root);

  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsedMs = (double)(end.tv_sec  - start.tv_sec)  * 1000.0
            + (double)(end.tv_nsec - start.tv_nsec) / 1.0e6;

  freeArchiveEntries(entries, fileCount);
  free(outputDir);
  freeTree(root);

  fprintf(stdout, "*** Archivo %s descomprimido correctamente ***\n", argv[1]);
  printf("| Archivos: %u |\n",                    fileCount);
  printf("| Procesos: %u |\n",                    fileCount);
  printf("| Tiempo transcurrido: %.6f ms |\n",     elapsedMs);

  return EXIT_SUCCESS;
}
