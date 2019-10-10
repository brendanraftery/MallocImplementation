//
// CS252: MyMalloc Project
//
// The current implementation gets memory from the OS
// every time memory is requested and never frees memory.
//
// You will implement the allocator as indicated in the handout.
//
// Also you will need to add the necessary locking mechanisms to
// support multi-threaded programs.
//

#include "MyMalloc.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <pthread.h>


// This mutex must be held whenever modifying internal allocator state,
// or referencing state that is subject to change. The skeleton should
// take care of this automatically, unless you make modifications to the
// C interface (malloc(), calloc(), realloc(), and free()).

static pthread_mutex_t mutex;

// The size of block to get from the OS (2MB).

#define ARENA_SIZE ((size_t) 2097152)

// HELPER METHODS

void replace_in_list(object_header *, object_header *);

void add_to_list(object_header *);

void request_more_memory();

// STATE VARIABLES

// Sum total size of heap

static size_t heap_size;

// Start of memory pool

static void *mem_start;

// Number of chunks requested from OS so far

static int num_chunks;

// Verbose mode enabled via environment variable
// (See initialize())

static int verbose;

// Keep track of the number of calls to each function

static int malloc_calls;
static int free_calls;
static int realloc_calls;
static int calloc_calls;

// The free list is a doubly-linked list, with a constant sentinel.

static object_header free_list_sentinel;
static object_header *free_list;


/*
 * Increments the number of calls to malloc().
 */

void increase_malloc_calls() {
  malloc_calls++;
} /* increase_malloc_calls() */

/*
 * Increase the number of calls to realloc().
 */

void increase_realloc_calls() {
  realloc_calls++;
} /* increase_realloc_calls() */

/*
 * Increase the number of calls to calloc().
 */

void increase_calloc_calls() {
  calloc_calls++;
} /* increase_calloc_calls() */

/*
 * Increase the number of calls to free().
 */

void increase_free_calls() {
  free_calls++;
} /* increase_free_calls() */

/*
 * externed version of at_exit_handler(), which can be passed to atexit().
 * See atexit(3).
 */

extern void at_exit_handler_in_c() {
  at_exit_handler();
} /* at_exit_handler_in_c() */

/*
 * Initialize the allocator by setting initial state
 * and making the first allocation.
 */

void initialize() {
  // Set this environment variable to the specified value
  // to disable verbose logging.

#define VERBOSE_ENV_VAR "MALLOCVERBOSE"
#define VERBOSE_DISABLE_STRING "NO"

  pthread_mutex_init(&mutex, NULL);

  // We default to verbose mode, but if it has been disabled in
  // the environment, disable it correctly.

  const char *env_verbose = getenv(VERBOSE_ENV_VAR);
  verbose = (!env_verbose || strcmp(env_verbose, VERBOSE_DISABLE_STRING));

  // Disable printf's buffer, so that it won't call malloc and make
  // debugging even more difficult

  setvbuf(stdout, NULL, _IONBF, 0);

  // Get initial memory block from OS

  void *new_block = get_memory_from_os(ARENA_SIZE +
                                       (2 * sizeof(object_header)) +
                                       (2 * sizeof(object_footer)));

  // In verbose mode register function to print statistics at exit

  atexit(at_exit_handler_in_c);

  // Establish memory locations for objects within the new block

  object_footer *start_fencepost = (object_footer *) new_block;
  object_header *current_header =
    (object_header *) ((char *) start_fencepost +
                              sizeof(object_footer));
  object_footer *current_footer =
    (object_footer *) ((char *) current_header +
                              ARENA_SIZE +
                              sizeof(object_header));
  object_header *end_fencepost =
    (object_header *) ((char *) current_footer +
                              sizeof(object_footer));

  // Establish fenceposts
  // We set fencepost size to 0 as an arbitrary value which would
  // be impossible as a value for a valid memory block

  start_fencepost->status = ALLOCATED;
  start_fencepost->object_size = 0;

  end_fencepost->status = ALLOCATED;
  end_fencepost->object_size = 0;
  end_fencepost->next = NULL;
  end_fencepost->prev = NULL;

  // Establish main free object

  current_header->status = UNALLOCATED;
  current_header->object_size = ARENA_SIZE +
                                sizeof(object_header) +
                                sizeof(object_footer);

  current_footer->status = UNALLOCATED;
  current_footer->object_size = current_header->object_size;

  // Initialize free list and add the new first object

  free_list = &free_list_sentinel;
  free_list->prev = current_header;
  free_list->next = current_header;
  current_header->next = free_list;
  current_header->prev = free_list;

  // Mark sentinel as such. Do not coalesce the sentinel.

  free_list->status = SENTINEL;
  free_list->object_size = 0;

  // Set start of memory pool

  mem_start = (char *) current_header;
} /* initialize() */

/*
 * Allocate an object of size size. Ideally, we can allocate from the free
 * list but if we don't have a free object large enough, go get more memory
 * from the OS. Return a pointer to the newly allocated memory.
 */

void *allocate_object(size_t size) {
  // SIZE_PRECISION determines how to round.
  // By default, round up to nearest 8 bytes.
  // It must be a power of 2.
  // MINIMUM_SIZE is the minimum size that can be requested, not including
  // header and footer. Smaller requests are rounded up to this minimum.

#define SIZE_PRECISION (8)
#define MINIMUM_SIZE (8)

  if (size < MINIMUM_SIZE) {
    size = MINIMUM_SIZE;
  }

  // Add the object_header/Footer to the size and round the total size
  // up to a multiple of 8 bytes for alignment.
  // Bitwise-and with ~(SIZE_PRECISION - 1) will set the last x bits to 0,
  // if SIZE_PRECISION = 2**x.

  size_t rounded_size = (size +
                         sizeof(object_header) +
                         sizeof(object_footer) +
                         (SIZE_PRECISION - 1)) & ~(SIZE_PRECISION - 1);

  object_header *free_list_object = free_list;

  do {
    free_list_object = free_list_object->next;

    if (free_list_object == &free_list_sentinel) {
      break;
    }

    int free_space = free_list_object->object_size;

    if (free_space >= rounded_size) {
      // If free block is large enough to split

      if (free_space >= rounded_size + sizeof(object_header) +
        sizeof(object_footer) + MINIMUM_SIZE) {
        char *start_of_split = (char *) free_list_object;
        start_of_split += rounded_size;

        // Create header and footer for new free block

        object_header *new_free_object = (object_header *) start_of_split;
        new_free_object->object_size = free_space - rounded_size;
        new_free_object->status = UNALLOCATED;
        object_footer *foot_of_block = (object_footer *) start_of_split;
        foot_of_block -= 1;
        foot_of_block->object_size = rounded_size;
        foot_of_block->status = ALLOCATED;

        // Modify header and footer of the to-be free block

        object_footer *foot_of_free = (object_footer *)
                                      (((void *) free_list_object) +
                                      free_space - sizeof(object_footer));
        foot_of_free->object_size = free_space - rounded_size;
        foot_of_free->status = UNALLOCATED;
        free_list_object->object_size = rounded_size;
        free_list_object->status = ALLOCATED;
        replace_in_list(free_list_object, new_free_object);

        // Return a pointer to the allocated block's memory section

        return (void *) free_list_object + sizeof(object_header);
      }
      else { // Not enough space to split block
        // Modify header and footer of block to be returned

        free_list_object->status = ALLOCATED;
        replace_in_list(free_list_object, NULL);
        object_footer *foot_of_block = (object_footer *)
                                        ((char *) free_list_object +
                                        free_list_object->object_size -
                                        sizeof(object_footer));

        foot_of_block->status = ALLOCATED;
        foot_of_block->object_size = free_list_object->object_size;

        return (void *) free_list_object + sizeof(object_header);
      }
    }
  } while (free_list_object != free_list);

  // If we get to here we need more memory from OS, then try allocating again

  request_more_memory();
  return allocate_object(size);
} /* allocate_object() */

/*
 * Remove a given memory segment from the freelist and replace it with
 * a new block so the replacement is in constant time and in order. If
 * the second argument is NULL, just remove the old_block from the list
 */

void replace_in_list(object_header *old_block, object_header *new_block) {
  if (new_block == NULL) {
    // <->[PREV]<->[OLD]<->[NEXT]<->  is now  <->[PREV]<->[NEXT]<->

    old_block->prev->next = old_block->next;
    old_block->next->prev = old_block->prev;

    return;
  }

  // <->[PREV]<->[OLD]<->[NEXT]<->  is now  <->[PREV]<->[NEW]<->[NEXT]<->

  new_block->next = old_block->next;
  new_block->prev = old_block->prev;
  old_block->prev->next = new_block;
  old_block->next->prev = new_block;
} /* replace_in_list() */


/*
 * Add a block to the list, making sure to keep the list in order of
 * header address (smallest to largest
 */

void add_to_list(object_header *block) {
  // Look for the correct place in the freelist for the new free block

  object_header *list_crawler = free_list;

  do {
    list_crawler = list_crawler->next;

    // If the current block is higher in memory, place new block before

    if (list_crawler > block) {
      block->prev = list_crawler->prev;
      block->next = list_crawler;
      list_crawler->prev->next = block;
      list_crawler->prev = block;
      return;
    }
  } while (list_crawler != free_list);

  // Got all the way to the end, place block at end

  free_list_sentinel.prev->next = block;
  block->prev = free_list_sentinel.prev;
  free_list_sentinel.prev = block;
  block->next = &free_list_sentinel;
} /* add_to_list() */

/*
 * Request for another chunk of memory to be requested from the OS and
 * added to the free list
 */

void request_more_memory() {
  // Set up pointers for all new objects and fenceposts

  void *new_chunk = get_memory_from_os(ARENA_SIZE +
                                       (2 * sizeof(object_header)) +
                                       (2 * sizeof(object_footer)));
  object_footer *left_fencepost = (object_footer *) new_chunk;
  object_header *current_header = (object_header *) ((char *) left_fencepost +
                                   sizeof(object_footer));
  object_footer *current_footer = (object_footer *) ((char *) current_header +
                                   ARENA_SIZE + sizeof(object_header));
  object_header *right_fencepost = (object_header *) ((char *) current_footer +
                                    sizeof(object_footer));

  // Establish fenceposts

  left_fencepost->status = ALLOCATED;
  left_fencepost->object_size = 0;

  right_fencepost->status = ALLOCATED;
  right_fencepost->object_size = 0;
  right_fencepost->next = NULL;
  right_fencepost->prev = NULL;

  // Establish free block

  current_header->status = UNALLOCATED;
  current_header->object_size = ARENA_SIZE + sizeof(object_header) +
                                sizeof(object_footer);

  current_footer->status = UNALLOCATED;
  current_footer->object_size = current_header->object_size;

  add_to_list(current_header);
} /* request_more_memory() */

/*
 * Free an object. ptr is a pointer to the usable block of memory in
 * the object. If possible, coalesce the object, then add to the free list.
 */

void free_object(void *ptr) {
  object_header *middle_head = (object_header *) ((char*) ptr -
                                sizeof(object_header));
  object_footer *middle_foot = (object_footer *) ((char *) middle_head +
                                middle_head->object_size
                                - sizeof(object_footer));
  object_footer *left_foot = (object_footer *) ((char *) middle_head -
                              sizeof(object_footer));
  object_header *right_head = (object_header *) ((char *) middle_foot +
                                sizeof(object_footer));

  // Blocks on either side of this block are both allocated

  if ((left_foot->status == ALLOCATED) && (right_head->status == ALLOCATED)) {
    middle_head->status = UNALLOCATED;
    middle_foot->status = UNALLOCATED;
    add_to_list(middle_head);
  }
  else if ((left_foot->status == UNALLOCATED) && // Left side is free block
           (right_head->status == ALLOCATED)) {
    // Remove middle head from free list if already free (from recursion)

    if (middle_head->status == UNALLOCATED) {
      replace_in_list(middle_head, NULL);
    }

    object_header *left_head = (object_header *) ((char *) middle_head -
                                left_foot->object_size);
    left_head->object_size = left_head->object_size + middle_head->object_size;
    middle_foot->object_size = left_head->object_size;
    middle_foot->status = UNALLOCATED;
  }
  else if (right_head->status == UNALLOCATED) { // Right side is free block
    middle_head->object_size = middle_head->object_size +
                                right_head->object_size;
    object_footer *right_foot = (object_footer *) ((char *) middle_foot +
                                  right_head->object_size);
    right_foot->object_size = middle_head->object_size;
    middle_head->status = UNALLOCATED;
    replace_in_list(right_head, middle_head);
  }

  if ((left_foot->status == UNALLOCATED) &&
      (right_head->status == UNALLOCATED)) { // Both sides are free blocks
    free_object((char *) middle_head + sizeof(object_header));
  }
} /* free_object() */

/*
 * Return the size of the object pointed by ptr. We assume that ptr points to
 * usable memory in a valid obejct.
 */

size_t object_size(void *ptr) {
  // ptr will point at the end of the header, so subtract the size of the
  // header to get the start of the header.

  object_header *object =
    (object_header *) ((char *) ptr - sizeof(object_header));

  return object->object_size;
} /* object_size() */

/*
 * Print statistics on heap size and
 * how many times each function has been called.
 */

void print_stats() {
  printf("\n-------------------\n");

  printf("HeapSize:\t%zd bytes\n", heap_size);
  printf("# mallocs:\t%d\n", malloc_calls);
  printf("# reallocs:\t%d\n", realloc_calls);
  printf("# callocs:\t%d\n", calloc_calls);
  printf("# frees:\t%d\n", free_calls);

  printf("\n-------------------\n");
} /* print_stats() */

/*
 * Print a representation of the current free list.
 * For each object in the free list, show the offset (distance in memory from
 * the start of the memory pool, mem_start) and the size of the object.
 */

void print_list() {
  pthread_mutex_lock(&mutex);

  printf("FreeList: ");

  object_header *ptr = free_list->next;

  while (ptr != free_list) {
    long offset = (long) ptr - (long) mem_start;
    printf("[offset:%ld,size:%zd]", offset, ptr->object_size);
    ptr = ptr->next;
    if (ptr != free_list) {
      printf("->");
    }
  }
  printf("\n");

  pthread_mutex_unlock(&mutex);
} /* print_list() */

/*
 * Use sbrk() to get the memory from the OS. See sbrk(2).
 */

void *get_memory_from_os(size_t size) {
  heap_size += size;

  void *new_block = sbrk(size);

  num_chunks++;

  return new_block;
} /* get_memory_from_os() */

/*
 * Run when the program exists, and prints final statistics about the allocator.
 */

void at_exit_handler() {
  if (verbose) {
    print_stats();
  }
} /* at_exit_handler() */



//
// C interface
//


/*
 * Allocates size bytes of memory and returns the pointer to the
 * newly-allocated memory. See malloc(3).
 */

extern void *malloc(size_t size) {
  pthread_mutex_lock(&mutex);
  increase_malloc_calls();

  void *memory = allocate_object(size);

  pthread_mutex_unlock(&mutex);

  return memory;
} /* malloc() */

/*
 * Frees a block of memory allocated by malloc(), calloc(), or realloc().
 * See malloc(3).
 */

extern void free(void *ptr) {
  pthread_mutex_lock(&mutex);
  increase_free_calls();

  if (ptr != NULL) {
    free_object(ptr);
  }

  pthread_mutex_unlock(&mutex);
} /* free() */

/*
 * Resizes the block of memory at ptr, which was allocated by
 * malloc(), realloc(), or calloc(), and returns a pointer to
 * the new resized block. See malloc(3).
 */

extern void *realloc(void *ptr, size_t size) {
  pthread_mutex_lock(&mutex);
  increase_realloc_calls();

  void *new_ptr = allocate_object(size);

  pthread_mutex_unlock(&mutex);

  // Copy old object only if ptr is non-null

  if (ptr != NULL) {
    // Copy everything from the old ptr.
    // We don't need to hold the mutex here because it is undefined behavior
    // (a double free) for the calling program to free() or realloc() this
    // memory once realloc() has already been called.

    size_t size_to_copy =  object_size(ptr);
    if (size_to_copy > size) {
      // If we are shrinking, don't write past the end of the new block

      size_to_copy = size;
    }

    memcpy(new_ptr, ptr, size_to_copy);

    pthread_mutex_lock(&mutex);
    free_object(ptr);
    pthread_mutex_unlock(&mutex);
  }

  return new_ptr;
} /* realloc() */

/*
 * Allocates contiguous memory large enough to fit num_elems elements
 * of size elem_size. Initialize the memory to 0. Return a pointer to
 * the beginning of the newly-allocated memory. See malloc(3).
 */

extern void *calloc(size_t num_elems, size_t elem_size) {
  pthread_mutex_lock(&mutex);
  increase_calloc_calls();

  // Find total size needed

  size_t size = num_elems * elem_size;

  void *ptr = allocate_object(size);

  pthread_mutex_unlock(&mutex);

  if (ptr) {
    // No error, so initialize chunk with 0s

    memset(ptr, 0, size);
  }

  return ptr;
} /* calloc() */
