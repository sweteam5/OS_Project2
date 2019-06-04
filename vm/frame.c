#include <hash.h>
#include <list.h>
#include <stdio.h>
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"

#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"


/* A global lock, to ensure critical sections on frame operations. */
static struct lock frame_lock;

/* A mapping from physical address to frame table entry. */
static struct hash frame_map;

/* A (circular) list of frames for the clock eviction algorithm. */
static struct list frame_list;      /* the list */
static struct list_elem *clock_ptr; /* the pointer in clock algorithm */

static unsigned frame_hash_func(const struct hash_elem *elem, void *aux);
static bool     frame_less_func(const struct hash_elem *, const struct hash_elem *, void *aux);


//frame table entry
struct frame_table_entry
  {
    void *kpage;               // use kernel page to map physical address 

    struct hash_elem helem;    // for frame map
    struct list_elem lelem;    // for frame list

    void *upage;               // user address. point to page (VM) 
    struct thread *t;          // save thread which call frame (current)

    bool pinned;               
  };


static struct frame_table_entry* pick_frame_to_evict(uint32_t* pagedir);
static void vm_frame_do_free (void *kpage, bool free_page);


void
vm_frame_init ()          // frame init(reset) , called only once
{
  lock_init (&frame_lock);
  hash_init (&frame_map, frame_hash_func, frame_less_func, NULL);
  list_init (&frame_list);
  clock_ptr = NULL;
}

void*
vm_frame_allocate (enum palloc_flags flags, void *upage) // create frame page to map with user virtual address
{
  lock_acquire (&frame_lock);

  void *frame_page = palloc_get_page (PAL_USER | flags); //PAL_USER = user page. get from user pool
  if (frame_page == NULL) { //if page allocation failed
    struct frame_table_entry *f_evicted = pick_frame_to_evict( thread_current()->pagedir ); //swap out

#if DEBUG
    printf("f_evicted: %x th=%x, pagedir = %x, up = %x, kp = %x, hash_size=%d\n", f_evicted, f_evicted->t,
        f_evicted->t->pagedir, f_evicted->upage, f_evicted->kpage, hash_size(&frame_map));
#endif
    ASSERT (f_evicted != NULL && f_evicted->t != NULL);

    // clear the page mapping, and replace it with swap
    ASSERT (f_evicted->t->pagedir != (void*)0xcccccccc);
    pagedir_clear_page(f_evicted->t->pagedir, f_evicted->upage);

    bool is_dirty = false;
    is_dirty = is_dirty || pagedir_is_dirty(f_evicted->t->pagedir, f_evicted->upage);
    is_dirty = is_dirty || pagedir_is_dirty(f_evicted->t->pagedir, f_evicted->kpage);

    swap_index_t swap_idx = vm_swap_out( f_evicted->kpage );
    vm_supt_set_swap(f_evicted->t->supt, f_evicted->upage, swap_idx);
    vm_supt_set_dirty(f_evicted->t->supt, f_evicted->upage, is_dirty);
    vm_frame_do_free(f_evicted->kpage, true); // f_evicted is also invalidated

    frame_page = palloc_get_page (PAL_USER | flags);
    ASSERT (frame_page != NULL); // should success in this chance
  }

  struct frame_table_entry *frame = malloc(sizeof(struct frame_table_entry));
  if(frame == NULL) {
    // frame allocation failed
    lock_release (&frame_lock);
    return NULL;
  }

  frame->t = thread_current ();
  frame->upage = upage;
  frame->kpage = frame_page;
  frame->pinned = true;         // can't be evicted yet

  // insert into hash table
  hash_insert (&frame_map, &frame->helem);
  list_push_back (&frame_list, &frame->lelem);

  lock_release (&frame_lock);
  return frame_page;
}

// make free to frame or page
void
vm_frame_free (void *kpage)
{
  lock_acquire (&frame_lock);
  vm_frame_do_free (kpage, true);
  lock_release (&frame_lock);
}

//Just removes then entry from table, do not palloc free.
void
vm_frame_remove_entry (void *kpage)
{
  lock_acquire (&frame_lock);
  vm_frame_do_free (kpage, false);
  lock_release (&frame_lock);
}


 //free() frame or page (internal procedure)
void
vm_frame_do_free (void *kpage, bool free_page)
{
  ASSERT (lock_held_by_current_thread(&frame_lock) == true);
  ASSERT (is_kernel_vaddr(kpage));
  ASSERT (pg_ofs (kpage) == 0); // should be aligned

  // hash lookup : a temporary entry
  struct frame_table_entry f_tmp;
  f_tmp.kpage = kpage;

  struct hash_elem *h = hash_find (&frame_map, &(f_tmp.helem));
  if (h == NULL) {
    PANIC ("The page to be freed is not stored in the table");
  }

  struct frame_table_entry *f;
  f = hash_entry(h, struct frame_table_entry, helem);

  hash_delete (&frame_map, &f->helem);
  list_remove (&f->lelem);

  // Free resources
  if(free_page) palloc_free_page(kpage);
  free(f);
}

//using clock algorithm to check out page
struct frame_table_entry* clock_frame_next(void);
struct frame_table_entry* pick_frame_to_evict( uint32_t *pagedir )
{
  size_t n = hash_size(&frame_map);
  if(n == 0) PANIC("Frame table is empty, can't happen - there is a leak somewhere");

  size_t it;
  for(it = 0; it <= n + n; ++ it) // prevent infinite loop. 2n iterations is enough
  {
    struct frame_table_entry *e = clock_frame_next();
    // if pinned, continue
    if(e->pinned) continue;
    // if referenced, using a second chance.
    else if( pagedir_is_accessed(pagedir, e->upage)) {
      pagedir_set_accessed(pagedir, e->upage, false);
      continue;
    }

    // unreferenced since its last chance. -> eviction
    return e;
  }

  PANIC ("Can't evict any frame -- Not enough memory!\n");
}
struct frame_table_entry* clock_frame_next(void)
{
  if (list_empty(&frame_list))
    PANIC("Frame table is empty, can't happen - there is a leak somewhere");

  if (clock_ptr == NULL || clock_ptr == list_end(&frame_list))
    clock_ptr = list_begin (&frame_list);
  else
    clock_ptr = list_next (clock_ptr);

  struct frame_table_entry *e = list_entry(clock_ptr, struct frame_table_entry, lelem);
  return e;
}


static void
vm_frame_set_pinned (void *kpage, bool new_value)
{
  lock_acquire (&frame_lock);

  // hash lookup : a temporary entry
  struct frame_table_entry f_tmp;
  f_tmp.kpage = kpage;
  struct hash_elem *h = hash_find (&frame_map, &(f_tmp.helem));
  if (h == NULL) {
    PANIC ("The frame to be pinned/unpinned does not exist");
  }

  struct frame_table_entry *f;
  f = hash_entry(h, struct frame_table_entry, helem);
  f->pinned = new_value;

  lock_release (&frame_lock);
}

void
vm_frame_unpin (void* kpage) {
  vm_frame_set_pinned (kpage, false);
}

void
vm_frame_pin (void* kpage) {
  vm_frame_set_pinned (kpage, true);
}


/* Helpers */

// Hash Functions required for [frame_map]. Uses 'kpage' as key.
static unsigned frame_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
  struct frame_table_entry *entry = hash_entry(elem, struct frame_table_entry, helem);
  return hash_bytes( &entry->kpage, sizeof entry->kpage );
}
static bool frame_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  struct frame_table_entry *a_entry = hash_entry(a, struct frame_table_entry, helem);
  struct frame_table_entry *b_entry = hash_entry(b, struct frame_table_entry, helem);
  return a_entry->kpage < b_entry->kpage;
}
