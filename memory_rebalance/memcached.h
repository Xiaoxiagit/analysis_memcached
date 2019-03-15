#pragma once

/**
 * When adding a setting, be sure to update process_stat_settings
 * Globally accessible settings as derived from commandline. 
 */
struct settings {
    size_t maxbytes;
    int verbose;
    rel_time_t oldest_live; // ignore existing items older than this
    uint64_t oldest_cas; // ignore existing items with CAS values lower than this
    double factor; // chunk size growth factor
    int chunk_size;
    bool use_cas;

    int item_size_max; // Maximum item size
    int slab_chunk_size_max; // Upper end for chunks within slab pages
    int slab_page_size; // Slab's page units

    bool slab_reassign; // Whether or not slab reassignment is allowed
    int slab_automove; // Whether or not to automatically move slabs
    double slab_automove_ratio;
    unsigned int slab_automove_window;
    int hashpower_init;
    int tail_repair_time;
    bool flush_enabled;
    char *hash_algorithm;
}

extern struct settings settings;

#define ITEM_LINKED 1
#define ITEM_CAS 2

/* temp */
#define ITEM_SLABBED  4


/* Item was fetched at least once in its lifetime */
#define ITEM_FETCHED 8
/* Appended on fetch, removed on LRU shuffling */
#define ITEM_ACTIVE 16
/* If an item's storage are chained chunks */
#define ITEM_CHUNKED 32
#define ITEM_CHUNK 64
#ifdef EXTSTORE
/* ITEM_data bulk is external to item */
#define ITEM_HDR  128
#endif

/**
 * Structure for storing items within memcached.
 */
typedef struct _stritem {
    /* Protected by LRU locks*/
    struct _stritem     *next;
    struct _stritem     *prev;
    /* Rest are protected by an item lock */
    struct _stritem     *h_next;    // hash chain next
    rel_time_t          time;       // least recent access
    rel_time_t          exptime;    // expire time
    int                 nbytes;     // size of data
    unsigned short      refcount;   
    uint8_t             nsuffix;    // length of flags-and-length string
    uint8_t             it_flags;   // ITEM_* above
    uint8_t             slabs_clsid; // which slab class we're in
    uint8_t             nkey;       // key length, w/terminating null and padding
    /**
     * this odd type prevents type-punning issues when we do
     * the little shuffle to save space when not using CAS.
     */
    union {
        uint64_t cas;
        char end;
    } data[];
    /* if it_flags & ITEM_CAS we have 8 bytes CAS
     * then null-terminated key
     * then " flags length\r\n" (to terminating null)
     * then data with terminating \r\m (no terminating null; it's binary!)
     */
} item;

/* Header when an item is actually a chunk of another item.*/
typedef struct _strchunk {
    struct _strchunk *next; // points within its own chain
    struct _strchunk *prev; // can potentially point to the head
    struct _stritem *head; // always points to the owner chunk
    int size;   // available chunk space in bytes
    int used;   // chunk space used
    int nbytes; // used.
    unsigned short refcount; // used?
    uint8_t orig_clsid; // for obj hdr chunks slabs_clsid is fake
    uint8_t it_flags; // ITEM_* above
    uint8_t slabs_clsid;    // Same as above
    char data[];
} item_chunk;

/* current time of day (updated periodically)*/
extern volatile rel_time_t current_time;

/* TODO: Move to slabs.h? */
extern volatile int slab_rebalance_signal;

struct slab_rebalance {
    void *slab_start;
    void *slab_end;
    void *slab_pos;
    int s_clsid;
    int d_clsid;
    uint32_t busy_items;
    uint32_t rescues;
    uint32_t evictions_nomem;
    uint32_t inline_reclaim;
    uint32_t chunk_rescues;
    uint32_t busy_deletes;
    uint32_t busy_loops;
    uint8_t done;
};

extern struct slab_rebalance slab_rebal;

#include "slabs.h"
#include "assoc.h"
#include "items.h"
#include "hash.h"

/*
 * Functions such as the libevent-related calls that need to do cross-thread
 * communication in multithreaded mode (rather than actually doing the work
 * in the current thread) are called via "dispatch_" frontends, which are
 * also #define-d to directly call the underlying code in singlethreaded mode.
 */
void memcached_thread_init(int nthreads, void *arg);

item *item_alloc(char *key, size_t nkey, int flags, rel_time_t exptime, int nbytes);
#define DO_UPDATE true
#define DONT_UPDATE false
item *item_get(const char *key, const size_t nkey, conn *c, const bool do_update);
item *item_touch(const char *key, const size_t nkey, uint32_t exptime, conn *c);
int item_link(item *it);
void item_remove(item *it);
int item_replace(item *it, item *new_it, const uint32_t hv);
void item_unlink(item *it);

void item_lock(uint32_t hv);

