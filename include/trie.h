#ifndef TRIE_H
#define TRIE_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct trienode{
    struct trienode* zero;
    struct trienode* one;
    bool terminal;
    struct route_table_entry* route;
}trienode;

/* create an empty trie node */
extern trienode* create_node(void);

extern bool trie_insert(trienode** root, uint8_t* address, size_t size, struct route_table_entry* route);

extern void print_trie(trienode* root);

extern struct route_table_entry* search_trie(trienode* root, uint8_t* address, size_t size);

extern void free_trie(trienode* root);
#endif
