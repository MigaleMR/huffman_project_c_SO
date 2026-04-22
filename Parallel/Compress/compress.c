/*
 * compress.c - Compresor Huffman paralelo (basado en procesos)
 *
 * Estrategia de paralelismo:
 *
 *   Fase 1 - countFrequenciesParallel()
 *     Un proceso hijo por archivo. Cada hijo cuenta su tabla de
 *     frecuencias local de forma independiente y la escribe en una
 *     fila exclusiva de una matriz en memoria compartida
 *     que luego el padre reduce para obtener la tabla global
 *     (mmap MAP_SHARED|MAP_ANONYMOUS).
 *     Los tamaños originales se escriben directamente en el arreglo
 *     compartido sin sincronizacion, ya que cada hijo escribe en su propio
 *     indice exclusivo.
 *
 *     No hay region critica en esta fase.
 *
 *   Fase 2 - compressFilesParallel()
 *     Un proceso hijo por archivo. Cada hijo comprime su archivo a un
 *     archivo temporal privado (/tmp/huff_<ppid>_<i>.tmp) usando los
 *     codigos Huffman construidos por el padre.
 *     El tamaño comprimido se escribe en un arreglo compartido sin
 *     mutex, ya que cada hijo usa su indice exclusivo.
 *     El padre ensambla los archivos temporales en el archivo final.
 *
 *   Comparacion con version concurrente (hilos):
 *     pthread_create / pthread_join  ->  fork / waitpid
 *     buffer CompressedFile en heap  ->  archivos temporales
 *         (fork produce espacios de direcciones separados; el hijo no
 *          puede devolver un puntero malloc al padre)
 *     fusion protegida con mutex     ->  reduccion final hecha por el padre
 */

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

/* Escritor de bits directo a FILE* (igual que version serial) */
typedef struct {
  FILE          *file;
  unsigned char  buffer;
  unsigned char  bitCount;
  uint64_t       bytesWritten;
} BitWriter;

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
 * Redimensiona un bloque de memoria y verifica el exito de la operacion.
 * Entrada : puntero al bloque existente y nuevo tamaño en bytes.
 * Salida  : puntero al bloque redimensionado.
 */
static void *reallocateMemory(void *ptr, size_t size) {
  void *tmp = realloc(ptr, size);
  if (tmp == NULL) {
    fprintf(stderr, "\033[41m\n----|ERROR: no se pudo reasignar memoria.|----\033[0m\n\n");
    free(ptr);
    exit(EXIT_FAILURE);
  }
  return tmp;
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

/*
 * Compara dos entradas de archivo por su ruta relativa (para qsort).
 * Entrada : punteros a dos FileEntry.
 * Salida  : resultado negativo, cero o positivo segun strcmp.
 */
static int compareFileEntries(const void *a, const void *b) {
  const FileEntry *left  = (const FileEntry *)a;
  const FileEntry *right = (const FileEntry *)b;
  return strcmp(left->relativePath, right->relativePath);
}

/* ------------------------------------------------------------------ */
/*  Lista de archivos                                                   */
/* ------------------------------------------------------------------ */

/*
 * Inicializa la lista dinamica de archivos.
 * Entrada : puntero a la lista.
 * Salida  : campos de la lista en estado inicial.
 */
static void initList(FileList *list) {
  list->items    = NULL;
  list->count    = 0;
  list->capacity = 0;
}

/*
 * Agrega una nueva entrada a la lista, redimensionando si es necesario.
 * Entrada : lista, ruta relativa y ruta completa del archivo.
 * Salida  : entrada insertada al final de la lista.
 */
static void addToList(FileList *list, const char *relativePath, const char *fullPath) {
  FileEntry *entry;

  if (list->count == list->capacity) {
    size_t newCapacity = (list->capacity == 0) ? 16 : list->capacity * 2;
    list->items = (FileEntry *)reallocateMemory(list->items,
                                                newCapacity * sizeof(FileEntry));
    list->capacity = newCapacity;
  }

  entry                = &list->items[list->count++];
  entry->relativePath  = duplicateMemory(relativePath);
  entry->fullPath      = duplicateMemory(fullPath);
  entry->originalSize  = 0;
  entry->compressedSize = 0;
}

/*
 * Libera toda la memoria dinamica de la lista.
 * Entrada : puntero a la lista.
 * Salida  : memoria de rutas y del arreglo liberada.
 */
static void freeList(FileList *list) {
  size_t i;
  for (i = 0; i < list->count; i++) {
    free(list->items[i].relativePath);
    free(list->items[i].fullPath);
  }
  free(list->items);
}

/* ------------------------------------------------------------------ */
/*  Recorrido de directorio                                             */
/* ------------------------------------------------------------------ */

/*
 * Verifica si un nombre de archivo tiene extension .txt.
 * Entrada : nombre del archivo.
 * Salida  : 1 si termina en .txt, 0 en caso contrario.
 */
static int hasTxtExtension(const char *fileName) {
  const char *dot = strrchr(fileName, '.');
  return dot != NULL && strcmp(dot, ".txt") == 0;
}

/*
 * Recorre un directorio de forma recursiva y recopila archivos .txt.
 * Entrada : directorio base, subdirectorio relativo actual y la lista.
 * Salida  : lista rellena con los archivos encontrados.
 */
static void collectFilesRecursive(const char *baseDir,
                                  const char *relativeDir,
                                  FileList   *list) {
  char          *currentDir = (relativeDir[0] == '\0')
                              ? duplicateMemory(baseDir)
                              : joinPaths(baseDir, relativeDir);
  DIR           *dir        = opendir(currentDir);
  struct dirent *entry;

  if (dir == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo abrir el directorio '%s': %s|----\033[0m\n\n",
            currentDir, strerror(errno));
    free(currentDir);
    exit(EXIT_FAILURE);
  }

  while ((entry = readdir(dir)) != NULL) {
    char       *relativePath;
    char       *fullPath;
    struct stat st;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    relativePath = (relativeDir[0] == '\0')
                   ? duplicateMemory(entry->d_name)
                   : joinPaths(relativeDir, entry->d_name);
    fullPath = joinPaths(baseDir, relativePath);

    if (stat(fullPath, &st) != 0) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: no se pudo aplicar stat a '%s': %s|----\033[0m\n\n",
              fullPath, strerror(errno));
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
 * Inicia la recopilacion de archivos y ordena la lista resultante.
 * Entrada : ruta del directorio de entrada y la lista.
 * Salida  : lista ordenada con todos los archivos .txt encontrados.
 */
static void collectFiles(const char *directoryPath, FileList *list) {
  collectFilesRecursive(directoryPath, "", list);

  if (list->count == 0) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se encontraron archivos en '%s'.|----\033[0m\n\n",
            directoryPath);
    exit(EXIT_FAILURE);
  }

  qsort(list->items, list->count, sizeof(FileEntry), compareFileEntries);
}

/* ------------------------------------------------------------------ */
/*  Escritor de bits (sobre FILE*)                                      */
/* ------------------------------------------------------------------ */

/*
 * Inicializa el escritor de bits apuntando al archivo destino.
 * Entrada : puntero al escritor y al archivo de salida.
 * Salida  : estado inicial del escritor.
 */
static void bitWriterInit(BitWriter *w, FILE *f) {
  w->file         = f;
  w->buffer       = 0;
  w->bitCount     = 0;
  w->bytesWritten = 0;
}

/*
 * Escribe los bits de un codigo Huffman en el flujo de salida.
 * Entrada : puntero al escritor y cadena del codigo ('0'/'1').
 * Salida  : bits acumulados en el buffer; se vacia al completar un byte.
 */
static void bitWriterWriteCode(BitWriter *w, const char *code) {
  size_t i;
  for (i = 0; code[i] != '\0'; i++) {
    w->buffer = (unsigned char)(w->buffer << 1);
    if (code[i] == '1') w->buffer |= 1u;
    w->bitCount++;
    if (w->bitCount == 8) {
      fputc(w->buffer, w->file);
      w->buffer   = 0;
      w->bitCount = 0;
      w->bytesWritten++;
    }
  }
}

/*
 * Vacia los bits pendientes al archivo completando el ultimo byte.
 * Entrada : puntero al escritor.
 * Salida  : byte final escrito si habia bits sin vaciar.
 */
static void bitWriterFlush(BitWriter *w) {
  if (w->bitCount > 0) {
    w->buffer = (unsigned char)(w->buffer << (8 - w->bitCount));
    fputc(w->buffer, w->file);
    w->buffer   = 0;
    w->bitCount = 0;
    w->bytesWritten++;
  }
}

/* ------------------------------------------------------------------ */
/*  Escritura binaria de enteros                                        */
/* ------------------------------------------------------------------ */

/*
 * Escribe un entero de 32 bits en formato little-endian.
 * Entrada : archivo de salida y valor a escribir.
 * Salida  : 1 si se escribieron los 4 bytes, 0 si hubo error.
 */
static int writeU32(FILE *output, uint32_t value) {
  unsigned char bytes[4];
  bytes[0] = (unsigned char)(value         & 0xFFu);
  bytes[1] = (unsigned char)((value >>  8) & 0xFFu);
  bytes[2] = (unsigned char)((value >> 16) & 0xFFu);
  bytes[3] = (unsigned char)((value >> 24) & 0xFFu);
  return fwrite(bytes, 1, 4, output) == 4;
}

/*
 * Escribe un entero de 64 bits en formato little-endian.
 * Entrada : archivo de salida y valor a escribir.
 * Salida  : 1 si se escribieron los 8 bytes, 0 si hubo error.
 */
static int writeU64(FILE *output, uint64_t value) {
  unsigned char bytes[8];
  int i;
  for (i = 0; i < 8; i++)
    bytes[i] = (unsigned char)((value >> (8 * i)) & 0xFFu);
  return fwrite(bytes, 1, 8, output) == 8;
}

/* ------------------------------------------------------------------ */
/*  FASE 1 PARALELA - Conteo de frecuencias                            */
/*                                                                      */
/*  Cada hijo escribe en una fila exclusiva de sharedLocalFreq.        */
/*  El padre reduce esas filas al final para obtener la frecuencia     */
/*  global. sharedOriginalSizes[i] tambien se escribe por indice       */
/*  exclusivo, sin condicion de carrera.                               */
/* ------------------------------------------------------------------ */

static void countFrequenciesParallel(FileList  *list,
                                     uint32_t   freq[ALPHABET_SIZE],
                                     uint64_t  *sharedOriginalSizes) {
  uint32_t *sharedLocalFreq;
  size_t    i;
  pid_t     pid;
  size_t    sharedSize = list->count * ALPHABET_SIZE * sizeof(uint32_t);

  sharedLocalFreq = (uint32_t *)mmap(NULL,
                                     sharedSize,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (sharedLocalFreq == MAP_FAILED) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: mmap fallo para frecuencias locales compartidas.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }

  memset(sharedLocalFreq, 0, sharedSize);
  for (i = 0; i < ALPHABET_SIZE; i++) freq[i] = 0;

  /* Crear un proceso hijo por archivo */
  for (i = 0; i < list->count; i++) {
    pid = fork();

    if (pid < 0) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: fork fallo en la fase de frecuencias.|----\033[0m\n\n");
      munmap(sharedLocalFreq, sharedSize);
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO ---------------------------------------------------- */
      unsigned char buffer[4096];
      uint64_t      originalSize = 0;
      FILE         *input;
      size_t        bytesRead;
      size_t        j;
      uint32_t     *localFreq = sharedLocalFreq + (i * ALPHABET_SIZE);

      input = fopen(list->items[i].fullPath, "rb");
      if (input == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n",
                list->items[i].fullPath);
        munmap(sharedLocalFreq, sharedSize);
        exit(EXIT_FAILURE);
      }

      /* Conteo local de frecuencias, sin acceder a estado compartido */
      while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        for (j = 0; j < bytesRead; j++) localFreq[buffer[j]]++;
        originalSize += (uint64_t)bytesRead;
      }
      fclose(input);

      /* Escritura del tamaño original; indice exclusivo, sin mutex */
      sharedOriginalSizes[i] = originalSize;

      munmap(sharedLocalFreq, sharedSize);
      exit(EXIT_SUCCESS);
    }
    /* El padre continua creando hijos */
  }

  /* Padre espera a que todos los hijos terminen */
  for (i = 0; i < list->count; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: proceso hijo fallo en la fase de frecuencias.|----\033[0m\n\n");
      munmap(sharedLocalFreq, sharedSize);
      exit(EXIT_FAILURE);
    }
  }

  /* Reducir las frecuencias locales al arreglo global */
  for (i = 0; i < list->count; i++) {
    const uint32_t *localFreq = sharedLocalFreq + (i * ALPHABET_SIZE);
    size_t j;
    for (j = 0; j < ALPHABET_SIZE; j++) freq[j] += localFreq[j];
  }

  munmap(sharedLocalFreq, sharedSize);
}

/* ------------------------------------------------------------------ */
/*  FASE 2 PARALELA - Compresion de archivos                           */
/*                                                                      */
/*  Cada hijo comprime un archivo a un archivo temporal privado.       */
/*  No hay estado compartido durante la compresion, por lo que        */
/*  no se necesita mutex en esta fase.                                 */
/*                                                                      */
/*  sharedCompressedSizes[i]: cada hijo escribe en su indice           */
/*  exclusivo, sin condicion de carrera.                               */
/*                                                                      */
/*  Nombre del temporal: /tmp/huff_<ppid>_<indice>.tmp                */
/*  El PID del padre se incluye para evitar colisiones si varias       */
/*  instancias del compresor corren al mismo tiempo.                   */
/* ------------------------------------------------------------------ */

static void compressFilesParallel(FileList  *list,
                                  char      *codes[ALPHABET_SIZE],
                                  uint64_t  *sharedCompressedSizes,
                                  pid_t      mainPid) {
  size_t i;
  pid_t  pid;

  /* Crear un proceso hijo por archivo */
  for (i = 0; i < list->count; i++) {
    pid = fork();

    if (pid < 0) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: fork fallo en la fase de compresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }

    if (pid == 0) {
      /* --- HIJO ---------------------------------------------------- */
      char          tmpPath[256];
      FILE         *input;
      FILE         *output;
      unsigned char buffer[4096];
      BitWriter     writer;
      size_t        bytesRead;

      snprintf(tmpPath, sizeof(tmpPath), "/tmp/huff_%d_%zu.tmp",
               (int)mainPid, i);

      input = fopen(list->items[i].fullPath, "rb");
      if (input == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: no se pudo abrir '%s'.|----\033[0m\n\n",
                list->items[i].fullPath);
        exit(EXIT_FAILURE);
      }

      output = fopen(tmpPath, "wb");
      if (output == NULL) {
        fprintf(stderr,
                "\033[41m\n----|ERROR: no se pudo crear el temporal '%s'.|----\033[0m\n\n",
                tmpPath);
        fclose(input);
        exit(EXIT_FAILURE);
      }

      /* Comprimir al temporal; no hay estado compartido, sin mutex */
      bitWriterInit(&writer, output);
      while ((bytesRead = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        size_t j;
        for (j = 0; j < bytesRead; j++)
          bitWriterWriteCode(&writer, codes[buffer[j]]);
      }
      bitWriterFlush(&writer);

      fclose(input);
      fclose(output);

      /* Escritura del tamaño comprimido; indice exclusivo, sin mutex */
      sharedCompressedSizes[i] = writer.bytesWritten;

      exit(EXIT_SUCCESS);
    }
    /* El padre continua creando hijos */
  }

  /* Padre espera a que todos los hijos terminen */
  for (i = 0; i < list->count; i++) {
    int status;
    waitpid(-1, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
      fprintf(stderr,
              "\033[41m\n----|ERROR: proceso hijo fallo en la fase de compresion.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Escritura del archivo comprimido final (secuencial, solo el padre) */
/* ------------------------------------------------------------------ */

/*
 * Escribe la cabecera, tabla de frecuencias, metadatos y datos
 * comprimidos de cada archivo en el archivo de salida final.
 * Los datos comprimidos se leen de los archivos temporales generados
 * por los hijos y se eliminan al terminar.
 * Entrada : ruta de salida, lista de archivos, tabla de frecuencias y PID.
 * Salida  : archivo .bin generado con todos los datos.
 */
static void writeArchive(const char     *outputPath,
                         const FileList *list,
                         const uint32_t  freq[ALPHABET_SIZE],
                         pid_t           mainPid) {
  FILE  *output;
  size_t i;

  output = fopen(outputPath, "wb");
  if (output == NULL) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo crear '%s'.|----\033[0m\n\n",
            outputPath);
    exit(EXIT_FAILURE);
  }

  /* Cabecera: numero de archivos */
  if (!writeU32(output, (uint32_t)list->count)) {
    fclose(output);
    fprintf(stderr,
            "\033[41m\n----|ERROR: no se pudo escribir la cabecera del archivo.|----\033[0m\n\n");
    exit(EXIT_FAILURE);
  }

  /* Tabla de frecuencias global */
  for (i = 0; i < ALPHABET_SIZE; i++) {
    if (!writeU32(output, freq[i])) {
      fclose(output);
      fprintf(stderr,
              "\033[41m\n----|ERROR: no se pudo escribir la tabla de frecuencias.|----\033[0m\n\n");
      exit(EXIT_FAILURE);
    }
  }

  /* Metadatos y datos comprimidos de cada archivo */
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

    /* Leer el temporal del hijo y copiarlo al archivo final */
    if (file->compressedSize > 0) {
      char          tmpPath[256];
      FILE         *tmpFile;
      unsigned char copyBuf[4096];
      size_t        bytesRead;

      snprintf(tmpPath, sizeof(tmpPath), "/tmp/huff_%d_%zu.tmp",
               (int)mainPid, i);
      tmpFile = fopen(tmpPath, "rb");
      if (tmpFile == NULL) {
        fclose(output);
        fprintf(stderr,
                "\033[41m\n----|ERROR: no se pudo abrir el temporal '%s'.|----\033[0m\n\n",
                tmpPath);
        exit(EXIT_FAILURE);
      }

      while ((bytesRead = fread(copyBuf, 1, sizeof(copyBuf), tmpFile)) > 0) {
        if (fwrite(copyBuf, 1, bytesRead, output) != bytesRead) {
          fclose(tmpFile);
          fclose(output);
          fprintf(stderr,
                  "\033[41m\n----|ERROR: no se pudo copiar datos comprimidos de '%s'.|----\033[0m\n\n",
                  file->relativePath);
          exit(EXIT_FAILURE);
        }
      }

      fclose(tmpFile);
      remove(tmpPath); /* eliminar el temporal */
    }
  }

  fclose(output);
}

/* ------------------------------------------------------------------ */
/*  Liberacion de memoria                                               */
/* ------------------------------------------------------------------ */

/*
 * Libera el arreglo de codigos Huffman.
 * Entrada : arreglo de punteros a los codigos.
 * Salida  : cada cadena de codigo liberada.
 */
static void freeCodes(char *codes[ALPHABET_SIZE]) {
  size_t i;
  for (i = 0; i < ALPHABET_SIZE; i++) free(codes[i]);
}

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
  FileList  list;
  HeapNode *root;
  char     *codes[ALPHABET_SIZE];
  char      path[ALPHABET_SIZE * 2];
  uint32_t  freq[ALPHABET_SIZE];
  size_t    i;

  /* Arreglos en memoria compartida; se asignan tras conocer list.count */
  uint64_t *sharedOriginalSizes   = NULL;
  uint64_t *sharedCompressedSizes = NULL;

  uint64_t totalOriginal   = 0;
  uint64_t totalCompressed = 0;

  pid_t mainPid = getpid();

  struct timespec start, end;
  double          elapsedMs;

  for (i = 0; i < ALPHABET_SIZE; i++) codes[i] = NULL;

  if (argc != 3) {
    fprintf(stderr,
            "\033[41m\n----|Uso: %s <directorio> <salida.bin>|----\033[0m\n\n",
            argv[0]);
    return EXIT_FAILURE;
  }

  clock_gettime(CLOCK_MONOTONIC, &start);

  /* Recopilar lista de archivos */
  initList(&list);
  collectFiles(argv[1], &list);

  /*
   * Dos arreglos mmap anonimos compartidos (uno por archivo).
   * Sin mutex: cada hijo escribe en su indice exclusivo.
   */
  sharedOriginalSizes = (uint64_t *)mmap(NULL,
                                          list.count * sizeof(uint64_t),
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (sharedOriginalSizes == MAP_FAILED) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: mmap fallo para tamaños originales.|----\033[0m\n\n");
    return EXIT_FAILURE;
  }

  sharedCompressedSizes = (uint64_t *)mmap(NULL,
                                            list.count * sizeof(uint64_t),
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (sharedCompressedSizes == MAP_FAILED) {
    fprintf(stderr,
            "\033[41m\n----|ERROR: mmap fallo para tamaños comprimidos.|----\033[0m\n\n");
    munmap(sharedOriginalSizes, list.count * sizeof(uint64_t));
    return EXIT_FAILURE;
  }

  /* Fase 1: contar frecuencias en paralelo */
  countFrequenciesParallel(&list, freq, sharedOriginalSizes);

  /* Copiar tamaños originales a las entradas de la lista */
  for (i = 0; i < list.count; i++) {
    list.items[i].originalSize = sharedOriginalSizes[i];
    totalOriginal += list.items[i].originalSize;
  }

  /* Construir arbol y codigos Huffman (secuencial, solo el padre) */
  root = buildHuffmanTree(freq);
  buildCodes(root, path, 0, codes);

  /* Fase 2: comprimir archivos en paralelo */
  compressFilesParallel(&list, codes, sharedCompressedSizes, mainPid);

  /* Copiar tamaños comprimidos a las entradas de la lista */
  for (i = 0; i < list.count; i++) {
    list.items[i].compressedSize = sharedCompressedSizes[i];
    totalCompressed += list.items[i].compressedSize;
  }

  munmap(sharedOriginalSizes,   list.count * sizeof(uint64_t));
  munmap(sharedCompressedSizes, list.count * sizeof(uint64_t));

  /* Escribir archivo final (secuencial) */
  writeArchive(argv[2], &list, freq, mainPid);

  clock_gettime(CLOCK_MONOTONIC, &end);
  elapsedMs = (double)(end.tv_sec  - start.tv_sec)  * 1000.0
            + (double)(end.tv_nsec - start.tv_nsec) / 1.0e6;

  fprintf(stdout, "*** Directorio %s comprimido correctamente ***\n", argv[1]);
  printf("| Archivos: %zu |\n",                                    list.count);
  printf("| Tamaño original: %llu bytes |\n",   (unsigned long long)totalOriginal);
  printf("| Tamaño comprimido: %llu bytes |\n", (unsigned long long)totalCompressed);
  printf("| Procesos: %zu |\n",                                    list.count);
  printf("| Tiempo transcurrido: %.6f ms |\n",                     elapsedMs);

  freeCodes(codes);
  freeTree(root);
  freeList(&list);
  return EXIT_SUCCESS;
}
