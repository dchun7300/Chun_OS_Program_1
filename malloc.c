#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

static int debug_checked = 0;
static int debug_enabled = 0;

static void debug_check() {
    if (!debug_checked) {
        debug_checked = 1;
        debug_enabled = (getenv("DEBUG_MALLOC") != NULL);
    }
}

static void debug_print(const char *fmt, ...) {
    debug_check();
    if (!debug_enabled)
        return;

    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0)
        write(2, buf, len);
}

#define DEBUG_PRINT(...) debug_print(__VA_ARGS__)

///////////////////////////////////////////////////////////////////////////////////////
typedef struct node {
    int occupied;
    size_t size;
    void *start_addr;
    struct node *prev_node;
    struct node *next_node;
} node;


void *heap_start_addr = NULL;
int first_time = 0;
size_t min_block_size = ((sizeof(node) + 15) & ~ 15) + 16;
size_t rounded_node_size = (sizeof(node) + 15) & ~15;
size_t chunk_size = 1024 * 64;


node *locator(void *ptr) {
    if (!ptr || !heap_start_addr || first_time == 0) return NULL;

    node *current = (node *)heap_start_addr;
    uintptr_t target = (uintptr_t)ptr;

    while (current) {
        uintptr_t beg = (uintptr_t)current->start_addr + (uintptr_t)rounded_node_size;
        uintptr_t end = beg + (uintptr_t)current->size;

        if (target >= beg && target < end)
            return current;

        current = current->next_node;
    }
    return NULL;
}


void free(void *pointer) {
    DEBUG_PRINT("MALLOC: free(%p)\n", pointer);
    if (pointer == NULL) return;

    node *current = locator(pointer);
    if (current == NULL) return;

    current->occupied = 0;

    int prev_occupied = (current->prev_node == NULL) ? 1 : current->prev_node->occupied;
    int next_occupied = (current->next_node == NULL) ? 1 : current->next_node->occupied;

    if (!prev_occupied && next_occupied) {
        current->prev_node->size += rounded_node_size + current->size;
        current->prev_node->next_node = current->next_node;
        if (current->next_node)
            current->next_node->prev_node = current->prev_node;
        current = current->prev_node;
    }
 
    if (prev_occupied && !next_occupied) {
        current->size += rounded_node_size + current->next_node->size;
        node *temp = current->next_node->next_node;
        current->next_node = temp;
        if (temp)
            temp->prev_node = current;
    }
 
    if (!prev_occupied && !next_occupied) {
        node *prev = current->prev_node;
        node *next = current->next_node;
        prev->size += current->size + next->size + (2 * rounded_node_size);
        prev->next_node = next->next_node;
        if (next->next_node)
            next->next_node->prev_node = prev;
        current = prev;
    }

    return;
}


void *malloc(size_t size){
        DEBUG_PRINT("MALLOC: malloc(%zu)", size);
        if (size == 0) { DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void*)NULL, (size_t)0); return NULL; }
	

        size_t rounded_request_size = (size + 15) & ~15;
        size_t total_size = rounded_node_size + rounded_request_size;
        size_t sbrk_size = (rounded_request_size >= (chunk_size - rounded_node_size)) ? total_size : chunk_size;

        if (first_time == 0){
		heap_start_addr = sbrk(0);
                node *new_node = sbrk(sbrk_size);
		first_time = 1;
                if (new_node == (void *) -1){
                        perror("sbrk()\n");
                        errno = ENOMEM;
                        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void*)NULL, (size_t)0);
                        return NULL;
                }
                new_node->size = total_size - rounded_node_size;
                new_node->start_addr = (void *) new_node;
                new_node->prev_node = NULL;
                new_node->next_node = NULL;
                new_node->occupied = 1;
                DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void *)((char *)new_node + rounded_node_size), size);
                return (void *)((char *)new_node + rounded_node_size);
        }

        if (first_time == 1){
                node *current = heap_start_addr;
                int found_existing_free_block = 0;
                int split_free_block = 0;
                int enough_space = 0;
                char *current_block_endpoint;
                char *block_splitpoint;
                size_t left_over_block_size;

                while(true){
                        current_block_endpoint = ((char *) current->start_addr) + rounded_node_size + current->size;

                        if ((current->size >= (rounded_request_size)) && current->occupied == 0){
                            found_existing_free_block = 1;
                            block_splitpoint = ((char *) (current->start_addr)) + total_size;
                            left_over_block_size = current_block_endpoint - block_splitpoint;

                            if (left_over_block_size < min_block_size) {
                                left_over_block_size = 0;
                                split_free_block = 0;
                            } else {
                                split_free_block = 1;
                            }
                            break;
                        }

                        enough_space = (current_block_endpoint + total_size <= (char *) sbrk(0)) ? 1 : 0;

                        if (current->next_node == NULL) break;
                        current = current->next_node;
                }

                void *return_addr;
                void *new_block_start_addr = ((void *) (char *) current_block_endpoint);

                if (found_existing_free_block && !split_free_block){
                        current->occupied = 1;
                        return_addr = current->start_addr;
                        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void *)((char *)return_addr + rounded_node_size), size);
                        return (void *)((char *)return_addr + rounded_node_size);
                }

                if (found_existing_free_block && split_free_block){
                        node *new_node = (void *) ((char *) current->start_addr + total_size);
                        new_node->prev_node = current;
                        new_node->next_node = current->next_node;
                        new_node->size = left_over_block_size - rounded_node_size;
                        new_node->start_addr = (char *) (current_block_endpoint) - left_over_block_size;
                        new_node->occupied = 0;

                        current->occupied = 1;
                        current->next_node = new_node;
                        current->size = rounded_request_size;
			
			if (new_node->next_node != NULL) new_node->next_node->prev_node = new_node;

                        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void *)((char *)current->start_addr + rounded_node_size), size);
                        return (void *)((char *)current->start_addr + rounded_node_size);
                }

                if (!enough_space && !found_existing_free_block){
                        void *increase = sbrk(sbrk_size);

                        if (increase == (void *) -1){
                                perror("Error sbrk()\n");
                                errno = ENOMEM;
                                DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void*)NULL, (size_t)0);
                                return NULL;
                        }

                        node *new_node = new_block_start_addr;
                        new_node->prev_node = current;
                        new_node->next_node = NULL;
                        new_node->size = rounded_request_size;
                        new_node->start_addr = new_block_start_addr;
                        new_node->occupied = 1;

                        current->next_node = new_node;
                        return_addr = new_node->start_addr;

                        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void *)((char *)return_addr + rounded_node_size), size);
                        return (void *)((char *)return_addr + rounded_node_size);
                }

                if (enough_space && !found_existing_free_block){
                        node *new_node = new_block_start_addr;
                        new_node->prev_node = current;
                        new_node->next_node = NULL;
                        new_node->size = rounded_request_size;
                        new_node->start_addr = new_block_start_addr;
                        new_node->occupied = 1;

                        current->next_node = new_node;
                        return_addr = new_node->start_addr;

                        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void *)((char *)return_addr + rounded_node_size), size);
                        return (void *)((char *)return_addr + rounded_node_size);
                }

                DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void*)NULL, (size_t)0);
                return NULL;
        }

        DEBUG_PRINT(" => (ptr=%p, size=%zu)\n", (void*)NULL, (size_t)0);
        return NULL;
}


void *calloc(size_t num_elements, size_t size){
    if (size != 0 && num_elements > SIZE_MAX / size){
	errno = ENOMEM;
	DEBUG_PRINT("MALLOC: calloc(%zu,%zu) => (ptr=%p, size=%zu)\n", num_elements, size, (void *)NULL, (size_t)0);
	return NULL;
    }

    size_t malloc_size = ((num_elements * size) + 15) & ~15;

    if (num_elements != 0 && ((malloc_size / num_elements) != size)){
        errno = ENOMEM;
        DEBUG_PRINT("MALLOC: calloc(%zu,%zu) => (ptr=%p, size=%zu)\n", num_elements, size, (void*)NULL, (size_t)0);
        return NULL;
    }

    void *pointer = malloc(malloc_size);

    if (pointer == NULL){
        DEBUG_PRINT("MALLOC: calloc(%zu,%zu) => (ptr=%p, size=%zu)\n", num_elements, size, (void*)NULL, (size_t)0);
        return NULL;
    }

    memset(pointer, 0, malloc_size);
    DEBUG_PRINT("MALLOC: calloc(%zu,%zu) => (ptr=%p, size=%zu)\n", num_elements, size, pointer, malloc_size);
    return pointer;
}


void *realloc(void *ptr, size_t size){
        if (size == 0 && ptr != NULL){
                free(ptr);
                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void*)NULL, (size_t)0);
                return NULL;
        }

        size_t rounded_request_size = (size + 15) & ~15;

        if (ptr == NULL) {
                void *r = malloc(rounded_request_size);
                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", (void*)NULL, size, r, size);
                return r;
        }

        node *current = locator(ptr);
        node *original_block = current;

        int requested_size_bigger = 0;
        size_t current_size = current->size;

        if (rounded_request_size == current_size) { DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, ptr, size); return ptr; }

        if (rounded_request_size > current_size) requested_size_bigger = 1;

        if (requested_size_bigger){
                int prev_neighbor_available = (current->prev_node !=  NULL && current->prev_node->occupied == 0) ? 1 : 0;
                int next_neighbor_available = (current->next_node != NULL && current->next_node->occupied == 0) ? 1 : 0;
                size_t total_size_available = current_size;
                size_t prev_and_current_size_available = current_size;
                size_t next_and_current_size_available = current_size;

                if (prev_neighbor_available){
                        total_size_available += current->prev_node->size + rounded_node_size;
                        prev_and_current_size_available += current->prev_node->size + rounded_node_size;
                }

                if (next_neighbor_available){
                        total_size_available += current->next_node->size + rounded_node_size;
                        next_and_current_size_available += current->next_node->size + rounded_node_size;
                }

                int enough_size = (total_size_available >= rounded_request_size) ? 1 : 0;
                size_t total_size = rounded_node_size + rounded_request_size;
                size_t left_over_block_size;
                char *merge_splitpoint;

                if (enough_size && prev_neighbor_available && !next_neighbor_available){
                        merge_splitpoint = ((char *) current->prev_node->start_addr) + total_size;
                        left_over_block_size = prev_and_current_size_available - rounded_request_size;

                        if (left_over_block_size >= min_block_size){
                                node *new_node = (void *) merge_splitpoint;
                                new_node->occupied = 0;
                                new_node->prev_node = current->prev_node;
                                new_node->next_node = current->next_node;
				if (current->next_node) current->next_node->prev_node = new_node;
                                new_node->size = left_over_block_size - rounded_node_size;
                                new_node->start_addr = (void *) merge_splitpoint;

                                current->prev_node->next_node = new_node;
                                current->prev_node->size = total_size - rounded_node_size;
                                current->prev_node->occupied = 1;

                                void *data = (char *)current + rounded_node_size;
                                void *new_addr = (char *)current->prev_node + rounded_node_size;
                                size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                                memmove(new_addr, data, copy_len);
                                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current->prev_node + rounded_node_size), size);
                                return (void *)((char *)current->prev_node + rounded_node_size);
                        }

                        current->prev_node->next_node = current->next_node;
			if (current->next_node != NULL) current->next_node->prev_node = current->prev_node;
                        current->prev_node->size = prev_and_current_size_available;
                        current->prev_node->occupied = 1;

                        void *data = (char *)current + rounded_node_size;
                        void *new_addr = (char *)current->prev_node + rounded_node_size;
                        size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                        memmove(new_addr, data, copy_len);
                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current->prev_node + rounded_node_size), size);
                        return (void *)((char *)current->prev_node + rounded_node_size);
                }

                if (enough_size && !prev_neighbor_available && next_neighbor_available){
                        merge_splitpoint = ((char *) current->start_addr) + total_size;
                        left_over_block_size = next_and_current_size_available - rounded_request_size;

                        if (left_over_block_size >= min_block_size){
                                node *new_node = (void *) merge_splitpoint;
                                new_node->occupied = 0;
                                new_node->prev_node = current;
                                new_node->next_node = current->next_node;
				if (current->next_node != NULL) current->next_node->prev_node = new_node;
                                new_node->size = left_over_block_size - rounded_node_size;
                                new_node->start_addr = (void *) merge_splitpoint;

                                current->next_node = new_node;
                                current->size = total_size - rounded_node_size;
                                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                                return (void *)((char *)current + rounded_node_size);
                        }

                        current->next_node = current->next_node->next_node;
			if (current->next_node != NULL) current->next_node->prev_node = current;
                        current->size = next_and_current_size_available;

                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                        return (void *)((char *)current + rounded_node_size);
                }

                if (enough_size && prev_neighbor_available && next_neighbor_available){
                        merge_splitpoint = ((char *) current->prev_node->start_addr) + total_size;
                        left_over_block_size = total_size_available - rounded_request_size;

                        if (left_over_block_size >= min_block_size){
                                node *prev = current->prev_node;
				node *next = current->next_node;

				node *new_node = (void *) merge_splitpoint;
                                new_node->occupied = 0;
                                new_node->prev_node = prev;
                                new_node->next_node = (next != NULL) ? next->next_node : NULL;
				if (new_node->next_node != NULL) new_node->next_node->prev_node = new_node;
                                new_node->size = left_over_block_size - rounded_node_size;
                                new_node->start_addr = (void *) merge_splitpoint;
				
				prev->next_node = new_node;
				prev->size = rounded_request_size;
				prev->occupied = 1;

                                void *data = (char *)current + rounded_node_size;
                                void *new_addr = (char *)prev + rounded_node_size;
                                size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                                memmove(new_addr, data, copy_len);
                                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current->prev_node + rounded_node_size), size);
                                return (void *)((char *)prev + rounded_node_size);
                        }

                        node *prev = current->prev_node;
			node *next = current->next_node;
			
			prev->next_node = (next != NULL) ? next->next_node : NULL;
			if (prev->next_node != NULL) prev->next_node->prev_node = prev;
			prev->size = total_size_available;
			prev->occupied = 1;

                        void *data = (char *)current + rounded_node_size;
                        void *new_addr = (char *)prev + rounded_node_size;
                        size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                        memmove(new_addr, data, copy_len);
                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current->prev_node + rounded_node_size), size);
                        return (void *)((char *)current->prev_node + rounded_node_size);
                }

                int found_existing_free_block = 0;
                if (!enough_size){
                        current = heap_start_addr;

                        while (true){
                                if (current->size >= rounded_request_size && current->occupied == 0){
                                        found_existing_free_block = 1;
                                        break;
                                }

                                if (current->next_node == NULL) break;
                                current = current->next_node;
                        }

                        if (found_existing_free_block){
                                left_over_block_size = current->size - rounded_request_size;

                                if (left_over_block_size >= min_block_size){
                                        merge_splitpoint = ((char *) current->start_addr) + total_size;
                                        node *new_node = (void *) merge_splitpoint;
                                        new_node->prev_node = current;
                                        new_node->next_node = current->next_node;
                                        new_node->size = left_over_block_size - rounded_node_size;
                                        new_node->start_addr = (void *) merge_splitpoint;
                                        new_node->occupied = 0;

                                        current->next_node = new_node;
                                        current->size = rounded_request_size;
					current->occupied = 1;

                                        void *data = (char *)original_block + rounded_node_size;
                                        void *new_addr = (char *)current + rounded_node_size;
                                        size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                                        memmove(new_addr, data, copy_len);
                                        free((char *) original_block + rounded_node_size);
                                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                                        return (void *)((char *)current + rounded_node_size);
                                }

				current->occupied = 1;

                                void *old_data = (char *)original_block + rounded_node_size;
                                void *new_addr = (char *)current + rounded_node_size;
                                size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                                memmove(new_addr, old_data, copy_len);
                                free((char *)original_block + rounded_node_size);
                                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                                return (void *)((char *)current + rounded_node_size);
                        }

                        if (!found_existing_free_block){
                                void *increase = sbrk(chunk_size);

                                if (increase == (void *) -1){
                                        perror("Error sbrk()\n");
                                        errno = ENOMEM;
                                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void*)NULL, (size_t)0);
                                        return NULL;
                                }

                                DEBUG_PRINT("MALLOC: realloc sbrk extended\n");
                                void *new_block_start_addr = ((void *) ((char *)(current->start_addr) + rounded_node_size + current_size));

                                node *new_node = new_block_start_addr;
                                new_node->start_addr = new_block_start_addr;
                                new_node->size = rounded_request_size;
                                new_node->prev_node = current;
                                new_node->next_node = NULL;
                                new_node->occupied = 1;

                                current->next_node = new_node;

                                void *data = (char *)original_block + rounded_node_size;
                                void *new_addr = (char *)new_node + rounded_node_size;
                                size_t copy_len = (current_size < rounded_request_size) ? current_size : rounded_request_size;

                                memmove(new_addr, data, copy_len);
                                free((char *) original_block + rounded_node_size);
                                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)new_node + rounded_node_size), size);
                                return (void *)((char *)new_node + rounded_node_size);
                        }
                }
        }

        if (!requested_size_bigger){
                size_t leftover_block_size = current_size - rounded_request_size;
                char *merge_splitpoint;

                if (leftover_block_size >= min_block_size){
                        merge_splitpoint = ((char *) current->start_addr) + rounded_node_size + rounded_request_size;
			
			node *right = current->next_node;
        		node *new_node = (void *)merge_splitpoint;
        		new_node->occupied = 0;
        		new_node->prev_node = current;
        		new_node->next_node = right;
        		new_node->size = leftover_block_size - rounded_node_size;
        		new_node->start_addr = (void *)merge_splitpoint;

        		if (right != NULL && right->occupied == 0){
            			new_node->size += rounded_node_size + right->size;
            			new_node->next_node = right->next_node;
            			if (new_node->next_node) new_node->next_node->prev_node = new_node;
        		}else{
            			if (right != NULL) right->prev_node = new_node;
        		}
			
			current->size = rounded_request_size;
			current->next_node = new_node;
				
                        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                        return (void *)((char *)current + rounded_node_size);
                }
                DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void *)((char *)current + rounded_node_size), size);
                return (void *)((char *)current + rounded_node_size);
        }
        DEBUG_PRINT("MALLOC: realloc(%p,%zu) => (ptr=%p, size=%zu)\n", ptr, size, (void*)NULL, (size_t)0);
        return NULL;
}
