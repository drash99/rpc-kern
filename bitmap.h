#ifndef __BITMAP_H__
#define __BITMAP_H__

#include <linux/types.h>

// 64 bit words
typedef uint64_t word_t, word_addr_t, bit_index_t;
// Find the index of the first bit that is NOT set.
// Returns 1 if a bit is clear, otherwise 0
// Index of first zero bit is stored in the integer pointed to by `result`
// If no bit is zero result is not changed
char bit_array_find_first_clear_bit( char * bitarr, bit_index_t* result);

void bit_array_set_bit( char * bitarr, bit_index_t b);
void bit_array_clear_bit( char * bitarr, bit_index_t b);
void bit_array_free( char * bitarray);

#endif