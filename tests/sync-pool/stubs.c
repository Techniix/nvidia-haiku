#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
struct util_dynarray;
unsigned util_dynarray_is_data_stack_allocated;
void ralloc_free(void *p) { free(p); }
void *reralloc_size(const void *ctx, void *p, size_t s) { (void)ctx; return realloc(p, s); }
