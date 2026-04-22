#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "huffman.h"

struct HeapNode {
  unsigned char character;
  uint32_t frequency;
  struct HeapNode *left;
  struct HeapNode *right;
};

typedef struct {
  size_t size;
  size_t capacity;
  HeapNode **nodes;
} MinHeap;

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
    fprintf(stderr, "ERROR: memory couldn't be allocated.\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

/*
Function that creates a dynamic copy of a string.
Input:
    - text: pointer to the original string.
Output:
    - returns a pointer to the new copied string.
*/

static char *duplicateMemory(const char *text) {
  size_t len = strlen(text);
  char *copy = (char *)allocateMemory(len + 1);
  memcpy(copy, text, len + 1);
  return copy;
}

/*
Function that creates a new Huffman tree node.
Input:
    - character: byte value that will be stored in the node.
    - frequency: frequency associated with the byte.
Output:
    - returns a pointer to the new created node.
*/

static HeapNode *newNode(unsigned char character, uint32_t frequency) {
  HeapNode *node = (HeapNode *)allocateMemory(sizeof(HeapNode));
  node->character = character;
  node->frequency = frequency;
  node->left = NULL;
  node->right = NULL;
  return node;
}

/*
Function that creates a new min heap used to build the Huffman tree.
Input:
    - capacity: maximum number of nodes that the heap can store.
Output:
    - returns a pointer to the new created heap.
*/

static MinHeap *newMinHeap(size_t capacity) {
  MinHeap *heap = (MinHeap *)allocateMemory(sizeof(MinHeap));
  heap->size = 0;
  heap->capacity = capacity;
  heap->nodes = (HeapNode **)allocateMemory(capacity * sizeof(HeapNode *));
  return heap;
}

/*
Function that swaps the position of two nodes inside the heap.
Input:
    - a: pointer to the first node pointer.
    - b: pointer to the second node pointer.
Output:
    - exchanges the two node pointers.
*/

static void swapNodes(HeapNode **a, HeapNode **b) {
  HeapNode *tmp = *a;
  *a = *b;
  *b = tmp;
}

/*
Function that calculates the parent index of a node in the heap.
Input:
    - pos: position of the current node.
Output:
    - returns the index of the parent node.
*/

static size_t parentIndex(size_t pos) {
  return (pos - 1) / 2;
}

/*
Function that calculates the left child index of a node in the heap.
Input:
    - pos: position of the current node.
Output:
    - returns the index of the left child node.
*/

static size_t leftChildIndex(size_t pos) {
  return 2 * pos + 1;
}

/*
Function that calculates the right child index of a node in the heap.
Input:
    - pos: position of the current node.
Output:
    - returns the index of the right child node.
*/

static size_t rightChildIndex(size_t pos) {
  return 2 * pos + 2;
}

/*
Function that moves a node up in the heap until the min heap property is restored.
Input:
    - heap: pointer to the min heap structure.
    - pos: position of the node that will move up.
Output:
    - reorganizes the heap after an insertion.
*/

static void siftUp(MinHeap *heap, size_t pos) {
  while (pos > 0 && heap->nodes[pos]->frequency < heap->nodes[parentIndex(pos)]->frequency) {
    swapNodes(&heap->nodes[pos], &heap->nodes[parentIndex(pos)]);
    pos = parentIndex(pos);
  }
}

/*
Function that moves a node down in the heap until the min heap property is restored.
Input:
    - heap: pointer to the min heap structure.
    - pos: position of the node that will move down.
Output:
    - reorganizes the heap after a removal.
*/

static void siftDown(MinHeap *heap, size_t pos) {
  while (1) {
    size_t left = leftChildIndex(pos);
    size_t right = rightChildIndex(pos);
    size_t smallest = pos;

    if (left < heap->size && heap->nodes[left]->frequency < heap->nodes[smallest]->frequency) {
      smallest = left;
    }

    if (right < heap->size && heap->nodes[right]->frequency < heap->nodes[smallest]->frequency) {
      smallest = right;
    }

    if (smallest == pos) {
      break;
    }

    swapNodes(&heap->nodes[pos], &heap->nodes[smallest]);
    pos = smallest;
  }
}

/*
Function that inserts a new node into the min heap.
Input:
    - heap: pointer to the min heap structure.
    - node: pointer to the node that will be inserted.
Output:
    - adds the node to the heap and restores the min heap property.
*/

static void insertHeap(MinHeap *heap, HeapNode *node) {
  if (heap->size >= heap->capacity) {
    fprintf(stderr, "ERROR: heap lleno.\n");
    exit(EXIT_FAILURE);
  }

  heap->nodes[heap->size] = node;
  siftUp(heap, heap->size);
  heap->size++;
}

/*
Function that removes and returns the node with the lowest frequency from the heap.
Input:
    - heap: pointer to the min heap structure.
Output:
    - returns a pointer to the node with minimum frequency.
*/

static HeapNode *removeMin(MinHeap *heap) {
  HeapNode *minNode;

  if (heap->size == 0) {
    fprintf(stderr, "ERROR: heap vacio.\n");
    exit(EXIT_FAILURE);
  }

  minNode = heap->nodes[0];
  heap->size--;

  if (heap->size > 0) {
    heap->nodes[0] = heap->nodes[heap->size];
    siftDown(heap, 0);
  }

  return minNode;
}

/*
Function that builds the Huffman tree from a frequency table.
Input:
    - freq: array with the frequency of every byte value from 0 to 255.
Output:
    - returns a pointer to the root of the Huffman tree.
*/

HeapNode *buildHuffmanTree(const uint32_t freq[ALPHABET_SIZE]) {
  size_t i;
  MinHeap *heap = newMinHeap(ALPHABET_SIZE);

  for (i = 0; i < ALPHABET_SIZE; i++) {
    insertHeap(heap, newNode((unsigned char)i, freq[i]));
  }

  while (heap->size > 1) {
    HeapNode *left = removeMin(heap);
    HeapNode *right = removeMin(heap);
    HeapNode *parent = newNode('#', left->frequency + right->frequency);
    parent->left = left;
    parent->right = right;
    insertHeap(heap, parent);
  }

  {
    HeapNode *root = removeMin(heap);
    free(heap->nodes);
    free(heap);
    return root;
  }
}

/*
Function that traverses the Huffman tree and generates the binary code for each leaf.
Input:
    - node: pointer to the current node of the Huffman tree.
    - path: array that stores the current binary path.
    - depth: current length of the Huffman code.
    - codes: array of pointers where the generated codes will be stored.
Output:
    - stores the Huffman code of every byte in the codes array.
*/

void buildCodes(HeapNode *node, char *path, int depth, char *codes[ALPHABET_SIZE]) {
  if (node->left == NULL && node->right == NULL) {
    if (depth == 0) {
      path[0] = '0';
      depth = 1;
    }
    path[depth] = '\0';
    codes[node->character] = duplicateMemory(path);
    return;
  }

  if (node->left != NULL) {
    path[depth] = '0';
    buildCodes(node->left, path, depth + 1, codes);
  }

  if (node->right != NULL) {
    path[depth] = '1';
    buildCodes(node->right, path, depth + 1, codes);
  }
}

/*
Function that releases all memory used by the Huffman tree.
Input:
    - node: pointer to the current node of the Huffman tree.
Output:
    - frees the complete Huffman tree recursively.
*/

void freeTree(HeapNode *node) {
  if (node == NULL) {
    return;
  }

  freeTree(node->left);
  freeTree(node->right);
  free(node);
}

/*
Function that verifies if a Huffman tree node is a leaf.
Input:
    - node: pointer to the Huffman tree node.
Output:
    - returns 1 if the node is a leaf or 0 otherwise.
*/

int huffmanIsLeaf(const HeapNode *node) {
  return node != NULL && node->left == NULL && node->right == NULL;
}

/*
Function that returns the left child of a Huffman tree node.
Input:
    - node: pointer to the Huffman tree node.
Output:
    - returns a pointer to the left child node.
*/

const HeapNode *huffmanGetLeft(const HeapNode *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->left;
}

/*
Function that returns the right child of a Huffman tree node.
Input:
    - node: pointer to the Huffman tree node.
Output:
    - returns a pointer to the right child node.
*/

const HeapNode *huffmanGetRight(const HeapNode *node) {
  if (node == NULL) {
    return NULL;
  }
  return node->right;
}

/*
Function that returns the byte stored in a Huffman tree node.
Input:
    - node: pointer to the Huffman tree node.
Output:
    - returns the character stored in the node.
*/

unsigned char huffmanGetCharacter(const HeapNode *node) {
  if (node == NULL) {
    return 0;
  }
  return node->character;
}
