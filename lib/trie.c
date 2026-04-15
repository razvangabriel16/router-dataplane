#include "trie.h"

trienode* create_node(void) {
    trienode* node = malloc(sizeof(*node));
    node->one = node->zero = NULL;
    node->terminal = false;
    node->route = NULL;
    return node;
}

bool trie_insert(trienode** root, uint8_t* address, size_t size, struct route_table_entry* route) {
    if(!(*root))
        *root = create_node();
    for(size_t i = 0; i < size; ++i){
        uint8_t curr = address[i];
        if(!curr) {
            if(!(*root)->zero)
                (*root)->zero = create_node();
            root = &((*root)->zero);
        }
        else {
            if(!(*root)->one)
                (*root)->one = create_node();
            root = &((*root)->one);
        }
    }
    if((*root)->terminal)
        return false;
    (*root)->route = route;
    return (*root)->terminal = true;
}

void print_trie_rec(trienode* root, uint8_t* prefix, size_t length){
    uint8_t newprefix[length + 1];
    if(length > 0)
        memcpy(newprefix, prefix, length);
    if(root->terminal){
        for(int i = 0; i < length; ++i)
            printf("%hhd", prefix[i]);
        printf("\n");
    }
    if(root->zero){
        newprefix[length] = 0;
        print_trie_rec(root->zero, newprefix, length + 1);
    } 
    if(root->one){
        newprefix[length] = 1;
        print_trie_rec(root->one, newprefix, length + 1);
    }
}

void print_trie(trienode* root){
    if(!root){
        printf("Empty address\n");
    }
    print_trie_rec(root, NULL, 0);
}

struct route_table_entry* search_trie(trienode* root, uint8_t* address, size_t size) {
    struct route_table_entry* best = NULL;
    for(size_t i = 0; i < size; ++i) {
        if(!root) break;
        if(address[i] == 0)
            root = root->zero;
        else
            root = root->one;
        if(root && root->route)
            best = root->route;
    }
    return best;
}

void free_trie_rec(trienode* root) {
    if (!root)
        return;

    free_trie_rec(root->zero);
    free_trie_rec(root->one);

    free(root);
}

void free_trie(trienode* root){
    free_trie_rec(root);
    root = NULL;
}