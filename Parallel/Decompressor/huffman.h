#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>

#define ALPHABET_SIZE 256

typedef struct HeapNode HeapNode;

HeapNode *buildHuffmanTree(const uint32_t freq[ALPHABET_SIZE]);
void buildCodes(HeapNode *node, char *path, int depth, char *codes[ALPHABET_SIZE]);
void freeTree(HeapNode *node);

int huffmanIsLeaf(const HeapNode *node);
const HeapNode *huffmanGetLeft(const HeapNode *node);
const HeapNode *huffmanGetRight(const HeapNode *node);
unsigned char huffmanGetCharacter(const HeapNode *node);

#endif
