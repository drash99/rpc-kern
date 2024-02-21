#include "bitmap.h"
#include <linux/slab.h>

#define BIT_ARR_SIZE 4

char bit_array_find_first_clear_bit(char * bitarr, bit_index_t* result) {
    int i;
    for (i=0; i<BIT_ARR_SIZE; i++) {
        if (!bitarr[i]) {
            *result = i;
            return 1;
        }
    }
    return 0;
}

void bit_array_set_bit(char * bitarr, bit_index_t b) {
    if (0 <= b && b < BIT_ARR_SIZE) {
        bitarr[b] = 1;
    }
}
void bit_array_clear_bit(char * bitarr, bit_index_t b) {
    if (0 <= b && b < BIT_ARR_SIZE) {
        bitarr[b] = 0;
    }
}

void bit_array_free(char * bitarray) {
    kfree(bitarray);
}