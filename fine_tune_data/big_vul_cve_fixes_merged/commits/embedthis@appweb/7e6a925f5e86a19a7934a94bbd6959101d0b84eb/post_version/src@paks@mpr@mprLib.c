/*
 * Embedthis MPR Library Source 
*/

#include "mpr.h"



/********* Start of file src/mem.c ************/


/**
    mem.c - Memory Allocator and Garbage Collector. 

    This is the MPR memory allocation service. It provides an application specific memory allocator to use instead 
    of malloc. This allocator is tailored to the needs of embedded applications and is faster than most general 
    purpose malloc allocators. It is deterministic and allocates and frees in constant time O(1). It exhibits very 
    low fragmentation and accurate coalescing.

    The allocator uses a garbage collector for freeing unused memory. The collector is a cooperative, non-compacting,
    parallel collector.  The allocator is optimized for frequent allocations of small blocks (< 4K) and uses a 
    scheme of free queues for fast allocation.
    
    The allocator handles memory allocation errors globally. The application may configure a memory limit so that
    memory depletion can be proactively detected and handled before memory allocations actually fail.
   
    A memory block that is being used must be marked as active to prevent the garbage collector from reclaiming it.
    To mark a block as active, #mprMarkBlock must be called during each garbage collection cycle. When allocating
    non-temporal memory blocks, a manager callback can be specified via #mprAllocObj. This manager routine will be
    called by the collector so that dependent memory blocks can be marked as active.
  
    The collector performs the marking phase by invoking the manager routines for a set of root blocks. A block can be
    added to the set of roots by calling #mprAddRoot. Each root's manager routine will mark other blocks which will cause
    their manager routines to run and so on, until all active blocks have been marked. Non-marked blocks can then safely
    be reclaimed as garbage. A block may alternatively be permanently marked as active by calling #mprHold.
 
    The mark phase begins when all threads explicitly "yield" to the garbage collector. This cooperative approach ensures
    that user threads will not inadvertendly loose allocated blocks to the collector. Once all active blocks are marked,
    user threads are resumed and the garbage sweeper frees unused blocks in parallel with user threads.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Defines ***********************************/

#undef GET_MEM
#undef GET_PTR
#define GET_MEM(ptr)                ((MprMem*) (((char*) (ptr)) - sizeof(MprMem)))
#define GET_PTR(mp)                 ((char*) (((char*) mp) + sizeof(MprMem)))
#define GET_USIZE(mp)               ((size_t) (mp->size - sizeof(MprMem) - (mp->hasManager * sizeof(void*))))

/*
    These routines are stable and will work, lock-free regardless of block splitting or joining.
    There is be a race where GET_NEXT will skip a block if the allocator is splits mp.
 */
#define GET_NEXT(mp)                ((MprMem*) ((char*) mp + mp->size))
#define GET_REGION(mp)              ((MprRegion*) (((char*) mp) - MPR_ALLOC_ALIGN(sizeof(MprRegion))))

/*
    Memory checking and breakpoints
    ME_MPR_ALLOC_DEBUG checks that blocks are valid and keeps track of the location where memory is allocated from.
 */
#if ME_MPR_ALLOC_DEBUG
    /*
        Set this address to break when this address is allocated or freed
        Only used for debug, but defined regardless so we can have constant exports.
     */
    static MprMem *stopAlloc = 0;
    static int stopSeqno = -1;

    #define BREAKPOINT(mp)          breakpoint(mp)
    #define CHECK(mp)               if (mp) { mprCheckBlock((MprMem*) mp); } else
    #define CHECK_PTR(ptr)          CHECK(GET_MEM(ptr))
    #define SCRIBBLE(mp)            if (heap->scribble && mp != GET_MEM(MPR)) { \
                                        memset((char*) mp + MPR_ALLOC_MIN_BLOCK, 0xFE, mp->size - MPR_ALLOC_MIN_BLOCK); \
                                    } else
    #define SCRIBBLE_RANGE(ptr, size) if (heap->scribble) { \
                                        memset((char*) ptr, 0xFE, size); \
                                    } else
    #define SET_MAGIC(mp)           mp->magic = MPR_ALLOC_MAGIC
    #define SET_SEQ(mp)             mp->seqno = heap->nextSeqno++
    #define VALID_BLK(mp)           validBlk(mp)
    #define SET_NAME(mp, value)     mp->name = value

#else
    #define BREAKPOINT(mp)
    #define CHECK(mp)
    #define CHECK_PTR(mp)
    #define SCRIBBLE(mp)
    #define SCRIBBLE_RANGE(ptr, size)
    #define SET_NAME(mp, value)
    #define SET_MAGIC(mp)
    #define SET_SEQ(mp)
    #define VALID_BLK(mp) 1
#endif

#define ATOMIC_ADD(field, adj) mprAtomicAdd64((int64*) &heap->stats.field, adj)

#if ME_MPR_ALLOC_STATS
    #define ATOMIC_INC(field) mprAtomicAdd64((int64*) &heap->stats.field, 1)
    #define INC(field) heap->stats.field++
#else
    #define ATOMIC_INC(field)
    #define INC(field)
#endif

#if LINUX || ME_BSD_LIKE
    #define findFirstBit(word) ffsl((long) word)
#endif
#if MACOSX
    #define findLastBit(x) flsl((long) x)
#endif
#ifndef findFirstBit
    static ME_INLINE int findFirstBit(size_t word);
#endif
#ifndef findLastBit
    static ME_INLINE int findLastBit(size_t word);
#endif

#define YIELDED_THREADS     0x1         /* Resume threads that are yielded (only) */
#define WAITING_THREADS     0x2         /* Resume threads that are waiting for GC sweep to complete */

/********************************** Data **************************************/

#undef              MPR
PUBLIC Mpr          *MPR;
static MprHeap      *heap;
static MprMemStats  memStats;
static int          padding[] = { 0, MPR_MANAGER_SIZE };
static int          pauseGC;

/***************************** Forward Declarations ***************************/

static ME_INLINE bool acquire(MprFreeQueue *freeq);
static void allocException(int cause, size_t size);
static MprMem *allocMem(size_t size);
static ME_INLINE int cas(size_t *target, size_t expected, size_t value);
static ME_INLINE bool claim(MprMem *mp);
static ME_INLINE void clearbitmap(size_t *bitmap, int bindex);
static void dummyManager(void *ptr, int flags);
static void freeBlock(MprMem *mp);
static void getSystemInfo();
static MprMem *growHeap(size_t size);
static void invokeAllDestructors();
static ME_INLINE size_t qtosize(int qindex);
static ME_INLINE bool linkBlock(MprMem *mp); 
static ME_INLINE void linkSpareBlock(char *ptr, size_t size);
static ME_INLINE void initBlock(MprMem *mp, size_t size, int first);
static int initQueues();
static void invokeDestructors();
static void markAndSweep();
static void markRoots();
static int pauseThreads();
static void printMemReport();
static ME_INLINE void release(MprFreeQueue *freeq);
static void resumeThreads(int flags);
static ME_INLINE void setbitmap(size_t *bitmap, int bindex);
static ME_INLINE int sizetoq(size_t size);
static void sweep();
static void sweeperThread(void *unused, MprThread *tp);
static ME_INLINE void triggerGC();
static ME_INLINE void unlinkBlock(MprMem *mp);
static void *vmalloc(size_t size, int mode);
static void vmfree(void *ptr, size_t size);

#if ME_WIN_LIKE
    static int winPageModes(int flags);
#endif
#if ME_MPR_ALLOC_DEBUG
    static void breakpoint(MprMem *mp);
    static int validBlk(MprMem *mp);
    static void freeLocation(MprMem *mp);
#else
    #define freeLocation(mp)
#endif
#if ME_MPR_ALLOC_STATS
    static void printQueueStats();
    static void printGCStats();
#endif
#if ME_MPR_ALLOC_STACK
    static void monitorStack();
#else
    #define monitorStack()
#endif

/************************************* Code ***********************************/

PUBLIC Mpr *mprCreateMemService(MprManager manager, int flags)
{
    MprMem      *mp;
    MprRegion   *region;
    size_t      size, mprSize, spareSize, regionSize;

    getSystemInfo();
    size = MPR_PAGE_ALIGN(sizeof(MprHeap), memStats.pageSize);
    if ((heap = vmalloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        return NULL;
    }
    memset(heap, 0, sizeof(MprHeap));
    heap->stats.cpuCores = memStats.cpuCores;
    heap->stats.pageSize = memStats.pageSize;
    heap->stats.maxHeap = (size_t) -1;
    heap->stats.warnHeap = ((size_t) -1) / 100 * 95;

    /*
        Hand-craft the Mpr structure from the first region. Free the remainder below.
     */
    mprSize = MPR_ALLOC_ALIGN(sizeof(MprMem) + sizeof(Mpr) + (MPR_MANAGER_SIZE * sizeof(void*)));
    regionSize = MPR_ALLOC_ALIGN(sizeof(MprRegion));
    size = max(mprSize + regionSize, ME_MPR_ALLOC_REGION_SIZE);
    if ((region = mprVirtAlloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        return NULL;
    }
    mp = region->start = (MprMem*) (((char*) region) + regionSize);
    region->end = (MprMem*) (((char*) region) + size);
    region->size = size;

    MPR = (Mpr*) GET_PTR(mp);
    initBlock(mp, mprSize, 1);
    SET_MANAGER(mp, manager);
    mprSetName(MPR, "Mpr");
    MPR->heap = heap;

    heap->flags = flags;
    heap->nextSeqno = 1;
    heap->regionSize = ME_MPR_ALLOC_REGION_SIZE;
    heap->stats.maxHeap = (size_t) -1;
    heap->stats.warnHeap = ((size_t) -1) / 100 * 95;
    heap->stats.cacheHeap = ME_MPR_ALLOC_CACHE;
    heap->stats.lowHeap = max(ME_MPR_ALLOC_CACHE / 8, ME_MPR_ALLOC_REGION_SIZE);
    heap->workQuota = ME_MPR_ALLOC_QUOTA;
    heap->gcEnabled = !(heap->flags & MPR_DISABLE_GC);

    /* Internal testing use only */
    if (scmp(getenv("MPR_DISABLE_GC"), "1") == 0) {
        heap->gcEnabled = 0;
    }
#if ME_MPR_ALLOC_DEBUG
    if (scmp(getenv("MPR_SCRIBBLE_MEM"), "1") == 0) {
        heap->scribble = 1;
    }
    if (scmp(getenv("MPR_VERIFY_MEM"), "1") == 0) {
        heap->verify = 1;
    }
    if (scmp(getenv("MPR_TRACK_MEM"), "1") == 0) {
        heap->track = 1;
    }
#endif
    heap->stats.bytesAllocated += size;
    heap->stats.bytesAllocatedPeak = heap->stats.bytesAllocated;
    INC(allocs);
    initQueues();

    /*
        Free the remaining memory after MPR
     */
    spareSize = size - regionSize - mprSize;
    if (spareSize > 0) {
        linkSpareBlock(((char*) mp) + mprSize, spareSize);
        heap->regions = region;
    }
    heap->gcCond = mprCreateCond();
    heap->roots = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    mprAddRoot(MPR);
    return MPR;
}


/*
    Destroy all allocated memory including the MPR itself
 */
PUBLIC void mprDestroyMemService()
{
    MprRegion   *region, *next;
    ssize       size;

    for (region = heap->regions; region; ) {
        next = region->next;
        mprVirtFree(region, region->size);
        region = next;
    }
    size = MPR_PAGE_ALIGN(sizeof(MprHeap), memStats.pageSize);
    mprVirtFree(heap, size);
    MPR = 0;
    heap = 0;
}


static ME_INLINE void initBlock(MprMem *mp, size_t size, int first)
{
    static MprMem empty = {0};

    *mp = empty;
    /* Implicit:  mp->free = 0; */
    mp->first = first;
    mp->mark = heap->mark;
    mp->size = (MprMemSize) size;
    SET_MAGIC(mp);
    SET_SEQ(mp);
    SET_NAME(mp, NULL);
    CHECK(mp);
}


PUBLIC void *mprAllocMem(size_t usize, int flags)
{
    MprMem      *mp;
    void        *ptr;
    size_t      size;
    int         padWords;

    assert(!heap->marking);

    padWords = padding[flags & MPR_ALLOC_PAD_MASK];
    size = usize + sizeof(MprMem) + (padWords * sizeof(void*));
    size = max(size, MPR_ALLOC_MIN_BLOCK);
    size = MPR_ALLOC_ALIGN(size);

    if ((mp = allocMem(size)) == NULL) {
        return NULL;
    }
    mp->hasManager = (flags & MPR_ALLOC_MANAGER) ? 1 : 0;
    ptr = GET_PTR(mp);
    if (flags & MPR_ALLOC_ZERO && !mp->fullRegion) {
        /* Regions are zeroed by vmalloc */
        memset(ptr, 0, GET_USIZE(mp));
    }
    CHECK(mp);
    monitorStack();
    return ptr;
}


/*
    Optimized allocation for blocks without managers or zeroing
 */
PUBLIC void *mprAllocFast(size_t usize)
{
    MprMem  *mp;
    size_t  size;

    size = usize + sizeof(MprMem);
    size = max(size, MPR_ALLOC_MIN_BLOCK);
    size = MPR_ALLOC_ALIGN(size);
    if ((mp = allocMem(size)) == NULL) {
        return NULL;
    }
    return GET_PTR(mp);
}


PUBLIC void *mprReallocMem(void *ptr, size_t usize)
{
    MprMem      *mp, *newb;
    void        *newptr;
    size_t      oldSize, oldUsize;

    assert(usize > 0);
    if (ptr == 0) {
        return mprAllocZeroed(usize);
    }
    mp = GET_MEM(ptr);
    CHECK(mp);

    oldUsize = GET_USIZE(mp);
    if (usize <= oldUsize) {
        return ptr;
    }
    if ((newptr = mprAllocMem(usize, mp->hasManager ? MPR_ALLOC_MANAGER : 0)) == NULL) {
        return 0;
    }
    newb = GET_MEM(newptr);
    if (mp->hasManager) {
        SET_MANAGER(newb, GET_MANAGER(mp));
    }
    oldSize = mp->size;
    memcpy(newptr, ptr, oldSize - sizeof(MprMem));
    /*
        New memory is zeroed
     */
    memset(&((char*) newptr)[oldUsize], 0, GET_USIZE(newb) - oldUsize);
    return newptr;
}


PUBLIC void *mprMemdupMem(cvoid *ptr, size_t usize)
{
    char    *newp;

    if ((newp = mprAllocMem(usize, 0)) != 0) {
        memcpy(newp, ptr, usize);
    }
    return newp;
}


PUBLIC int mprMemcmp(cvoid *s1, size_t s1Len, cvoid *s2, size_t s2Len)
{
    int         rc;

    assert(s1);
    assert(s2);
    assert(s1Len >= 0);
    assert(s2Len >= 0);

    if ((rc = memcmp(s1, s2, min(s1Len, s2Len))) == 0) {
        if (s1Len < s2Len) {
            return -1;
        } else if (s1Len > s2Len) {
            return 1;
        }
    }
    return rc;
}


/*
    mprMemcpy will support insitu copy where src and destination overlap
 */
PUBLIC size_t mprMemcpy(void *dest, size_t destMax, cvoid *src, size_t nbytes)
{
    assert(dest);
    assert(destMax <= 0 || destMax >= nbytes);
    assert(src);
    assert(nbytes >= 0);

    if (destMax > 0 && nbytes > destMax) {
        assert(!MPR_ERR_WONT_FIT);
        return 0;
    }
    if (nbytes > 0) {
        memmove(dest, src, nbytes);
        return nbytes;
    } else {
        return 0;
    }
}

/*************************** Allocator *************************/

static int initQueues() 
{
    MprFreeQueue    *freeq;
    int             qindex;

    for (freeq = heap->freeq, qindex = 0; freeq < &heap->freeq[MPR_ALLOC_NUM_QUEUES]; freeq++, qindex++) {
        /* Size includes MprMem header */
        freeq->minSize = (MprMemSize) qtosize(qindex);
#if (ME_MPR_ALLOC_STATS && ME_MPR_ALLOC_DEBUG) && KEEP
        printf("Queue: %d, usize %u  size %u\n",
            (int) (freeq - heap->freeq), (int) freeq->minSize - (int) sizeof(MprMem), (int) freeq->minSize);
#endif
        assert(sizetoq(freeq->minSize) == qindex);
        freeq->next = freeq->prev = (MprFreeMem*) freeq;
        mprInitSpinLock(&freeq->lock);
    }
    return 0;
}


/*
    Memory allocator. This routine races with the sweeper.
 */
static MprMem *allocMem(size_t required)
{
    MprFreeQueue    *freeq;
    MprFreeMem      *fp;
    MprMem          *mp;
    size_t          *bitmap, localMap;
    int             baseBindex, bindex, qindex, baseQindex, retryIndex;

    ATOMIC_INC(requests);

    if ((qindex = sizetoq(required)) >= 0) {
        /*
            Check if the requested size is the smallest possible size in a queue. If not the smallest, must look at the 
            next queue higher up to guarantee a block of sufficient size. This implements a Good-fit strategy.
         */
        freeq = &heap->freeq[qindex];
        if (required > freeq->minSize) {
            if (++qindex >= MPR_ALLOC_NUM_QUEUES) {
                qindex = -1;
            } else {
                assert(required < heap->freeq[qindex].minSize);
            }
        }
    }
    baseQindex = qindex;

    if (qindex >= 0) {
        heap->workDone += required;
    retry:
        retryIndex = -1;
        baseBindex = qindex / MPR_ALLOC_BITMAP_BITS;
        bitmap = &heap->bitmap[baseBindex];

        /*
            Non-blocking search for a free block. If contention of any kind, simply skip the queue and try the next queue.
         */
        for (bindex = baseBindex; bindex < MPR_ALLOC_NUM_BITMAPS; bitmap++, bindex++) {
            /* Mask queues lower than the base queue */
            localMap = *bitmap & ((size_t) ((uint64) -1 << max(0, (qindex - (MPR_ALLOC_BITMAP_BITS * bindex)))));

            while (localMap) {
                qindex = (bindex * MPR_ALLOC_BITMAP_BITS) + findFirstBit(localMap) - 1;
                freeq = &heap->freeq[qindex];
                ATOMIC_INC(trys);
                if (acquire(freeq)) {
                    if (freeq->next != (MprFreeMem*) freeq) {
                        /* Inline unlinkBlock for speed */
                        fp = freeq->next;
                        fp->prev->next = fp->next;
                        fp->next->prev = fp->prev;
                        fp->blk.qindex = 0;
                        fp->blk.mark = heap->mark;
                        fp->blk.free = 0;
                        if (--freeq->count == 0) {
                            clearbitmap(bitmap, qindex % MPR_ALLOC_BITMAP_BITS);
                        }
                        assert(freeq->count >= 0);
                        mp = (MprMem*) fp;
                        release(freeq);
                        mprAtomicAdd64((int64*) &heap->stats.bytesFree, -(int64) mp->size);

                        if (mp->size >= (size_t) (required + MPR_ALLOC_MIN_SPLIT)) {
                            linkSpareBlock(((char*) mp) + required, mp->size - required);
                            mp->size = (MprMemSize) required;
                            ATOMIC_INC(splits);
                        }
                        if (!heap->gcRequested && heap->workDone > heap->workQuota) {
                            triggerGC();
                        }
                        ATOMIC_INC(reuse);
                        assert(mp->size >= required);
                        return mp;
                    } else {
                        /* Another thread raced for the last block */
                        ATOMIC_INC(race);
                        if (freeq->count == 0) {
                            clearbitmap(bitmap, qindex % MPR_ALLOC_BITMAP_BITS);
                        }
                        release(freeq);
                    }
                } else {
                    /* Contention on this queue */
                    ATOMIC_INC(tryFails);
                    if (freeq->count > 0 && retryIndex < 0) {
                        retryIndex = qindex;
                    }
                }
                /* 
                    Refresh the bitmap incase threads have split or depleted suitable queues. 
                    +1 to step past the current queue.
                 */
                localMap = *bitmap & ((size_t) ((uint64) -1 << max(0, (qindex + 1 - (MPR_ALLOC_BITMAP_BITS * bindex)))));
                ATOMIC_INC(qrace);
            }
        }
        /*
            Avoid growing the heap if there is a suitable block in the heap.
         */
        if (retryIndex >= 0) {
            /* Contention on a suitable queue - retry that */
            ATOMIC_INC(retries);
            qindex = retryIndex;
            goto retry;
        }
        if (heap->stats.bytesFree > heap->stats.lowHeap) {
            /* A suitable block may be available - try again */
            bitmap = &heap->bitmap[baseBindex];
            for (bindex = baseBindex; bindex < MPR_ALLOC_NUM_BITMAPS; bitmap++, bindex++) {
                if (*bitmap & ((size_t) ((uint64) -1 << max(0, (baseQindex - (MPR_ALLOC_BITMAP_BITS * bindex)))))) {
                    qindex = baseQindex;
                    goto retry;
                }
            }
        }
    }
    return growHeap(required);
}


/*
    Grow the heap and return a block of the required size (unqueued)
 */
static MprMem *growHeap(size_t required)
{
    MprRegion   *region;
    MprMem      *mp;
    size_t      size, rsize, spareLen;

    if (required < MPR_ALLOC_MAX_BLOCK && (heap->workDone > heap->workQuota)) {
        triggerGC();
    }
    if (required >= MPR_ALLOC_MAX) {
        allocException(MPR_MEM_TOO_BIG, required);
        return 0;
    }
    rsize = MPR_ALLOC_ALIGN(sizeof(MprRegion));
    size = max((size_t) required + rsize, (size_t) heap->regionSize);
    if ((region = mprVirtAlloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        allocException(MPR_MEM_TOO_BIG, size);
        return 0;
    }
    region->size = size;
    region->start = (MprMem*) (((char*) region) + rsize);
    region->end = (MprMem*) ((char*) region + size);
    region->freeable = 0;
    mp = (MprMem*) region->start;
    spareLen = size - required - rsize;

    /*
        If a block is big, don't split the block. This improves the chances it will be unpinned.
     */
    if (spareLen < MPR_ALLOC_MIN_BLOCK || required >= MPR_ALLOC_MAX_BLOCK) {
        required = size - rsize; 
        spareLen = 0;
    }
    initBlock(mp, required, 1);
    if (spareLen > 0) {
        assert(spareLen >= MPR_ALLOC_MIN_BLOCK);
        linkSpareBlock(((char*) mp) + required, spareLen);
    } else {
        mp->fullRegion = 1;
    }
    mprAtomicListInsert((void**) &heap->regions, (void**) &region->next, region);
    ATOMIC_ADD(bytesAllocated, size);
    /*
        Compute peak heap stats. Not an accurate stat - tolerate races.
     */
    if (heap->stats.bytesAllocated > heap->stats.bytesAllocatedPeak) {
        heap->stats.bytesAllocatedPeak = heap->stats.bytesAllocated;
#if (ME_MPR_ALLOC_STATS && ME_MPR_ALLOC_DEBUG) && KEEP
        printf("MPR: Heap new max %lld request %lu\n", heap->stats.bytesAllocatedPeak, required);
#endif
    }
    CHECK(mp);
    ATOMIC_INC(allocs);
    return mp;
}


static void freeBlock(MprMem *mp)
{
    MprRegion   *region;

    assert(!mp->free);
    SCRIBBLE(mp);
#if ME_DEBUG || ME_MPR_ALLOC_STATS
    heap->stats.swept++;
    heap->stats.sweptBytes += mp->size;
#endif
    heap->freedBlocks = 1;
#if ME_MPR_ALLOC_STATS
    heap->stats.freed += mp->size;
#endif
    if (mp->first) {
        region = GET_REGION(mp);
        if (GET_NEXT(mp) >= region->end) {
            if (mp->fullRegion || heap->stats.bytesFree >= heap->stats.cacheHeap) {
                region->freeable = 1;
                return;
            }
        }
    }
    linkBlock(mp);
}


/*
    Map a queue index to a block size. This size includes the MprMem header.
 */
static ME_INLINE size_t qtosize(int qindex)
{
    size_t  size;
    int     high, low;

    high = qindex / MPR_ALLOC_NUM_QBITS;
    low = qindex % MPR_ALLOC_NUM_QBITS;
    if (high) {
        low += MPR_ALLOC_NUM_QBITS;
    }
    high = max(0, high - 1);
    size = (low << high) << ME_MPR_ALLOC_ALIGN_SHIFT;
    size += sizeof(MprMem);
    return size;
}


/*
    Map a block size to a queue index. The block size includes the MprMem header. However, determine the free queue 
    based on user sizes (sans header). This permits block searches to avoid scanning the next highest queue for 
    common block sizes: eg. 1K.
 */
static ME_INLINE int sizetoq(size_t size)
{
    size_t      asize;
    int         msb, shift, high, low, qindex;

    assert(MPR_ALLOC_ALIGN(size) == size);

    if (size > MPR_ALLOC_MAX_BLOCK) {
        /* Large block, don't put on queues */
        return -1;
    }
    size -= sizeof(MprMem);
    asize = (size >> ME_MPR_ALLOC_ALIGN_SHIFT);
    msb = findLastBit(asize) - 1;
    high = max(0, msb - MPR_ALLOC_QBITS_SHIFT + 1);
    shift = max(0, high - 1);
    low = (asize >> shift) & (MPR_ALLOC_NUM_QBITS - 1);
    qindex = (high * MPR_ALLOC_NUM_QBITS) + low;
    assert(qindex < MPR_ALLOC_NUM_QUEUES);
    return qindex;
}


/*
    Add a block to a free q. Called by user threads from allocMem and by sweeper from freeBlock.
    WARNING: Must be called with the freelist not acquired. This is the opposite of unlinkBlock.
 */
static ME_INLINE bool linkBlock(MprMem *mp) 
{
    MprFreeQueue    *freeq;
    MprFreeMem      *fp;
    ssize           size;
    int             qindex;

    CHECK(mp);

    size = mp->size;
    qindex = sizetoq(size);
    assert(qindex >= 0);
    freeq = &heap->freeq[qindex];

    /*
        Acquire the free queue. Racing with multiple-threads in allocMem(). If we fail to acquire, the sweeper
        will retry next time. Note: the bitmap is updated with the queue acquired to safeguard the integrity of 
        this queue's free bit.
     */
    ATOMIC_INC(trys);
    if (!acquire(freeq)) {
        ATOMIC_INC(tryFails);
        mp->mark = !mp->mark;
        assert(!mp->free);
        return 0;
    }
    assert(qindex >= 0);
    mp->qindex = qindex;
    mp->free = 1;
    mp->hasManager = 0;
    fp = (MprFreeMem*) mp;
    fp->next = freeq->next;
    fp->prev = (MprFreeMem*) freeq;
    freeq->next->prev = fp;
    freeq->next = fp;
    freeq->count++;
    setbitmap(&heap->bitmap[mp->qindex / MPR_ALLOC_BITMAP_BITS], mp->qindex % MPR_ALLOC_BITMAP_BITS);
    release(freeq);
    mprAtomicAdd64((int64*) &heap->stats.bytesFree, size);
    return 1;
}


/*
    Remove a block from a free q.
    WARNING: Must be called with the freelist acquired.
 */
static ME_INLINE void unlinkBlock(MprMem *mp) 
{
    MprFreeQueue    *freeq;
    MprFreeMem      *fp;

    fp = (MprFreeMem*) mp;
    fp->prev->next = fp->next;
    fp->next->prev = fp->prev;
    assert(mp->qindex);
    freeq = &heap->freeq[mp->qindex];
    freeq->count--;
    mp->qindex = 0;
#if ME_MPR_ALLOC_DEBUG
    fp->next = fp->prev = NULL;
#endif
    mprAtomicAdd64((int64*) &heap->stats.bytesFree, -(int64) mp->size);
}


/*
    This must be robust. i.e. the block spare memory must end up on the freeq
 */
static ME_INLINE void linkSpareBlock(char *ptr, size_t size)
{ 
    MprMem  *mp;
    size_t  len;

    assert(size >= MPR_ALLOC_MIN_BLOCK);
    mp = (MprMem*) ptr;
    len = size;

    while (size > 0) {
        initBlock(mp, len, 0);
        if (!linkBlock(mp)) {
            /* Cannot acquire queue. Break into pieces and try lesser queue */
            if (len >= (MPR_ALLOC_MIN_BLOCK * 8)) {
                len = MPR_ALLOC_ALIGN(len / 2);
                len = min(size, len);
            }
        } else {
            size -= len;
            mp = (MprMem*) ((char*) mp + len);
            len = size;
        }
    } 
    assert(size == 0);
}


/*
    Allocate virtual memory and check a memory allocation request against configured maximums and redlines. 
    An application-wide memory allocation failure routine can be invoked from here when a memory redline is exceeded. 
    It is the application's responsibility to set the red-line value suitable for the system.
    Memory is zereod on all platforms.
 */
PUBLIC void *mprVirtAlloc(size_t size, int mode)
{
    size_t      used;
    void        *ptr;

    used = mprGetMem();
    if (memStats.pageSize) {
        size = MPR_PAGE_ALIGN(size, memStats.pageSize);
    }
    if ((size + used) > heap->stats.maxHeap) {
        allocException(MPR_MEM_LIMIT, size);

    } else if ((size + used) > heap->stats.warnHeap) {
        allocException(MPR_MEM_WARNING, size);
    }
    if ((ptr = vmalloc(size, mode)) == 0) {
        allocException(MPR_MEM_FAIL, size);
        return 0;
    }
    return ptr;
}


PUBLIC void mprVirtFree(void *ptr, size_t size)
{
    vmfree(ptr, size);
}


static void *vmalloc(size_t size, int mode)
{
    void    *ptr;

#if ME_MPR_ALLOC_VIRTUAL
    #if ME_UNIX_LIKE
        if ((ptr = mmap(0, size, mode, MAP_PRIVATE | MAP_ANON, -1, 0)) == (void*) -1) {
            return 0;
        }
    #elif ME_WIN_LIKE
        ptr = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, winPageModes(mode));
    #else
        if ((ptr = malloc(size)) != 0) {
            memset(ptr, 0, size);
        }
    #endif
#else
    if ((ptr = malloc(size)) != 0) {
        memset(ptr, 0, size);
    }
#endif
    return ptr;
}


static void vmfree(void *ptr, size_t size)
{
#if ME_MPR_ALLOC_VIRTUAL
    #if ME_UNIX_LIKE
        if (munmap(ptr, size) != 0) {
            assert(0);
        }
    #elif ME_WIN_LIKE
        VirtualFree(ptr, 0, MEM_RELEASE);
    #else
        if (heap->scribble) {
            memset(ptr, 0x11, size);
        }
        free(ptr);
    #endif
#else
    free(ptr);
#endif
}


/***************************************************** Garbage Colllector *************************************************/

PUBLIC void mprStartGCService()
{
    if (heap->gcEnabled) {
        if ((heap->sweeper = mprCreateThread("sweeper", sweeperThread, NULL, 0)) == 0) {
            mprLog("critical mpr memory", 0, "Cannot create sweeper thread");
            MPR->hasError = 1;
        } else {
            mprStartThread(heap->sweeper);
        }
    }
}


PUBLIC void mprStopGCService()
{
    int     i;

    mprWakeGCService();
    for (i = 0; heap->sweeper && i < MPR_TIMEOUT_STOP; i++) {
        mprNap(1);
    }
    invokeAllDestructors();
}


PUBLIC void mprWakeGCService()
{
    mprSignalCond(heap->gcCond);
}


static ME_INLINE void triggerGC()
{
    if (!heap->gcRequested && heap->gcEnabled && !pauseGC) {
        heap->gcRequested = 1;
        heap->mustYield = 1;
        mprSignalCond(heap->gcCond);
    }
}


/*
    Trigger a GC collection worthwhile. If MPR_GC_FORCE is set, force the collection regardless. Flags:

    MPR_CG_DEFAULT      Run GC if necessary. Will yield and block for GC
    MPR_GC_FORCE        Force a GC whether it is required or not
    MPR_GC_NO_BLOCK     Run GC if necessary and return without yielding
    MPR_GC_COMPLETE     Force a GC and wait until all threads yield and GC completes including sweeper
 */
PUBLIC int mprGC(int flags)
{
    MprThreadService    *ts;

    ts = MPR->threadService;
    heap->freedBlocks = 0;
    if ((flags & (MPR_GC_FORCE | MPR_GC_COMPLETE)) || (heap->workDone > heap->workQuota)) {
        assert(!heap->marking);
        lock(ts->threads);
        triggerGC();
        unlock(ts->threads);
    }
    if (!(flags & MPR_GC_NO_BLOCK)) {
        mprYield((flags & MPR_GC_COMPLETE) ? MPR_YIELD_COMPLETE : 0);
    }
    return MPR->heap->freedBlocks;
}


/*
    Called by user code to signify the thread is ready for GC and all object references are saved.  Flags:

    MPR_YIELD_DEFAULT   If GC is required, yield and wait for mark phase to coplete, otherwise return without blocking.
    MPR_YIELD_COMPLETE  Yield and wait until the GC entirely completes including sweeper.
    MPR_YIELD_STICKY    Yield and remain yielded until reset. Does not block.

    A yielding thread may block for up to MPR_TIMEOUT_GC_SYNC (1/10th sec) for other threads to also yield. If one more
    more threads do not yield, the marker will resume all yielded threads. If all threads yield, they will wait until
    the mark phase has completed and then be resumed by the marker.
 */
PUBLIC void mprYield(int flags)
{
    MprThreadService    *ts;
    MprThread           *tp;

    ts = MPR->threadService;
    if ((tp = mprGetCurrentThread()) == 0) {
        mprLog("error mpr memory", 0, "Yield called from an unknown thread");
        return;
    }
    assert(!tp->waiting);
    assert(!tp->yielded);
    assert(!tp->stickyYield);

    if (flags & MPR_YIELD_STICKY) {
        tp->stickyYield = 1;
        tp->yielded = 1;
    }
    /*
        Double test to be lock free for the common case
        - but mustYield may not be set and gcRequested is
        - must handle waitForSweeper
     */
    if (heap->mustYield && heap->sweeper) {
        lock(ts->threads);
        tp->waitForSweeper = (flags & MPR_YIELD_COMPLETE);
        while (heap->mustYield && !pauseGC) {
            tp->yielded = 1;
            tp->waiting = 1;
            unlock(ts->threads);

            mprSignalCond(ts->pauseThreads);
            if (tp->stickyYield) {
                tp->waiting = 0;
                return;
            }
            mprWaitForCond(tp->cond, -1);
            lock(ts->threads);
            tp->waiting = 0;
            if (tp->yielded && !tp->stickyYield) {
                /*
                    WARNING: this wait above may return without tp->yielded having been cleared. 
                    This can happen because the cond may have already been triggered by a 
                    previous sticky yield. i.e. it did not wait.
                 */
                tp->yielded = 0;
            }
        }
        unlock(ts->threads);
    }
    if (!tp->stickyYield) {
        assert(!tp->yielded);
        assert(!heap->marking);
    }
}


#ifndef mprNeedYield
PUBLIC bool mprNeedYield()
{
    return heap->mustYield && !pauseGC;
}
#endif


PUBLIC void mprResetYield()
{
    MprThreadService    *ts;
    MprThread           *tp;

    ts = MPR->threadService;
    if ((tp = mprGetCurrentThread()) == 0) {
        mprLog("error mpr memory", 0, "Yield called from an unknown thread");
        return;
    }
    assert(tp->stickyYield);
    if (tp->stickyYield) {
        /*
            Marking could have started again while sticky yielded. So must yield here regardless.
         */
        lock(ts->threads);
        tp->stickyYield = 0;
        if (heap->marking && !pauseGC) {
            tp->yielded = 0;
            unlock(ts->threads);
            mprYield(0);
            assert(!tp->yielded);
        } else {
            tp->yielded = 0;
            unlock(ts->threads);
        }
    }
    assert(!tp->yielded);
}


/*
    Pause until all threads have yielded. Called by the GC marker only.
 */
static int pauseThreads()
{
    MprThreadService    *ts;
    MprThread           *tp;
    MprTicks            start;
    int                 i, allYielded, timeout;

    /*
        Short timeout wait for all threads to yield. Typically set to 1/10 sec
     */
    heap->mustYield = 1;
    timeout = MPR_TIMEOUT_GC_SYNC;
    ts = MPR->threadService;

    start = mprGetTicks();
    if (mprGetDebugMode()) {
        timeout = timeout * 500;
    }
    do {
        lock(ts->threads);
        if (pauseGC) {
            allYielded = 0;
        } else {
            allYielded = 1;
            for (i = 0; i < ts->threads->length; i++) {
                tp = (MprThread*) mprGetItem(ts->threads, i);
                if (!tp->yielded) {
                    allYielded = 0;
                    break;
                }
            }
        }
        if (allYielded) {
            heap->marking = 1;
            unlock(ts->threads);
            break;
        } else if (pauseGC) {
            unlock(ts->threads);
            break;
        }
        unlock(ts->threads);
        if (mprGetState() >= MPR_DESTROYING) {
            /* Do not wait for paused threads if shutting down */
            break;
        }
        mprWaitForCond(ts->pauseThreads, 20);

    } while (mprGetElapsedTicks(start) < timeout);

    return (allYielded) ? 1 : 0;
}


static void resumeThreads(int flags)
{
    MprThreadService    *ts;
    MprThread           *tp;
    int                 i;

    ts = MPR->threadService;
    lock(ts->threads);
    heap->mustYield = 0;
    for (i = 0; i < ts->threads->length; i++) {
        tp = (MprThread*) mprGetItem(ts->threads, i);
        if (tp && tp->yielded) {
            if (flags == WAITING_THREADS && !tp->waitForSweeper) {
                continue;
            }
            if (flags == YIELDED_THREADS && tp->waitForSweeper) {
                continue;
            }
            if (!tp->stickyYield) {
                tp->yielded = 0;
            }
            tp->waitForSweeper = 0;
            if (tp->waiting) {
                assert(tp->stickyYield || !tp->yielded);
                mprSignalCond(tp->cond);
            }
        }
    }
    unlock(ts->threads);
}


/*
    Garbage collector sweeper main thread
 */
static void sweeperThread(void *unused, MprThread *tp)
{
    tp->stickyYield = 1;
    tp->yielded = 1;

    while (!mprIsDestroyed()) {
        if (!heap->mustYield) {
            heap->gcRequested = 0;
            mprWaitForCond(heap->gcCond, -1);
        }
        if (pauseGC || mprIsDestroyed()) {
            heap->mustYield = 0;
            continue;
        }
        markAndSweep();
    }
    invokeDestructors();
    resumeThreads(YIELDED_THREADS | WAITING_THREADS);
    heap->sweeper = 0;
}


/*
    The mark phase will run with all user threads yielded. The sweep phase then runs in parallel.
    The mark phase is relatively quick.
 */
static void markAndSweep()
{
    static int warnOnce = 0;

    if (!pauseThreads()) {
        if (!pauseGC && warnOnce == 0 && !mprGetDebugMode()) {
            warnOnce++;
            mprLog("error mpr memory", 5, "GC synchronization timed out, some threads did not yield.");
            mprLog("error mpr memory", 5, "This can be caused by a thread doing a long running operation and not first calling mprYield.");
            mprLog("error mpr memory", 5, "If debugging, run the process with -D to enable debug mode.");
        }
        resumeThreads(YIELDED_THREADS | WAITING_THREADS);
        return;
    }
    assert(!pauseGC);
    INC(collections);
    heap->priorWorkDone = heap->workDone;
    heap->workDone = 0;
#if ME_MPR_ALLOC_STATS
    heap->priorFree = heap->stats.bytesFree;
#endif
    /*
        Toggle the mark each collection
     */
    heap->mark = !heap->mark;

    /*
        Mark all roots. All user threads are paused here
     */
    markRoots();

    heap->sweeping = 1;
    mprAtomicBarrier();
    heap->marking = 0;
    assert(!pauseGC);

#if ME_MPR_ALLOC_PARALLEL
    /* This is the default to run the sweeper in parallel with user threads */
    resumeThreads(YIELDED_THREADS);
#endif
    /*
        Sweep unused memory with user threads resumed
     */
    sweep();
    heap->sweeping = 0;

#if ME_MPR_ALLOC_PARALLEL
    /* Now resume threads who are waiting for the sweeper to complete */
    resumeThreads(WAITING_THREADS);
#else
    resumeThreads(YIELDED_THREADS | WAITING_THREADS);
#endif
}


static void markRoots()
{
    void    *root;
    int     next;

#if ME_MPR_ALLOC_STATS
    heap->stats.markVisited = 0;
    heap->stats.marked = 0;
#endif
    mprMark(heap->roots);
    mprMark(heap->gcCond);

    for (ITERATE_ITEMS(heap->roots, root, next)) {
        mprMark(root);
    }
}


static void invokeDestructors()
{
    MprRegion   *region;
    MprMem      *mp;
    MprManager  mgr;

    for (region = heap->regions; region; region = region->next) {
        for (mp = region->start; mp < region->end; mp = GET_NEXT(mp)) {
            /*
                OPT - could optimize by requiring a separate flag for managers that implement destructors.
             */
            if (mp->mark != heap->mark && !mp->free && mp->hasManager && !mp->eternal) {
                mgr = GET_MANAGER(mp);
                if (mgr) {
                    (mgr)(GET_PTR(mp), MPR_MANAGE_FREE);
                    /* Retest incase the manager routine revied the object */
                    if (mp->mark != heap->mark) {
                        mp->hasManager = 0;
                    }
                }
            }
        }
    }
}


static void invokeAllDestructors()
{
#if FUTURE
    MprRegion   *region;
    MprMem      *mp;
    MprManager  mgr;

    if (MPR->flags & MPR_NOT_ALL) {
        return;
    }
    for (region = heap->regions; region; region = region->next) {
        for (mp = region->start; mp < region->end; mp = GET_NEXT(mp)) {
            if (!mp->free && mp->hasManager) {
                mgr = GET_MANAGER(mp);
                if (mgr) {
                    (mgr)(GET_PTR(mp), MPR_MANAGE_FREE);
                    /* Retest incase the manager routine revied the object */
                    if (mp->mark != heap->mark) {
                        mp->hasManager = 0;
                    }
                }
            }
        }
    }
#endif
}


/*
    Claim a block from its freeq for the sweeper. This removes the block from the freeq and clears the "free" bit.
 */
static ME_INLINE bool claim(MprMem *mp)
{
    MprFreeQueue    *freeq;
    int             qindex;

    if ((qindex = mp->qindex) == 0) {
        /* allocator won the race */
        return 0;
    }
    freeq = &heap->freeq[qindex];
    ATOMIC_INC(trys);
    if (!acquire(freeq)) {
        ATOMIC_INC(tryFails);
        return 0;
    }
    if (mp->qindex != qindex) {
        /* No on this queue. Allocator must have claimed this block */
        release(freeq);
        return 0;
    }
    unlinkBlock(mp);
    assert(mp->free);
    mp->free = 0;
    release(freeq);
    return 1;
}


/*
    Sweep up the garbage. The sweeper runs in parallel with the program. Dead blocks will have (MprMem.mark != heap->mark). 
*/
static void sweep()
{
    MprRegion   *region, *nextRegion, *prior, *rp;
    MprMem      *mp, *next;
    int         joinBlocks, rcount;

    if (!heap->gcEnabled) {
        return;
    }
#if ME_DEBUG || ME_MPR_ALLOC_STATS
    heap->stats.swept = 0;
    heap->stats.sweptBytes = 0;
#endif
#if ME_MPR_ALLOC_STATS
    heap->stats.sweepVisited = 0;
    heap->stats.freed = 0;
#endif
    /*
        First run managers so that dependant memory blocks will still exist when the manager executes.
        Actually free the memory in a 2nd pass below. 
     */
    invokeDestructors();

    /*
        RACE: Racing with growHeap. This traverses the region list lock-free. growHeap() will insert new regions to 
        the front of heap->regions. This code is the only code that frees regions.
     */
    prior = NULL;
    rcount = 0;
    for (region = heap->regions; region; region = nextRegion) {
        nextRegion = region->next;
        joinBlocks = heap->stats.bytesFree >= heap->stats.cacheHeap;

        for (mp = region->start; mp < region->end; mp = next) {
            assert(mp->size > 0);
            next = GET_NEXT(mp);
            assert(next != mp);
            CHECK(mp);
            INC(sweepVisited);

            if (mp->eternal) {
                assert(!region->freeable);
                continue;
            } 
            if (mp->free && joinBlocks) {
                /*
                    Coalesce already free blocks if the next is also free
                    This may be needed because the code below only coalesces forward.
                 */
                if (next < region->end && !next->free && next->mark != heap->mark && claim(mp)) {
                    mp->mark = !heap->mark;
                    INC(compacted);
                }
            }
            if (!mp->free && mp->mark != heap->mark) {
                freeLocation(mp);
                if (joinBlocks) {
                    /*
                        Try to join this block with successors
                     */
                    while (next < region->end && !next->eternal) {
                        if (next->free) {
                            /*
                                Block is free and on a freeq - must claim
                             */
                            if (!claim(next)) {
                                break;
                            }
                            mp->size += next->size;
                            freeLocation(next);
                            assert(!next->free);
                            SCRIBBLE_RANGE(next, MPR_ALLOC_MIN_BLOCK);
                            INC(joins);

                        } else if (next->mark != heap->mark) {
                            /*
                                Block is now free and NOT on a freeq - no need to claim
                             */
                            assert(!next->free);
                            assert(next->qindex == 0);
                            mp->size += next->size;
                            freeLocation(next);
                            SCRIBBLE_RANGE(next, MPR_ALLOC_MIN_BLOCK);
                            INC(joins);

                        } else {
                            break;
                        }
                        next = GET_NEXT(mp);
                    }
                }
                freeBlock(mp);
            }
        }
        if (region->freeable) {
            if (prior) {
                prior->next = nextRegion;
            } else {
                if (!mprAtomicCas((void**) &heap->regions, region, nextRegion)) {
                    prior = 0;
                    for (rp = heap->regions; rp != region; prior = rp, rp = rp->next) { }
                    assert(prior);
                    if (prior) {
                        prior->next = nextRegion;
                    }
                }
            }
            ATOMIC_ADD(bytesAllocated, - (int64) region->size);
            mprVirtFree(region, region->size);
            INC(unpins);
        } else {
            prior = region;
            rcount++;
        }
    }
    heap->stats.heapRegions = rcount;
    heap->stats.sweeps++;
#if (ME_MPR_ALLOC_STATS && ME_MPR_ALLOC_DEBUG) && KEEP
    printf("GC: Marked %lld / %lld, Swept %lld / %lld, freed %lld, bytesFree %lld (prior %lld)\n"
                 "    WeightedCount %d / %d, allocated blocks %lld allocated bytes %lld\n"
                 "    Unpins %lld, Collections %lld\n",
        heap->stats.marked, heap->stats.markVisited, heap->stats.swept, heap->stats.sweepVisited, 
        heap->stats.freed, heap->stats.bytesFree, heap->priorFree, heap->priorWorkDone, heap->workQuota,
        heap->stats.sweepVisited - heap->stats.swept, heap->stats.bytesAllocated, heap->stats.unpins, 
        heap->stats.collections);
#endif
#if KEEP
    printf("SWEPT blocks %lld bytes %lld, workDone %d\n", heap->stats.swept, heap->stats.sweptBytes, heap->priorWorkDone);
#endif
    if (heap->printStats) {
        printMemReport();
        heap->printStats = 0;
    }
}


/*
    Permanent allocation. Immune to garbage collector.
 */
void *palloc(size_t size)
{
    void    *ptr;

    if ((ptr = mprAllocZeroed(size)) != 0) {
        mprHold(ptr);
    }
    return ptr;
}


/*
    Normal free. Note: this must not be called with a block allocated via "malloc".
    No harm in calling this on a block allocated with mprAlloc and not "palloc".
 */
PUBLIC void pfree(void *ptr)
{
    if (ptr) {
        mprRelease(ptr);

    }
}


PUBLIC void *prealloc(void *ptr, size_t size)
{
    if (ptr) {
        mprRelease(ptr);
    }
    if ((ptr =  mprRealloc(ptr, size)) != 0) {
        mprHold(ptr);
    }
    return ptr;
}


PUBLIC size_t psize(void *ptr)
{
    return mprGetBlockSize(ptr);
}


/* 
    WARNING: this does not mark component members. If that is required, use mprAddRoot.
 */
PUBLIC void mprHold(cvoid *ptr)
{
    MprMem  *mp;

    if (ptr) {
        mp = GET_MEM(ptr);
        if (!mp->free && VALID_BLK(mp)) {
            mp->eternal = 1;
        }
    }
}


PUBLIC void mprRelease(cvoid *ptr)
{
    MprMem  *mp;

    if (ptr) {
        mp = GET_MEM(ptr);
        if (!mp->free && VALID_BLK(mp)) {
            mp->eternal = 0;
        }
    }
}


/* 
    WARNING: this does not mark component members. If that is required, use mprAddRoot.
 */
PUBLIC void mprHoldBlocks(cvoid *ptr, ...)
{
    va_list args;

    if (ptr) {
        mprHold(ptr);
        va_start(args, ptr);
        while ((ptr = va_arg(args, char*)) != 0) {
            mprHold(ptr);
        }
        va_end(args);
    }
}


PUBLIC void mprReleaseBlocks(cvoid *ptr, ...)
{
    va_list args;

    if (ptr) {
        mprRelease(ptr);
        va_start(args, ptr);
        while ((ptr = va_arg(args, char*)) != 0) {
            mprRelease(ptr);
        }
        va_end(args);
    }
}


typedef struct OutSideEvent {
    void    (*proc)(void *data);
    void    *data;
    MprCond *cond;
} OutsideEvent;


static void relayInside(void *data, struct MprEvent *event)
{
    OutsideEvent    *op;

    op = data;
    mprResumeGC();

    /*
        GC is now enabled, but shutdown is paused because this thread means !idle
        However, normal graceful shutdown timeouts apply and this is now just an ordinary event.
        So there are races with the graceful MPR->exitTimeout. It is the users responsibility to
        synchronize shutodown and outside events.
     */
    (op->proc)(op->data);
    if (op->cond) {
        mprSignalCond(op->cond);
    }
}


/*
    This routine creates an event and is safe to call from outside MPR in a foreign thread. Notes:
    1. Safe to use at any point before, before or during a GC or shutdown 
    2. If using MPR_EVENT_BLOCK, will not shutdown until the event callback completes. The API will return after the
        users callback returns.
    3. In the non-blocking case, the event may run before the function returns
    4. The function always returns a valid status indicating whether the event could be scheduled.

    Issues for caller
        - Dispatcher must be NULL or held incase it is destroyed just prior to calling mprCreateEventOutside
        - Caller is responsible for races with shutdown. If shutdown is started, an immediate shutdown or graceful
            shutdown with an expiring exit timeout cannot be stopped.
 */
PUBLIC int mprCreateEventOutside(MprDispatcher *dispatcher, cchar *name, void *proc, void *data, int flags)
{
    OutsideEvent    *op;

    /* 
        Atomic pause GC and shutdown. Must do this to allocate memory from outside.
        This call will return false if the MPR is shutting down. Once paused, shutdown will be paused.
     */
    if (!mprPauseGC()) {
        return MPR_ERR_BAD_STATE;
    }
    /*
        The MPR is prevented from stopping now and a new GC sweep wont start, but we need to wait for a running GC to finish.
     */
    while (heap->mustYield || heap->marking) {
        mprNap(0);
        mprAtomicBarrier();
    }
    if ((op = mprAlloc(sizeof(OutsideEvent))) == 0) {
        return MPR_ERR_MEMORY;
    }
    op->proc = proc;
    op->data = data;

    if (flags & MPR_EVENT_BLOCK) {
        op->cond = mprCreateCond();
        mprHold(op->cond);
    }
    mprCreateEvent(dispatcher, name, 0, relayInside, op, flags);

    if (flags & MPR_EVENT_BLOCK) {
        mprWaitForCond(op->cond, -1);
        mprRelease(op->cond);
    } else {
        mprResumeGC();
        /* Shutdown could happen before the event runs */ 
    }
    return 0;
}


PUBLIC bool mprGCPaused()
{
    return pauseGC;
}


PUBLIC bool mprPauseGC()
{
    mprAtomicAdd((int*) &pauseGC, 1);
    if (mprIsStopping()) {
        mprAtomicAdd((int*) &pauseGC, -1);
        return 0;
    }
    return 1;
}


PUBLIC void mprResumeGC() {
    assert(pauseGC > 0);
    mprAtomicAdd((int*) &pauseGC, -1);
    assert(pauseGC >= 0);
}


PUBLIC bool mprEnableGC(bool on)
{
    bool    old;

    old = heap->gcEnabled;
    heap->gcEnabled = on;
    return old;
}


PUBLIC void mprAddRoot(cvoid *root)
{
    mprAddItem(heap->roots, root);
}


PUBLIC void mprRemoveRoot(cvoid *root)
{
    mprRemoveItem(heap->roots, root);
}


/****************************************************** Debug *************************************************************/

#if ME_MPR_ALLOC_STATS
static void printQueueStats() 
{
    MprFreeQueue    *freeq;
    double          mb;
    int             i;

    mb = 1024.0 * 1024;
    /*
        Note the total size is a minimum as blocks may be larger than minSize
     */
    printf("\nFree Queue Stats\n  Queue           Usize         Count          Total\n");
    for (i = 0, freeq = heap->freeq; freeq < &heap->freeq[MPR_ALLOC_NUM_QUEUES]; freeq++, i++) {
        if (freeq->count) {
            printf("%7d %14d %14d %14d\n", i, freeq->minSize - (int) sizeof(MprMem), freeq->count, 
                freeq->minSize * freeq->count);
        }
    }
    printf("\n");
    printf("Heap-used    %8.1f MB\n", (heap->stats.bytesAllocated - heap->stats.bytesFree) / mb);
}


#if ME_MPR_ALLOC_DEBUG
static MprLocationStats sortLocations[MPR_TRACK_HASH];

static int sortLocation(cvoid *l1, cvoid *l2)
{
    MprLocationStats    *lp1, *lp2;

    lp1 = (MprLocationStats*) l1;
    lp2 = (MprLocationStats*) l2;
    if (lp1->total < lp2->total) {
        return -1;
    } else if (lp1->total == lp2->total) {
        return 0;
    }
    return 1;
}


static void printTracking() 
{
    MprLocationStats     *lp;
    double              mb;
    size_t              total;
    cchar                **np;

    printf("\nAllocation Stats\n     Size Location\n");
    memcpy(sortLocations, heap->stats.locations, sizeof(sortLocations));
    qsort(sortLocations, MPR_TRACK_HASH, sizeof(MprLocationStats), sortLocation);

    total = 0;
    for (lp = sortLocations; lp < &sortLocations[MPR_TRACK_HASH]; lp++) {
        if (lp->total) {
            for (np = &lp->names[0]; *np && np < &lp->names[MPR_TRACK_NAMES]; np++) {
                if (*np) {
                    if (np == lp->names) {
                        printf("%10d %-24s %d\n", (int) lp->total, *np, lp->count);
                    } else {
                        printf("           %-24s\n", *np);
                    }
                }
            }
            total += lp->total;
        }
    }
    mb = 1024.0 * 1024;
    printf("Total:    %8.1f MB\n", total / (1024.0 * 1024));
    printf("Heap-used %8.1f MB\n", (MPR->heap->stats.bytesAllocated - MPR->heap->stats.bytesFree) / mb);
}
#endif /* ME_MPR_ALLOC_DEBUG */


static void printGCStats()
{
    MprRegion   *region;
    MprMem      *mp;
    uint64      freeBytes, activeBytes, eternalBytes, regionBytes, available;
    double      mb;
    char        *tag;
    int         regions, freeCount, activeCount, eternalCount, regionCount;

    mb = 1024.0 * 1024;
    printf("\nRegion Stats:\n");
    regions = 0;
    activeBytes = eternalBytes = freeBytes = 0;
    activeCount = eternalCount = freeCount = 0;

    for (region = heap->regions; region; region = region->next, regions++) {
        regionCount = 0;
        regionBytes = 0;

        for (mp = region->start; mp < region->end; mp = GET_NEXT(mp)) {
            assert(mp->size > 0);
            if (mp->free) {
                freeBytes += mp->size;
                freeCount++;

            } else if (mp->eternal) {
                eternalBytes += mp->size;
                eternalCount++;
                regionCount++;
                regionBytes += mp->size;

            } else {
                activeBytes += mp->size;
                activeCount++;
                regionCount++;
                regionBytes += mp->size;
            }
        }
        available = region->size - regionBytes - MPR_ALLOC_ALIGN(sizeof(MprRegion));
        if (available == 0) {
            tag = "(fully used)";
        } else if (regionBytes == 0) {
            tag = "(empty)";
        } else {
            tag = "";
        }
        printf("  Region %2d size %d, allocated %4d blocks, %7d bytes free %s\n", regions, (int) region->size, 
            regionCount, (int) available, tag);
    }
    printf("\nGC Stats:\n");
    printf("  Active:  %8d blocks, %6.1f MB\n", activeCount, activeBytes / mb);
    printf("  Eternal: %8d blocks, %6.1f MB\n", eternalCount, eternalBytes / mb);
    printf("  Free:    %8d blocks, %6.1f MB\n", freeCount, freeBytes / mb);
}
#endif /* ME_MPR_ALLOC_STATS */


PUBLIC void mprPrintMem(cchar *msg, int flags)
{
    printf("%s:\n\n", msg);
    heap->printStats = (flags & MPR_MEM_DETAIL) ? 2 : 1;
    mprGC(MPR_GC_FORCE | MPR_GC_COMPLETE);
}


static void printMemReport()
{
    MprMemStats     *ap;
    double          mb;

    ap = mprGetMemStats();
    mb = 1024.0 * 1024;

    printf("Memory Stats:\n");
    printf("  Memory          %12.1f MB\n", mprGetMem() / mb);
    printf("  Heap            %12.1f MB\n", ap->bytesAllocated / mb);
    printf("  Heap-peak       %12.1f MB\n", ap->bytesAllocatedPeak / mb);
    printf("  Heap-used       %12.1f MB\n", (ap->bytesAllocated - ap->bytesFree) / mb);
    printf("  Heap-free       %12.1f MB\n", ap->bytesFree / mb);
    printf("  Heap cache      %12.1f MB (%.2f %%)\n", ap->cacheHeap / mb, ap->cacheHeap * 100.0 / ap->maxHeap);

    if (ap->maxHeap == (size_t) -1) {
        printf("  Heap limit         unlimited\n");
        printf("  Heap readline      unlimited\n");
    } else {
        printf("  Heap limit      %12.1f MB\n", ap->maxHeap / mb);
        printf("  Heap redline    %12.1f MB\n", ap->warnHeap / mb);
    }
    printf("  Errors          %12d\n", (int) ap->errors);
    printf("  CPU cores       %12d\n", (int) ap->cpuCores);
    printf("\n");

#if ME_MPR_ALLOC_STATS
    printf("Allocator Stats:\n");
    printf("  Memory requests %12d\n",                (int) ap->requests);
    printf("  Region allocs   %12.2f %% (%d)\n",      ap->allocs * 100.0 / ap->requests, (int) ap->allocs);
    printf("  Region unpins   %12.2f %% (%d)\n",      ap->unpins * 100.0 / ap->requests, (int) ap->unpins);
    printf("  Reuse           %12.2f %%\n",           ap->reuse * 100.0 / ap->requests);
    printf("  Joins           %12.2f %% (%d)\n",      ap->joins * 100.0 / ap->requests, (int) ap->joins);
    printf("  Splits          %12.2f %% (%d)\n",      ap->splits * 100.0 / ap->requests, (int) ap->splits);
    printf("  Q races         %12.2f %% (%d)\n",      ap->qrace * 100.0 / ap->requests, (int) ap->qrace);
    printf("  Q contention    %12.2f %% (%d / %d)\n", ap->tryFails * 100.0 / ap->trys, (int) ap->tryFails, (int) ap->trys);
    printf("  Alloc retries   %12.2f %% (%d / %d)\n", ap->retries * 100.0 / ap->requests, (int) ap->retries, (int) ap->requests);
    printf("  GC collections  %12.2f %% (%d)\n",      ap->collections * 100.0 / ap->requests, (int) ap->collections);
    printf("  Compact next    %12.2f %% (%d)\n",      ap->compacted * 100.0 / ap->requests, (int) ap->compacted);
    printf("  MprMem size     %12d\n",                (int) sizeof(MprMem));
    printf("  MprFreeMem size %12d\n",                (int) sizeof(MprFreeMem));

    printGCStats();
    if (heap->printStats > 1) {
        printQueueStats();
#if ME_MPR_ALLOC_DEBUG
        if (heap->track) {
            printTracking();
        }
#endif
    }
#endif /* ME_MPR_ALLOC_STATS */
}


#if ME_MPR_ALLOC_DEBUG
static int validBlk(MprMem *mp)
{
    assert(mp->magic == MPR_ALLOC_MAGIC);
    assert(mp->size > 0);
    return (mp->magic == MPR_ALLOC_MAGIC) && (mp->size > 0);
}


PUBLIC void mprCheckBlock(MprMem *mp)
{
    BREAKPOINT(mp);
    if (mp->magic != MPR_ALLOC_MAGIC || mp->size == 0) {
        mprLog("critical mpr memory", 0, "Memory corruption in memory block %x (MprBlk %x, seqno %d). " \
            "This most likely happend earlier in the program execution.", GET_PTR(mp), mp, mp->seqno);
    }
}


static void breakpoint(MprMem *mp) 
{
    if (mp == stopAlloc || mp->seqno == stopSeqno) {
        mprBreakpoint();
    }
}


#if ME_MPR_ALLOC_DEBUG
/*
    Called to set the memory block name when doing an allocation
 */
PUBLIC void *mprSetAllocName(void *ptr, cchar *name)
{
    MprMem  *mp;

    assert(name && *name);

    mp = GET_MEM(ptr);
    mp->name = name;

    if (heap->track) {
        MprLocationStats    *lp;
        cchar               **np, *n;
        int                 index;

        if (name == 0) {
            name = "";
        }
        index = shash(name, strlen(name)) % MPR_TRACK_HASH;
        lp = &heap->stats.locations[index];
        for (np = lp->names; np <= &lp->names[MPR_TRACK_NAMES]; np++) {
            n = *np;
            if (n == 0 || n == name || strcmp(n, name) == 0) {
                break;
            }
            /* Collision */
        }
        if (np < &lp->names[MPR_TRACK_NAMES]) {
            *np = (char*) name;
        }
        mprAtomicAdd64((int64*) &lp->total, mp->size);
        mprAtomicAdd(&lp->count, 1);
    }
    return ptr;
}


static void freeLocation(MprMem *mp)
{
    MprLocationStats    *lp;
    cchar               *name;
    int                 index;

    if (!heap->track) {
        return;
    }
    name = mp->name;
    if (name == 0) {
        return;
    }
    index = shash(name, strlen(name)) % MPR_TRACK_HASH;
    lp = &heap->stats.locations[index];
    mprAtomicAdd(&lp->count, -1);
    if (lp->total >= mp->size) {
        mprAtomicAdd64((int64*) &lp->total, - (int64) mp->size);
    } else {
        lp->total = 0;
    }
    SET_NAME(mp, NULL);
}
#endif


PUBLIC void *mprSetName(void *ptr, cchar *name) 
{
    MprMem  *mp;

    assert(name && *name);
    mp = GET_MEM(ptr);
    if (mp->name) {
        freeLocation(mp);
    }
    mprSetAllocName(ptr, name);
    return ptr;
}


PUBLIC void *mprCopyName(void *dest, void *src) 
{
    return mprSetName(dest, mprGetName(src));
}
#endif

/********************************************* Misc ***************************************************/

static void printMemWarn(size_t used, bool critical)
{
    static int once = 0;

    if (once++ == 0 || critical) {
        mprLog("warn mpr memory", 0, "Memory used %'d, redline %'d, limit %'d.", (int) used, (int) heap->stats.warnHeap,
            (int) heap->stats.maxHeap);
    }
}


static void allocException(int cause, size_t size)
{
    size_t      used;
    static int  once = 0;

    INC(errors);
    if (heap->stats.inMemException || mprIsStopping()) {
        return;
    }
    heap->stats.inMemException = 1;
    used = mprGetMem();

    if (cause == MPR_MEM_FAIL) {
        heap->hasError = 1;
        mprLog("error mpr memory", 0, "Cannot allocate memory block of size %'zd bytes.", size);
        printMemWarn(used, 1);

    } else if (cause == MPR_MEM_TOO_BIG) {
        heap->hasError = 1;
        mprLog("error mpr memory", 0, "Cannot allocate memory block of size %'zd bytes.", size);
        printMemWarn(used, 1);

    } else if (cause == MPR_MEM_WARNING) {
        if (once++ == 0) {
            mprLog("error mpr memory", 0, "Memory request for %'zd bytes exceeds memory red-line.", size);
        }
        mprPruneCache(NULL);
        printMemWarn(used, 0);

    } else if (cause == MPR_MEM_LIMIT) {
        mprLog("error mpr memory", 0, "Memory request for %'zd bytes exceeds memory limit.", size);
        printMemWarn(used, 1);
    }

    if (heap->notifier) {
        (heap->notifier)(cause, heap->allocPolicy,  size, used);
    }
    if (cause & (MPR_MEM_TOO_BIG | MPR_MEM_FAIL)) {
        /*
            Allocation failed
         */
        mprLog("critical mpr memory", 0, "Application exiting immediately due to memory depletion.");
        mprShutdown(MPR_EXIT_ABORT, -1, 0);

    } else if (cause & MPR_MEM_LIMIT) {
        /*
            Over memory max limit
         */
        if (heap->allocPolicy == MPR_ALLOC_POLICY_RESTART) {
            mprLog("critical mpr memory", 0, "Application restarting due to low memory condition.");
            mprShutdown(MPR_EXIT_RESTART, -1, 0);

        } else if (heap->allocPolicy == MPR_ALLOC_POLICY_EXIT) {
            mprLog("critical mpr memory", 0, "Application exiting due to memory depletion.");
            mprShutdown(MPR_EXIT_NORMAL, -1, MPR_EXIT_TIMEOUT);
        }
    }
    heap->stats.inMemException = 0;
}


static void getSystemInfo()
{
    memStats.cpuCores = 1;

#if MACOSX
    #ifdef _SC_NPROCESSORS_ONLN
        memStats.cpuCores = (uint) sysconf(_SC_NPROCESSORS_ONLN);
    #else
        memStats.cpuCores = 1;
    #endif
    memStats.pageSize = (uint) sysconf(_SC_PAGESIZE);
#elif SOLARIS
{
    FILE *ptr;
    if  ((ptr = popen("psrinfo -p", "r")) != NULL) {
        fscanf(ptr, "%d", &alloc.cpuCores);
        (void) pclose(ptr);
    }
    alloc.pageSize = sysconf(_SC_PAGESIZE);
}
#elif ME_WIN_LIKE
{
    SYSTEM_INFO     info;

    GetSystemInfo(&info);
    memStats.cpuCores = info.dwNumberOfProcessors;
    memStats.pageSize = info.dwPageSize;

}
#elif ME_BSD_LIKE
    {
        int     cmd[2];
        size_t  len;

        cmd[0] = CTL_HW;
        cmd[1] = HW_NCPU;
        len = sizeof(memStats.cpuCores);
        memStats.cpuCores = 0;
        if (sysctl(cmd, 2, &memStats.cpuCores, &len, 0, 0) < 0) {
            memStats.cpuCores = 1;
        }
        memStats.pageSize = sysconf(_SC_PAGESIZE);
    }
#elif LINUX
    {
        static const char processor[] = "processor\t:";
        char    c;
        int     fd, col, match;

        fd = open("/proc/cpuinfo", O_RDONLY);
        if (fd < 0) {
            return;
        }
        match = 1;
        memStats.cpuCores = 0;
        for (col = 0; read(fd, &c, 1) == 1; ) {
            if (c == '\n') {
                col = 0;
                match = 1;
            } else {
                if (match && col < (sizeof(processor) - 1)) {
                    if (c != processor[col]) {
                        match = 0;
                    }
                    col++;

                } else if (match) {
                    memStats.cpuCores++;
                    match = 0;
                }
            }
        }
        if (memStats.cpuCores <= 0) {
            memStats.cpuCores = 1;
        }
        close(fd);
        memStats.pageSize = sysconf(_SC_PAGESIZE);
    }
#else
        memStats.pageSize = 4096;
#endif
    if (memStats.pageSize <= 0 || memStats.pageSize >= (16 * 1024)) {
        memStats.pageSize = 4096;
    }
}


#if ME_WIN_LIKE
static int winPageModes(int flags)
{
    if (flags & MPR_MAP_EXECUTE) {
        return PAGE_EXECUTE_READWRITE;
    } else if (flags & MPR_MAP_WRITE) {
        return PAGE_READWRITE;
    }
    return PAGE_READONLY;
}
#endif


PUBLIC MprMemStats *mprGetMemStats()
{
#if LINUX
    char            buf[1024], *cp;
    size_t          len;
    int             fd;

    heap->stats.ram = MAXSSIZE;
    if ((fd = open("/proc/meminfo", O_RDONLY)) >= 0) {
        if ((len = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = '\0';
            if ((cp = strstr(buf, "MemTotal:")) != 0) {
                for (; *cp && !isdigit((uchar) *cp); cp++) {}
                heap->stats.ram = ((size_t) atoi(cp) * 1024);
            }
        }
        close(fd);
    }
#endif
#if ME_BSD_LIKE
    size_t      len;
    int         mib[2];
#if FREEBSD
    size_t      ram, usermem;
    mib[1] = HW_MEMSIZE;
#else
    int64 ram, usermem;
    mib[1] = HW_PHYSMEM;
#endif
#if MACOSX
    sysctlbyname("hw.memsize", &ram, &len, NULL, 0);
#else
    mib[0] = CTL_HW;
    len = sizeof(ram);
    ram = 0;
    sysctl(mib, 2, &ram, &len, NULL, 0);
#endif
    heap->stats.ram = ram;

    mib[0] = CTL_HW;
    mib[1] = HW_USERMEM;
    len = sizeof(usermem);
    usermem = 0;
    sysctl(mib, 2, &usermem, &len, NULL, 0);
    heap->stats.user = usermem;
#endif
    heap->stats.rss = mprGetMem();
    heap->stats.cpuUsage = mprGetCPU();
    return &heap->stats;
}


/*
    Return the amount of memory currently in use. This routine may open files and thus is not very quick on some 
    platforms. On FREEBDS it returns the peak resident set size using getrusage. If a suitable O/S API is not available,
    the amount of heap memory allocated by the MPR is returned.
 */
PUBLIC size_t mprGetMem()
{
    size_t      size = 0;

#if LINUX
    static int  procfd = -1;
    char        buf[ME_MAX_BUFFER], *cp;
    int         nbytes;

    if (procfd < 0) {
        procfd = open("/proc/self/statm", O_RDONLY);
    }
    if (procfd >= 0) {
        lseek(procfd, 0, 0);
        if ((nbytes = read(procfd, buf, sizeof(buf) - 1)) > 0) {
            buf[nbytes] = '\0';
            for (cp = buf; *cp && *cp != ' '; cp++) {}
            for (; *cp == ' '; cp++) {}
            size = stoi(cp) * memStats.pageSize;
        }
    }
    if (size == 0) {
        struct rusage rusage;
        getrusage(RUSAGE_SELF, &rusage);
        size = rusage.ru_maxrss * 1024;
    }
#elif MACOSX
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        size = info.resident_size;
    }
#elif ME_BSD_LIKE
    struct rusage   rusage;
    getrusage(RUSAGE_SELF, &rusage);
    size = rusage.ru_maxrss;
#endif
    if (size == 0) {
        size = (size_t) heap->stats.bytesAllocated;
    }
    return size;
}


PUBLIC uint64 mprGetCPU()
{
    uint64     ticks;

    ticks = 0;
#if LINUX
    int fd;
    char path[ME_MAX_PATH];
    sprintf(path, "/proc/%d/stat", getpid());
    if ((fd = open(path, O_RDONLY)) >= 0) {
        char buf[ME_MAX_BUFFER];
        int nbytes = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (nbytes > 0) {
            ulong utime, stime;
            buf[nbytes] = '\0';
            sscanf(buf, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime);
            ticks = (utime + stime) * MPR_TICKS_PER_SEC / sysconf(_SC_CLK_TCK);
        }
    }
#elif MACOSX
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        uint64 utime, stime;
        utime = info.user_time.seconds * MPR_TICKS_PER_SEC + info.user_time.microseconds / 1000;
        stime = info.system_time.seconds * MPR_TICKS_PER_SEC + info.system_time.microseconds / 1000;
        ticks = utime + stime;
    }
#endif
    return ticks;
}


#ifndef findFirstBit
static ME_INLINE int findFirstBit(size_t word)
{
    int     b;
    for (b = 0; word; word >>= 1, b++) {
        if (word & 0x1) {
            b++;
            break;
        }
    }
    return b;
}
#endif


#ifndef findLastBit
static ME_INLINE int findLastBit(size_t word)
{
    int     b;

    for (b = 0; word; word >>= 1, b++) ;
    return b;
}
#endif


/*
    Acquire the freeq. Note: this is only ever used by non-blocking algorithms.
 */
static ME_INLINE bool acquire(MprFreeQueue *freeq)
{
#if MACOSX
    return OSSpinLockTry(&freeq->lock.cs);
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    return pthread_spin_trylock(&freeq->lock.cs) == 0;
#elif ME_UNIX_LIKE
    return pthread_mutex_trylock(&freeq->lock.cs) == 0;
#elif ME_WIN_LIKE
    return TryEnterCriticalSection(&freeq->lock.cs) != 0;
#elif VXWORKS
    return semTake(freeq->lock.cs, NO_WAIT) == OK;
#else
    #error "Operting system not supported in acquire()"
#endif
}


static ME_INLINE void release(MprFreeQueue *freeq)
{
#if MACOSX
    OSSpinLockUnlock(&freeq->lock.cs);
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    pthread_spin_unlock(&freeq->lock.cs);
#elif ME_UNIX_LIKE
    pthread_mutex_unlock(&freeq->lock.cs);
#elif ME_WIN_LIKE
    LeaveCriticalSection(&freeq->lock.cs);
#elif VXWORKS
    semGive(freeq->lock.cs);
#endif
}


static ME_INLINE int cas(size_t *target, size_t expected, size_t value)
{
    return mprAtomicCas((void**) target, (void*) expected, (cvoid*) value);
}


static ME_INLINE void clearbitmap(size_t *bitmap, int index) 
{
    size_t  bit, prior;

    bit = (((size_t) 1) << index);
    do {
        prior = *bitmap;
        if (!(prior & bit)) {
            break;
        }
    } while (!cas(bitmap, prior, prior & ~bit));
}


static ME_INLINE void setbitmap(size_t *bitmap, int index) 
{
    size_t  bit, prior;

    bit = (((size_t) 1) << index);
    do {
        prior = *bitmap;
        if (prior & bit) {
            break;
        }
    } while (!cas(bitmap, prior, prior | bit));
}


#if ME_WIN_LIKE
PUBLIC Mpr *mprGetMpr()
{
    return MPR;
}
#endif


PUBLIC int mprGetPageSize()
{
    return memStats.pageSize;
}


PUBLIC size_t mprGetBlockSize(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    if (ptr == 0 || !VALID_BLK(mp)) {
        return 0;
    }
    CHECK(mp);
    return GET_USIZE(mp);
}


PUBLIC int mprGetHeapFlags()
{
    return heap->flags;
}


PUBLIC void mprSetMemNotifier(MprMemNotifier cback)
{
    heap->notifier = cback;
}


PUBLIC void mprSetMemLimits(ssize warnHeap, ssize maxHeap, ssize cacheHeap)
{
    if (warnHeap > 0) {
        heap->stats.warnHeap = warnHeap;
    }
    if (maxHeap > 0) {
        heap->stats.maxHeap = maxHeap;
    }
    if (cacheHeap >= 0) {
        heap->stats.cacheHeap = cacheHeap;
        heap->stats.lowHeap = cacheHeap ? cacheHeap / 8 : ME_MPR_ALLOC_REGION_SIZE;
    }
}


PUBLIC void mprSetMemPolicy(int policy)
{
    heap->allocPolicy = policy;
}


PUBLIC void mprSetMemError()
{
    heap->hasError = 1;
}


PUBLIC bool mprHasMemError()
{
    return heap->hasError;
}


PUBLIC void mprResetMemError()
{
    heap->hasError = 0;
}


PUBLIC int mprIsValid(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    if (mp->free) {
        return 0;
    }
#if ME_WIN
    if (isBadWritePtr(mp, sizeof(MprMem))) {
        return 0;
    }
    if (!VALID_BLK(GET_MEM(ptr)) {
        return 0;
    }
    if (isBadWritePtr(ptr, mp->size)) {
        return 0;
    }
    return 0;
#else
#if ME_MPR_ALLOC_DEBUG
    return ptr && mp->magic == MPR_ALLOC_MAGIC && mp->size > 0;
#else
    return ptr && mp->size > 0;
#endif
#endif
}


static void dummyManager(void *ptr, int flags) 
{
}


PUBLIC void *mprSetManager(void *ptr, MprManager manager)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    if (mp->hasManager) {
        if (!manager) {
            manager = dummyManager;
        }
        SET_MANAGER(mp, manager);
    }
    return ptr;
}


#if ME_MPR_ALLOC_STACK
static void monitorStack()
{
    MprThread   *tp;
    int         diff;

    if (MPR->threadService && (tp = mprGetCurrentThread()) != 0) {
        if (tp->stackBase == 0) {
            tp->stackBase = &tp;
        }
        diff = (int) ((char*) tp->stackBase - (char*) &diff);
        if (diff < 0) {
            tp->peakStack -= diff;
            tp->stackBase = (void*) &diff;
            diff = 0;
        }
        if (diff > tp->peakStack) {
            tp->peakStack = diff;
        }
    }
}
#endif

#if !ME_MPR_ALLOC_DEBUG
#undef mprSetName
#undef mprCopyName
#undef mprSetAllocName

/*
    Re-instate defines for combo releases, where source will be appended below here
 */
#define mprCopyName(dest, src)
#define mprGetName(ptr) ""
#define mprSetAllocName(ptr, name) ptr
#define mprSetName(ptr, name)
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/mpr.c ************/


/*
    mpr.c - Multithreaded Portable Runtime (MPR). Initialization, start/stop and control of the MPR.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/



/*********************************** Locals ***********************************/
/*
    Define an illegal exit status value
 */
#define NO_STATUS   0x100000

static int mprExitStatus;
static int mprState;

/**************************** Forward Declarations ****************************/

static void manageMpr(Mpr *mpr, int flags);
static void serviceEventsThread(void *data, MprThread *tp);
static void setNames(Mpr *mpr, int argc, char **argv);

/************************************* Code ***********************************/
/*
    Create and initialize the MPR service.
 */
PUBLIC Mpr *mprCreate(int argc, char **argv, int flags)
{
    MprFileSystem   *fs;
    Mpr             *mpr;

    srand((uint) time(NULL));

    if (flags & MPR_DAEMON) {
        mprDaemon();
    }
    mprAtomicOpen();
    if ((mpr = mprCreateMemService((MprManager) manageMpr, flags)) == 0) {
        assert(mpr);
        return 0;
    }
    mpr->flags = flags;
    mpr->start = mprGetTime(); 
    mpr->exitStrategy = 0;
    mpr->emptyString = sclone("");
    mpr->oneString = sclone("1");
    mpr->idleCallback = mprServicesAreIdle;
    mpr->mimeTypes = mprCreateMimeTypes(NULL);
    mpr->terminators = mprCreateList(0, MPR_LIST_STATIC_VALUES);
    mpr->keys = mprCreateHash(0, 0);
    mpr->verifySsl = 1;

    fs = mprCreateFileSystem("/");
    mprAddFileSystem(fs);
    setNames(mpr, argc, argv);

    mprCreateOsService();
    mprCreateTimeService();
    mpr->mutex = mprCreateLock();
    mpr->spin = mprCreateSpinLock();

    mprCreateLogService();
    mprCreateCacheService();

    mpr->signalService = mprCreateSignalService();
    mpr->threadService = mprCreateThreadService();
    mpr->moduleService = mprCreateModuleService();
    mpr->eventService = mprCreateEventService();
    mpr->cmdService = mprCreateCmdService();
    mpr->workerService = mprCreateWorkerService();
    mpr->waitService = mprCreateWaitService();
    mpr->socketService = mprCreateSocketService();
    mpr->pathEnv = sclone(getenv("PATH"));
    mpr->cond = mprCreateCond();
    mpr->stopCond = mprCreateCond();

    mpr->dispatcher = mprCreateDispatcher("main", 0);
    mpr->nonBlock = mprCreateDispatcher("nonblock", 0);
    mprSetDispatcherImmediate(mpr->nonBlock);

    if (flags & MPR_USER_EVENTS_THREAD) {
        if (!(flags & MPR_NO_WINDOW)) {
            /* Used by apps that need to use FindWindow after calling mprCreate() (appwebMonitor) */
            mprSetWindowsThread(0);
        }
    } else {
        mprStartEventsThread();
    }
    if (!(flags & MPR_DELAY_GC_THREAD)) {
        mprStartGCService();
    }
    mprState = MPR_CREATED;
    mprExitStatus = NO_STATUS;

    if (MPR->hasError || mprHasMemError()) {
        return 0;
    }
    return mpr;
}


static void manageMpr(Mpr *mpr, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mpr->logFile);
        mprMark(mpr->mimeTypes);
        mprMark(mpr->timeTokens);
        mprMark(mpr->keys);
        mprMark(mpr->stdError);
        mprMark(mpr->stdInput);
        mprMark(mpr->stdOutput);
        mprMark(mpr->appPath);
        mprMark(mpr->appDir);
        /* 
            Argv will do a single allocation into argv == argBuf. May reallocate the program name in argv[0] 
         */
        mprMark(mpr->argv);
        mprMark(mpr->argv[0]);
        mprMark(mpr->logPath);
        mprMark(mpr->pathEnv);
        mprMark(mpr->name);
        mprMark(mpr->title);
        mprMark(mpr->version);
        mprMark(mpr->domainName);
        mprMark(mpr->hostName);
        mprMark(mpr->ip);
        mprMark(mpr->serverName);
        mprMark(mpr->cmdService);
        mprMark(mpr->eventService);
        mprMark(mpr->fileSystem);
        mprMark(mpr->moduleService);
        mprMark(mpr->osService);
        mprMark(mpr->signalService);
        mprMark(mpr->socketService);
        mprMark(mpr->threadService);
        mprMark(mpr->workerService);
        mprMark(mpr->waitService);
        mprMark(mpr->dispatcher);
        mprMark(mpr->nonBlock);
        mprMark(mpr->appwebService);
        mprMark(mpr->ediService);
        mprMark(mpr->ejsService);
        mprMark(mpr->espService);
        mprMark(mpr->httpService);
        mprMark(mpr->terminators);
        mprMark(mpr->mutex);
        mprMark(mpr->spin);
        mprMark(mpr->cond);
        mprMark(mpr->stopCond);
        mprMark(mpr->emptyString);
        mprMark(mpr->oneString);
    }
}


/*
    The monitor event is invoked from mprShutdown() for graceful shutdowns if the application has requests still running.
    This event monitors the application to see when it becomes is idle.
    WARNING: this races with other threads
 */
static void shutdownMonitor(void *data, MprEvent *event)
{
    MprTicks        remaining;

    if (mprIsIdle(1)) {
        if (mprState <= MPR_STOPPING) {
            mprLog("info mpr", 2, "Shutdown proceeding, system is idle");
            mprState = MPR_STOPPED;
        }
        return;
    } 
    remaining = mprGetRemainingTicks(MPR->shutdownStarted, MPR->exitTimeout);
    if (remaining <= 0) {
        if (MPR->exitStrategy & MPR_EXIT_SAFE && mprCancelShutdown()) {
            mprLog("warn mpr", 2, "Shutdown cancelled due to continuing requests");
        } else {
            mprLog("warn mpr", 2, "Timeout while waiting for requests to complete");
            if (mprState <= MPR_STOPPING) {
                mprState = MPR_STOPPED;
            }
        }
    } else {
        mprLog("info mpr", 2, "Waiting for requests to complete, %lld secs remaining ...", remaining / MPR_TICKS_PER_SEC);
        mprRescheduleEvent(event, 1000);
    }
}


/*
    Start shutdown of the Mpr. This sets the state to stopping and invokes the shutdownMonitor. This is done for
    all shutdown strategies regardless. Immediate shutdowns must still give threads some time to exit.
    This routine does no destructive actions.
    WARNING: this races with other threads.
 */
PUBLIC void mprShutdown(int how, int exitStatus, MprTicks timeout)
{
    MprTerminator   terminator;
    int             next;

    mprGlobalLock();
    if (mprState >= MPR_STOPPING) {
        mprGlobalUnlock();
        return;
    }
    mprState = MPR_STOPPING;
    mprSignalMultiCond(MPR->stopCond);
    mprGlobalUnlock();

    MPR->exitStrategy = how;
    mprExitStatus = exitStatus;
    MPR->exitTimeout = (timeout >= 0) ? timeout : MPR->exitTimeout;
    if (mprGetDebugMode()) {
        MPR->exitTimeout = MPR_MAX_TIMEOUT;
    }
    MPR->shutdownStarted = mprGetTicks();

    if (how & MPR_EXIT_ABORT) {
        if (how & MPR_EXIT_RESTART) {
            mprLog("info mpr", 3, "Abort with restart.");
            mprRestart();
        } else {
            mprLog("info mpr", 3, "Abortive exit.");
            exit(exitStatus);
        }
        /* No continue */
    }
    mprLog("info mpr", 3, "Application exit, waiting for existing requests to complete.");

    if (!mprIsIdle(0)) {
        mprCreateTimerEvent(NULL, "shutdownMonitor", 0, shutdownMonitor, 0, MPR_EVENT_QUICK);
    }
    mprWakeDispatchers();
    mprWakeNotifier();

    /*
        Note: terminators must take not destructive actions for the MPR_STOPPED state
     */
    for (ITERATE_ITEMS(MPR->terminators, terminator, next)) {
        (terminator)(mprState, how, mprExitStatus & ~NO_STATUS);
    }
}


PUBLIC bool mprCancelShutdown()
{
    mprGlobalLock();
    if (mprState == MPR_STOPPING) {
        mprState = MPR_STARTED;
        mprGlobalUnlock();
        return 1;
    }
    mprGlobalUnlock();
    return 0;
}


/*
    Destroy the Mpr and all services
    If the application has a user events thread and mprShutdown was called, then we will come here when already idle.
    This routine will call service terminators to allow them to shutdown their services in an orderly manner 
 */
PUBLIC bool mprDestroy()
{
    MprTerminator   terminator;
    MprTicks        timeout;
    int             i, next;

    if (mprState < MPR_STOPPING) {
        mprShutdown(MPR->exitStrategy, mprExitStatus, MPR->exitTimeout);
    }
    timeout = MPR->exitTimeout;
    if (MPR->shutdownStarted) {
        timeout -= (mprGetTicks() - MPR->shutdownStarted);
    }
    /* 
        Wait for events thread to exit and the app to become idle 
     */
    while (MPR->eventing) {
        mprWakeNotifier();
        mprWaitForCond(MPR->cond, 10);
        if (mprGetRemainingTicks(MPR->shutdownStarted, timeout) <= 0) {
            break;
        }
    }
    if (!mprIsIdle(0) || MPR->eventing) {
        if (MPR->exitStrategy & MPR_EXIT_SAFE) {
            /* Note: Pending outside events will pause GC which will make mprIsIdle return false */
            mprLog("warn mpr", 2, "Cancel termination due to continuing requests, application resumed.");
            mprCancelShutdown();
        } else if (MPR->exitTimeout > 0) {
            /* If a non-zero graceful timeout applies, always exit with non-zero status */
            exit(mprExitStatus != NO_STATUS ? mprExitStatus : 1);
        } else {
            exit(mprExitStatus & ~NO_STATUS);
        }
        return 0;
    }
    mprGlobalLock();
    if (mprState == MPR_STARTED) {
        mprGlobalUnlock();
        /* User cancelled shutdown */
        return 0;
    }
    /* 
        Point of no return 
     */
    mprState = MPR_DESTROYING;
    mprGlobalUnlock();

    for (ITERATE_ITEMS(MPR->terminators, terminator, next)) {
        (terminator)(mprState, MPR->exitStrategy, mprExitStatus & ~NO_STATUS);
    }
    mprStopWorkers();
    mprStopCmdService();
    mprStopModuleService();
    mprStopEventService();
    mprStopThreadService();
    mprStopWaitService();

    /*
        Run GC to finalize all memory until we are not freeing any memory. This IS deterministic.
     */
    for (i = 0; i < 25; i++) {
        if (mprGC(MPR_GC_FORCE | MPR_GC_COMPLETE) == 0) {
            break;
        }
    }
    mprState = MPR_DESTROYED;

    mprLog("info mpr", 2, (MPR->exitStrategy & MPR_EXIT_RESTART) ? "Restarting" : "Exiting");
    mprStopModuleService();
    mprStopSignalService();
    mprStopGCService();
    mprStopOsService();

    if (MPR->exitStrategy & MPR_EXIT_RESTART) {
        mprRestart();
    }
    mprDestroyMemService();
    return 1;
}


static void setNames(Mpr *mpr, int argc, char **argv)
{
    if (argv) {
#if ME_WIN_LIKE
        if (argc >= 2 && strstr(argv[1], "--cygroot") != 0) {
            /*
                Cygwin shebang is broken. It will catenate args into argv[1]
             */
            char *args, *arg0;
            int  i;
            args = argv[1];
            for (i = 2; i < argc; i++) {
                args = sjoin(args, " ", argv[i], NULL);
            }
            arg0 = argv[0];
            argc = mprMakeArgv(args, &mpr->argBuf, MPR_ARGV_ARGS_ONLY);
            argv = mpr->argBuf;
            argv[0] = arg0;
            mpr->argv = (cchar**) argv;
        } else {
            mpr->argv = mprAllocZeroed(sizeof(void*) * (argc + 1));
            memcpy((char*) mpr->argv, argv, sizeof(void*) * argc);
        }
#else
        mpr->argv = mprAllocZeroed(sizeof(void*) * (argc + 1));
        memcpy((char*) mpr->argv, argv, sizeof(void*) * argc);
#endif
        mpr->argc = argc;
        if (!mprIsPathAbs(mpr->argv[0])) {
            mpr->argv[0] = mprGetAppPath();
        } else {
            mpr->argv[0] = sclone(mprGetAppPath());
        }
    } else {
        mpr->name = sclone(ME_NAME);
        mpr->argv = mprAllocZeroed(2 * sizeof(void*));
        mpr->argv[0] = mpr->name;
        mpr->argc = 0;
    }
    mpr->name = mprTrimPathExt(mprGetPathBase(mpr->argv[0]));
    mpr->title = sfmt("%s %s", stitle(ME_COMPANY), stitle(mpr->name));
    mpr->version = sclone(ME_VERSION);
}


PUBLIC int mprGetExitStatus()
{
    return mprExitStatus & ~NO_STATUS;
}


PUBLIC void mprSetExitStatus(int status)
{
    mprExitStatus = status;
}


PUBLIC void mprAddTerminator(MprTerminator terminator)
{
    mprAddItem(MPR->terminators, terminator);
}


PUBLIC void mprRestart()
{
#if ME_UNIX_LIKE
    int     i;

    for (i = 3; i < MPR_MAX_FILE; i++) {
        close(i);
    }
    execv(MPR->argv[0], (char*const*) MPR->argv);

    /*
        Last-ditch trace. Can only use stdout. Logging may be closed.
     */
    printf("Failed to exec errno %d: ", errno);
    for (i = 0; MPR->argv[i]; i++) {
        printf("%s ", MPR->argv[i]);
    }
    printf("\n");
#else
    mprLog("error mpr", 0, "mprRestart not supported on this platform");
#endif
}


PUBLIC int mprStart()
{
    int     rc;

    rc = mprStartOsService();
    rc += mprStartModuleService();
    rc += mprStartWorkerService();
    if (rc != 0) {
        mprLog("error mpr", 0, "Cannot start MPR services");
        return MPR_ERR_CANT_INITIALIZE;
    }
    mprState = MPR_STARTED;
    return 0;
}


PUBLIC int mprStartEventsThread()
{
    MprThread   *tp;
    MprTicks    timeout;

    if ((tp = mprCreateThread("events", serviceEventsThread, NULL, 0)) == 0) {
        MPR->hasError = 1;
    } else {
        MPR->threadService->eventsThread = tp;
        MPR->cond = mprCreateCond();
        mprStartThread(tp);
        timeout = mprGetDebugMode() ? MPR_MAX_TIMEOUT : MPR_TIMEOUT_START_TASK;
        mprWaitForCond(MPR->cond, timeout);
    }
    return 0;
}


static void serviceEventsThread(void *data, MprThread *tp)
{
    mprLog("info mpr", 2, "Service thread started");
    mprSetWindowsThread(tp);
    mprSignalCond(MPR->cond);
    mprServiceEvents(-1, 0);
    mprRescheduleDispatcher(MPR->dispatcher);
}


/*
    Services should call this to determine if they should accept new services
 */
PUBLIC bool mprShouldAbortRequests()
{
    return mprIsStopped();
}


PUBLIC bool mprShouldDenyNewRequests()
{
    return mprIsStopping();
}


PUBLIC bool mprIsStopping()
{
    return mprState >= MPR_STOPPING;
}


PUBLIC bool mprIsStopped()
{
    return mprState >= MPR_STOPPED;
}


PUBLIC bool mprIsDestroying()
{
    return mprState >= MPR_DESTROYING;
}


PUBLIC bool mprIsDestroyed()
{
    return mprState >= MPR_DESTROYED;
}


PUBLIC int mprGetState()
{
    return mprState;
}


PUBLIC void mprSetState(int state)
{
    mprGlobalLock();
    mprState = state;
    mprGlobalUnlock();
}



/*
    Test if the Mpr services are idle. Use mprIsIdle to determine if the entire process is idle.
    Note: this counts worker threads but ignores other threads created via mprCreateThread
 */
PUBLIC bool mprServicesAreIdle(bool traceRequests)
{
    bool    idle;

    /*
        Only test top level services. Dispatchers may have timers scheduled, but that is okay. If not, users can install 
        their own idleCallback.
     */
    idle = mprGetBusyWorkerCount() == 0 && mprGetActiveCmdCount() == 0 && !mprGCPaused();
    if (!idle && traceRequests) {
        mprDebug("mpr", 3, "Services are not idle: cmds %d, busy threads %d, eventing %d",
            mprGetListLength(MPR->cmdService->cmds), mprGetListLength(MPR->workerService->busyThreads), MPR->eventing);
    }
    return idle;
}


PUBLIC bool mprIsIdle(bool traceRequests)
{
    return (MPR->idleCallback)(traceRequests);
}


/*
    Parse the args and return the count of args. If argv is NULL, the args are parsed read-only. If argv is set,
    then the args will be extracted, back-quotes removed and argv will be set to point to all the args.
    NOTE: this routine does not allocate.
 */
PUBLIC int mprParseArgs(char *args, char **argv, int maxArgc)
{
    char    *dest, *src, *start;
    int     quote, argc;

    /*
        Example     "showColors" red 'light blue' "yellow white" 'Cannot \"render\"'
        Becomes:    ["showColors", "red", "light blue", "yellow white", "Cannot \"render\""]
     */
    for (argc = 0, src = args; src && *src != '\0' && argc < maxArgc; argc++) {
        while (isspace((uchar) *src)) {
            src++;
        }
        if (*src == '\0')  {
            break;
        }
        start = dest = src;
        if (*src == '"' || *src == '\'') {
            quote = *src;
            src++; 
            dest++;
        } else {
            quote = 0;
        }
        if (argv) {
            argv[argc] = src;
        }
        while (*src) {
            if (*src == '\\' && src[1] && (src[1] == '\\' || src[1] == '"' || src[1] == '\'')) { 
                src++;
            } else {
                if (quote) {
                    if (*src == quote && !(src > start && src[-1] == '\\')) {
                        break;
                    }
                } else if (*src == ' ') {
                    break;
                }
            }
            if (argv) {
                *dest++ = *src;
            }
            src++;
        }
        if (*src != '\0') {
            src++;
        }
        if (argv) {
            *dest++ = '\0';
        }
    }
    return argc;
}


/*
    Make an argv array. All args are in a single memory block of which argv points to the start.
    Set MPR_ARGV_ARGS_ONLY if not passing in a program name. 
    Always returns and argv[0] reserved for the program name or empty string.  First arg starts at argv[1].
 */
PUBLIC int mprMakeArgv(cchar *command, cchar ***argvp, int flags)
{
    char    **argv, *vector, *args;
    ssize   len;
    int     argc;

    assert(command);

    /*
        Allocate one vector for argv and the actual args themselves
     */
    len = slen(command) + 1;
    argc = mprParseArgs((char*) command, NULL, INT_MAX);
    if (flags & MPR_ARGV_ARGS_ONLY) {
        argc++;
    }
    if ((vector = (char*) mprAlloc(((argc + 1) * sizeof(char*)) + len)) == 0) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    args = &vector[(argc + 1) * sizeof(char*)];
    strcpy(args, command);
    argv = (char**) vector;

    if (flags & MPR_ARGV_ARGS_ONLY) {
        mprParseArgs(args, &argv[1], argc);
        argv[0] = MPR->emptyString;
    } else {
        mprParseArgs(args, argv, argc);
    }
    argv[argc] = 0;
    *argvp = (cchar**) argv;
    return argc;
}


PUBLIC MprIdleCallback mprSetIdleCallback(MprIdleCallback idleCallback)
{
    MprIdleCallback old;

    old = MPR->idleCallback;
    MPR->idleCallback = idleCallback;
    return old;
}


PUBLIC int mprSetAppName(cchar *name, cchar *title, cchar *version)
{
    char    *cp;

    if (name) {
        if ((MPR->name = (char*) mprGetPathBase(name)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
        if ((cp = strrchr(MPR->name, '.')) != 0) {
            *cp = '\0';
        }
    }
    if (title) {
        if ((MPR->title = sclone(title)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    if (version) {
        if ((MPR->version = sclone(version)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    return 0;
}


PUBLIC cchar *mprGetAppName()
{
    return MPR->name;
}


PUBLIC cchar *mprGetAppTitle()
{
    return MPR->title;
}


/*
    Full host name with domain. E.g. "server.domain.com"
 */
PUBLIC void mprSetHostName(cchar *s)
{
    MPR->hostName = sclone(s);
}


/*
    Return the fully qualified host name
 */
PUBLIC cchar *mprGetHostName()
{
    return MPR->hostName;
}


/*
    Server name portion (no domain name)
 */
PUBLIC void mprSetServerName(cchar *s)
{
    MPR->serverName = sclone(s);
}


PUBLIC cchar *mprGetServerName()
{
    return MPR->serverName;
}


PUBLIC void mprSetDomainName(cchar *s)
{
    MPR->domainName = sclone(s);
}


PUBLIC cchar *mprGetDomainName()
{
    return MPR->domainName;
}


/*
    Set the IP address
 */
PUBLIC void mprSetIpAddr(cchar *s)
{
    MPR->ip = sclone(s);
}


/*
    Return the IP address
 */
PUBLIC cchar *mprGetIpAddr()
{
    return MPR->ip;
}


PUBLIC cchar *mprGetAppVersion()
{
    return MPR->version;
}


PUBLIC bool mprGetDebugMode()
{
    return MPR->debugMode;
}


PUBLIC void mprSetDebugMode(bool on)
{
    MPR->debugMode = on;
}


PUBLIC MprDispatcher *mprGetDispatcher()
{
    return MPR->dispatcher;
}


PUBLIC MprDispatcher *mprGetNonBlockDispatcher()
{
    return MPR->nonBlock;
}


PUBLIC cchar *mprCopyright()
{
    return  "Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.\n"
            "Copyright (c) Michael O'Brien, 1993-2014. All Rights Reserved.";
}


PUBLIC int mprGetEndian()
{
    char    *probe;
    int     test;

    test = 1;
    probe = (char*) &test;
    return (*probe == 1) ? ME_LITTLE_ENDIAN : ME_BIG_ENDIAN;
}


PUBLIC char *mprEmptyString()
{
    return MPR->emptyString;
}


PUBLIC void mprSetEnv(cchar *key, cchar *value)
{
#if ME_UNIX_LIKE
    setenv(key, value, 1);
#else
    char *cmd = sjoin(key, "=", value, NULL);
    putenv(cmd);
#endif
    if (scaselessmatch(key, "PATH")) {
        MPR->pathEnv = sclone(value);
    }
}


PUBLIC void mprSetExitTimeout(MprTicks timeout)
{
    MPR->exitTimeout = timeout;
}


PUBLIC void mprNop(void *ptr) {
}


/*
    This should not be called after mprCreate() as it will orphan the GC and events threads.
 */
PUBLIC int mprDaemon()
{
#if ME_UNIX_LIKE
    struct sigaction    act, old;
    int                 i, pid, status;

    /*
        Ignore child death signals
     */
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = (void (*)(int, siginfo_t*, void*)) SIG_DFL;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NOCLDSTOP | SA_RESTART | SA_SIGINFO;

    if (sigaction(SIGCHLD, &act, &old) < 0) {
        fprintf(stderr, "Cannot initialize signals");
        return MPR_ERR_BAD_STATE;
    }
    /*
        Close stdio so shells won't hang
     */
    for (i = 0; i < 3; i++) {
        close(i);
    }
    /*
        Fork twice to get a free child with no parent
     */
    if ((pid = fork()) < 0) {
        fprintf(stderr, "Fork failed for background operation");
        return MPR_ERR;

    } else if (pid == 0) {
        /* Child of first fork */
        if ((pid = fork()) < 0) {
            fprintf(stderr, "Second fork failed");
            exit(127);

        } else if (pid > 0) {
            /* Parent of second child -- must exit. This is waited for below */
            exit(0);
        }

        /*
            This is the real child that will continue as a daemon
         */
        setsid();
        if (sigaction(SIGCHLD, &old, 0) < 0) {
            fprintf(stderr, "Cannot restore signals");
            return MPR_ERR_BAD_STATE;
        }
        return 0;
    }

    /*
        Original (parent) process waits for first child here. Must get child death notification with a successful exit status.
     */
    while (waitpid(pid, &status, 0) != pid) {
        if (errno == EINTR) {
            mprSleep(100);
            continue;
        }
        fprintf(stderr, "Cannot wait for daemon parent.");
        exit(0);
    }
    if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Daemon parent had bad exit status.");
        exit(0);
    }
    if (sigaction(SIGCHLD, &old, 0) < 0) {
        fprintf(stderr, "Cannot restore signals");
        return MPR_ERR_BAD_STATE;
    }
    exit(0);
#else
    return 0;
#endif
}


PUBLIC void mprSetKey(cchar *key, void *value)
{
    mprAddKey(MPR->keys, key, value);
}


PUBLIC void *mprGetKey(cchar *key)
{
    return mprLookupKey(MPR->keys, key);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/async.c ************/


/**
    async.c - Wait for I/O on Windows.

    This module provides io management for sockets on Windows like systems. 
    A windows may be created per thread and will be retained until shutdown.
    Typically, only one window is required and that is for the notifier thread
    executing mprServiceEvents.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_EVENT_NOTIFIER == MPR_EVENT_ASYNC

/***************************** Forward Declarations ***************************/

static LRESULT CALLBACK msgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    ws->socketMessage = MPR_SOCKET_MESSAGE;
    return 0;
}


PUBLIC HWND mprSetWindowsThread(MprThread *tp)
{
    MprWaitService  *ws;

    ws = MPR->waitService;
    if (!tp) {
        tp = mprGetCurrentThread();
    }
    if (!tp->hwnd) {
        tp->hwnd = mprCreateWindow(tp);
    }
    ws->hwnd = tp->hwnd;
    return ws->hwnd;
}


PUBLIC void mprManageAsync(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_FREE) {
        if (ws->wclass) {
            mprDestroyWindowClass(ws->wclass);
            ws->wclass = 0;
        }
    }
}


PUBLIC int mprNotifyOn(MprWaitHandler *wp, int mask)
{
    MprWaitService  *ws;
    int             rc, winMask;

    ws = wp->service;
    lock(ws);
    winMask = 0;
    if (wp->desiredMask != mask) {
        if (mask & MPR_READABLE) {
            winMask |= FD_ACCEPT | FD_CONNECT | FD_CLOSE | FD_READ;
        }
        if (mask & MPR_WRITABLE) {
            winMask |= FD_WRITE;
        }
        wp->desiredMask = mask;
        assert(ws->hwnd);
        if (!(wp->flags & MPR_WAIT_NOT_SOCKET)) {
            /* 
                FUTURE: should use WaitForMultipleObjects in a wait thread for non-socket handles
             */
            if ((rc = WSAAsyncSelect(wp->fd, ws->hwnd, ws->socketMessage, winMask)) != 0) {
                mprDebug("mpr event", 5, "mprNotifyOn WSAAsyncSelect failed %d, errno %d", rc, GetLastError());
            }
        }
#if UNUSED
        /*
            Disabled because mprRemoteEvent schedules the dispatcher AND lockes the event service.
            This may cause deadlocks and specifically, mprRemoveEvent may crash while it races with event service
            on another thread.
         */ 
        if (wp->event) {
            mprRemoveEvent(wp->event);
            wp->event = 0;
        }
#endif
    }
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single descriptor. Return the number of I/O events found. Mask is the events of interest.
    Timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTicks timeout)
{
    struct timeval  tval;
    fd_set          readMask, writeMask;
    int             rc, result;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
    tval.tv_sec = (int) (timeout / 1000);
    tval.tv_usec = (int) ((timeout % 1000) * 1000);

    FD_ZERO(&readMask);
    if (mask & MPR_READABLE) {
        FD_SET(fd, &readMask);
    }
    FD_ZERO(&writeMask);
    if (mask & MPR_WRITABLE) {
        FD_SET(fd, &writeMask);
    }
    mprYield(MPR_YIELD_STICKY);
    /*
        The select() API has no impact on masks registered via WSAAsyncSelect. i.e. no need to save/restore.
     */
    rc = select(fd + 1, &readMask, &writeMask, NULL, &tval);
    mprResetYield();

    result = 0;
    if (rc < 0) {
        mprLog("error mpr event", 0, "Select returned %d, errno %d", rc, mprGetOsError());

    } else if (rc > 0) {
        if (FD_ISSET(fd, &readMask)) {
            result |= MPR_READABLE;
        }
        if (FD_ISSET(fd, &writeMask)) {
            result |= MPR_WRITABLE;
        }
    }
    return result;
}


/*
    Wait for I/O on all registered descriptors. Timeout is in milliseconds. Return the number of events serviced.
    Should only be called by the thread that calls mprServiceEvents.
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTicks timeout)
{
    MSG     msg;
    HWND    hwnd;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if ME_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        
    } else if ((hwnd = mprGetWindow(0)) == 0) {
        mprLog("critical mpr event", 0, "mprWaitForIO: Cannot get window");

    } else {
        /*
            Timer must be after yield
         */
        mprYield(MPR_YIELD_STICKY);
        SetTimer(hwnd, 0, (UINT) timeout, NULL);
        if (GetMessage(&msg, NULL, 0, 0) == 0) {
            mprResetYield();
            mprShutdown(MPR_EXIT_NORMAL, 0, MPR_EXIT_TIMEOUT);
        } else {
            mprClearWaiting();
            mprResetYield();
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    ws->wakeRequested = 0;
}


PUBLIC void mprServiceWinIO(MprWaitService *ws, int sockFd, int winMask)
{
    MprWaitHandler      *wp;
    int                 index;

    lock(ws);
    for (index = 0; (wp = (MprWaitHandler*) mprGetNextItem(ws->handlers, &index)) != 0; ) {
        if (wp->fd == sockFd) {
            break;
        }
    }
    if (wp == 0) {
        /* If the server forcibly closed the socket, we may still get a read event. Just ignore it.  */
        unlock(ws);
        return;
    }
    /*
        Mask values: READ==1, WRITE=2, ACCEPT=8, CONNECT=10, CLOSE=20
     */
    wp->presentMask = 0;
    if (winMask & (FD_READ | FD_ACCEPT | FD_CLOSE)) {
        wp->presentMask |= MPR_READABLE;
    }
    if (winMask & (FD_WRITE | FD_CONNECT)) {
        wp->presentMask |= MPR_WRITABLE;
    }
    wp->presentMask &= wp->desiredMask;
    if (wp->presentMask) {
        if (wp->flags & MPR_WAIT_IMMEDIATE) {
            (wp->proc)(wp->handlerData, NULL);
        } else {
            mprNotifyOn(wp, 0);
            mprQueueIOEvent(wp);
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;

    ws = MPR->waitService;
    if (!ws->wakeRequested && ws->hwnd) {
        ws->wakeRequested = 1;
        PostMessage(ws->hwnd, WM_NULL, 0, 0L);
    }
}


/*
    Windows message processing loop for wakeup and socket messages
 */
static LRESULT CALLBACK msgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MprWaitService      *ws;
    int                 sock, winMask;

    ws = MPR->waitService;

    if (msg == WM_DESTROY || msg == WM_QUIT) {
        mprShutdown(MPR_EXIT_NORMAL, 0, MPR_EXIT_TIMEOUT);

    } else if (msg && msg == ws->socketMessage) {
        sock = (int) wp;
        winMask = LOWORD(lp);
        mprServiceWinIO(MPR->waitService, sock, winMask);

    } else if (ws->msgCallback) {
        return ws->msgCallback(hwnd, msg, wp, lp);

    } else {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}


PUBLIC void mprSetWinMsgCallback(MprMsgCallback callback)
{
    MprWaitService  *ws;

    ws = MPR->waitService;
    ws->msgCallback = callback;
}


PUBLIC ATOM mprCreateWindowClass(cchar *name)
{
    WNDCLASS    wc;
    ATOM        atom;

    memset(&wc, 0, sizeof(wc));
    wc.lpszClassName = wide(name);
    wc.lpfnWndProc = msgProc;

    if ((atom = RegisterClass(&wc)) == 0) {
        mprLog("critical mpr event", 0, "Cannot register windows class");
        return 0;
    }
    return atom;
}


PUBLIC void mprDestroyWindowClass(ATOM wclass)
{
    if (wclass) {
        UnregisterClass((LPCTSTR) wclass, 0);
    }
}


PUBLIC HWND mprCreateWindow(MprThread *tp)
{
    MprWaitService  *ws;
    cchar           *name;

    ws = MPR->waitService;
    name = mprGetAppName();
    if (!ws->wclass && (ws->wclass = mprCreateWindowClass(name)) == 0) {
        mprLog("critical mpr event", 0, "Cannot create window class");
        return 0;
    }
    assert(!tp->hwnd);
    if ((tp->hwnd = CreateWindow((LPCTSTR) ws->wclass, wide(name), WS_OVERLAPPED, CW_USEDEFAULT, 0, 0, 0, 
            NULL, NULL, 0, NULL)) == 0) {
        mprLog("critical mpr event", 0, "Cannot create window");
        return 0;
    }
    return tp->hwnd;
}


PUBLIC void mprDestroyWindow(HWND hwnd)
{
    if (hwnd) {
        DestroyWindow(hwnd);
    }
}


PUBLIC HWND mprGetWindow(bool *created)
{
    MprThread   *tp;

    if ((tp = mprGetCurrentThread()) == 0) {
        return 0;
    }
    if (tp->hwnd == 0) {
        if (created) {
            *created = 1;
        }
        tp->hwnd = mprCreateWindow(tp);
    }
    return tp->hwnd;
}


#else
void asyncDummy() {}
#endif /* MPR_EVENT_ASYNC */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/atomic.c ************/


/**
    atomic.c - Atomic operations

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/*********************************** Includes *********************************/



/*********************************** Local ************************************/

static MprSpin  atomicSpinLock;
static MprSpin  *atomicSpin = &atomicSpinLock;

/************************************ Code ************************************/

PUBLIC void mprAtomicOpen()
{
    mprInitSpinLock(atomicSpin);
}


/*
    Full memory barrier
 */
PUBLIC void mprAtomicBarrier()
{
    #if MACOSX
        OSMemoryBarrier();

    #elif defined(VX_MEM_BARRIER_RW)
        VX_MEM_BARRIER_RW();

    #elif ME_WIN_LIKE
        MemoryBarrier();

    #elif ME_COMPILER_HAS_ATOMIC
        __atomic_thread_fence(__ATOMIC_SEQ_CST);

    #elif ME_COMPILER_HAS_SYNC
        __sync_synchronize();

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X86 || ME_CPU_ARCH == ME_CPU_X64)
        asm volatile ("mfence" : : : "memory");

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_PPC)
        asm volatile ("sync" : : : "memory");

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_ARM) && KEEP
        asm volatile ("isync" : : : "memory");

    #else
        if (mprTrySpinLock(atomicSpin)) {
            mprSpinUnlock(atomicSpin);
        }
    #endif
}


/*
    Atomic compare and swap a pointer with a full memory barrier
 */
PUBLIC int mprAtomicCas(void * volatile *addr, void *expected, cvoid *value)
{
    #if MACOSX
        return OSAtomicCompareAndSwapPtrBarrier(expected, (void*) value, (void*) addr);

    #elif VXWORKS && _VX_ATOMIC_INIT && !ME_64
        /* vxCas operates with integer values */
        return vxCas((atomic_t*) addr, (atomicVal_t) expected, (atomicVal_t) value);

    #elif ME_WIN_LIKE
        return InterlockedCompareExchangePointer(addr, (void*) value, expected) == expected;

    #elif ME_COMPILER_HAS_ATOMIC
        void *localExpected = expected;
        return __atomic_compare_exchange(addr, &localExpected, (void**) &value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

    #elif ME_COMPILER_HAS_SYNC_CAS
        return __sync_bool_compare_and_swap(addr, expected, value);

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X86)
        void *prev;
        asm volatile ("lock; cmpxchgl %2, %1"
            : "=a" (prev), "=m" (*addr)
            : "r" (value), "m" (*addr), "0" (expected));
        return expected == prev;

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X64)
        void *prev;
        asm volatile ("lock; cmpxchgq %q2, %1"
            : "=a" (prev), "=m" (*addr)
            : "r" (value), "m" (*addr),
              "0" (expected));
            return expected == prev;

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_ARM) && KEEP

    #else
        mprSpinLock(atomicSpin);
        if (*addr == expected) {
            *addr = (void*) value;
            mprSpinUnlock(atomicSpin);
            return 1;
        }
        mprSpinUnlock(atomicSpin);
        return 0;
    #endif
}


/*
    Atomic add of a signed value. Used for add, subtract, inc, dec
 */
PUBLIC void mprAtomicAdd(volatile int *ptr, int value)
{
    #if MACOSX
        OSAtomicAdd32(value, ptr);

    #elif ME_WIN_LIKE
        InterlockedExchangeAdd(ptr, value);

    #elif ME_COMPILER_HAS_ATOMIC
        //  OPT - could use __ATOMIC_RELAXED
        __atomic_add_fetch(ptr, value, __ATOMIC_SEQ_CST);

    #elif ME_COMPILER_HAS_SYNC_CAS
        __sync_add_and_fetch(ptr, value);

    #elif VXWORKS && _VX_ATOMIC_INIT
        vxAtomicAdd(ptr, value);

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X86 || ME_CPU_ARCH == ME_CPU_X64)
        asm volatile("lock; addl %1,%0"
             : "+m" (*ptr)
             : "ir" (value));
    #else
        mprSpinLock(atomicSpin);
        *ptr += value;
        mprSpinUnlock(atomicSpin);

    #endif
}


/*
    On some platforms, this operation is only atomic with respect to other calls to mprAtomicAdd64
 */
PUBLIC void mprAtomicAdd64(volatile int64 *ptr, int64 value)
{
    #if MACOSX
        OSAtomicAdd64(value, ptr);
    
    #elif ME_WIN_LIKE && ME_64
        InterlockedExchangeAdd64(ptr, value);
    
    #elif ME_COMPILER_HAS_ATOMIC64 && (ME_64 || ME_CPU_ARCH == ME_CPU_X86 || ME_CPU_ARCH == ME_CPU_X64)
        //  OPT - could use __ATOMIC_RELAXED
        __atomic_add_fetch(ptr, value, __ATOMIC_SEQ_CST);

    #elif ME_COMPILER_HAS_SYNC64 && (ME_64 || ME_CPU_ARCH == ME_CPU_X86 || ME_CPU_ARCH == ME_CPU_X64)
        __sync_add_and_fetch(ptr, value);

    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X86)
        asm volatile ("lock; xaddl %0,%1"
            : "=r" (value), "=m" (*ptr)
            : "0" (value), "m" (*ptr)
            : "memory", "cc");
    
    #elif __GNUC__ && (ME_CPU_ARCH == ME_CPU_X64)
        asm volatile("lock; addq %1,%0"
             : "=m" (*ptr)
             : "er" (value), "m" (*ptr));

    #else
        mprSpinLock(atomicSpin);
        *ptr += value;
        mprSpinUnlock(atomicSpin);
    
    #endif
}


#if KEEP
PUBLIC void *mprAtomicExchange(void *volatile *addr, cvoid *value)
{
    #if MACOSX
        void *old = *(void**) addr;
        OSAtomicCompareAndSwapPtrBarrier(old, (void*) value, addr);
        return old;
    
    #elif ME_WIN_LIKE
        return (void*) InterlockedExchange((volatile LONG*) addr, (LONG) value);
    
    #elif ME_COMPILER_HAS_ATOMIC
        __atomic_exchange_n(addr, value, __ATOMIC_SEQ_CST);

    #elif ME_COMPILER_HAS_SYNC
        return __sync_lock_test_and_set(addr, (void*) value);
    
    #else
        void    *old;
        mprSpinLock(atomicSpin);
        old = *(void**) addr;
        *addr = (void*) value;
        mprSpinUnlock(atomicSpin);
        return old;
    #endif
}
#endif


/*
    Atomic list insertion. Inserts "item" at the "head" of the list. The "link" field is the next field in item.
 */
PUBLIC void mprAtomicListInsert(void **head, void **link, void *item)
{
    do {
        *link = *head;
    } while (!mprAtomicCas(head, (void*) *link, item));
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/buf.c ************/


/**
    buf.c - Dynamic buffer module

    This module is not thread-safe for performance. Callers must do their own locking.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Forwards **********************************/

static void manageBuf(MprBuf *buf, int flags);

/*********************************** Code *************************************/
/*
    Create a new buffer. "maxsize" is the limit to which the buffer can ever grow. -1 means no limit. "initialSize" is 
    used to define the amount to increase the size of the buffer each time if it becomes full. (Note: mprGrowBuf() will 
    exponentially increase this number for performance.)
 */
PUBLIC MprBuf *mprCreateBuf(ssize initialSize, ssize maxSize)
{
    MprBuf      *bp;

    if (initialSize <= 0) {
        initialSize = ME_MAX_BUFFER;
    }
    if ((bp = mprAllocObj(MprBuf, manageBuf)) == 0) {
        return 0;
    }
    mprSetBufSize(bp, initialSize, maxSize);
    return bp;
}


static void manageBuf(MprBuf *bp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(bp->data);
        mprMark(bp->refillArg);
    } 
}


PUBLIC MprBuf *mprCloneBuf(MprBuf *orig)
{
    MprBuf      *bp;
    ssize       len;

    if ((bp = mprCreateBuf(orig->growBy, orig->maxsize)) == 0) {
        return 0;
    }
    bp->refillProc = orig->refillProc;
    bp->refillArg = orig->refillArg;
    if ((len = mprGetBufLength(orig)) > 0) {
        memcpy(bp->data, orig->data, len);
        bp->end = &bp->data[len];
    }
    return bp;
}


PUBLIC char *mprCloneBufMem(MprBuf *bp)
{
    char    *result;
    ssize   len;

    len = mprGetBufLength(bp);
    if ((result = mprAlloc(len + 1)) == 0) {
        return 0;
    }
    memcpy(result, mprGetBufStart(bp), len);
    result[len] = 0;
    return result;
}


PUBLIC char *mprCloneBufAsString(MprBuf *bp)
{
    char    *result;
    ssize   len;

    mprAddNullToBuf(bp);
    len = slen(bp->start);
    if ((result = mprAlloc(len + 1)) == 0) {
        return 0;
    }
    memcpy(result, mprGetBufStart(bp), len);
    result[len] = 0;
    return result;
}


/*
    Set the current buffer size and maximum size limit.
 */
PUBLIC int mprSetBufSize(MprBuf *bp, ssize initialSize, ssize maxSize)
{
    assert(bp);

    if (initialSize <= 0) {
        if (maxSize > 0) {
            bp->maxsize = maxSize;
        }
        return 0;
    }
    if (maxSize > 0 && initialSize > maxSize) {
        initialSize = maxSize;
    }
    assert(initialSize > 0);

    if (bp->data) {
        /*
            Buffer already exists
         */
        if (bp->buflen < initialSize) {
            if (mprGrowBuf(bp, initialSize - bp->buflen) < 0) {
                return MPR_ERR_MEMORY;
            }
        }
        bp->maxsize = maxSize;
        return 0;
    }
    if ((bp->data = mprAlloc(initialSize)) == 0) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    bp->growBy = initialSize;
    bp->maxsize = maxSize;
    bp->buflen = initialSize;
    bp->endbuf = &bp->data[bp->buflen];
    bp->start = bp->data;
    bp->end = bp->data;
    *bp->start = '\0';
    return 0;
}


PUBLIC void mprSetBufMax(MprBuf *bp, ssize max)
{
    bp->maxsize = max;
}


/*
    This appends a silent null. It does not count as one of the actual bytes in the buffer
 */
PUBLIC void mprAddNullToBuf(MprBuf *bp)
{
    ssize      space;

    if (bp) {
        space = bp->endbuf - bp->end;
        if (space < sizeof(char)) {
            if (mprGrowBuf(bp, 1) < 0) {
                return;
            }
        }
        assert(bp->end < bp->endbuf);
        if (bp->end < bp->endbuf) {
            *((char*) bp->end) = (char) '\0';
        }
    }
}


PUBLIC void mprAdjustBufEnd(MprBuf *bp, ssize size)
{
    assert(bp->buflen == (bp->endbuf - bp->data));
    assert(size <= bp->buflen);
    assert((bp->end + size) >= bp->data);
    assert((bp->end + size) <= bp->endbuf);

    bp->end += size;
    if (bp->end > bp->endbuf) {
        assert(bp->end <= bp->endbuf);
        bp->end = bp->endbuf;
    }
    if (bp->end < bp->data) {
        bp->end = bp->data;
    }
}


/*
    Adjust the start pointer after a user copy. Note: size can be negative.
 */
PUBLIC void mprAdjustBufStart(MprBuf *bp, ssize size)
{
    assert(bp->buflen == (bp->endbuf - bp->data));
    assert(size <= bp->buflen);
    assert((bp->start + size) >= bp->data);
    assert((bp->start + size) <= bp->end);

    bp->start += size;
    if (bp->start > bp->end) {
        bp->start = bp->end;
    }
    if (bp->start <= bp->data) {
        bp->start = bp->data;
    }
}


PUBLIC void mprFlushBuf(MprBuf *bp)
{
    bp->start = bp->data;
    bp->end = bp->data;
}


PUBLIC int mprGetCharFromBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return (uchar) *bp->start++;
}


PUBLIC ssize mprGetBlockFromBuf(MprBuf *bp, char *buf, ssize size)
{
    ssize     thisLen, bytesRead;

    assert(buf);
    assert(size >= 0);
    assert(bp->buflen == (bp->endbuf - bp->data));

    /*
        Get the max bytes in a straight copy
     */
    bytesRead = 0;
    while (size > 0) {
        thisLen = mprGetBufLength(bp);
        thisLen = min(thisLen, size);
        if (thisLen <= 0) {
            break;
        }

        memcpy(buf, bp->start, thisLen);
        buf += thisLen;
        bp->start += thisLen;
        size -= thisLen;
        bytesRead += thisLen;
    }
    return bytesRead;
}


#ifndef mprGetBufLength
PUBLIC ssize mprGetBufLength(MprBuf *bp)
{
    return (bp->end - bp->start);
}
#endif


#ifndef mprGetBufSize
PUBLIC ssize mprGetBufSize(MprBuf *bp)
{
    return bp->buflen;
}
#endif


#ifndef mprGetBufSpace
PUBLIC ssize mprGetBufSpace(MprBuf *bp)
{
    return (bp->endbuf - bp->end);
}
#endif


#ifndef mprGetBuf
PUBLIC char *mprGetBuf(MprBuf *bp)
{
    return (char*) bp->data;
}
#endif


#ifndef mprGetBufStart
PUBLIC char *mprGetBufStart(MprBuf *bp)
{
    return (char*) bp->start;
}
#endif


#ifndef mprGetBufEnd
PUBLIC char *mprGetBufEnd(MprBuf *bp)
{
    return (char*) bp->end;
}
#endif


PUBLIC int mprInsertCharToBuf(MprBuf *bp, int c)
{
    if (bp->start == bp->data) {
        return MPR_ERR_BAD_STATE;
    }
    *--bp->start = c;
    return 0;
}


PUBLIC int mprLookAtNextCharInBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return *bp->start;
}


PUBLIC int mprLookAtLastCharInBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return bp->end[-1];
}


PUBLIC int mprPutCharToBuf(MprBuf *bp, int c)
{
    char       *cp;
    ssize      space;

    assert(bp->buflen == (bp->endbuf - bp->data));

    space = bp->buflen - mprGetBufLength(bp);
    if (space < sizeof(char)) {
        if (mprGrowBuf(bp, 1) < 0) {
            return -1;
        }
    }
    cp = (char*) bp->end;
    *cp++ = (char) c;
    bp->end = (char*) cp;

    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return 1;
}


/*
    Return the number of bytes written to the buffer. If no more bytes will fit, may return less than size.
    Never returns < 0.
 */
PUBLIC ssize mprPutBlockToBuf(MprBuf *bp, cchar *str, ssize size)
{
    ssize      thisLen, bytes, space;

    assert(str);
    assert(size >= 0);
    assert(size < MAXINT);

    bytes = 0;
    while (size > 0) {
        space = mprGetBufSpace(bp);
        thisLen = min(space, size);
        if (thisLen <= 0) {
            if (mprGrowBuf(bp, size) < 0) {
                break;
            }
            space = mprGetBufSpace(bp);
            thisLen = min(space, size);
        }
        memcpy(bp->end, str, thisLen);
        str += thisLen;
        bp->end += thisLen;
        size -= thisLen;
        bytes += thisLen;
    }
    if (bp && bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return bytes;
}


PUBLIC ssize mprPutStringToBuf(MprBuf *bp, cchar *str)
{
    if (str) {
        return mprPutBlockToBuf(bp, str, slen(str));
    }
    return 0;
}


PUBLIC ssize mprPutSubStringToBuf(MprBuf *bp, cchar *str, ssize count)
{
    ssize     len;

    if (str) {
        len = slen(str);
        len = min(len, count);
        if (len > 0) {
            return mprPutBlockToBuf(bp, str, len);
        }
    }
    return 0;
}


PUBLIC ssize mprPutPadToBuf(MprBuf *bp, int c, ssize count)
{
    assert(count < MAXINT);

    while (count-- > 0) {
        if (mprPutCharToBuf(bp, c) < 0) {
            return -1;
        }
    }
    return count;
}


PUBLIC ssize mprPutToBuf(MprBuf *bp, cchar *fmt, ...)
{
    va_list     ap;
    char        *buf;

    if (fmt == 0) {
        return 0;
    }
    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    return mprPutStringToBuf(bp, buf);
}


/*
    Grow the buffer. Return 0 if the buffer grows. Increase by the growBy size specified when creating the buffer. 
 */
PUBLIC int mprGrowBuf(MprBuf *bp, ssize need)
{
    char    *newbuf;
    ssize   growBy;

    if (bp->maxsize > 0 && bp->buflen >= bp->maxsize) {
        return MPR_ERR_TOO_MANY;
    }
    if (bp->start > bp->end) {
        mprCompactBuf(bp);
    }
    if (need > 0) {
        growBy = max(bp->growBy, need);
    } else {
        growBy = bp->growBy;
    }
    if ((newbuf = mprAlloc(bp->buflen + growBy)) == 0) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    if (bp->data) {
        memcpy(newbuf, bp->data, bp->buflen);
    }
    bp->buflen += growBy;
    bp->end = newbuf + (bp->end - bp->data);
    bp->start = newbuf + (bp->start - bp->data);
    bp->data = newbuf;
    bp->endbuf = &bp->data[bp->buflen];

    /*
        Increase growBy to reduce overhead
     */
    if (bp->maxsize > 0) {
        if ((bp->buflen + (bp->growBy * 2)) > bp->maxsize) {
            bp->growBy = min(bp->maxsize - bp->buflen, bp->growBy * 2);
        }
    } else {
        if ((bp->buflen + (bp->growBy * 2)) > bp->maxsize) {
            bp->growBy = min(bp->buflen, bp->growBy * 2);
        }
    }
    return 0;
}


/*
    Add a number to the buffer (always null terminated).
 */
PUBLIC ssize mprPutIntToBuf(MprBuf *bp, int64 i)
{
    ssize       rc;

    rc = mprPutStringToBuf(bp, itos(i));
    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return rc;
}


PUBLIC void mprCompactBuf(MprBuf *bp)
{
    if (mprGetBufLength(bp) == 0) {
        mprFlushBuf(bp);
        return;
    }
    if (bp->start > bp->data) {
        memmove(bp->data, bp->start, (bp->end - bp->start));
        bp->end -= (bp->start - bp->data);
        bp->start = bp->data;
    }
}


PUBLIC MprBufProc mprGetBufRefillProc(MprBuf *bp) 
{
    return bp->refillProc;
}


PUBLIC void mprSetBufRefillProc(MprBuf *bp, MprBufProc fn, void *arg)
{ 
    bp->refillProc = fn; 
    bp->refillArg = arg; 
}


PUBLIC int mprRefillBuf(MprBuf *bp) 
{ 
    return (bp->refillProc) ? (bp->refillProc)(bp, bp->refillArg) : 0; 
}


PUBLIC void mprResetBufIfEmpty(MprBuf *bp)
{
    if (mprGetBufLength(bp) == 0) {
        mprFlushBuf(bp);
    }
}


PUBLIC char *mprBufToString(MprBuf *bp)
{
    mprAddNullToBuf(bp);
    return sclone(mprGetBufStart(bp));
}


#if ME_CHAR_LEN > 1 && KEEP
PUBLIC void mprAddNullToWideBuf(MprBuf *bp)
{
    ssize      space;

    space = bp->endbuf - bp->end;
    if (space < sizeof(wchar)) {
        if (mprGrowBuf(bp, sizeof(wchar)) < 0) {
            return;
        }
    }
    assert(bp->end < bp->endbuf);
    if (bp->end < bp->endbuf) {
        *((wchar*) bp->end) = (char) '\0';
    }
}


PUBLIC int mprPutCharToWideBuf(MprBuf *bp, int c)
{
    wchar *cp;
    ssize   space;

    assert(bp->buflen == (bp->endbuf - bp->data));

    space = bp->buflen - mprGetBufLength(bp);
    if (space < (sizeof(wchar) * 2)) {
        if (mprGrowBuf(bp, sizeof(wchar) * 2) < 0) {
            return -1;
        }
    }
    cp = (wchar*) bp->end;
    *cp++ = (wchar) c;
    bp->end = (char*) cp;

    if (bp->end < bp->endbuf) {
        *((wchar*) bp->end) = (char) '\0';
    }
    return 1;
}


PUBLIC ssize mprPutFmtToWideBuf(MprBuf *bp, cchar *fmt, ...)
{
    va_list     ap;
    wchar       *wbuf;
    char        *buf;
    ssize       len, space;
    ssize       rc;

    if (fmt == 0) {
        return 0;
    }
    va_start(ap, fmt);
    space = mprGetBufSpace(bp);
    space += (bp->maxsize - bp->buflen);
    buf = sfmtv(fmt, ap);
    wbuf = amtow(buf, &len);
    rc = mprPutBlockToBuf(bp, (char*) wbuf, len * sizeof(wchar));
    va_end(ap);
    return rc;
}


PUBLIC ssize mprPutStringToWideBuf(MprBuf *bp, cchar *str)
{
    wchar       *wstr;
    ssize       len;

    if (str) {
        wstr = amtow(str, &len);
        return mprPutBlockToBuf(bp, (char*) wstr, len);
    }
    return 0;
}

#endif /* ME_CHAR_LEN > 1 */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/cache.c ************/


/**
    cache.c - In-process caching

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/************************************ Locals **********************************/

static MprCache *shared;                /* Singleton shared cache */

typedef struct CacheItem
{
    char            *key;               /* Original key */
    char            *data;              /* Cache data */
    void            *link;              /* Linked managed reference */
    MprTicks        lifespan;           /* Lifespan after each access to key (msec) */
    MprTicks        lastAccessed;       /* Last accessed time */
    MprTicks        expires;            /* Fixed expiry date. If zero, key is imortal. */
    MprTime         lastModified;       /* Last update time. This is an MprTime and records world-time. */
    int64           version;
} CacheItem;

#define CACHE_TIMER_PERIOD      (60 * MPR_TICKS_PER_SEC)
#define CACHE_LIFESPAN          (86400 * MPR_TICKS_PER_SEC)
#define CACHE_HASH_SIZE         257

/*********************************** Forwards *********************************/

static void manageCache(MprCache *cache, int flags);
static void manageCacheItem(CacheItem *item, int flags);
static void pruneCache(MprCache *cache, MprEvent *event);
static void removeItem(MprCache *cache, CacheItem *item);

/************************************* Code ***********************************/

PUBLIC int mprCreateCacheService()
{
    shared = 0;
    return 0;
}


PUBLIC MprCache *mprCreateCache(int options)
{
    MprCache    *cache;
    int         wantShared;

    if ((cache = mprAllocObj(MprCache, manageCache)) == 0) {
        return 0;
    }
    wantShared = (options & MPR_CACHE_SHARED);
    if (wantShared && shared) {
        cache->shared = shared;
    } else {
        cache->mutex = mprCreateLock();
        cache->store = mprCreateHash(CACHE_HASH_SIZE, 0);
        cache->maxMem = MAXSSIZE;
        cache->maxKeys = MAXSSIZE;
        cache->resolution = CACHE_TIMER_PERIOD;
        cache->lifespan = CACHE_LIFESPAN;
        if (wantShared) {
            shared = cache;
        }
    }
    return cache;
}


PUBLIC void *mprDestroyCache(MprCache *cache)
{
    assert(cache);

    if (cache->timer && cache != shared) {
        mprRemoveEvent(cache->timer);
        cache->timer = 0;
    }
    if (cache == shared) {
        shared = 0;
    }
    return 0;
}


/*
    Set expires to zero to remove
 */
PUBLIC int mprExpireCacheItem(MprCache *cache, cchar *key, MprTicks expires)
{
    CacheItem   *item;

    assert(cache);
    assert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        unlock(cache);
        return MPR_ERR_CANT_FIND;
    }
    if (expires == 0) {
        removeItem(cache, item);
    } else {
        item->expires = expires;
    }
    unlock(cache);
    return 0;
}


PUBLIC int64 mprIncCache(MprCache *cache, cchar *key, int64 amount)
{
    CacheItem   *item;
    int64       value;

    assert(cache);
    assert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    value = amount;

    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        if ((item = mprAllocObj(CacheItem, manageCacheItem)) == 0) {
            return 0;
        }
    } else {
        value += stoi(item->data);
    }
    if (item->data) {
        cache->usedMem -= slen(item->data);
    }
    item->data = itos(value);
    cache->usedMem += slen(item->data);
    item->version++;
    item->lastAccessed = mprGetTicks();
    item->expires = item->lastAccessed + item->lifespan;
    unlock(cache);
    return value;
}


PUBLIC char *mprLookupCache(MprCache *cache, cchar *key, MprTime *modified, int64 *version)
{
    CacheItem   *item;
    char        *result;

    assert(cache);
    assert(key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        unlock(cache);
        return 0;
    }
    if (item->expires && item->expires <= mprGetTicks()) {
        unlock(cache);
        return 0;
    }
    if (version) {
        *version = item->version;
    }
    if (modified) {
        *modified = item->lastModified;
    }
    result = item->data;
    unlock(cache);
    return result;
}


PUBLIC char *mprReadCache(MprCache *cache, cchar *key, MprTime *modified, int64 *version)
{
    CacheItem   *item;
    char        *result;

    assert(cache);
    assert(key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        unlock(cache);
        return 0;
    }
    if (item->expires && item->expires <= mprGetTicks()) {
        unlock(cache);
        return 0;
    }
    if (version) {
        *version = item->version;
    }
    if (modified) {
        *modified = item->lastModified;
    }
    item->lastAccessed = mprGetTicks();
    item->expires = item->lastAccessed + item->lifespan;
    result = item->data;
    unlock(cache);
    return result;
}


PUBLIC bool mprRemoveCache(MprCache *cache, cchar *key)
{
    CacheItem   *item;
    bool        result;

    assert(cache);
    assert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    lock(cache);
    if (key) {
        if ((item = mprLookupKey(cache->store, key)) != 0) {
            cache->usedMem -= (slen(key) + slen(item->data));
            mprRemoveKey(cache->store, key);
            result = 1;
        } else {
            result = 0;
        }

    } else {
        /* Remove all keys */
        result = mprGetHashLength(cache->store) ? 1 : 0;
        cache->store = mprCreateHash(CACHE_HASH_SIZE, 0);
        cache->usedMem = 0;
    }
    unlock(cache);
    return result;
}


PUBLIC void mprSetCacheNotify(MprCache *cache, MprCacheProc notify)
{
    assert(cache);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    cache->notify = notify;
}


PUBLIC void mprSetCacheLimits(MprCache *cache, int64 keys, MprTicks lifespan, int64 memory, int resolution)
{
    assert(cache);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    if (keys > 0) {
        cache->maxKeys = (ssize) keys;
        if (cache->maxKeys <= 0) {
            cache->maxKeys = MAXSSIZE;
        }
    }
    if (lifespan > 0) {
        cache->lifespan = lifespan;
    }
    if (memory > 0) {
        cache->maxMem = (ssize) memory;
        if (cache->maxMem <= 0) {
            cache->maxMem = MAXSSIZE;
        }
    }
    if (resolution > 0) {
        cache->resolution = resolution;
        if (cache->resolution <= 0) {
            cache->resolution = CACHE_TIMER_PERIOD;
        }
    }
}


PUBLIC ssize mprWriteCache(MprCache *cache, cchar *key, cchar *value, MprTime modified, MprTicks lifespan, int64 version, int options)
{
    CacheItem   *item;
    MprKey      *kp;
    ssize       len, oldLen;
    int         exists, add, set, prepend, append, throw, event;

    assert(cache);
    assert(key && *key);
    assert(value);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    exists = add = prepend = append = throw = 0;
    add = options & MPR_CACHE_ADD;
    append = options & MPR_CACHE_APPEND;
    prepend = options & MPR_CACHE_PREPEND;
    set = options & MPR_CACHE_SET;
    if ((add + append + prepend) == 0) {
        set = 1;
    }
    lock(cache);
    if ((kp = mprLookupKeyEntry(cache->store, key)) != 0) {
        exists++;
        item = (CacheItem*) kp->data;
        if (version) {
            if (item->version != version) {
                unlock(cache);
                return MPR_ERR_BAD_STATE;
            }
        }
    } else {
        if ((item = mprAllocObj(CacheItem, manageCacheItem)) == 0) {
            unlock(cache);
            return 0;
        }
        mprAddKey(cache->store, key, item);
        item->key = sclone(key);
        set = 1;
    }
    oldLen = (item->data) ? (slen(item->key) + slen(item->data)) : 0;
    if (set) {
        item->data = sclone(value);
    } else if (add) {
        if (exists) {
            return 0;
        }
        item->data = sclone(value);
    } else if (append) {
        item->data = sjoin(item->data, value, NULL);
    } else if (prepend) {
        item->data = sjoin(value, item->data, NULL);
    }
    if (lifespan >= 0) {
        item->lifespan = lifespan;
    }
    item->lastModified = modified ? modified : mprGetTime();
    item->lastAccessed = mprGetTicks();
    item->expires = item->lastAccessed + item->lifespan;
    item->version++;
    len = slen(item->key) + slen(item->data);
    cache->usedMem += (len - oldLen);

    if (cache->timer == 0) {
        cache->timer = mprCreateTimerEvent(MPR->dispatcher, "localCacheTimer", cache->resolution, pruneCache, cache, 
            MPR_EVENT_STATIC_DATA); 
    }
    if (cache->notify) {
        if (exists) {
            event = MPR_CACHE_NOTIFY_CREATE;
        } else {
            event = MPR_CACHE_NOTIFY_UPDATE;
        }
        (cache->notify)(cache, item->key, item->data, event);
    }
    unlock(cache);
    return len;
}


PUBLIC void *mprGetCacheLink(MprCache *cache, cchar *key)
{
    CacheItem   *item;
    MprKey      *kp;
    void        *result;

    assert(cache);
    assert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    result = 0;
    lock(cache);
    if ((kp = mprLookupKeyEntry(cache->store, key)) != 0) {
        item = (CacheItem*) kp->data;
        result = item->link;
    }
    unlock(cache);
    return result;
}


PUBLIC int mprSetCacheLink(MprCache *cache, cchar *key, void *link)
{
    CacheItem   *item;
    MprKey      *kp;

    assert(cache);
    assert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        assert(cache == shared);
    }
    lock(cache);
    if ((kp = mprLookupKeyEntry(cache->store, key)) != 0) {
        item = (CacheItem*) kp->data;
        item->link = link;
    }
    unlock(cache);
    return kp ? 0 : MPR_ERR_CANT_FIND;
}


static void removeItem(MprCache *cache, CacheItem *item)
{
    assert(cache);
    assert(item);

    lock(cache);
    if (cache->notify) {
        (cache->notify)(cache, item->key, item->data, MPR_CACHE_NOTIFY_REMOVE);
    }
    mprRemoveKey(cache->store, item->key);
    cache->usedMem -= (slen(item->key) + slen(item->data));
    unlock(cache);
}


static void pruneCache(MprCache *cache, MprEvent *event)
{
    MprTicks        when, factor;
    MprKey          *kp;
    CacheItem       *item;
    ssize           excessKeys;

    if (!cache) {
        cache = shared;
        if (!cache) {
            return;
        }
    }
    if (event) {
        when = mprGetTicks();
    } else {
        /* Expire all items by setting event to NULL */
        when = MPR_MAX_TIMEOUT;
    }
    if (mprTryLock(cache->mutex)) {
        /*
            Check for expired items
         */
        for (kp = 0; (kp = mprGetNextKey(cache->store, kp)) != 0; ) {
            item = (CacheItem*) kp->data;
            if (item->expires && item->expires <= when) {
                mprDebug("debug mpr cache", 5, "Prune expired key %s", kp->key);
                removeItem(cache, item);
            }
        }
        assert(cache->usedMem >= 0);

        /*
            If too many keys or too much memory used, prune keys that expire soonest.
         */
        if (cache->maxKeys < MAXSSIZE || cache->maxMem < MAXSSIZE) {
            /*
                Look for those expiring in the next 5 minutes, then 20 mins, then 80 ...
             */
            excessKeys = mprGetHashLength(cache->store) - cache->maxKeys;
            if (excessKeys < 0) {
                excessKeys = 0;
            }
            factor = 5 * 60 * MPR_TICKS_PER_SEC; 
            when += factor;
            while (excessKeys > 0 || cache->usedMem > cache->maxMem) {
                for (kp = 0; (kp = mprGetNextKey(cache->store, kp)) != 0; ) {
                    item = (CacheItem*) kp->data;
                    if (item->expires && item->expires <= when) {
                        mprDebug("debug mpr cache", 3, "Cache too big, execess keys %zd, mem %zd, prune key %s", 
                            excessKeys, (cache->maxMem - cache->usedMem), kp->key);
                        removeItem(cache, item);
                    }
                }
                factor *= 4;
                when += factor;
            }
        }
        assert(cache->usedMem >= 0);

        if (mprGetHashLength(cache->store) == 0) {
            if (event) {
                mprRemoveEvent(event);
                cache->timer = 0;
            }
        }
        unlock(cache);
    }
}


PUBLIC void mprPruneCache(MprCache *cache)
{
    pruneCache(cache, NULL);
}


static void manageCache(MprCache *cache, int flags) 
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cache->store);
        mprMark(cache->mutex);
        mprMark(cache->timer);
        mprMark(cache->shared);

    } else if (flags & MPR_MANAGE_FREE) {
        if (cache == shared) {
            shared = 0;
        }
    }
}


static void manageCacheItem(CacheItem *item, int flags) 
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(item->key);
        mprMark(item->data);
        mprMark(item->link);
    }
}


PUBLIC void mprGetCacheStats(MprCache *cache, int *numKeys, ssize *mem)
{
    if (numKeys) {
        *numKeys = mprGetHashLength(cache->store);
    }
    if (mem) {
        *mem = cache->usedMem;
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/cmd.c ************/


/*
    cmd.c - Run external commands

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/******************************* Forward Declarations *************************/

static int blendEnv(MprCmd *cmd, cchar **env, int flags);
static void closeFiles(MprCmd *cmd);
static void defaultCmdCallback(MprCmd *cmd, int channel, void *data);
static int makeChannel(MprCmd *cmd, int index);
static int makeCmdIO(MprCmd *cmd);
static void manageCmdService(MprCmdService *cmd, int flags);
static void manageCmd(MprCmd *cmd, int flags);
static void reapCmd(MprCmd *cmd, bool finalizing);
static void resetCmd(MprCmd *cmd, bool finalizing);
static int sanitizeArgs(MprCmd *cmd, int argc, cchar **argv, cchar **env, int flags);
static int startProcess(MprCmd *cmd);
static void stdinCallback(MprCmd *cmd, MprEvent *event);
static void stdoutCallback(MprCmd *cmd, MprEvent *event);
static void stderrCallback(MprCmd *cmd, MprEvent *event);
#if ME_WIN_LIKE
static void pollWinCmd(MprCmd *cmd, MprTicks timeout);
static void pollWinTimer(MprCmd *cmd, MprEvent *event);
static cchar *makeWinEnvBlock(MprCmd *cmd);
#endif

#if VXWORKS
typedef int (*MprCmdTaskFn)(int argc, char **argv, char **envp);
static void cmdTaskEntry(char *program, MprCmdTaskFn entry, int cmdArg);
#endif

/*
    Cygwin process creation is not thread-safe (1.7)
 */
#if CYGWIN
    #define slock(cmd) mprLock(MPR->cmdService->mutex)
    #define sunlock(cmd) mprUnlock(MPR->cmdService->mutex)
#else
    #define slock(cmd)
    #define sunlock(cmd)
#endif

/************************************* Code ***********************************/

PUBLIC MprCmdService *mprCreateCmdService()
{
    MprCmdService   *cs;

    if ((cs = (MprCmdService*) mprAllocObj(MprCmd, manageCmdService)) == 0) {
        return 0;
    }
    cs->cmds = mprCreateList(0, 0);
    cs->mutex = mprCreateLock();
    return cs;
}


PUBLIC void mprStopCmdService()
{
    mprClearList(MPR->cmdService->cmds);
}


static void manageCmdService(MprCmdService *cs, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cs->cmds);
        mprMark(cs->mutex);
    }
}


PUBLIC MprCmd *mprCreateCmd(MprDispatcher *dispatcher)
{
    MprCmd          *cmd;
    MprCmdFile      *files;
    int             i;

    if ((cmd = mprAllocObj(MprCmd, manageCmd)) == 0) {
        return 0;
    }
#if KEEP
    cmd->timeoutPeriod = MPR_TIMEOUT_CMD;
    cmd->timestamp = mprGetTicks();
#endif
    cmd->forkCallback = (MprForkCallback) closeFiles;
    cmd->dispatcher = dispatcher ? dispatcher : MPR->dispatcher;
    cmd->status = -1;

#if VXWORKS
    cmd->startCond = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    cmd->exitCond = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#endif
    files = cmd->files;
    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        files[i].clientFd = -1;
        files[i].fd = -1;
    }
    cmd->mutex = mprCreateLock();
    mprAddItem(MPR->cmdService->cmds, cmd);
    return cmd;
}


PUBLIC ssize mprGetActiveCmdCount()
{
    return mprGetListLength(MPR->cmdService->cmds);
}


static void manageCmd(MprCmd *cmd, int flags)
{
    int             i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(cmd->program);
        mprMark(cmd->makeArgv);
        mprMark(cmd->defaultEnv);
        mprMark(cmd->dir);
        mprMark(cmd->env);
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            mprMark(cmd->files[i].name);
        }
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            mprMark(cmd->handlers[i]);
        }
        mprMark(cmd->dispatcher);
        mprMark(cmd->callbackData);
        mprMark(cmd->signal);
        mprMark(cmd->forkData);
        mprMark(cmd->stdoutBuf);
        mprMark(cmd->stderrBuf);
        mprMark(cmd->userData);
        mprMark(cmd->mutex);
        mprMark(cmd->searchPath);
#if ME_WIN_LIKE
        mprMark(cmd->command);
        mprMark(cmd->arg0);
#endif

    } else if (flags & MPR_MANAGE_FREE) {
        resetCmd(cmd, 1);
#if VXWORKS
        if (cmd->startCond) {
            semDelete(cmd->startCond);
        }
        if (cmd->exitCond) {
            semDelete(cmd->exitCond);
        }
#endif
    }
}


static void resetCmd(MprCmd *cmd, bool finalizing)
{
    MprCmdFile      *files;
    int             i;

    assert(cmd);
    files = cmd->files;
    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->handlers[i]) {
            mprDestroyWaitHandler(cmd->handlers[i]);
            cmd->handlers[i] = 0;
        }
        if (files[i].clientFd >= 0) {
            close(files[i].clientFd);
            files[i].clientFd = -1;
        }
        if (files[i].fd >= 0) {
            close(files[i].fd);
            files[i].fd = -1;
        }
#if VXWORKS
        if (files[i].name) {
            DEV_HDR *dev;
            cchar   *tail;
            if ((dev = iosDevFind(files[i].name, &tail)) != NULL) {
                iosDevDelete(dev);
            }
        }
#endif
    }
    cmd->eofCount = 0;
    cmd->complete = 0;
    cmd->status = -1;

    if (cmd->pid && (!(cmd->flags & MPR_CMD_DETACH) || finalizing)) {
        mprStopCmd(cmd, -1);
        reapCmd(cmd, finalizing);
        cmd->pid = 0;
    }
    if (cmd->signal) {
        mprRemoveSignalHandler(cmd->signal);
        cmd->signal = 0;
    }
}


PUBLIC void mprDestroyCmd(MprCmd *cmd)
{
    assert(cmd);
    resetCmd(cmd, 0);
    mprRemoveItem(MPR->cmdService->cmds, cmd);
}


static void completeCommand(MprCmd *cmd)
{
    /*
        After removing the command from the cmds list, it can be garbage collected if no other reference is retained
     */
    cmd->complete = 1;
    mprDisconnectCmd(cmd);
    mprRemoveItem(MPR->cmdService->cmds, cmd);
}


PUBLIC void mprDisconnectCmd(MprCmd *cmd)
{
    int     i;

    assert(cmd);

    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->handlers[i]) {
            mprDestroyWaitHandler(cmd->handlers[i]);
            cmd->handlers[i] = 0;
        }
    }
}


/*
    Close a command channel. Must be able to be called redundantly.
 */
PUBLIC void mprCloseCmdFd(MprCmd *cmd, int channel)
{
    assert(cmd);
    assert(0 <= channel && channel <= MPR_CMD_MAX_PIPE);

    if (cmd->handlers[channel]) {
        assert(cmd->handlers[channel]->fd >= 0);
        mprDestroyWaitHandler(cmd->handlers[channel]);
        cmd->handlers[channel] = 0;
    }
    if (cmd->files[channel].fd != -1) {
        close(cmd->files[channel].fd);
        cmd->files[channel].fd = -1;
#if ME_WIN_LIKE
        cmd->files[channel].handle = 0;
#endif
        if (channel != MPR_CMD_STDIN) {
            cmd->eofCount++;
            if (cmd->eofCount >= cmd->requiredEof) {
#if VXWORKS
                reapCmd(cmd, 0);
#endif
                if (cmd->pid == 0) {
                    completeCommand(cmd);
                }
            }
        }
    }
}


PUBLIC void mprFinalizeCmd(MprCmd *cmd)
{
    assert(cmd);
    mprCloseCmdFd(cmd, MPR_CMD_STDIN);
}


PUBLIC int mprIsCmdComplete(MprCmd *cmd)
{
    return cmd->complete;
}


PUBLIC int mprRun(MprDispatcher *dispatcher, cchar *command, cchar *input, char **output, char **error, MprTicks timeout)
{
    MprCmd  *cmd;

    cmd = mprCreateCmd(dispatcher);
    return mprRunCmd(cmd, command, NULL, input, output, error, timeout, MPR_CMD_IN  | MPR_CMD_OUT | MPR_CMD_ERR);
}


/*
    Run a simple blocking command. See arg usage below in mprRunCmdV.
 */
PUBLIC int mprRunCmd(MprCmd *cmd, cchar *command, cchar **envp, cchar *in, char **out, char **err, MprTicks timeout, int flags)
{
    cchar   **argv;
    int     argc;

    if (cmd == 0 && (cmd = mprCreateCmd(0)) == 0) {
        return MPR_ERR_BAD_STATE;
    }
    if ((argc = mprMakeArgv(command, &argv, 0)) < 0 || argv == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    cmd->makeArgv = argv;
    return mprRunCmdV(cmd, argc, argv, envp, in, out, err, timeout, flags);
}


/*
    This routine runs a command and waits for its completion. Stdoutput and Stderr are returned in *out and *err
    respectively. The command returns the exit status of the command.
    Valid flags are:
        MPR_CMD_NEW_SESSION     Create a new session on Unix
        MPR_CMD_SHOW            Show the commands window on Windows
        MPR_CMD_IN              Connect to stdin
 */
PUBLIC int mprRunCmdV(MprCmd *cmd, int argc, cchar **argv, cchar **envp, cchar *in, char **out, char **err, MprTicks timeout, int flags)
{
    ssize   len;
    int     rc, status;

    assert(cmd);
    if (in) {
        flags |= MPR_CMD_IN;
    }
    if (err) {
        *err = 0;
        flags |= MPR_CMD_ERR;
    } else {
        flags &= ~MPR_CMD_ERR;
    }
    if (out) {
        *out = 0;
        flags |= MPR_CMD_OUT;
    } else {
        flags &= ~MPR_CMD_OUT;
    }
    if (flags & MPR_CMD_OUT) {
        cmd->stdoutBuf = mprCreateBuf(ME_MAX_BUFFER, -1);
    }
    if (flags & MPR_CMD_ERR) {
        cmd->stderrBuf = mprCreateBuf(ME_MAX_BUFFER, -1);
    }
    mprSetCmdCallback(cmd, defaultCmdCallback, NULL);
    rc = mprStartCmd(cmd, argc, argv, envp, flags);

    if (in) {
        len = slen(in);
        if (mprWriteCmdBlock(cmd, MPR_CMD_STDIN, in, len) != len) {
            *err = sfmt("Cannot write to command %s", cmd->program);
            return MPR_ERR_CANT_WRITE;
        }
    }
    if (cmd->files[MPR_CMD_STDIN].fd >= 0) {
        mprFinalizeCmd(cmd);
    }
    if (rc < 0) {
        if (err) {
            if (rc == MPR_ERR_CANT_ACCESS) {
                *err = sfmt("Cannot access command %s", cmd->program);
            } else if (rc == MPR_ERR_CANT_OPEN) {
                *err = sfmt("Cannot open standard I/O for command %s", cmd->program);
            } else if (rc == MPR_ERR_CANT_CREATE) {
                *err = sfmt("Cannot create process for %s", cmd->program);
            }
        }
        return rc;
    }
    if (cmd->flags & MPR_CMD_DETACH) {
        return 0;
    }
    if (mprWaitForCmd(cmd, timeout) < 0) {
        return MPR_ERR_NOT_READY;
    }
    if ((status = mprGetCmdExitStatus(cmd)) < 0) {
        return MPR_ERR;
    }
    if (err && flags & MPR_CMD_ERR) {
        *err = mprGetBufStart(cmd->stderrBuf);
    }
    if (out && flags & MPR_CMD_OUT) {
        *out = mprGetBufStart(cmd->stdoutBuf);
    }
    return status;
}


static int addCmdHandlers(MprCmd *cmd)
{
    int     stdinFd, stdoutFd, stderrFd;

    stdinFd = cmd->files[MPR_CMD_STDIN].fd;
    stdoutFd = cmd->files[MPR_CMD_STDOUT].fd;
    stderrFd = cmd->files[MPR_CMD_STDERR].fd;

    if (stdinFd >= 0 && cmd->handlers[MPR_CMD_STDIN] == 0) {
        if ((cmd->handlers[MPR_CMD_STDIN] = mprCreateWaitHandler(stdinFd, MPR_WRITABLE, cmd->dispatcher,
                stdinCallback, cmd, MPR_WAIT_NOT_SOCKET)) == 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    if (stdoutFd >= 0 && cmd->handlers[MPR_CMD_STDOUT] == 0) {
        if ((cmd->handlers[MPR_CMD_STDOUT] = mprCreateWaitHandler(stdoutFd, MPR_READABLE, cmd->dispatcher,
                stdoutCallback, cmd, MPR_WAIT_NOT_SOCKET)) == 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    if (stderrFd >= 0 && cmd->handlers[MPR_CMD_STDERR] == 0) {
        if ((cmd->handlers[MPR_CMD_STDERR] = mprCreateWaitHandler(stderrFd, MPR_READABLE, cmd->dispatcher,
                stderrCallback, cmd, MPR_WAIT_NOT_SOCKET)) == 0) {
            return MPR_ERR_CANT_OPEN;
        }
    }
    return 0;
}


/*
    Env is an array of "KEY=VALUE" strings. Null terminated
    The user must preserve the environment. This module does not clone the environment and uses the supplied reference.
 */
PUBLIC void mprSetCmdDefaultEnv(MprCmd *cmd, cchar **env)
{
    /* WARNING: defaultEnv is not cloned, but is marked */
    cmd->defaultEnv = env;
}


PUBLIC void mprSetCmdSearchPath(MprCmd *cmd, cchar *search)
{
    cmd->searchPath = sclone(search);
}


/*
    Start the command to run (stdIn and stdOut are named from the client's perspective). This is the lower-level way to
    run a command. The caller needs to do code like mprRunCmd() themselves to wait for completion and to send/receive data.
    The routine does not wait. Callers must call mprWaitForCmd to wait for the command to complete.
 */
PUBLIC int mprStartCmd(MprCmd *cmd, int argc, cchar **argv, cchar **envp, int flags)
{
    MprPath     info;
    cchar       *program, *search, *pair;
    int         rc, next, i;

    assert(cmd);
    assert(argv);

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return MPR_ERR_BAD_ARGS;
    }
    resetCmd(cmd, 0);
    program = argv[0];
    cmd->program = sclone(program);
    cmd->flags = flags;

    if (sanitizeArgs(cmd, argc, argv, envp, flags) < 0) {
        return MPR_ERR_MEMORY;
    }
    if (envp == 0) {
        envp = cmd->defaultEnv;
    }
    if (blendEnv(cmd, envp, flags) < 0) {
        return MPR_ERR_MEMORY;
    }
    search = cmd->searchPath ? cmd->searchPath : MPR->pathEnv;
    if ((program = mprSearchPath(program, MPR_SEARCH_EXE, search, NULL)) == 0) {
        mprLog("error mpr cmd", 0, "Cannot access %s, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_ACCESS;
    }
    cmd->program = cmd->argv[0] = program;

    if (mprGetPathInfo(program, &info) == 0 && info.isDir) {
        mprLog("error mpr cmd", 0, "Program \"%s\", is a directory", program);
        return MPR_ERR_CANT_ACCESS;
    }
    mprLog("info mpr cmd", 5, "Program: %s", cmd->program);
    for (i = 0; i < cmd->argc; i++) {
        mprLog("info mpr cmd", 5, "    arg[%d]: %s", i, cmd->argv[i]);
    }
    for (ITERATE_ITEMS(cmd->env, pair, next)) {
        mprLog("info mpr cmd", 5, "    env[%d]: %s", next, pair);
    }
    slock(cmd);
    if (makeCmdIO(cmd) < 0) {
        sunlock(cmd);
        return MPR_ERR_CANT_OPEN;
    }
    /*
        Determine how many end-of-files will be seen when the child dies
     */
    cmd->requiredEof = 0;
    if (cmd->flags & MPR_CMD_OUT) {
        cmd->requiredEof++;
    }
    if (cmd->flags & MPR_CMD_ERR) {
        cmd->requiredEof++;
    }
    if (addCmdHandlers(cmd) < 0) {
        mprLog("error mpr cmd", 0, "Cannot open command handlers - insufficient I/O handles");
        return MPR_ERR_CANT_OPEN;
    }
    rc = startProcess(cmd);
    cmd->originalPid = cmd->pid;
    sunlock(cmd);
#if ME_WIN_LIKE
    if (!rc) {
        mprCreateTimerEvent(cmd->dispatcher, "pollWinTimer", 10, pollWinTimer, cmd, 0);
    }
#endif
    return rc;
}


static int makeCmdIO(MprCmd *cmd)
{
    int     rc;

    rc = 0;
    if (cmd->flags & MPR_CMD_IN) {
        rc += makeChannel(cmd, MPR_CMD_STDIN);
    }
    if (cmd->flags & MPR_CMD_OUT) {
        rc += makeChannel(cmd, MPR_CMD_STDOUT);
    }
    if (cmd->flags & MPR_CMD_ERR) {
        rc += makeChannel(cmd, MPR_CMD_STDERR);
    }
    return rc;
}


/*
    Stop the command
    WARNING: Called from the finalizer. Must not block or lock.
 */
PUBLIC int mprStopCmd(MprCmd *cmd, int signal)
{
    mprDebug("mpr cmd", 5, "cmd: stop");
    if (signal < 0) {
        signal = SIGTERM;
    }
    cmd->stopped = 1;
    if (cmd->pid) {
#if ME_WIN_LIKE
        return TerminateProcess(cmd->process, 2) == 0;
#elif VXWORKS
        return taskDelete(cmd->pid);
#else
        return kill(cmd->pid, signal);
#endif
    }
    return 0;
}


/*
    Do non-blocking I/O - except on windows - will block
 */
PUBLIC ssize mprReadCmd(MprCmd *cmd, int channel, char *buf, ssize bufsize)
{
#if ME_WIN_LIKE
    int     rc, count;
    /*
        Need to detect EOF in windows. Pipe always in blocking mode, but reads block even with no one on the other end.
     */
    assert(cmd->files[channel].handle);
    rc = PeekNamedPipe(cmd->files[channel].handle, NULL, 0, NULL, &count, NULL);
    if (rc > 0 && count > 0) {
        return read(cmd->files[channel].fd, buf, (uint) bufsize);
    }
    if (cmd->process == 0 || WaitForSingleObject(cmd->process, 0) == WAIT_OBJECT_0) {
        /* Process has exited - EOF */
        return 0;
    }
    /* This maps to EAGAIN */
    SetLastError(WSAEWOULDBLOCK);
    return -1;

#elif VXWORKS
    /*
        Only needed when using non-blocking I/O
     */
    int     rc;

    rc = read(cmd->files[channel].fd, buf, bufsize);

    /*
        VxWorks cannot signal EOF on non-blocking pipes. Need a pattern indicator.
     */
    if (rc == MPR_CMD_VXWORKS_EOF_LEN && strncmp(buf, MPR_CMD_VXWORKS_EOF, MPR_CMD_VXWORKS_EOF_LEN) == 0) {
        /* EOF */
        return 0;

    } else if (rc == 0) {
        rc = -1;
        errno = EAGAIN;
    }
    return rc;

#else
    assert(cmd->files[channel].fd >= 0);
    return read(cmd->files[channel].fd, buf, bufsize);
#endif
}


/*
    Do non-blocking I/O - except on windows - will block
 */
PUBLIC ssize mprWriteCmd(MprCmd *cmd, int channel, cchar *buf, ssize bufsize)
{
#if ME_WIN_LIKE
    /*
        No waiting. Use this just to check if the process has exited and thus EOF on the pipe.
     */
    if (cmd->pid == 0 || WaitForSingleObject(cmd->process, 0) == WAIT_OBJECT_0) {
        return -1;
    }
#endif
    if (bufsize <= 0) {
        bufsize = slen(buf);
    }
    return write(cmd->files[channel].fd, (char*) buf, (wsize) bufsize);
}


/*
    Do blocking I/O
 */
PUBLIC ssize mprWriteCmdBlock(MprCmd *cmd, int channel, cchar *buf, ssize bufsize)
{
#if ME_UNIX_LIKE
    MprCmdFile  *file;
    ssize       wrote;

    file = &cmd->files[channel];
    fcntl(file->fd, F_SETFL, fcntl(file->fd, F_GETFL) & ~O_NONBLOCK);
    wrote = mprWriteCmd(cmd, channel, buf, bufsize);
    fcntl(file->fd, F_SETFL, fcntl(file->fd, F_GETFL) | O_NONBLOCK);
    return wrote;
#else
    return mprWriteCmd(cmd, channel, buf, bufsize);
#endif
}


PUBLIC bool mprAreCmdEventsEnabled(MprCmd *cmd, int channel)
{
    MprWaitHandler  *wp;

    int mask = (channel == MPR_CMD_STDIN) ? MPR_WRITABLE : MPR_READABLE;
    return ((wp = cmd->handlers[channel]) != 0) && (wp->desiredMask & mask);
}


PUBLIC void mprEnableCmdOutputEvents(MprCmd *cmd, bool on)
{
    int     mask;

    mask = on ? MPR_READABLE : 0;
    if (cmd->handlers[MPR_CMD_STDOUT]) {
        mprWaitOn(cmd->handlers[MPR_CMD_STDOUT], mask);
    }
    if (cmd->handlers[MPR_CMD_STDERR]) {
        mprWaitOn(cmd->handlers[MPR_CMD_STDERR], mask);
    }
}


PUBLIC void mprEnableCmdEvents(MprCmd *cmd, int channel)
{
    int mask = (channel == MPR_CMD_STDIN) ? MPR_WRITABLE : MPR_READABLE;
    if (cmd->handlers[channel]) {
        mprWaitOn(cmd->handlers[channel], mask);
    }
}


PUBLIC void mprDisableCmdEvents(MprCmd *cmd, int channel)
{
    if (cmd->handlers[channel]) {
        mprWaitOn(cmd->handlers[channel], 0);
    }
}


#if ME_WIN_LIKE
/*
    Windows only routine to wait for I/O on the channels to the gateway and the child process.
    This will queue events on the dispatcher queue when I/O occurs or the process dies.
    NOTE: NamedPipes cannot use WaitForMultipleEvents, so we dedicate a thread to polling.
    WARNING: this should not be called from a dispatcher other than cmd->dispatcher.
 */
static void pollWinCmd(MprCmd *cmd, MprTicks timeout)
{
    MprTicks        mark, delay;
    MprWaitHandler  *wp;
    int             i, rc, nbytes;

    mark = mprGetTicks();
    if (cmd->stopped) {
        timeout = 0;
    }
    slock(cmd);
    for (i = MPR_CMD_STDOUT; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->files[i].handle) {
            wp = cmd->handlers[i];
            if (wp && wp->desiredMask & MPR_READABLE) {
                rc = PeekNamedPipe(cmd->files[i].handle, NULL, 0, NULL, &nbytes, NULL);
                if (rc && nbytes > 0 || cmd->process == 0) {
                    wp->presentMask |= MPR_READABLE;
                    mprQueueIOEvent(wp);
                }
            }
        }
    }
    if (cmd->files[MPR_CMD_STDIN].handle) {
        wp = cmd->handlers[MPR_CMD_STDIN];
        if (wp && wp->desiredMask & MPR_WRITABLE) {
            wp->presentMask |= MPR_WRITABLE;
            mprQueueIOEvent(wp);
        }
    }
    if (cmd->process) {
        delay = (cmd->eofCount == cmd->requiredEof && cmd->files[MPR_CMD_STDIN].handle == 0) ? timeout : 0;
        do {
            mprYield(MPR_YIELD_STICKY);
            if (WaitForSingleObject(cmd->process, (DWORD) delay) == WAIT_OBJECT_0) {
                mprResetYield();
                reapCmd(cmd, 0);
                break;
            } else {
                mprResetYield();
            }
            delay = mprGetRemainingTicks(mark, timeout);
        } while (cmd->eofCount == cmd->requiredEof);
    }
    sunlock(cmd);
}


static void pollWinTimer(MprCmd *cmd, MprEvent *event)
{
    if (!cmd->complete) {
        pollWinCmd(cmd, 0);
    }
    if (cmd->complete) {
        mprStopContinuousEvent(event);
    }
}
#endif


/*
    Wait for a command to complete. Return 0 if the command completed, otherwise it will return MPR_ERR_TIMEOUT.
 */
PUBLIC int mprWaitForCmd(MprCmd *cmd, MprTicks timeout)
{
    MprTicks    expires, remaining, delay;
    int64       dispatcherMark;

    assert(cmd);
    if (timeout < 0) {
        timeout = MAXINT;
    }
    if (mprGetDebugMode()) {
        timeout = MAXINT;
    }
    if (cmd->stopped) {
        timeout = 0;
    }
    expires = mprGetTicks() + timeout;
    remaining = timeout;

    /* Add root to allow callers to use mprRunCmd without first managing the cmd */
    mprAddRoot(cmd);
    dispatcherMark = mprGetEventMark(cmd->dispatcher);

    while (!cmd->complete && remaining > 0) {
        if (mprShouldAbortRequests()) {
            break;
        }
        delay = (cmd->eofCount >= cmd->requiredEof) ? 10 : remaining;
        mprWaitForEvent(cmd->dispatcher, delay, dispatcherMark);
        remaining = (expires - mprGetTicks());
        dispatcherMark = mprGetEventMark(cmd->dispatcher);
    }
    mprRemoveRoot(cmd);
    if (cmd->pid) {
        return MPR_ERR_TIMEOUT;
    }
    return 0;
}


/*
    Gather the child's exit status.
    WARNING: this may be called with a false-positive, ie. SIGCHLD will get invoked for all process deaths and not just
    when this cmd has completed.
 */
static void reapCmd(MprCmd *cmd, bool finalizing)
{
    int     status, rc;

    status = 0;
    if (cmd->pid == 0) {
        return;
    }
#if ME_UNIX_LIKE
    if ((rc = waitpid(cmd->pid, &status, WNOHANG | __WALL)) < 0) {
        mprLog("error mpr cmd", 0, "Waitpid failed for pid %d, errno %d", cmd->pid, errno);

    } else if (rc == cmd->pid) {
        if (!WIFSTOPPED(status)) {
            if (WIFEXITED(status)) {
                cmd->status = WEXITSTATUS(status);
                mprDebug("mpr cmd", 5, "Process exited pid %d, status %d", cmd->pid, cmd->status);
            } else if (WIFSIGNALED(status)) {
                cmd->status = WTERMSIG(status);
            }
            cmd->pid = 0;
            assert(cmd->signal);
            mprRemoveSignalHandler(cmd->signal);
            cmd->signal = 0;
        }
    } else {
        mprDebug("mpr cmd", 5, "Still running pid %d, thread %s", cmd->pid, mprGetCurrentThreadName());
    }
#endif
#if VXWORKS
    /*
        The command exit status (cmd->status) is set in cmdTaskEntry
     */
    if (!cmd->stopped) {
        if (semTake(cmd->exitCond, MPR_TIMEOUT_STOP_TASK) != OK) {
            mprLog("error mpr cmd", 0, "Child %s did not exit, errno %d", cmd->program);
            return;
        }
    }
    semDelete(cmd->exitCond);
    cmd->exitCond = 0;
    cmd->pid = 0;
    rc = 0;
#endif
#if ME_WIN_LIKE
    if (GetExitCodeProcess(cmd->process, (ulong*) &status) == 0) {
        mprLog("error mpr cmd", 0, "GetExitProcess error");
        return;
    }
    if (status != STILL_ACTIVE) {
        cmd->status = status;
        rc = CloseHandle(cmd->process);
        assert(rc != 0);
        rc = CloseHandle(cmd->thread);
        assert(rc != 0);
        cmd->process = 0;
        cmd->thread = 0;
        cmd->pid = 0;
    }
#endif
    if (cmd->pid == 0) {
        if (cmd->eofCount >= cmd->requiredEof) {
            completeCommand(cmd);
        }
        mprDebug("mpr cmd", 5, "Process reaped: status %d, pid %d, eof %d / %d", cmd->status, cmd->pid, 
                cmd->eofCount, cmd->requiredEof);
        if (cmd->callback) {
            (cmd->callback)(cmd, -1, cmd->callbackData);
            /* WARNING - this above call may invoke httpPump and complete the request. HttpConn.tx may be null */
        }
    }
}


/*
    Default callback routine for the mprRunCmd routines. Uses may supply their own callback instead of this routine.
    The callback is run whenever there is I/O to read/write to the CGI gateway.
 */
static void defaultCmdCallback(MprCmd *cmd, int channel, void *data)
{
    MprBuf      *buf;
    ssize       len, space;
    int         errCode;

    /*
        Note: stdin, stdout and stderr are named from the client's perspective
     */
    buf = 0;
    switch (channel) {
    case MPR_CMD_STDIN:
        return;
    case MPR_CMD_STDOUT:
        buf = cmd->stdoutBuf;
        break;
    case MPR_CMD_STDERR:
        buf = cmd->stderrBuf;
        break;
    default:
        /* Child death notification */
        return;
    }
    /*
        Read and aggregate the result into a single string
     */
    space = mprGetBufSpace(buf);
    if (space < (ME_MAX_BUFFER / 4)) {
        if (mprGrowBuf(buf, ME_MAX_BUFFER) < 0) {
            mprCloseCmdFd(cmd, channel);
            return;
        }
        space = mprGetBufSpace(buf);
    }
    len = mprReadCmd(cmd, channel, mprGetBufEnd(buf), space);
    errCode = mprGetError();
#if KEEP
    mprDebug("mpr cmd", 5, "defaultCmdCallback channel %d, read len %zd, pid %d, eof %d/%d", channel, len, cmd->pid, 
            cmd->eofCount, cmd->requiredEof);
#endif
    if (len <= 0) {
        if (len == 0 || (len < 0 && !(errCode == EAGAIN || errCode == EWOULDBLOCK))) {
            mprCloseCmdFd(cmd, channel);
            return;
        }
    } else {
        mprAdjustBufEnd(buf, len);
    }
    mprAddNullToBuf(buf);
    mprEnableCmdEvents(cmd, channel);
}


static void stdinCallback(MprCmd *cmd, MprEvent *event)
{
    if (cmd->callback && cmd->files[MPR_CMD_STDIN].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDIN, cmd->callbackData);
    }
}


static void stdoutCallback(MprCmd *cmd, MprEvent *event)
{
    if (cmd->callback && cmd->files[MPR_CMD_STDOUT].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDOUT, cmd->callbackData);
    }
}


static void stderrCallback(MprCmd *cmd, MprEvent *event)
{
    if (cmd->callback && cmd->files[MPR_CMD_STDERR].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDERR, cmd->callbackData);
    }
}


PUBLIC void mprSetCmdCallback(MprCmd *cmd, MprCmdProc proc, void *data)
{
    cmd->callback = proc;
    cmd->callbackData = data;
}


PUBLIC int mprGetCmdExitStatus(MprCmd *cmd)
{
    assert(cmd);

    if (cmd->pid == 0) {
        return cmd->status;
    }
    return MPR_ERR_NOT_READY;
}


PUBLIC bool mprIsCmdRunning(MprCmd *cmd)
{
    return cmd->pid > 0;
}


/* KEEP - not yet supported */

PUBLIC void mprSetCmdTimeout(MprCmd *cmd, MprTicks timeout)
{
    assert(0);
#if KEEP
    cmd->timeoutPeriod = timeout;
#endif
}


PUBLIC int mprGetCmdFd(MprCmd *cmd, int channel)
{
    return cmd->files[channel].fd;
}


PUBLIC MprBuf *mprGetCmdBuf(MprCmd *cmd, int channel)
{
    return (channel == MPR_CMD_STDOUT) ? cmd->stdoutBuf : cmd->stderrBuf;
}


PUBLIC void mprSetCmdDir(MprCmd *cmd, cchar *dir)
{
#if VXWORKS
    mprLog("error mpr cmd", 0, "Setting working directory on VxWorks is global: %s", dir);
#else
    assert(dir && *dir);
    cmd->dir = sclone(dir);
#endif
}


#if ME_WIN_LIKE
static int sortEnv(char **str1, char **str2)
{
    cchar    *s1, *s2;
    int     c1, c2;

    for (s1 = *str1, s2 = *str2; *s1 && *s2; s1++, s2++) {
        c1 = tolower((uchar) *s1);
        c2 = tolower((uchar) *s2);
        if (c1 < c2) {
            return -1;
        } else if (c1 > c2) {
            return 1;
        }
    }
    if (*s2) {
        return -1;
    } else if (*s1) {
        return 1;
    }
    return 0;
}
#endif


/*
    Match two environment keys up to the '='
 */
static bool matchEnvKey(cchar *s1, cchar *s2)
{
    for (; *s1 && *s2; s1++, s2++) {
        if (*s1 != *s2) {
            break;
        } else if (*s1 == '=') {
            return 1;
        }
    }
    return 0;
}


static int blendEnv(MprCmd *cmd, cchar **env, int flags)
{
    cchar       **ep, *prior;
    int         next;

    cmd->env = 0;

    if ((cmd->env = mprCreateList(128, MPR_LIST_STATIC_VALUES | MPR_LIST_STABLE)) == 0) {
        return MPR_ERR_MEMORY;
    }
#if !VXWORKS
    /*
        Add prior environment to the list
     */
    if (!(flags & MPR_CMD_EXACT_ENV)) {
        for (ep = (cchar**) environ; ep && *ep; ep++) {
#if MACOSX
            if (sstarts(*ep, "DYLD_LIBRARY_PATH=")) {
                continue;
            }
#endif
            mprAddItem(cmd->env, *ep);
        }
    }
#endif
    /*
        Add new env keys. Detect and overwrite duplicates
     */
    for (ep = env; ep && *ep; ep++) {
        prior = 0;
        for (ITERATE_ITEMS(cmd->env, prior, next)) {
            if (matchEnvKey(*ep, prior)) {
                mprSetItem(cmd->env, next - 1, *ep);
                break;
            }
        }
        if (prior == 0) {
            mprAddItem(cmd->env, *ep);
        }
    }
#if ME_WIN_LIKE
    /*
        Windows requires a caseless sort with two trailing nulls
     */
    mprSortList(cmd->env, (MprSortProc) sortEnv, 0);
#endif
    mprAddItem(cmd->env, NULL);
    return 0;
}


#if ME_WIN_LIKE
static cchar *makeWinEnvBlock(MprCmd *cmd)
{
    char    *item, *dp, *ep, *env;
    ssize   len;
    int     next;

    for (len = 2, ITERATE_ITEMS(cmd->env, item, next)) {
        len += slen(item) + 1;
    }
    if ((env = mprAlloc(len)) == 0) {
        return 0;
    }
    ep = &env[len];
    dp = env;
    for (ITERATE_ITEMS(cmd->env, item, next)) {
        strcpy(dp, item);
        dp += slen(item) + 1;
    }
    /* Windows requires two nulls */
    *dp++ = '\0';
    *dp++ = '\0';
    assert(dp <= ep);
    return env;
}
#endif


/*
    Sanitize args. Convert "/" to "\" and converting '\r' and '\n' to spaces, quote all args and put the program as argv[0].
 */
static int sanitizeArgs(MprCmd *cmd, int argc, cchar **argv, cchar **env, int flags)
{
#if ME_UNIX_LIKE || VXWORKS
    cmd->argv = argv;
    cmd->argc = argc;
#endif

#if ME_WIN_LIKE
    /*
        WARNING: If starting a program compiled with Cygwin, there is a bug in Cygwin's parsing of the command
        string where embedded quotes are parsed incorrectly by the Cygwin CRT runtime. If an arg starts with a
        drive spec, embedded backquoted quotes will be stripped and the backquote will be passed in. Windows CRT
        handles this correctly.  For example:
            ./args "c:/path \"a b\"
            Cygwin will parse as  argv[1] == c:/path \a \b
            Windows will parse as argv[1] == c:/path "a b"
     */
    cchar       *saveArg0, **ap, *start, *cp;
    char        *pp, *program, *dp, *localArgv[2];
    ssize       len;
    int         quote;

    assert(argc > 0 && argv[0] != NULL);

    cmd->argv = argv;
    cmd->argc = argc;

    program = cmd->arg0 = mprAlloc(slen(argv[0]) * 2 + 1);
    strcpy(program, argv[0]);

    for (pp = program; *pp; pp++) {
        if (*pp == '/') {
            *pp = '\\';
        } else if (*pp == '\r' || *pp == '\n') {
            *pp = ' ';
        }
    }
    if (*program == '\"') {
        if ((pp = strrchr(++program, '"')) != 0) {
            *pp = '\0';
        }
    }
    if (argv == 0) {
        argv = localArgv;
        argv[1] = 0;
        saveArg0 = program;
    } else {
        saveArg0 = argv[0];
    }
    /*
        Set argv[0] to the program name while creating the command line. Restore later.
     */
    argv[0] = program;
    argc = 0;
    for (len = 0, ap = argv; *ap; ap++) {
        len += (slen(*ap) * 2) + 1 + 2;         /* Space and possible quotes and worst case backquoting */
        argc++;
    }
    cmd->command = mprAlloc(len + 1);
    cmd->command[len] = '\0';

    /*
        Add quotes around all args that have spaces and backquote double quotes.
        Example:    ["showColors", "red", "light blue", "Cannot \"render\""]
        Becomes:    "showColors" "red" "light blue" "Cannot \"render\""
     */
    dp = cmd->command;
    for (ap = &argv[0]; *ap; ) {
        start = cp = *ap;
        quote = '"';
        if (cp[0] != quote && (strchr(cp, ' ') != 0 || strchr(cp, quote) != 0)) {
            for (*dp++ = quote; *cp; ) {
                if (*cp == quote && !(cp > start && cp[-1] == '\\')) {
                    *dp++ = '\\';
                }
                *dp++ = *cp++;
            }
            *dp++ = quote;
        } else {
            strcpy(dp, cp);
            dp += strlen(cp);
        }
        if (*++ap) {
            *dp++ = ' ';
        }
    }
    *dp = '\0';
    argv[0] = saveArg0;
    mprLog("info mpr cmd", 5, "Windows command line: %s", cmd->command);
#endif /* ME_WIN_LIKE */
    return 0;
}


#if ME_WIN_LIKE
static int startProcess(MprCmd *cmd)
{
    PROCESS_INFORMATION procInfo;
    STARTUPINFO         startInfo;
    cchar               *envBlock;
    int                 err;

    memset(&startInfo, 0, sizeof(startInfo));
    startInfo.cb = sizeof(startInfo);

    startInfo.dwFlags = STARTF_USESHOWWINDOW;
    if (cmd->flags & MPR_CMD_SHOW) {
        startInfo.wShowWindow = SW_SHOW;
    } else {
        startInfo.wShowWindow = SW_HIDE;
    }
    startInfo.dwFlags |= STARTF_USESTDHANDLES;

    if (cmd->flags & MPR_CMD_IN) {
        if (cmd->files[MPR_CMD_STDIN].clientFd > 0) {
            startInfo.hStdInput = (HANDLE) _get_osfhandle(cmd->files[MPR_CMD_STDIN].clientFd);
        }
    } else {
        startInfo.hStdInput = (HANDLE) _get_osfhandle((int) fileno(stdin));
    }
    if (cmd->flags & MPR_CMD_OUT) {
        if (cmd->files[MPR_CMD_STDOUT].clientFd > 0) {
            startInfo.hStdOutput = (HANDLE)_get_osfhandle(cmd->files[MPR_CMD_STDOUT].clientFd);
        }
    } else {
        startInfo.hStdOutput = (HANDLE)_get_osfhandle((int) fileno(stdout));
    }
    if (cmd->flags & MPR_CMD_ERR) {
        if (cmd->files[MPR_CMD_STDERR].clientFd > 0) {
            startInfo.hStdError = (HANDLE) _get_osfhandle(cmd->files[MPR_CMD_STDERR].clientFd);
        }
    } else {
        startInfo.hStdError = (HANDLE) _get_osfhandle((int) fileno(stderr));
    }
    envBlock = makeWinEnvBlock(cmd);
    if (! CreateProcess(0, wide(cmd->command), 0, 0, 1, 0, (char*) envBlock, wide(cmd->dir), &startInfo, &procInfo)) {
        err = mprGetOsError();
        if (err == ERROR_DIRECTORY) {
            mprLog("error mpr cmd", 0, "Cannot create process: %s, directory %s is invalid", cmd->program, cmd->dir);
        } else {
            mprLog("error mpr cmd", 0, "Cannot create process: %s, %d", cmd->program, err);
        }
        return MPR_ERR_CANT_CREATE;
    }
    cmd->thread = procInfo.hThread;
    cmd->process = procInfo.hProcess;
    cmd->pid = procInfo.dwProcessId;
    return 0;
}


static int makeChannel(MprCmd *cmd, int index)
{
    SECURITY_ATTRIBUTES clientAtt, serverAtt, *att;
    HANDLE              readHandle, writeHandle;
    MprCmdFile          *file;
    MprTicks            now;
    char                *pipeName;
    int                 openMode, pipeMode, readFd, writeFd;
    static int          tempSeed = 0;

    memset(&clientAtt, 0, sizeof(clientAtt));
    clientAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    clientAtt.bInheritHandle = 1;

    /*
        Server fds are not inherited by the child
     */
    memset(&serverAtt, 0, sizeof(serverAtt));
    serverAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    serverAtt.bInheritHandle = 0;

    file = &cmd->files[index];
    now = ((int) mprGetTicks() & 0xFFFF) % 64000;

    lock(MPR->cmdService);
    pipeName = sfmt("\\\\.\\pipe\\MPR_%d_%d_%d.tmp", getpid(), (int) now, ++tempSeed);
    unlock(MPR->cmdService);

    /*
        Pipes are always inbound. The file below is outbound. we swap whether the client or server
        inherits the pipe or file. MPR_CMD_STDIN is the clients input pipe.
        Pipes are blocking since both ends share the same blocking mode. Client must be blocking.
     */
    openMode = PIPE_ACCESS_INBOUND;
    pipeMode = 0;

    att = (index == MPR_CMD_STDIN) ? &clientAtt : &serverAtt;
    readHandle = CreateNamedPipe(wide(pipeName), openMode, pipeMode, 1, 0, 256 * 1024, 1, att);
    if (readHandle == INVALID_HANDLE_VALUE) {
        mprLog("error mpr cmd", 0, "Cannot create stdio pipes %s. Err %d", pipeName, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    readFd = (int) (int64) _open_osfhandle((long) readHandle, 0);

    att = (index == MPR_CMD_STDIN) ? &serverAtt: &clientAtt;
    writeHandle = CreateFile(wide(pipeName), GENERIC_WRITE, 0, att, OPEN_EXISTING, openMode, 0);
    writeFd = (int) _open_osfhandle((long) writeHandle, 0);

    if (readFd < 0 || writeFd < 0) {
        mprLog("error mpr cmd", 0, "Cannot create stdio pipes %s. Err %d", pipeName, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (index == MPR_CMD_STDIN) {
        file->clientFd = readFd;
        file->fd = writeFd;
        file->handle = writeHandle;
    } else {
        file->clientFd = writeFd;
        file->fd = readFd;
        file->handle = readHandle;
    }
    return 0;
}


#elif ME_UNIX_LIKE
static int makeChannel(MprCmd *cmd, int index)
{
    MprCmdFile      *file;
    int             fds[2];

    file = &cmd->files[index];

    if (pipe(fds) < 0) {
        mprLog("error mpr cmd", 0, "Cannot create stdio pipes. Err %d", mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (index == MPR_CMD_STDIN) {
        file->clientFd = fds[0];        /* read fd */
        file->fd = fds[1];              /* write fd */
    } else {
        file->clientFd = fds[1];        /* write fd */
        file->fd = fds[0];              /* read fd */
    }
    fcntl(file->fd, F_SETFL, fcntl(file->fd, F_GETFL) | O_NONBLOCK);
    return 0;
}

#elif VXWORKS
static int makeChannel(MprCmd *cmd, int index)
{
    MprCmdFile      *file;
    int             nonBlock;
    static int      tempSeed = 0;

    file = &cmd->files[index];
    file->name = sfmt("/pipe/%s_%d_%d", ME_NAME, taskIdSelf(), tempSeed++);

    if (pipeDevCreate(file->name, 5, ME_MAX_BUFFER) < 0) {
        mprLog("error mpr cmd", 0, "Cannot create pipes to run %s", cmd->program);
        return MPR_ERR_CANT_OPEN;
    }
    /*
        Open the server end of the pipe. MPR_CMD_STDIN is from the client's perspective.
     */
    if (index == MPR_CMD_STDIN) {
        file->fd = open(file->name, O_WRONLY, 0644);
    } else {
        file->fd = open(file->name, O_RDONLY, 0644);
    }
    if (file->fd < 0) {
        mprLog("error mpr cmd", 0, "Cannot create stdio pipes. Err %d", mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    nonBlock = 1;
    ioctl(file->fd, FIONBIO, (int) &nonBlock);
    return 0;
}
#endif


#if ME_UNIX_LIKE
/*
    Called on the cmd dispatcher in response to a child death
 */
static void cmdChildDeath(MprCmd *cmd, MprSignal *sp)
{
    reapCmd(cmd, 0);
}


static int startProcess(MprCmd *cmd)
{
    MprCmdFile      *files;
    int             i;

    files = cmd->files;
    if (!cmd->signal) {
        cmd->signal = mprAddSignalHandler(SIGCHLD, cmdChildDeath, cmd, cmd->dispatcher, MPR_SIGNAL_BEFORE);
    }
    /*
        Create the child
     */
    cmd->pid = vfork();

    if (cmd->pid < 0) {
        mprLog("error mpr cmd", 0, "Cannot fork a new process to run %s, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_INITIALIZE;

    } else if (cmd->pid == 0) {
        /*
            Child
         */
        umask(022);
        if (cmd->flags & MPR_CMD_NEW_SESSION) {
            setsid();
        }
        if (cmd->dir) {
            if (chdir(cmd->dir) < 0) {
                mprLog("error mpr cmd", 0, "Cannot change directory to %s", cmd->dir);
                return MPR_ERR_CANT_INITIALIZE;
            }
        }
        if (cmd->flags & MPR_CMD_IN) {
            if (files[MPR_CMD_STDIN].clientFd >= 0) {
                dup2(files[MPR_CMD_STDIN].clientFd, 0);
                close(files[MPR_CMD_STDIN].fd);
            } else {
                close(0);
            }
        }
        if (cmd->flags & MPR_CMD_OUT) {
            if (files[MPR_CMD_STDOUT].clientFd >= 0) {
                dup2(files[MPR_CMD_STDOUT].clientFd, 1);
                close(files[MPR_CMD_STDOUT].fd);
            } else {
                close(1);
            }
        }
        if (cmd->flags & MPR_CMD_ERR) {
            if (files[MPR_CMD_STDERR].clientFd >= 0) {
                dup2(files[MPR_CMD_STDERR].clientFd, 2);
                close(files[MPR_CMD_STDERR].fd);
            } else {
                close(2);
            }
        }
        cmd->forkCallback(cmd->forkData);
        if (cmd->env) {
            (void) execve(cmd->program, (char**) cmd->argv, (char**) &cmd->env->items[0]);
        } else {
            (void) execv(cmd->program, (char**) cmd->argv);
        }
        /*
            Use _exit to avoid flushing I/O any other I/O.
         */
        _exit(-(MPR_ERR_CANT_INITIALIZE));

    } else {
        /*
            Close the client handles
         */
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            if (files[i].clientFd >= 0) {
                close(files[i].clientFd);
                files[i].clientFd = -1;
            }
        }
    }
    return 0;
}


#elif VXWORKS
/*
    Start the command to run (stdIn and stdOut are named from the client's perspective)
 */
PUBLIC int startProcess(MprCmd *cmd)
{
    MprCmdTaskFn    entryFn;
    MprModule       *mp;
    char            *entryPoint, *program, *pair;
    int             pri, next;

    mprLog("info mpr cmd", 4, "Program %s", cmd->program);
    entryPoint = 0;
    if (cmd->env) {
        for (ITERATE_ITEMS(cmd->env, pair, next)) {
            if (sncmp(pair, "entryPoint=", 11) == 0) {
                entryPoint = sclone(&pair[11]);
            }
        }
    }
    program = mprGetPathBase(cmd->program);
    if (entryPoint == 0) {
        program = mprTrimPathExt(program);
        entryPoint = program;
    }
#if ME_CPU_ARCH == MPR_CPU_IX86 || ME_CPU_ARCH == MPR_CPU_IX64 || ME_CPU_ARCH == MPR_CPU_SH
    /*
        A leading underscore is required on some architectures
     */
    entryPoint = sjoin("_", entryPoint, NULL);
#endif
    if (mprFindVxSym(sysSymTbl, entryPoint, (char**) (void*) &entryFn) < 0) {
        if ((mp = mprCreateModule(cmd->program, cmd->program, NULL, NULL)) == 0) {
            mprLog("error mpr cmd", 0, "Cannot create module");
            return MPR_ERR_CANT_CREATE;
        }
        if (mprLoadModule(mp) < 0) {
            mprLog("error mpr cmd", 0, "Cannot load DLL %s, errno %d", program, mprGetOsError());
            return MPR_ERR_CANT_READ;
        }
        if (mprFindVxSym(sysSymTbl, entryPoint, (char**) (void*) &entryFn) < 0) {
            mprLog("error mpr cmd", 0, "Cannot find symbol %s, errno %d", entryPoint, mprGetOsError());
            return MPR_ERR_CANT_ACCESS;
        }
    }
    taskPriorityGet(taskIdSelf(), &pri);

    cmd->pid = taskSpawn(entryPoint, pri, VX_FP_TASK | VX_PRIVATE_ENV, ME_STACK_SIZE, (FUNCPTR) cmdTaskEntry,
        (int) cmd->program, (int) entryFn, (int) cmd, 0, 0, 0, 0, 0, 0, 0);

    if (cmd->pid < 0) {
        mprLog("error mpr cmd", 0, "Cannot create task %s, errno %d", entryPoint, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (semTake(cmd->startCond, MPR_TIMEOUT_START_TASK) != OK) {
        mprLog("error mpr cmd", 0, "Child %s did not initialize, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    semDelete(cmd->startCond);
    cmd->startCond = 0;
    return 0;
}


/*
    Executed by the child process
 */
static void cmdTaskEntry(char *program, MprCmdTaskFn entry, int cmdArg)
{
    MprCmd          *cmd;
    MprCmdFile      *files;
    WIND_TCB        *tcb;
    char            *item;
    int             inFd, outFd, errFd, id, next;

    cmd = (MprCmd*) cmdArg;

    /*
        Open standard I/O files (in/out are from the server's perspective)
     */
    files = cmd->files;
    inFd = open(files[MPR_CMD_STDIN].name, O_RDONLY, 0666);
    outFd = open(files[MPR_CMD_STDOUT].name, O_WRONLY, 0666);
    errFd = open(files[MPR_CMD_STDERR].name, O_WRONLY, 0666);

    if (inFd < 0 || outFd < 0 || errFd < 0) {
        exit(255);
    }
    id = taskIdSelf();
    ioTaskStdSet(id, 0, inFd);
    ioTaskStdSet(id, 1, outFd);
    ioTaskStdSet(id, 2, errFd);

    /*
        Now that we have opened the stdin and stdout, wakeup our parent.
     */
    semGive(cmd->startCond);

    /*
        Create the environment
     */
    if (envPrivateCreate(id, -1) < 0) {
        exit(254);
    }
    for (ITERATE_ITEMS(cmd->env, item, next)) {
        putenv(item);
    }

#if !VXWORKS
{
    char    *dir;
    int     rc;

    /*
        Set current directory if required
        WARNING: Setting working directory on VxWorks is global
     */
    if (cmd->dir) {
        rc = chdir(cmd->dir);
    } else {
        dir = mprGetPathDir(cmd->program);
        rc = chdir(dir);
    }
    if (rc < 0) {
        mprLog("error mpr cmd", 0, "Cannot change directory to %s", cmd->dir);
        exit(255);
    }
}
#endif

    /*
        Call the user's entry point
     */
    (entry)(cmd->argc, (char**) cmd->argv, (char**) cmd->env);

    tcb = taskTcb(id);
    cmd->status = tcb->exitCode;

    /*
        Cleanup
     */
    envPrivateDestroy(id);
    close(inFd);
    close(outFd);
    close(errFd);
    semGive(cmd->exitCond);
}


#endif /* VXWORKS */


static void closeFiles(MprCmd *cmd)
{
    int     i;
    for (i = 3; i < MPR_MAX_FILE; i++) {
        close(i);
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/cond.c ************/


/**
    cond.c - Thread Conditional variables

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */



/***************************** Forward Declarations ***************************/

static void manageCond(MprCond *cp, int flags);

/************************************ Code ************************************/
/*
    Create a condition variable for use by single or multiple waiters
 */

PUBLIC MprCond *mprCreateCond()
{
    MprCond     *cp;

    if ((cp = mprAllocObjNoZero(MprCond, manageCond)) == 0) {
        return 0;
    }
    cp->triggered = 0;
    cp->mutex = mprCreateLock();

#if ME_WIN_LIKE
    cp->cv = CreateEvent(NULL, FALSE, FALSE, NULL);
#elif VXWORKS
    cp->cv = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#else
    pthread_cond_init(&cp->cv, NULL);
#endif
    return cp;
}


static void manageCond(MprCond *cp, int flags)
{
    assert(cp);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(cp->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        assert(cp->mutex);
#if ME_WIN_LIKE
        CloseHandle(cp->cv);
#elif VXWORKS
        semDelete(cp->cv);
#else
        pthread_cond_destroy(&cp->cv);
#endif
    }
}


/*
    Wait for the event to be triggered. Should only be used when there are single waiters. If the event is already
    triggered, then it will return immediately. Timeout of -1 means wait forever. Timeout of 0 means no wait.
    Returns 0 if the event was signalled. Returns < 0 for a timeout.

    WARNING: On unix, the pthread_cond_timedwait uses an absolute time (Ugh!). So time-warps for daylight-savings may
    cause waits to prematurely return.
 */
PUBLIC int mprWaitForCond(MprCond *cp, MprTicks timeout)
{
    MprTicks            now, expire;
    int                 rc;
#if ME_UNIX_LIKE
    struct timespec     waitTill;
    struct timeval      current;
    int                 usec;
#endif
    /*
        Avoid doing a mprGetTicks() if timeout is < 0
     */
    rc = 0;
    if (timeout >= 0) {
        now = mprGetTicks();
        expire = now + timeout;
#if ME_UNIX_LIKE
        gettimeofday(&current, NULL);
        usec = current.tv_usec + ((int) (timeout % 1000)) * 1000;
        waitTill.tv_sec = current.tv_sec + ((int) (timeout / 1000)) + (usec / 1000000);
        waitTill.tv_nsec = (usec % 1000000) * 1000;
#endif
    } else {
        expire = -1;
        now = 0;
    }
    mprLock(cp->mutex);
    /*
        NOTE: The WaitForSingleObject and semTake APIs keeps state as to whether the object is signalled.
        WaitForSingleObject and semTake will not block if the object is already signalled. However, pthread_cond_
        is different and does not keep such state. If it is signalled before pthread_cond_wait, the thread will
        still block. Consequently we need to keep our own state in cp->triggered. This also protects against
        spurious wakeups which can happen (on windows).
     */
    do {
#if ME_WIN_LIKE
        /*
            Regardless of the state of cp->triggered, we must call WaitForSingleObject to consume the signalled
            internal state of the object.
         */
        mprUnlock(cp->mutex);
        rc = WaitForSingleObject(cp->cv, (int) (expire - now));
        mprLock(cp->mutex);
        if (rc == WAIT_OBJECT_0) {
            rc = 0;
            ResetEvent(cp->cv);
        } else if (rc == WAIT_TIMEOUT) {
            rc = MPR_ERR_TIMEOUT;
        } else {
            rc = MPR_ERR;
        }
#elif VXWORKS
        /*
            Regardless of the state of cp->triggered, we must call semTake to consume the semaphore signalled state
         */
        mprUnlock(cp->mutex);
        rc = semTake(cp->cv, (int) (expire - now));
        mprLock(cp->mutex);
        if (rc != 0) {
            if (errno == S_objLib_OBJ_UNAVAILABLE) {
                rc = MPR_ERR_TIMEOUT;
            } else {
                rc = MPR_ERR;
            }
        }

#elif ME_UNIX_LIKE
        /*
            The pthread_cond_wait routines will atomically unlock the mutex before sleeping and will relock on awakening.
            WARNING: pthreads may do spurious wakeups without being triggered
         */
        if (!cp->triggered) {
            do {
                if (now) {
                    rc = pthread_cond_timedwait(&cp->cv, &cp->mutex->cs,  &waitTill);
                } else {
                    rc = pthread_cond_wait(&cp->cv, &cp->mutex->cs);
                }
            } while ((rc == 0 || rc == EAGAIN) && !cp->triggered);
            if (rc == ETIMEDOUT) {
                rc = MPR_ERR_TIMEOUT;
            } else if (rc == EAGAIN) {
                rc = 0;
            } else if (rc != 0) {
                mprLog("error mpr thread", 0, "pthread_cond_timedwait error rc %d", rc);
                rc = MPR_ERR;
            }
        }
#endif
    } while (!cp->triggered && rc == 0 && (!now || (now = mprGetTicks()) < expire));

    if (cp->triggered) {
        cp->triggered = 0;
        rc = 0;
    } else if (rc == 0) {
        rc = MPR_ERR_TIMEOUT;
    }
    mprUnlock(cp->mutex);
    return rc;
}


/*
    Signal a condition and wakeup the waiter. Note: this may be called prior to the waiter waiting.
 */
PUBLIC void mprSignalCond(MprCond *cp)
{
    mprLock(cp->mutex);
    if (!cp->triggered) {
        cp->triggered = 1;
#if ME_WIN_LIKE
        SetEvent(cp->cv);
#elif VXWORKS
        semGive(cp->cv);
#else
        pthread_cond_signal(&cp->cv);
#endif
    }
    mprUnlock(cp->mutex);
}


PUBLIC void mprResetCond(MprCond *cp)
{
    mprLock(cp->mutex);
    cp->triggered = 0;
#if ME_WIN_LIKE
    ResetEvent(cp->cv);
#elif VXWORKS
    semDelete(cp->cv);
    cp->cv = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#else
    pthread_cond_destroy(&cp->cv);
    pthread_cond_init(&cp->cv, NULL);
#endif
    mprUnlock(cp->mutex);
}


/*
    Wait for the event to be triggered when there may be multiple waiters. This routine may return early due to
    other signals or events. The caller must verify if the signalled condition truly exists. If the event is already
    triggered, then it will return immediately. This call will not reset cp->triggered and must be reset manually.
    A timeout of -1 means wait forever. Timeout of 0 means no wait.  Returns 0 if the event was signalled.
    Returns < 0 for a timeout.

    WARNING: On unix, the pthread_cond_timedwait uses an absolute time (Ugh!). So time-warps for daylight-savings may
    cause waits to prematurely return.
 */
PUBLIC int mprWaitForMultiCond(MprCond *cp, MprTicks timeout)
{
    int         rc;
#if ME_UNIX_LIKE
    struct timespec     waitTill;
    struct timeval      current;
    int                 usec;
#else
    MprTicks            now, expire;
#endif

    if (timeout < 0) {
        timeout = MAXINT;
    }
#if ME_UNIX_LIKE
    gettimeofday(&current, NULL);
    usec = current.tv_usec + ((int) (timeout % 1000)) * 1000;
    waitTill.tv_sec = current.tv_sec + ((int) (timeout / 1000)) + (usec / 1000000);
    waitTill.tv_nsec = (usec % 1000000) * 1000;
#else
    now = mprGetTicks();
    expire = now + timeout;
#endif

#if ME_WIN_LIKE
    rc = WaitForSingleObject(cp->cv, (int) (expire - now));
    if (rc == WAIT_OBJECT_0) {
        rc = 0;
    } else if (rc == WAIT_TIMEOUT) {
        rc = MPR_ERR_TIMEOUT;
    } else {
        rc = MPR_ERR;
    }
#elif VXWORKS
    rc = semTake(cp->cv, (int) (expire - now));
    if (rc != 0) {
        if (errno == S_objLib_OBJ_UNAVAILABLE) {
            rc = MPR_ERR_TIMEOUT;
        } else {
            rc = MPR_ERR;
        }
    }
#elif ME_UNIX_LIKE
    mprLock(cp->mutex);
    rc = pthread_cond_timedwait(&cp->cv, &cp->mutex->cs,  &waitTill);
    if (rc == ETIMEDOUT) {
        rc = MPR_ERR_TIMEOUT;
    } else if (rc != 0) {
        assert(rc == 0);
        rc = MPR_ERR;
    }
    mprUnlock(cp->mutex);
#endif
    return rc;
}


/*
    Signal a condition and wakeup the all the waiters. Note: this may be called before or after to the waiter waiting.
 */
PUBLIC void mprSignalMultiCond(MprCond *cp)
{
    mprLock(cp->mutex);
#if ME_WIN_LIKE
    /* Pulse event */
    SetEvent(cp->cv);
    ResetEvent(cp->cv);
#elif VXWORKS
    /* Reset sem count and then give once. Prevents accumulation */
    while (semTake(cp->cv, 0) == OK) ;
    semGive(cp->cv);
    semFlush(cp->cv);
#else
    pthread_cond_broadcast(&cp->cv);
#endif
    mprUnlock(cp->mutex);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/crypt.c ************/


/*
    crypt.c - Base-64 encoding and decoding and MD5 support.

    Algorithms by RSA. See license at the end of the file. 
    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Locals ***********************************/

#define BLOWFISH_SALT_LENGTH   16
#define BLOWFISH_ROUNDS        128

/*
    MD5 Constants for transform routine.
 */
#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

static uchar PADDING[64] = {
  0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/*
   MD5 F, G, H and I are basic MD5 functions.
 */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

/*
   MD5 ROTATE_LEFT rotates x left n bits.
 */
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

/*
     MD5 - FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4.
     Rotation is separate from addition to prevent recomputation.
 */
#define FF(a, b, c, d, x, s, ac) { \
    (a) += F ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define GG(a, b, c, d, x, s, ac) { \
    (a) += G ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define HH(a, b, c, d, x, s, ac) { \
    (a) += H ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define II(a, b, c, d, x, s, ac) { \
    (a) += I ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}

typedef struct {
    uint state[4];
    uint count[2];
    uchar buffer[64];
} MD5CONTEXT;

/******************************* Base 64 Data *********************************/

#define CRYPT_HASH_SIZE   16

/*
    Encoding map lookup
 */
static char encodeMap[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};


/*
    Decode map
 */
static signed char decodeMap[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, 
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};


#define SHA_SIZE   20

typedef struct MprSha {
    uint    hash[SHA_SIZE / 4];     /* Message Digest */
    uint    lowLength;              /* Message length in bits */
    uint    highLength;             /* Message length in bits */
    int     index;                  /* Index into message block array   */
    uchar   block[64];              /* 512-bit message blocks */
} MprSha;

#define shaShift(bits,word) (((word) << (bits)) | ((word) >> (32-(bits))))

/*************************** Forward Declarations *****************************/

static void decode(uint *output, uchar *input, uint len);
static void encode(uchar *output, uint *input, uint len);
static void finalizeMD5(uchar digest[16], MD5CONTEXT *context);
static void initMD5(MD5CONTEXT *context);
static void transform(uint state[4], uchar block[64]);
static void update(MD5CONTEXT *context, uchar *input, uint inputLen);

static void shaInit(MprSha *sha);
static void shaUpdate(MprSha *sha, cuchar *msg, ssize len);
static void shaFinalize(uchar *digest, MprSha *sha);
static void shaPad(MprSha *sha);
static void shaProcess(MprSha *sha);

/*********************************** Code *************************************/

PUBLIC int mprRandom()
{
#if WINDOWS || VXWORKS
    return rand();
#else
    return (int) random();
#endif
}


PUBLIC char *mprGetRandomString(ssize size)
{
    MprTicks    now;
    char        *hex = "0123456789abcdef";
    char        *bytes, *ascii, *ap, *cp, *bp;
    ssize       len;
    int         i, pid;

    len = size / 2;
    bytes = mprAlloc(size / 2);
    ascii = mprAlloc(size + 1);

    if (mprGetRandomBytes(bytes, sizeof(bytes), 0) < 0) {
        mprLog("critical mpr", 0, "Failed to get random bytes");
        now = mprGetTime();
        pid = (int) getpid();
        cp = (char*) &now;
        bp = bytes;
        for (i = 0; i < sizeof(now) && bp < &bytes[len]; i++) {
            *bp++= *cp++;
        }
        cp = (char*) &now;
        for (i = 0; i < sizeof(pid) && bp < &bytes[len]; i++) {
            *bp++ = *cp++;
        }
    }
    ap = ascii;
    for (i = 0; i < len; i++) {
        *ap++ = hex[((uchar) bytes[i]) >> 4];
        *ap++ = hex[((uchar) bytes[i]) & 0xf];
    }
    *ap = '\0';
    return ascii;
}


/*
    Decode a null terminated string and returns a null terminated string.
    Stops decoding at the end of string or '='
 */
PUBLIC char *mprDecode64(cchar *s)
{
    return mprDecode64Block(s, NULL, MPR_DECODE_TOKEQ);
}


/*
    Decode a null terminated string and return a block with length.
    Stops decoding at the end of the block or '=' if MPR_DECODE_TOKEQ is specified.
 */
PUBLIC char *mprDecode64Block(cchar *s, ssize *len, int flags)
{
    uint    bitBuf;
    char    *buffer, *bp;
    cchar   *end;
    ssize   size;
    int     c, i, j, shift;

    size = slen(s);
    if ((buffer = mprAlloc(size + 1)) == 0) {
        return NULL;
    }
    bp = buffer;
    *bp = '\0';
    end = &s[size];
    while (s < end && (*s != '=' || !(flags & MPR_DECODE_TOKEQ))) {
        bitBuf = 0;
        shift = 18;
        for (i = 0; i < 4 && (s < end && (*s != '=' || !(flags & MPR_DECODE_TOKEQ))); i++, s++) {
            c = decodeMap[*s & 0xff];
            if (c == -1) {
                return NULL;
            } 
            bitBuf = bitBuf | (c << shift);
            shift -= 6;
        }
        --i;
        assert((bp + i) < &buffer[size]);
        for (j = 0; j < i; j++) {
            *bp++ = (char) ((bitBuf >> (8 * (2 - j))) & 0xff);
        }
        *bp = '\0';
    }
    if (len) {
        *len = bp - buffer;
    }
    return buffer;
}


/*
    Encode a null terminated string.
    Returns a null terminated block
 */
PUBLIC char *mprEncode64(cchar *s)
{
    return mprEncode64Block(s, slen(s));
}


/*
    Encode a block of a given length
    Returns a null terminated block
 */
PUBLIC char *mprEncode64Block(cchar *s, ssize len)
{
    uint    shiftbuf;
    char    *buffer, *bp;
    cchar   *end;
    ssize   size;
    int     i, j, shift;

    size = len * 2;
    if ((buffer = mprAlloc(size + 1)) == 0) {
        return NULL;
    }
    bp = buffer;
    *bp = '\0';
    end = &s[len];
    while (s < end) {
        shiftbuf = 0;
        for (j = 2; s < end && j >= 0; j--, s++) {
            shiftbuf |= ((*s & 0xff) << (j * 8));
        }
        shift = 18;
        for (i = ++j; i < 4 && bp < &buffer[size] ; i++) {
            *bp++ = encodeMap[(shiftbuf >> shift) & 0x3f];
            shift -= 6;
        }
        while (j-- > 0) {
            *bp++ = '=';
        }
        *bp = '\0';
    }
    return buffer;
}


PUBLIC char *mprGetMD5(cchar *s)
{
    return mprGetMD5WithPrefix(s, slen(s), NULL);
}


/*
    Return the MD5 hash of a block. Returns allocated string. A prefix for the result can be supplied.
 */
PUBLIC char *mprGetMD5WithPrefix(cchar *buf, ssize length, cchar *prefix)
{
    MD5CONTEXT      context;
    uchar           hash[CRYPT_HASH_SIZE];
    cchar           *hex = "0123456789abcdef";
    char            *r, *str;
    char            result[(CRYPT_HASH_SIZE * 2) + 1];
    ssize           len;
    int             i;

    if (length < 0) {
        length = slen(buf);
    }
    initMD5(&context);
    update(&context, (uchar*) buf, (uint) length);
    finalizeMD5(hash, &context);

    for (i = 0, r = result; i < 16; i++) {
        *r++ = hex[hash[i] >> 4];
        *r++ = hex[hash[i] & 0xF];
    }
    *r = '\0';
    len = (prefix) ? slen(prefix) : 0;
    str = mprAlloc(sizeof(result) + len);
    if (str) {
        if (prefix) {
            strcpy(str, prefix);
        }
        strcpy(str + len, result);
    }
    return str;
}


/*
    MD5 initialization. Begins an MD5 operation, writing a new context.
 */ 
static void initMD5(MD5CONTEXT *context)
{
    context->count[0] = context->count[1] = 0;
    context->state[0] = 0x67452301;
    context->state[1] = 0xefcdab89;
    context->state[2] = 0x98badcfe;
    context->state[3] = 0x10325476;
}


/*
    MD5 block update operation. Continues an MD5 message-digest operation, processing another message block, 
    and updating the context.
 */
static void update(MD5CONTEXT *context, uchar *input, uint inputLen)
{
    uint    i, index, partLen;

    index = (uint) ((context->count[0] >> 3) & 0x3F);

    if ((context->count[0] += ((uint)inputLen << 3)) < ((uint)inputLen << 3)){
        context->count[1]++;
    }
    context->count[1] += ((uint)inputLen >> 29);
    partLen = 64 - index;

    if (inputLen >= partLen) {
        memcpy((uchar*) &context->buffer[index], (uchar*) input, partLen);
        transform(context->state, context->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64) {
            transform(context->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    memcpy((uchar*) &context->buffer[index], (uchar*) &input[i], inputLen-i);
}


/*
    MD5 finalization. Ends an MD5 message-digest operation, writing the message digest and zeroizing the context.
 */ 
static void finalizeMD5(uchar digest[16], MD5CONTEXT *context)
{
    uchar   bits[8];
    uint    index, padLen;

    /* Save number of bits */
    encode(bits, context->count, 8);

    /* Pad out to 56 mod 64. */
    index = (uint)((context->count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    update(context, PADDING, padLen);

    /* Append length (before padding) */
    update(context, bits, 8);
    /* Store state in digest */
    encode(digest, context->state, 16);

    /* Zero sensitive information. */
    memset((uchar*)context, 0, sizeof (*context));
}


/*
    MD5 basic transformation. Transforms state based on block.
 */
static void transform(uint state[4], uchar block[64])
{
    uint a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    decode(x, block, 64);

    /* Round 1 */
    FF(a, b, c, d, x[ 0], S11, 0xd76aa478); /* 1 */
    FF(d, a, b, c, x[ 1], S12, 0xe8c7b756); /* 2 */
    FF(c, d, a, b, x[ 2], S13, 0x242070db); /* 3 */
    FF(b, c, d, a, x[ 3], S14, 0xc1bdceee); /* 4 */
    FF(a, b, c, d, x[ 4], S11, 0xf57c0faf); /* 5 */
    FF(d, a, b, c, x[ 5], S12, 0x4787c62a); /* 6 */
    FF(c, d, a, b, x[ 6], S13, 0xa8304613); /* 7 */
    FF(b, c, d, a, x[ 7], S14, 0xfd469501); /* 8 */
    FF(a, b, c, d, x[ 8], S11, 0x698098d8); /* 9 */
    FF(d, a, b, c, x[ 9], S12, 0x8b44f7af); /* 10 */
    FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
    FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
    FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
    FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
    FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
    FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

    /* Round 2 */
    GG(a, b, c, d, x[ 1], S21, 0xf61e2562); /* 17 */
    GG(d, a, b, c, x[ 6], S22, 0xc040b340); /* 18 */
    GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
    GG(b, c, d, a, x[ 0], S24, 0xe9b6c7aa); /* 20 */
    GG(a, b, c, d, x[ 5], S21, 0xd62f105d); /* 21 */
    GG(d, a, b, c, x[10], S22,  0x2441453); /* 22 */
    GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
    GG(b, c, d, a, x[ 4], S24, 0xe7d3fbc8); /* 24 */
    GG(a, b, c, d, x[ 9], S21, 0x21e1cde6); /* 25 */
    GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
    GG(c, d, a, b, x[ 3], S23, 0xf4d50d87); /* 27 */
    GG(b, c, d, a, x[ 8], S24, 0x455a14ed); /* 28 */
    GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
    GG(d, a, b, c, x[ 2], S22, 0xfcefa3f8); /* 30 */
    GG(c, d, a, b, x[ 7], S23, 0x676f02d9); /* 31 */
    GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

    /* Round 3 */
    HH(a, b, c, d, x[ 5], S31, 0xfffa3942); /* 33 */
    HH(d, a, b, c, x[ 8], S32, 0x8771f681); /* 34 */
    HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
    HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
    HH(a, b, c, d, x[ 1], S31, 0xa4beea44); /* 37 */
    HH(d, a, b, c, x[ 4], S32, 0x4bdecfa9); /* 38 */
    HH(c, d, a, b, x[ 7], S33, 0xf6bb4b60); /* 39 */
    HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
    HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
    HH(d, a, b, c, x[ 0], S32, 0xeaa127fa); /* 42 */
    HH(c, d, a, b, x[ 3], S33, 0xd4ef3085); /* 43 */
    HH(b, c, d, a, x[ 6], S34,  0x4881d05); /* 44 */
    HH(a, b, c, d, x[ 9], S31, 0xd9d4d039); /* 45 */
    HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
    HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
    HH(b, c, d, a, x[ 2], S34, 0xc4ac5665); /* 48 */

    /* Round 4 */
    II(a, b, c, d, x[ 0], S41, 0xf4292244); /* 49 */
    II(d, a, b, c, x[ 7], S42, 0x432aff97); /* 50 */
    II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
    II(b, c, d, a, x[ 5], S44, 0xfc93a039); /* 52 */
    II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
    II(d, a, b, c, x[ 3], S42, 0x8f0ccc92); /* 54 */
    II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
    II(b, c, d, a, x[ 1], S44, 0x85845dd1); /* 56 */
    II(a, b, c, d, x[ 8], S41, 0x6fa87e4f); /* 57 */
    II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
    II(c, d, a, b, x[ 6], S43, 0xa3014314); /* 59 */
    II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
    II(a, b, c, d, x[ 4], S41, 0xf7537e82); /* 61 */
    II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
    II(c, d, a, b, x[ 2], S43, 0x2ad7d2bb); /* 63 */
    II(b, c, d, a, x[ 9], S44, 0xeb86d391); /* 64 */

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    /* Zero sensitive information. */
    memset((uchar*) x, 0, sizeof(x));
}


/*
    Encodes input(uint) into output(uchar). Assumes len is a multiple of 4.
 */
static void encode(uchar *output, uint *input, uint len)
{
    uint i, j;

    for (i = 0, j = 0; j < len; i++, j += 4) {
        output[j] = (uchar) (input[i] & 0xff);
        output[j+1] = (uchar) ((input[i] >> 8) & 0xff);
        output[j+2] = (uchar) ((input[i] >> 16) & 0xff);
        output[j+3] = (uchar) ((input[i] >> 24) & 0xff);
    }
}


/*
    Decodes input(uchar) into output(uint). Assumes len is a multiple of 4.
 */
static void decode(uint *output, uchar *input, uint len)
{
    uint    i, j;

    for (i = 0, j = 0; j < len; i++, j += 4)
        output[i] = ((uint) input[j]) | (((uint) input[j+1]) << 8) | (((uint) input[j+2]) << 16) | 
            (((uint) input[j+3]) << 24);
}


/************************************* Sha1 **********************************/

PUBLIC char *mprGetSHA(cchar *s)
{
    return mprGetSHAWithPrefix(s, slen(s), NULL);
}


PUBLIC char *mprGetSHABase64(cchar *s)
{
    MprSha  sha;
    uchar   hash[SHA_SIZE + 1];

    shaInit(&sha);
    shaUpdate(&sha, (cuchar*) s, slen(s));
    shaFinalize(hash, &sha);
    hash[SHA_SIZE] = '\0';
    return mprEncode64Block((char*) hash, SHA_SIZE);
}


PUBLIC char *mprGetSHAWithPrefix(cchar *buf, ssize length, cchar *prefix)
{
    MprSha  sha;
    uchar   hash[SHA_SIZE];
    cchar   *hex = "0123456789abcdef";
    char    *r, *str;
    char    result[(SHA_SIZE * 2) + 1];
    ssize   len;
    int     i;

    if (length < 0) {
        length = slen(buf);
    }
    shaInit(&sha);
    shaUpdate(&sha, (cuchar*) buf, length);
    shaFinalize(hash, &sha);

    for (i = 0, r = result; i < SHA_SIZE; i++) {
        *r++ = hex[hash[i] >> 4];
        *r++ = hex[hash[i] & 0xF];
    }
    *r = '\0';
    len = (prefix) ? slen(prefix) : 0;
    str = mprAlloc(sizeof(result) + len);
    if (str) {
        if (prefix) {
            strcpy(str, prefix);
        }
        strcpy(str + len, result);
    }
    return str;
}


static void shaInit(MprSha *sha)
{
    sha->lowLength = 0;
    sha->highLength = 0;
    sha->index = 0;
    sha->hash[0] = 0x67452301;
    sha->hash[1] = 0xEFCDAB89;
    sha->hash[2] = 0x98BADCFE;
    sha->hash[3] = 0x10325476;
    sha->hash[4] = 0xC3D2E1F0;
}


static void shaUpdate(MprSha *sha, cuchar *msg, ssize len)
{
    while (len--) {
        sha->block[sha->index++] = (*msg & 0xFF);
        sha->lowLength += 8;
        if (sha->lowLength == 0) {
            sha->highLength++;
        }
        if (sha->index == 64) {
            shaProcess(sha);
        }
        msg++;
    }
}


static void shaFinalize(uchar *digest, MprSha *sha)
{
    int i;

    shaPad(sha);
    memset(sha->block, 0, 64);
    sha->lowLength = 0;
    sha->highLength = 0;
    for  (i = 0; i < SHA_SIZE; i++) {
        digest[i] = sha->hash[i >> 2] >> 8 * (3 - (i & 0x03));
    }
}


static void shaProcess(MprSha *sha)
{
    uint    K[] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    uint    temp, W[80], A, B, C, D, E;
    int     t;

    for  (t = 0; t < 16; t++) {
        W[t] = sha->block[t * 4] << 24;
        W[t] |= sha->block[t * 4 + 1] << 16;
        W[t] |= sha->block[t * 4 + 2] << 8;
        W[t] |= sha->block[t * 4 + 3];
    }
    for (t = 16; t < 80; t++) {
       W[t] = shaShift(1, W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]);
    }
    A = sha->hash[0];
    B = sha->hash[1];
    C = sha->hash[2];
    D = sha->hash[3];
    E = sha->hash[4];

    for (t = 0; t < 20; t++) {
        temp =  shaShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 20; t < 40; t++) {
        temp = shaShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 40; t < 60; t++) {
        temp = shaShift(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 60; t < 80; t++) {
        temp = shaShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    sha->hash[0] += A;
    sha->hash[1] += B;
    sha->hash[2] += C;
    sha->hash[3] += D;
    sha->hash[4] += E;
    sha->index = 0;
}


static void shaPad(MprSha *sha)
{
    if (sha->index > 55) {
        sha->block[sha->index++] = 0x80;
        while(sha->index < 64) {
            sha->block[sha->index++] = 0;
        }
        shaProcess(sha);
        while (sha->index < 56) {
            sha->block[sha->index++] = 0;
        }
    } else {
        sha->block[sha->index++] = 0x80;
        while(sha->index < 56) {
            sha->block[sha->index++] = 0;
        }
    }
    sha->block[56] = sha->highLength >> 24;
    sha->block[57] = sha->highLength >> 16;
    sha->block[58] = sha->highLength >> 8;
    sha->block[59] = sha->highLength;
    sha->block[60] = sha->lowLength >> 24;
    sha->block[61] = sha->lowLength >> 16;
    sha->block[62] = sha->lowLength >> 8;
    sha->block[63] = sha->lowLength;
    shaProcess(sha);
}

/************************************ Blowfish *******************************/

#define BF_ROUNDS 16

typedef struct {
  uint P[16 + 2];
  uint S[4][256];
} MprBlowfish;

static const uint ORIG_P[16 + 2] = {
        0x243F6A88L, 0x85A308D3L, 0x13198A2EL, 0x03707344L,
        0xA4093822L, 0x299F31D0L, 0x082EFA98L, 0xEC4E6C89L,
        0x452821E6L, 0x38D01377L, 0xBE5466CFL, 0x34E90C6CL,
        0xC0AC29B7L, 0xC97C50DDL, 0x3F84D5B5L, 0xB5470917L,
        0x9216D5D9L, 0x8979FB1BL
};

/*
    Digits of PI
 */
static const uint ORIG_S[4][256] = {
    {   0xD1310BA6L, 0x98DFB5ACL, 0x2FFD72DBL, 0xD01ADFB7L,
        0xB8E1AFEDL, 0x6A267E96L, 0xBA7C9045L, 0xF12C7F99L,
        0x24A19947L, 0xB3916CF7L, 0x0801F2E2L, 0x858EFC16L,
        0x636920D8L, 0x71574E69L, 0xA458FEA3L, 0xF4933D7EL,
        0x0D95748FL, 0x728EB658L, 0x718BCD58L, 0x82154AEEL,
        0x7B54A41DL, 0xC25A59B5L, 0x9C30D539L, 0x2AF26013L,
        0xC5D1B023L, 0x286085F0L, 0xCA417918L, 0xB8DB38EFL,
        0x8E79DCB0L, 0x603A180EL, 0x6C9E0E8BL, 0xB01E8A3EL,
        0xD71577C1L, 0xBD314B27L, 0x78AF2FDAL, 0x55605C60L,
        0xE65525F3L, 0xAA55AB94L, 0x57489862L, 0x63E81440L,
        0x55CA396AL, 0x2AAB10B6L, 0xB4CC5C34L, 0x1141E8CEL,
        0xA15486AFL, 0x7C72E993L, 0xB3EE1411L, 0x636FBC2AL,
        0x2BA9C55DL, 0x741831F6L, 0xCE5C3E16L, 0x9B87931EL,
        0xAFD6BA33L, 0x6C24CF5CL, 0x7A325381L, 0x28958677L,
        0x3B8F4898L, 0x6B4BB9AFL, 0xC4BFE81BL, 0x66282193L,
        0x61D809CCL, 0xFB21A991L, 0x487CAC60L, 0x5DEC8032L,
        0xEF845D5DL, 0xE98575B1L, 0xDC262302L, 0xEB651B88L,
        0x23893E81L, 0xD396ACC5L, 0x0F6D6FF3L, 0x83F44239L,
        0x2E0B4482L, 0xA4842004L, 0x69C8F04AL, 0x9E1F9B5EL,
        0x21C66842L, 0xF6E96C9AL, 0x670C9C61L, 0xABD388F0L,
        0x6A51A0D2L, 0xD8542F68L, 0x960FA728L, 0xAB5133A3L,
        0x6EEF0B6CL, 0x137A3BE4L, 0xBA3BF050L, 0x7EFB2A98L,
        0xA1F1651DL, 0x39AF0176L, 0x66CA593EL, 0x82430E88L,
        0x8CEE8619L, 0x456F9FB4L, 0x7D84A5C3L, 0x3B8B5EBEL,
        0xE06F75D8L, 0x85C12073L, 0x401A449FL, 0x56C16AA6L,
        0x4ED3AA62L, 0x363F7706L, 0x1BFEDF72L, 0x429B023DL,
        0x37D0D724L, 0xD00A1248L, 0xDB0FEAD3L, 0x49F1C09BL,
        0x075372C9L, 0x80991B7BL, 0x25D479D8L, 0xF6E8DEF7L,
        0xE3FE501AL, 0xB6794C3BL, 0x976CE0BDL, 0x04C006BAL,
        0xC1A94FB6L, 0x409F60C4L, 0x5E5C9EC2L, 0x196A2463L,
        0x68FB6FAFL, 0x3E6C53B5L, 0x1339B2EBL, 0x3B52EC6FL,
        0x6DFC511FL, 0x9B30952CL, 0xCC814544L, 0xAF5EBD09L,
        0xBEE3D004L, 0xDE334AFDL, 0x660F2807L, 0x192E4BB3L,
        0xC0CBA857L, 0x45C8740FL, 0xD20B5F39L, 0xB9D3FBDBL,
        0x5579C0BDL, 0x1A60320AL, 0xD6A100C6L, 0x402C7279L,
        0x679F25FEL, 0xFB1FA3CCL, 0x8EA5E9F8L, 0xDB3222F8L,
        0x3C7516DFL, 0xFD616B15L, 0x2F501EC8L, 0xAD0552ABL,
        0x323DB5FAL, 0xFD238760L, 0x53317B48L, 0x3E00DF82L,
        0x9E5C57BBL, 0xCA6F8CA0L, 0x1A87562EL, 0xDF1769DBL,
        0xD542A8F6L, 0x287EFFC3L, 0xAC6732C6L, 0x8C4F5573L,
        0x695B27B0L, 0xBBCA58C8L, 0xE1FFA35DL, 0xB8F011A0L,
        0x10FA3D98L, 0xFD2183B8L, 0x4AFCB56CL, 0x2DD1D35BL,
        0x9A53E479L, 0xB6F84565L, 0xD28E49BCL, 0x4BFB9790L,
        0xE1DDF2DAL, 0xA4CB7E33L, 0x62FB1341L, 0xCEE4C6E8L,
        0xEF20CADAL, 0x36774C01L, 0xD07E9EFEL, 0x2BF11FB4L,
        0x95DBDA4DL, 0xAE909198L, 0xEAAD8E71L, 0x6B93D5A0L,
        0xD08ED1D0L, 0xAFC725E0L, 0x8E3C5B2FL, 0x8E7594B7L,
        0x8FF6E2FBL, 0xF2122B64L, 0x8888B812L, 0x900DF01CL,
        0x4FAD5EA0L, 0x688FC31CL, 0xD1CFF191L, 0xB3A8C1ADL,
        0x2F2F2218L, 0xBE0E1777L, 0xEA752DFEL, 0x8B021FA1L,
        0xE5A0CC0FL, 0xB56F74E8L, 0x18ACF3D6L, 0xCE89E299L,
        0xB4A84FE0L, 0xFD13E0B7L, 0x7CC43B81L, 0xD2ADA8D9L,
        0x165FA266L, 0x80957705L, 0x93CC7314L, 0x211A1477L,
        0xE6AD2065L, 0x77B5FA86L, 0xC75442F5L, 0xFB9D35CFL,
        0xEBCDAF0CL, 0x7B3E89A0L, 0xD6411BD3L, 0xAE1E7E49L,
        0x00250E2DL, 0x2071B35EL, 0x226800BBL, 0x57B8E0AFL,
        0x2464369BL, 0xF009B91EL, 0x5563911DL, 0x59DFA6AAL,
        0x78C14389L, 0xD95A537FL, 0x207D5BA2L, 0x02E5B9C5L,
        0x83260376L, 0x6295CFA9L, 0x11C81968L, 0x4E734A41L,
        0xB3472DCAL, 0x7B14A94AL, 0x1B510052L, 0x9A532915L,
        0xD60F573FL, 0xBC9BC6E4L, 0x2B60A476L, 0x81E67400L,
        0x08BA6FB5L, 0x571BE91FL, 0xF296EC6BL, 0x2A0DD915L,
        0xB6636521L, 0xE7B9F9B6L, 0xFF34052EL, 0xC5855664L,
        0x53B02D5DL, 0xA99F8FA1L, 0x08BA4799L, 0x6E85076AL
    }, {
        0x4B7A70E9L, 0xB5B32944L, 0xDB75092EL, 0xC4192623L,
        0xAD6EA6B0L, 0x49A7DF7DL, 0x9CEE60B8L, 0x8FEDB266L,
        0xECAA8C71L, 0x699A17FFL, 0x5664526CL, 0xC2B19EE1L,
        0x193602A5L, 0x75094C29L, 0xA0591340L, 0xE4183A3EL,
        0x3F54989AL, 0x5B429D65L, 0x6B8FE4D6L, 0x99F73FD6L,
        0xA1D29C07L, 0xEFE830F5L, 0x4D2D38E6L, 0xF0255DC1L,
        0x4CDD2086L, 0x8470EB26L, 0x6382E9C6L, 0x021ECC5EL,
        0x09686B3FL, 0x3EBAEFC9L, 0x3C971814L, 0x6B6A70A1L,
        0x687F3584L, 0x52A0E286L, 0xB79C5305L, 0xAA500737L,
        0x3E07841CL, 0x7FDEAE5CL, 0x8E7D44ECL, 0x5716F2B8L,
        0xB03ADA37L, 0xF0500C0DL, 0xF01C1F04L, 0x0200B3FFL,
        0xAE0CF51AL, 0x3CB574B2L, 0x25837A58L, 0xDC0921BDL,
        0xD19113F9L, 0x7CA92FF6L, 0x94324773L, 0x22F54701L,
        0x3AE5E581L, 0x37C2DADCL, 0xC8B57634L, 0x9AF3DDA7L,
        0xA9446146L, 0x0FD0030EL, 0xECC8C73EL, 0xA4751E41L,
        0xE238CD99L, 0x3BEA0E2FL, 0x3280BBA1L, 0x183EB331L,
        0x4E548B38L, 0x4F6DB908L, 0x6F420D03L, 0xF60A04BFL,
        0x2CB81290L, 0x24977C79L, 0x5679B072L, 0xBCAF89AFL,
        0xDE9A771FL, 0xD9930810L, 0xB38BAE12L, 0xDCCF3F2EL,
        0x5512721FL, 0x2E6B7124L, 0x501ADDE6L, 0x9F84CD87L,
        0x7A584718L, 0x7408DA17L, 0xBC9F9ABCL, 0xE94B7D8CL,
        0xEC7AEC3AL, 0xDB851DFAL, 0x63094366L, 0xC464C3D2L,
        0xEF1C1847L, 0x3215D908L, 0xDD433B37L, 0x24C2BA16L,
        0x12A14D43L, 0x2A65C451L, 0x50940002L, 0x133AE4DDL,
        0x71DFF89EL, 0x10314E55L, 0x81AC77D6L, 0x5F11199BL,
        0x043556F1L, 0xD7A3C76BL, 0x3C11183BL, 0x5924A509L,
        0xF28FE6EDL, 0x97F1FBFAL, 0x9EBABF2CL, 0x1E153C6EL,
        0x86E34570L, 0xEAE96FB1L, 0x860E5E0AL, 0x5A3E2AB3L,
        0x771FE71CL, 0x4E3D06FAL, 0x2965DCB9L, 0x99E71D0FL,
        0x803E89D6L, 0x5266C825L, 0x2E4CC978L, 0x9C10B36AL,
        0xC6150EBAL, 0x94E2EA78L, 0xA5FC3C53L, 0x1E0A2DF4L,
        0xF2F74EA7L, 0x361D2B3DL, 0x1939260FL, 0x19C27960L,
        0x5223A708L, 0xF71312B6L, 0xEBADFE6EL, 0xEAC31F66L,
        0xE3BC4595L, 0xA67BC883L, 0xB17F37D1L, 0x018CFF28L,
        0xC332DDEFL, 0xBE6C5AA5L, 0x65582185L, 0x68AB9802L,
        0xEECEA50FL, 0xDB2F953BL, 0x2AEF7DADL, 0x5B6E2F84L,
        0x1521B628L, 0x29076170L, 0xECDD4775L, 0x619F1510L,
        0x13CCA830L, 0xEB61BD96L, 0x0334FE1EL, 0xAA0363CFL,
        0xB5735C90L, 0x4C70A239L, 0xD59E9E0BL, 0xCBAADE14L,
        0xEECC86BCL, 0x60622CA7L, 0x9CAB5CABL, 0xB2F3846EL,
        0x648B1EAFL, 0x19BDF0CAL, 0xA02369B9L, 0x655ABB50L,
        0x40685A32L, 0x3C2AB4B3L, 0x319EE9D5L, 0xC021B8F7L,
        0x9B540B19L, 0x875FA099L, 0x95F7997EL, 0x623D7DA8L,
        0xF837889AL, 0x97E32D77L, 0x11ED935FL, 0x16681281L,
        0x0E358829L, 0xC7E61FD6L, 0x96DEDFA1L, 0x7858BA99L,
        0x57F584A5L, 0x1B227263L, 0x9B83C3FFL, 0x1AC24696L,
        0xCDB30AEBL, 0x532E3054L, 0x8FD948E4L, 0x6DBC3128L,
        0x58EBF2EFL, 0x34C6FFEAL, 0xFE28ED61L, 0xEE7C3C73L,
        0x5D4A14D9L, 0xE864B7E3L, 0x42105D14L, 0x203E13E0L,
        0x45EEE2B6L, 0xA3AAABEAL, 0xDB6C4F15L, 0xFACB4FD0L,
        0xC742F442L, 0xEF6ABBB5L, 0x654F3B1DL, 0x41CD2105L,
        0xD81E799EL, 0x86854DC7L, 0xE44B476AL, 0x3D816250L,
        0xCF62A1F2L, 0x5B8D2646L, 0xFC8883A0L, 0xC1C7B6A3L,
        0x7F1524C3L, 0x69CB7492L, 0x47848A0BL, 0x5692B285L,
        0x095BBF00L, 0xAD19489DL, 0x1462B174L, 0x23820E00L,
        0x58428D2AL, 0x0C55F5EAL, 0x1DADF43EL, 0x233F7061L,
        0x3372F092L, 0x8D937E41L, 0xD65FECF1L, 0x6C223BDBL,
        0x7CDE3759L, 0xCBEE7460L, 0x4085F2A7L, 0xCE77326EL,
        0xA6078084L, 0x19F8509EL, 0xE8EFD855L, 0x61D99735L,
        0xA969A7AAL, 0xC50C06C2L, 0x5A04ABFCL, 0x800BCADCL,
        0x9E447A2EL, 0xC3453484L, 0xFDD56705L, 0x0E1E9EC9L,
        0xDB73DBD3L, 0x105588CDL, 0x675FDA79L, 0xE3674340L,
        0xC5C43465L, 0x713E38D8L, 0x3D28F89EL, 0xF16DFF20L,
        0x153E21E7L, 0x8FB03D4AL, 0xE6E39F2BL, 0xDB83ADF7L
    }, {
        0xE93D5A68L, 0x948140F7L, 0xF64C261CL, 0x94692934L,
        0x411520F7L, 0x7602D4F7L, 0xBCF46B2EL, 0xD4A20068L,
        0xD4082471L, 0x3320F46AL, 0x43B7D4B7L, 0x500061AFL,
        0x1E39F62EL, 0x97244546L, 0x14214F74L, 0xBF8B8840L,
        0x4D95FC1DL, 0x96B591AFL, 0x70F4DDD3L, 0x66A02F45L,
        0xBFBC09ECL, 0x03BD9785L, 0x7FAC6DD0L, 0x31CB8504L,
        0x96EB27B3L, 0x55FD3941L, 0xDA2547E6L, 0xABCA0A9AL,
        0x28507825L, 0x530429F4L, 0x0A2C86DAL, 0xE9B66DFBL,
        0x68DC1462L, 0xD7486900L, 0x680EC0A4L, 0x27A18DEEL,
        0x4F3FFEA2L, 0xE887AD8CL, 0xB58CE006L, 0x7AF4D6B6L,
        0xAACE1E7CL, 0xD3375FECL, 0xCE78A399L, 0x406B2A42L,
        0x20FE9E35L, 0xD9F385B9L, 0xEE39D7ABL, 0x3B124E8BL,
        0x1DC9FAF7L, 0x4B6D1856L, 0x26A36631L, 0xEAE397B2L,
        0x3A6EFA74L, 0xDD5B4332L, 0x6841E7F7L, 0xCA7820FBL,
        0xFB0AF54EL, 0xD8FEB397L, 0x454056ACL, 0xBA489527L,
        0x55533A3AL, 0x20838D87L, 0xFE6BA9B7L, 0xD096954BL,
        0x55A867BCL, 0xA1159A58L, 0xCCA92963L, 0x99E1DB33L,
        0xA62A4A56L, 0x3F3125F9L, 0x5EF47E1CL, 0x9029317CL,
        0xFDF8E802L, 0x04272F70L, 0x80BB155CL, 0x05282CE3L,
        0x95C11548L, 0xE4C66D22L, 0x48C1133FL, 0xC70F86DCL,
        0x07F9C9EEL, 0x41041F0FL, 0x404779A4L, 0x5D886E17L,
        0x325F51EBL, 0xD59BC0D1L, 0xF2BCC18FL, 0x41113564L,
        0x257B7834L, 0x602A9C60L, 0xDFF8E8A3L, 0x1F636C1BL,
        0x0E12B4C2L, 0x02E1329EL, 0xAF664FD1L, 0xCAD18115L,
        0x6B2395E0L, 0x333E92E1L, 0x3B240B62L, 0xEEBEB922L,
        0x85B2A20EL, 0xE6BA0D99L, 0xDE720C8CL, 0x2DA2F728L,
        0xD0127845L, 0x95B794FDL, 0x647D0862L, 0xE7CCF5F0L,
        0x5449A36FL, 0x877D48FAL, 0xC39DFD27L, 0xF33E8D1EL,
        0x0A476341L, 0x992EFF74L, 0x3A6F6EABL, 0xF4F8FD37L,
        0xA812DC60L, 0xA1EBDDF8L, 0x991BE14CL, 0xDB6E6B0DL,
        0xC67B5510L, 0x6D672C37L, 0x2765D43BL, 0xDCD0E804L,
        0xF1290DC7L, 0xCC00FFA3L, 0xB5390F92L, 0x690FED0BL,
        0x667B9FFBL, 0xCEDB7D9CL, 0xA091CF0BL, 0xD9155EA3L,
        0xBB132F88L, 0x515BAD24L, 0x7B9479BFL, 0x763BD6EBL,
        0x37392EB3L, 0xCC115979L, 0x8026E297L, 0xF42E312DL,
        0x6842ADA7L, 0xC66A2B3BL, 0x12754CCCL, 0x782EF11CL,
        0x6A124237L, 0xB79251E7L, 0x06A1BBE6L, 0x4BFB6350L,
        0x1A6B1018L, 0x11CAEDFAL, 0x3D25BDD8L, 0xE2E1C3C9L,
        0x44421659L, 0x0A121386L, 0xD90CEC6EL, 0xD5ABEA2AL,
        0x64AF674EL, 0xDA86A85FL, 0xBEBFE988L, 0x64E4C3FEL,
        0x9DBC8057L, 0xF0F7C086L, 0x60787BF8L, 0x6003604DL,
        0xD1FD8346L, 0xF6381FB0L, 0x7745AE04L, 0xD736FCCCL,
        0x83426B33L, 0xF01EAB71L, 0xB0804187L, 0x3C005E5FL,
        0x77A057BEL, 0xBDE8AE24L, 0x55464299L, 0xBF582E61L,
        0x4E58F48FL, 0xF2DDFDA2L, 0xF474EF38L, 0x8789BDC2L,
        0x5366F9C3L, 0xC8B38E74L, 0xB475F255L, 0x46FCD9B9L,
        0x7AEB2661L, 0x8B1DDF84L, 0x846A0E79L, 0x915F95E2L,
        0x466E598EL, 0x20B45770L, 0x8CD55591L, 0xC902DE4CL,
        0xB90BACE1L, 0xBB8205D0L, 0x11A86248L, 0x7574A99EL,
        0xB77F19B6L, 0xE0A9DC09L, 0x662D09A1L, 0xC4324633L,
        0xE85A1F02L, 0x09F0BE8CL, 0x4A99A025L, 0x1D6EFE10L,
        0x1AB93D1DL, 0x0BA5A4DFL, 0xA186F20FL, 0x2868F169L,
        0xDCB7DA83L, 0x573906FEL, 0xA1E2CE9BL, 0x4FCD7F52L,
        0x50115E01L, 0xA70683FAL, 0xA002B5C4L, 0x0DE6D027L,
        0x9AF88C27L, 0x773F8641L, 0xC3604C06L, 0x61A806B5L,
        0xF0177A28L, 0xC0F586E0L, 0x006058AAL, 0x30DC7D62L,
        0x11E69ED7L, 0x2338EA63L, 0x53C2DD94L, 0xC2C21634L,
        0xBBCBEE56L, 0x90BCB6DEL, 0xEBFC7DA1L, 0xCE591D76L,
        0x6F05E409L, 0x4B7C0188L, 0x39720A3DL, 0x7C927C24L,
        0x86E3725FL, 0x724D9DB9L, 0x1AC15BB4L, 0xD39EB8FCL,
        0xED545578L, 0x08FCA5B5L, 0xD83D7CD3L, 0x4DAD0FC4L,
        0x1E50EF5EL, 0xB161E6F8L, 0xA28514D9L, 0x6C51133CL,
        0x6FD5C7E7L, 0x56E14EC4L, 0x362ABFCEL, 0xDDC6C837L,
        0xD79A3234L, 0x92638212L, 0x670EFA8EL, 0x406000E0L
    }, {
        0x3A39CE37L, 0xD3FAF5CFL, 0xABC27737L, 0x5AC52D1BL,
        0x5CB0679EL, 0x4FA33742L, 0xD3822740L, 0x99BC9BBEL,
        0xD5118E9DL, 0xBF0F7315L, 0xD62D1C7EL, 0xC700C47BL,
        0xB78C1B6BL, 0x21A19045L, 0xB26EB1BEL, 0x6A366EB4L,
        0x5748AB2FL, 0xBC946E79L, 0xC6A376D2L, 0x6549C2C8L,
        0x530FF8EEL, 0x468DDE7DL, 0xD5730A1DL, 0x4CD04DC6L,
        0x2939BBDBL, 0xA9BA4650L, 0xAC9526E8L, 0xBE5EE304L,
        0xA1FAD5F0L, 0x6A2D519AL, 0x63EF8CE2L, 0x9A86EE22L,
        0xC089C2B8L, 0x43242EF6L, 0xA51E03AAL, 0x9CF2D0A4L,
        0x83C061BAL, 0x9BE96A4DL, 0x8FE51550L, 0xBA645BD6L,
        0x2826A2F9L, 0xA73A3AE1L, 0x4BA99586L, 0xEF5562E9L,
        0xC72FEFD3L, 0xF752F7DAL, 0x3F046F69L, 0x77FA0A59L,
        0x80E4A915L, 0x87B08601L, 0x9B09E6ADL, 0x3B3EE593L,
        0xE990FD5AL, 0x9E34D797L, 0x2CF0B7D9L, 0x022B8B51L,
        0x96D5AC3AL, 0x017DA67DL, 0xD1CF3ED6L, 0x7C7D2D28L,
        0x1F9F25CFL, 0xADF2B89BL, 0x5AD6B472L, 0x5A88F54CL,
        0xE029AC71L, 0xE019A5E6L, 0x47B0ACFDL, 0xED93FA9BL,
        0xE8D3C48DL, 0x283B57CCL, 0xF8D56629L, 0x79132E28L,
        0x785F0191L, 0xED756055L, 0xF7960E44L, 0xE3D35E8CL,
        0x15056DD4L, 0x88F46DBAL, 0x03A16125L, 0x0564F0BDL,
        0xC3EB9E15L, 0x3C9057A2L, 0x97271AECL, 0xA93A072AL,
        0x1B3F6D9BL, 0x1E6321F5L, 0xF59C66FBL, 0x26DCF319L,
        0x7533D928L, 0xB155FDF5L, 0x03563482L, 0x8ABA3CBBL,
        0x28517711L, 0xC20AD9F8L, 0xABCC5167L, 0xCCAD925FL,
        0x4DE81751L, 0x3830DC8EL, 0x379D5862L, 0x9320F991L,
        0xEA7A90C2L, 0xFB3E7BCEL, 0x5121CE64L, 0x774FBE32L,
        0xA8B6E37EL, 0xC3293D46L, 0x48DE5369L, 0x6413E680L,
        0xA2AE0810L, 0xDD6DB224L, 0x69852DFDL, 0x09072166L,
        0xB39A460AL, 0x6445C0DDL, 0x586CDECFL, 0x1C20C8AEL,
        0x5BBEF7DDL, 0x1B588D40L, 0xCCD2017FL, 0x6BB4E3BBL,
        0xDDA26A7EL, 0x3A59FF45L, 0x3E350A44L, 0xBCB4CDD5L,
        0x72EACEA8L, 0xFA6484BBL, 0x8D6612AEL, 0xBF3C6F47L,
        0xD29BE463L, 0x542F5D9EL, 0xAEC2771BL, 0xF64E6370L,
        0x740E0D8DL, 0xE75B1357L, 0xF8721671L, 0xAF537D5DL,
        0x4040CB08L, 0x4EB4E2CCL, 0x34D2466AL, 0x0115AF84L,
        0xE1B00428L, 0x95983A1DL, 0x06B89FB4L, 0xCE6EA048L,
        0x6F3F3B82L, 0x3520AB82L, 0x011A1D4BL, 0x277227F8L,
        0x611560B1L, 0xE7933FDCL, 0xBB3A792BL, 0x344525BDL,
        0xA08839E1L, 0x51CE794BL, 0x2F32C9B7L, 0xA01FBAC9L,
        0xE01CC87EL, 0xBCC7D1F6L, 0xCF0111C3L, 0xA1E8AAC7L,
        0x1A908749L, 0xD44FBD9AL, 0xD0DADECBL, 0xD50ADA38L,
        0x0339C32AL, 0xC6913667L, 0x8DF9317CL, 0xE0B12B4FL,
        0xF79E59B7L, 0x43F5BB3AL, 0xF2D519FFL, 0x27D9459CL,
        0xBF97222CL, 0x15E6FC2AL, 0x0F91FC71L, 0x9B941525L,
        0xFAE59361L, 0xCEB69CEBL, 0xC2A86459L, 0x12BAA8D1L,
        0xB6C1075EL, 0xE3056A0CL, 0x10D25065L, 0xCB03A442L,
        0xE0EC6E0EL, 0x1698DB3BL, 0x4C98A0BEL, 0x3278E964L,
        0x9F1F9532L, 0xE0D392DFL, 0xD3A0342BL, 0x8971F21EL,
        0x1B0A7441L, 0x4BA3348CL, 0xC5BE7120L, 0xC37632D8L,
        0xDF359F8DL, 0x9B992F2EL, 0xE60B6F47L, 0x0FE3F11DL,
        0xE54CDA54L, 0x1EDAD891L, 0xCE6279CFL, 0xCD3E7E6FL,
        0x1618B166L, 0xFD2C1D05L, 0x848FD2C5L, 0xF6FB2299L,
        0xF523F357L, 0xA6327623L, 0x93A83531L, 0x56CCCD02L,
        0xACF08162L, 0x5A75EBB5L, 0x6E163697L, 0x88D273CCL,
        0xDE966292L, 0x81B949D0L, 0x4C50901BL, 0x71C65614L,
        0xE6C6C7BDL, 0x327A140AL, 0x45E1D006L, 0xC3F27B9AL,
        0xC9AA53FDL, 0x62A80F00L, 0xBB25BFE2L, 0x35BDD2F6L,
        0x71126905L, 0xB2040222L, 0xB6CBCF7CL, 0xCD769C2BL,
        0x53113EC0L, 0x1640E3D3L, 0x38ABBD60L, 0x2547ADF0L,
        0xBA38209CL, 0xF746CE76L, 0x77AFA1C5L, 0x20756060L,
        0x85CBFE4EL, 0x8AE88DD8L, 0x7AAAF9B0L, 0x4CF9AA7EL,
        0x1948C25CL, 0x02FB8A8CL, 0x01C36AE4L, 0xD6EBE1F9L,
        0x90D4F869L, 0xA65CDEA0L, 0x3F09252DL, 0xC208E69FL,
        0xB74E6132L, 0xCE77E25BL, 0x578FDFE3L, 0x3AC372E6L
    }
};


static uint BF(MprBlowfish *bp, uint x) 
{
   ushort a, b, c, d;
   uint  y;

    d = x & 0x00FF;
    x >>= 8;
    c = x & 0x00FF;
    x >>= 8;
    b = x & 0x00FF;
    x >>= 8;
    a = x & 0x00FF;

    y = bp->S[0][a] + bp->S[1][b];
    y = y ^ bp->S[2][c];
    y = y + bp->S[3][d];
    return y;
}


static void bencrypt(MprBlowfish *bp, uint *xl, uint *xr) 
{
    uint    Xl, Xr, temp;
    int     i;

    Xl = *xl;
    Xr = *xr;

    for (i = 0; i < BF_ROUNDS; ++i) {
        Xl = Xl ^ bp->P[i];
        Xr = BF(bp, Xl) ^ Xr;
        temp = Xl;
        Xl = Xr;
        Xr = temp;
    }
    temp = Xl;
    Xl = Xr;
    Xr = temp;
    Xr = Xr ^ bp->P[BF_ROUNDS];
    Xl = Xl ^ bp->P[BF_ROUNDS + 1];
    *xl = Xl;
    *xr = Xr;
}


#if KEEP
static void bdecrypt(MprBlowfish *bp, uint *xl, uint *xr) 
{
    uint    Xl, Xr, temp;
    int     i;

    Xl = *xl;
    Xr = *xr;

    for (i = BF_ROUNDS + 1; i > 1; --i) {
        Xl = Xl ^ bp->P[i];
        Xr = BF(bp, Xl) ^ Xr;
        temp = Xl;
        Xl = Xr;
        Xr = temp;
    }
    temp = Xl;
    Xl = Xr;
    Xr = temp;
    Xr = Xr ^ bp->P[1];
    Xl = Xl ^ bp->P[0];
    *xl = Xl;
    *xr = Xr;
}
#endif


static void binit(MprBlowfish *bp, uchar *key, ssize keylen) 
{
  uint  data, datal, datar;
  int   i, j, k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 256; j++) {
            bp->S[i][j] = ORIG_S[i][j];
        }
    }
    for (j = i = 0; i < BF_ROUNDS + 2; ++i) {
        for (data = 0, k = 0; k < 4; ++k) {
            data = (data << 8) | key[j];
            j = j + 1;
            if (j >= keylen) {
                j = 0;
            }
        }
        bp->P[i] = ORIG_P[i] ^ data;
    }
    datal = datar = 0;

    for (i = 0; i < BF_ROUNDS + 2; i += 2) {
        bencrypt(bp, &datal, &datar);
        bp->P[i] = datal;
        bp->P[i + 1] = datar;
    }
    for (i = 0; i < 4; ++i) {
        for (j = 0; j < 256; j += 2) {
            bencrypt(bp, &datal, &datar);
            bp->S[i][j] = datal;
            bp->S[i][j + 1] = datar;
        }
    }
}


/*
    Text: "OrpheanBeholderScryDoubt"
 */
static uint cipherText[6] = {
    0x4f727068, 0x65616e42, 0x65686f6c,
    0x64657253, 0x63727944, 0x6f756274
};


PUBLIC char *mprCryptPassword(cchar *password, cchar *salt, int rounds)
{
    MprBlowfish     bf;
    char            *result, *key;
    uint            *text;
    ssize           len, limit;
    int             i, j;

    if (slen(password) > ME_MPR_MAX_PASSWORD) {
        return 0;
    }
    key = sfmt("%s:%s", salt, password);
    binit(&bf, (uchar*) key, slen(key));
    len = sizeof(cipherText);
    text = mprMemdup(cipherText, len);

    for (i = 0; i < rounds; i++) {
        limit = len / sizeof(uint);
        for (j = 0; j < limit; j += 2) {
            bencrypt(&bf, &text[j], &text[j + 1]);
        }
    }
    result = mprEncode64Block((cchar*) text, len);
    memset(&bf, 0, sizeof(bf));
    memset(text, 0, len);
    return result;
}


PUBLIC char *mprMakeSalt(ssize size)
{
    char    *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char    *rp, *result, *random;
    ssize   clen, i;

    size = (size + sizeof(int) - 1) & ~(sizeof(int) - 1);
    random = mprAlloc(size + 1);
    result = mprAlloc(size + 1);
    if (mprGetRandomBytes(random, size, 0) < 0) {
        return 0;
    }
    clen = slen(chars);
    for (i = 0, rp = result; i < size; i++) {
        *rp++ = chars[(random[i] & 0x7F) % clen];
    }
    *rp = '\0';
    return result;
}


/*
    Format of hashed password is:

    Algorithm: Rounds: Salt: Hash
*/
PUBLIC char *mprMakePassword(cchar *password, int saltLength, int rounds)
{
    cchar   *salt;

    if (slen(password) > ME_MPR_MAX_PASSWORD) {
        return 0;
    }
    if (saltLength <= 0) {
        saltLength = BLOWFISH_SALT_LENGTH;
    }
    if (rounds <= 0) {
        rounds = BLOWFISH_ROUNDS;
    }
    salt = mprMakeSalt(saltLength);
    return sfmt("BF1:%05d:%s:%s", rounds, salt, mprCryptPassword(password, salt, rounds));
}


PUBLIC bool mprCheckPassword(cchar *plainTextPassword, cchar *passwordHash)
{
    cchar   *given, *rounds, *salt, *s1, *s2;
    char    *tok, *hash;
    ssize   match;

    if (!passwordHash || !plainTextPassword) {
        return 0;
    }
    if (slen(plainTextPassword) > ME_MPR_MAX_PASSWORD) {
        return 0;
    }
    stok(sclone(passwordHash), ":", &tok);
    rounds = stok(NULL, ":", &tok);
    salt = stok(NULL, ":", &tok);
    hash = stok(NULL, ":", &tok);
    if (!rounds || !salt || !hash) {
        return 0;
    }
    given = mprCryptPassword(plainTextPassword, salt, atoi(rounds));

    match = slen(given) ^ slen(hash);
    for (s1 = given, s2 = hash; *s1 && *s2; s1++, s2++) {
        match |= (*s1 & 0xFF) ^ (*s2 & 0xFF);
    }
    return !match;
}


PUBLIC char *mprGetPassword(cchar *prompt)
{
    char    *cp, *password, *result;

#if ME_BSD_LIKE
    char    passbuf[ME_MAX_BUFFER];

    if (!prompt || !*prompt) {
        prompt = "Password: ";
    }
    if ((password = readpassphrase(prompt, passbuf, sizeof(passbuf), 0)) == 0) {
        return 0;
    }
#elif ME_UNIX_LIKE
    if (!prompt || !*prompt) {
        prompt = "Password: ";
    }
    if ((password = getpass(prompt)) == 0) {
        return 0;
    }
#elif ME_WIN_LIKE || VXWORKS
    char    passbuf[ME_MAX_BUFFER];
    int     c, i;

    if (!prompt || !*prompt) {
        prompt = "Password: ";
    }
    fputs(prompt, stderr);
    for (i = 0; i < (int) sizeof(passbuf) - 1; i++) {
#if VXWORKS
        c = getchar();
#else
        c = _getch();
#endif
        if (c == '\r' || c == EOF) {
            break;
        }
        if ((c == '\b' || c == 127) && i > 0) {
            passbuf[--i] = '\0';
            fputs("\b \b", stderr);
            i--;
        } else if (c == 26) {           /* Control Z */
            c = EOF;
            break;
        } else if (c == 3) {            /* Control C */
            fputs("^C\n", stderr);
            exit(255);
        } else if (!iscntrl((uchar) c) && (i < (int) sizeof(passbuf) - 1)) {
            passbuf[i] = c;
            fputc('*', stderr);
        } else {
            fputc('', stderr);
            i--;
        }
    }
    if (c == EOF) {
        return "";
    }
    fputc('\n', stderr);
    passbuf[i] = '\0';
    password = passbuf;
#else
    return 0;
#endif
    result = sclone(password);
    for (cp = password; *cp; cp++) {
        *cp = 0;
    }
    return result;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/disk.c ************/


/**
    disk.c - File services for systems with a (disk) based file system.

    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



#if !ME_ROM
/*********************************** Defines **********************************/

#if WINDOWS
/*
    Open/Delete retries to circumvent windows pending delete problems
 */
#define RETRIES 40

/*
    Windows only permits owner bits
 */
#define MASK_PERMS(perms)    perms & 0600
#else
#define MASK_PERMS(perms)    perms
#endif

/********************************** Forwards **********************************/

static int closeFile(MprFile *file);
static void manageDiskFile(MprFile *file, int flags);
static int getPathInfo(MprDiskFileSystem *fs, cchar *path, MprPath *info);

/************************************ Code ************************************/
#if KEEP
/*
    Open a file with support for cygwin paths. Tries windows path first then under /cygwin.
 */
static int cygOpen(MprFileSystem *fs, cchar *path, int omode, int perms)
{
    int     fd;

    fd = open(path, omode, MASK_PERMS(perms));
#if WINDOWS
    if (fd < 0) {
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        fd = open(path, omode, MASK_PERMS(perms));
    }
#endif
    return fd;
}
#endif

static MprFile *openFile(MprFileSystem *fs, cchar *path, int omode, int perms)
{
    MprFile     *file;

    assert(path);

    if ((file = mprAllocObj(MprFile, manageDiskFile)) == 0) {
        return NULL;
    }
    file->path = sclone(path);
    file->fd = open(path, omode, MASK_PERMS(perms));
    if (file->fd < 0) {
#if WINDOWS
        /*
            Windows opens can fail of immediately following a delete. Windows uses pending deletes which prevent opens.
         */
        int i, err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            for (i = 0; i < RETRIES; i++) {
                file->fd = open(path, omode, MASK_PERMS(perms));
                if (file->fd >= 0) {
                    break;
                }
                mprNap(10);
            }
            if (file->fd < 0) {
                file = NULL;
            }
        } else {
            file = NULL;
        }
#else
        file = NULL;
#endif
    }
    return file;
}


static void manageDiskFile(MprFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->path);
        mprMark(file->fileSystem);
        mprMark(file->buf);
#if ME_ROM
        mprMark(file->inode);
#endif
    } else if (flags & MPR_MANAGE_FREE) {
        if (file->fd >= 0) {
            close(file->fd);
            file->fd = -1;
        }
    }
}


/*
    WARNING: this may be called by finalizers, so no blocking or locking
 */
static int closeFile(MprFile *file)
{
    MprBuf  *bp;

    assert(file);

    if (file == 0) {
        return 0;
    }
    bp = file->buf;
    if (bp && (file->mode & (O_WRONLY | O_RDWR))) {
        mprFlushFile(file);
    }
    if (file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
    return 0;
}


static ssize readFile(MprFile *file, void *buf, ssize size)
{
    assert(file);
    assert(buf);

    return read(file->fd, buf, (uint) size);
}


static ssize writeFile(MprFile *file, cvoid *buf, ssize count)
{
    assert(file);
    assert(buf);

#if VXWORKS
    return write(file->fd, (void*) buf, count);
#else
    return write(file->fd, buf, (uint) count);
#endif
}


static MprOff seekFile(MprFile *file, int seekType, MprOff distance)
{
    assert(file);

    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
#if ME_WIN_LIKE
    return (MprOff) _lseeki64(file->fd, (int64) distance, seekType);
#elif ME_COMPILER_HAS_OFF64
    return (MprOff) lseek64(file->fd, (off64_t) distance, seekType);
#else
    return (MprOff) lseek(file->fd, (off_t) distance, seekType);
#endif
}


static bool accessPath(MprDiskFileSystem *fs, cchar *path, int omode)
{
#if ME_WIN && KEEP
    if (access(path, omode) < 0) {
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
    }
#endif
    return access(path, omode) == 0;
}


static int deletePath(MprDiskFileSystem *fs, cchar *path)
{
    MprPath     info;

    if (getPathInfo(fs, path, &info) == 0 && info.isDir && !info.isLink) {
        return rmdir((char*) path);
    }
#if WINDOWS
{
    /*
        NOTE: Windows delete makes a file pending delete which prevents immediate recreation. Rename and then delete.
     */
    int i, err;
    for (i = 0; i < RETRIES; i++) {
        if (DeleteFile(wide(path)) != 0) {
            return 0;
        }
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION) {
            break;
        }
        mprNap(10);
    }
    return MPR_ERR_CANT_DELETE;
}
#else
    return unlink((char*) path);
#endif
}
 

static int makeDir(MprDiskFileSystem *fs, cchar *path, int perms, int owner, int group)
{
    int     rc;

#if ME_WIN_LIKE
    if (sends(path, "/")) {
        /* Windows mkdir fails with a trailing "/" */
        path = strim(path, "/", MPR_TRIM_END);
    } else if (sends(path, "\\")) {
        path = strim(path, "\\", MPR_TRIM_END);
    }
#endif
#if VXWORKS
    rc = mkdir((char*) path);
#else
    rc = mkdir(path, perms);
#endif
    if (rc < 0) {
        return MPR_ERR_CANT_CREATE;
    }
#if ME_UNIX_LIKE
    if ((owner != -1 || group != -1) && chown(path, owner, group) < 0) {
        rmdir(path);
        return MPR_ERR_CANT_COMPLETE;
    }
#endif
    return 0;
}


static int makeLink(MprDiskFileSystem *fs, cchar *path, cchar *target, int hard)
{
#if ME_UNIX_LIKE
    if (hard) {
        return link(path, target);
    } else {
        return symlink(path, target);
    }
#else
    return MPR_ERR_BAD_STATE;
#endif
}


static int getPathInfo(MprDiskFileSystem *fs, cchar *path, MprPath *info)
{
#if ME_WIN_LIKE
    struct __stat64     s;
    cchar               *ext;

    assert(path);
    assert(info);
    info->checked = 1;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    info->size = 0;
    info->atime = 0;
    info->ctime = 0;
    info->mtime = 0;
    if (sends(path, "/")) {
        /* Windows stat fails with a trailing "/" */
        path = strim(path, "/", MPR_TRIM_END);
    } else if (sends(path, "\\")) {
        path = strim(path, "\\", MPR_TRIM_END);
    }
    if (_stat64(path, &s) < 0) {
#if ME_WIN && KEEP
        /*
            Try under /cygwin
         */
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        if (_stat64(path, &s) < 0) {
            return -1;
        }
#else
        return -1;
#endif
    }
    ext = mprGetPathExt(path);
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
    info->inode = s.st_ino;
    info->isDir = (s.st_mode & S_IFDIR) != 0;
    info->isReg = (s.st_mode & S_IFREG) != 0;
    info->isLink = 0;
    if (ext) {
        if (strcmp(ext, "lnk") == 0) {
            info->isLink = 1;
        } else if (strcmp(ext, "dll") == 0) {
            info->perms |= 111;
        }
    }
    /*
        Work hard on windows to determine if the file is a regular file.
     */
    if (info->isReg) {
        long    att;

        if ((att = GetFileAttributes(wide(path))) == -1) {
            return -1;
        }
        if (att & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ENCRYPTED |
                FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE)) {
            /*
                Catch accesses to devices like CON, AUX, NUL, LPT etc att will be set to ENCRYPTED on Win9X and NT.
             */
            info->isReg = 0;
        }
        if (info->isReg) {
            HANDLE handle;
            handle = CreateFile(wide(path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (handle == INVALID_HANDLE_VALUE) {
                info->isReg = 0;
            } else {
                long    fileType;
                fileType = GetFileType(handle);
                if (fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_PIPE) {
                    info->isReg = 0;
                }
                CloseHandle(handle);
            }
        }
    }
    if (strcmp(path, "nul") == 0) {
        info->isReg = 0;
    }

#elif VXWORKS
    struct stat s;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    info->size = 0;
    info->checked = 1;
    info->atime = 0;
    info->ctime = 0;
    info->mtime = 0;
    if (stat((char*) path, &s) < 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->inode = s.st_ino;
    info->isDir = S_ISDIR(s.st_mode);
    info->isReg = S_ISREG(s.st_mode);
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
#else
    struct stat s;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    info->size = 0;
    info->checked = 1;
    info->atime = 0;
    info->ctime = 0;
    info->mtime = 0;
    if (lstat((char*) path, &s) < 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    #ifdef S_ISLNK
        info->isLink = S_ISLNK(s.st_mode);
        if (info->isLink) {
            if (stat((char*) path, &s) < 0) {
                return MPR_ERR_CANT_ACCESS;
            }
        }
    #endif
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->inode = s.st_ino;
    info->isDir = S_ISDIR(s.st_mode);
    info->isReg = S_ISREG(s.st_mode);
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
    if (strcmp(path, "/dev/null") == 0) {
        info->isReg = 0;
    }
#endif
    return 0;
}
 
static char *getPathLink(MprDiskFileSystem *fs, cchar *path)
{
#if ME_UNIX_LIKE
    char    pbuf[ME_MAX_PATH];
    ssize   len;

    if ((len = readlink(path, pbuf, sizeof(pbuf) - 1)) < 0) {
        return NULL;
    }
    pbuf[len] = '\0';
    return sclone(pbuf);
#else
    return NULL;
#endif
}


static int truncateFile(MprDiskFileSystem *fs, cchar *path, MprOff size)
{
    if (!mprPathExists(path, F_OK)) {
#if ME_WIN_LIKE && KEEP
        /*
            Try under /cygwin
         */
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        if (!mprPathExists(path, F_OK))
#endif
        return MPR_ERR_CANT_ACCESS;
    }
#if ME_WIN_LIKE
{
    HANDLE  h;

    h = CreateFile(wide(path), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetFilePointer(h, (LONG) size, 0, FILE_BEGIN);
    if (h == INVALID_HANDLE_VALUE || SetEndOfFile(h) == 0) {
        CloseHandle(h);
        return MPR_ERR_CANT_WRITE;
    }
    CloseHandle(h);
}
#elif VXWORKS
{
#if KEEP
    int     fd;

    fd = open(path, O_WRONLY, 0664);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        return MPR_ERR_CANT_WRITE;
    }
    close(fd);
#endif
    return MPR_ERR_CANT_WRITE;
}
#else
    if (truncate(path, size) < 0) {
        return MPR_ERR_CANT_WRITE;
    }
#endif
    return 0;
}


static void manageDiskFileSystem(MprDiskFileSystem *dfs, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dfs->separators);
        mprMark(dfs->newline);
        mprMark(dfs->root);
#if ME_WIN_LIKE || CYGWIN
        mprMark(dfs->cygdrive);
        mprMark(dfs->cygwin);
#endif
    }
}


PUBLIC MprDiskFileSystem *mprCreateDiskFileSystem(cchar *path)
{
    MprFileSystem       *fs;
    MprDiskFileSystem   *dfs;

    if ((dfs = mprAllocObj(MprDiskFileSystem, manageDiskFileSystem)) == 0) {
        return 0;
    }
    /*
        Temporary
     */
    fs = (MprFileSystem*) dfs;
    dfs->accessPath = accessPath;
    dfs->deletePath = deletePath;
    dfs->getPathInfo = getPathInfo;
    dfs->getPathLink = getPathLink;
    dfs->makeDir = makeDir;
    dfs->makeLink = makeLink;
    dfs->openFile = openFile;
    dfs->closeFile = closeFile;
    dfs->readFile = readFile;
    dfs->seekFile = seekFile;
    dfs->truncateFile = truncateFile;
    dfs->writeFile = writeFile;

    if ((MPR->stdError = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdError, "stderr");
    MPR->stdError->fd = 2;
    MPR->stdError->fileSystem = fs;
    MPR->stdError->mode = O_WRONLY;

    if ((MPR->stdInput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdInput, "stdin");
    MPR->stdInput->fd = 0;
    MPR->stdInput->fileSystem = fs;
    MPR->stdInput->mode = O_RDONLY;

    if ((MPR->stdOutput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdOutput, "stdout");
    MPR->stdOutput->fd = 1;
    MPR->stdOutput->fileSystem = fs;
    MPR->stdOutput->mode = O_WRONLY;

    return dfs;
}
#endif /* !ME_ROM */


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/dispatcher.c ************/


/*
    dispatcher.c - Event dispatch services

    This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/***************************** Forward Declarations ***************************/

static MprDispatcher *createQhead(cchar *name);
static void dequeueDispatcher(MprDispatcher *dispatcher);
static int dispatchEvents(MprDispatcher *dispatcher);
static void dispatchEventsWorker(MprDispatcher *dispatcher);
static MprTicks getDispatcherIdleTicks(MprDispatcher *dispatcher, MprTicks timeout);
static MprTicks getIdleTicks(MprEventService *es, MprTicks timeout);
static MprDispatcher *getNextReadyDispatcher(MprEventService *es);
static void initDispatcher(MprDispatcher *q);
static void manageDispatcher(MprDispatcher *dispatcher, int flags);
static void manageEventService(MprEventService *es, int flags);
static void queueDispatcher(MprDispatcher *prior, MprDispatcher *dispatcher);

#define isIdle(dispatcher) (dispatcher->parent == dispatcher->service->idleQ)
#define isRunning(dispatcher) (dispatcher->parent == dispatcher->service->runQ)
#define isReady(dispatcher) (dispatcher->parent == dispatcher->service->readyQ)
#define isWaiting(dispatcher) (dispatcher->parent == dispatcher->service->waitQ)
#define isEmpty(dispatcher) (dispatcher->eventQ->next == dispatcher->eventQ)

/************************************* Code ***********************************/
/*
    Create the overall dispatch service. There may be many event dispatchers.
 */
PUBLIC MprEventService *mprCreateEventService()
{
    MprEventService     *es;

    if ((es = mprAllocObj(MprEventService, manageEventService)) == 0) {
        return 0;
    }
    MPR->eventService = es;
    es->now = mprGetTicks();
    es->mutex = mprCreateLock();
    es->waitCond = mprCreateCond();
    es->runQ = createQhead("running");
    es->readyQ = createQhead("ready");
    es->idleQ = createQhead("idle");
    es->pendingQ = createQhead("pending");
    es->waitQ = createQhead("waiting");
    return es;
}


static void manageEventService(MprEventService *es, int flags)
{
    MprDispatcher   *dp;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(es->runQ);
        mprMark(es->readyQ);
        mprMark(es->waitQ);
        mprMark(es->idleQ);
        mprMark(es->pendingQ);
        mprMark(es->waitCond);
        mprMark(es->mutex);

        for (dp = es->runQ->next; dp != es->runQ; dp = dp->next) {
            mprMark(dp);
        }
        for (dp = es->readyQ->next; dp != es->readyQ; dp = dp->next) {
            mprMark(dp);
        }
        for (dp = es->waitQ->next; dp != es->waitQ; dp = dp->next) {
            mprMark(dp);
        }
        for (dp = es->idleQ->next; dp != es->idleQ; dp = dp->next) {
            mprMark(dp);
        }
        for (dp = es->pendingQ->next; dp != es->pendingQ; dp = dp->next) {
            mprMark(dp);
        }
    }
}


static void destroyDispatcherQueue(MprDispatcher *q)
{
    MprDispatcher   *dp, *next;

    for (dp = q->next; dp != q; dp = next) {
        next = dp->next;
        mprDestroyDispatcher(dp);
        if (next == dp->next) { 
            break;
        }
    }
}


PUBLIC void mprStopEventService()
{
    MprEventService     *es;
        
    es = MPR->eventService;
    destroyDispatcherQueue(es->runQ);
    destroyDispatcherQueue(es->readyQ);
    destroyDispatcherQueue(es->waitQ);
    destroyDispatcherQueue(es->idleQ);
    destroyDispatcherQueue(es->pendingQ);
    es->mutex = 0;
}


PUBLIC void mprSetDispatcherImmediate(MprDispatcher *dispatcher)
{
    dispatcher->flags |= MPR_DISPATCHER_IMMEDIATE;
}


static MprDispatcher *createQhead(cchar *name)
{
    MprDispatcher       *dispatcher;

    if ((dispatcher = mprAllocObj(MprDispatcher, manageDispatcher)) == 0) {
        return 0;
    }
    dispatcher->service = MPR->eventService;
    dispatcher->name = sclone(name);
    initDispatcher(dispatcher);
    return dispatcher;
}


PUBLIC MprDispatcher *mprCreateDispatcher(cchar *name, int flags)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;

    es = MPR->eventService;
    if ((dispatcher = mprAllocObj(MprDispatcher, manageDispatcher)) == 0) {
        return 0;
    }
    dispatcher->flags = flags;
    dispatcher->service = es;
    dispatcher->name = sclone(name);
    dispatcher->cond = mprCreateCond();
    dispatcher->eventQ = mprCreateEventQueue();
    dispatcher->currentQ = mprCreateEventQueue();
    queueDispatcher(es->idleQ, dispatcher);
    return dispatcher;
}


PUBLIC void mprDestroyDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *q, *event, *next;

    if (dispatcher) {
        es = dispatcher->service;
        assert(es == MPR->eventService);
        lock(es);
        assert(dispatcher->service == MPR->eventService);
        q = dispatcher->eventQ;
        if (q) {
            for (event = q->next; event != q; event = next) {
                next = event->next;
                if (event->dispatcher) {
                    mprRemoveEvent(event);
                }
            }
        }
        dequeueDispatcher(dispatcher);
        dispatcher->flags |= MPR_DISPATCHER_DESTROYED;
        unlock(es);
    }
}


static void manageDispatcher(MprDispatcher *dispatcher, int flags)
{
    MprEvent    *q, *event, *next;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(dispatcher->name);
        mprMark(dispatcher->eventQ);
        mprMark(dispatcher->currentQ);
        mprMark(dispatcher->cond);
        mprMark(dispatcher->parent);
        mprMark(dispatcher->service);

        if ((q = dispatcher->eventQ) != 0) {
            for (event = q->next; event != q; event = next) {
                next = event->next;
                mprMark(event);
            }
        }
        if ((q = dispatcher->currentQ) != 0) {
            for (event = q->next; event != q; event = next) {
                next = event->next;
                mprMark(event);
            }
        }
    }
}


/*
    Schedule events. 
    This routine will service events until the timeout expires or if MPR_SERVICE_NO_BLOCK is specified in flags, 
    until there are no more events to service. This routine will also return when the MPR is stopping. This will 
    service all enabled non-running dispatcher queues and pending I/O events.
    An app should dedicate only one thread to be an event service thread. 
    @param timeout Time in milliseconds to wait. Set to zero for no wait. Set to -1 to wait forever.
    @param flags Set to MPR_SERVICE_NO_BLOCK for non-blocking.
    @returns Number of events serviced.
 */
PUBLIC int mprServiceEvents(MprTicks timeout, int flags)
{
    MprEventService     *es;
    MprDispatcher       *dp;
    MprTicks            expires, delay;
    int                 beginEventCount, eventCount;

    if (MPR->eventing) {
        mprLog("warn mpr event", 0, "mprServiceEvents called reentrantly");
        return 0;
    }
    mprAtomicBarrier();
    if (mprIsDestroying()) {
        return 0;
    }
    MPR->eventing = 1;
    es = MPR->eventService;
    beginEventCount = eventCount = es->eventCount;
    es->now = mprGetTicks();
    expires = timeout < 0 ? MPR_MAX_TIMEOUT : (es->now + timeout);
    if (expires < 0) {
        expires = MPR_MAX_TIMEOUT;
    }
    mprSetWindowsThread(0);

    while (es->now <= expires) {
        eventCount = es->eventCount;
        mprServiceSignals();

        while ((dp = getNextReadyDispatcher(es)) != NULL) {
            assert(!isRunning(dp));
            queueDispatcher(es->runQ, dp);
            if (dp->flags & MPR_DISPATCHER_IMMEDIATE) {
                dispatchEventsWorker(dp);
            } else {
                if (mprStartWorker((MprWorkerProc) dispatchEventsWorker, dp) < 0) {
                    /* Should not get here */
                    queueDispatcher(es->pendingQ, dp);
                    break;
                }
            }
        } 
        if (flags & MPR_SERVICE_NO_BLOCK) {
            expires = 0;
            /* But still service I/O events below */
        }
        if (es->eventCount == eventCount) {
            lock(es);
            delay = getIdleTicks(es, expires - es->now);
            es->willAwake = es->now + delay;
            es->waiting = 1;
            unlock(es);
            /*
                Service IO events
             */
            mprWaitForIO(MPR->waitService, delay);
        }
        es->now = mprGetTicks();
        if (flags & MPR_SERVICE_NO_BLOCK) {
            break;
        }
        if (mprIsStopping()) {
            if (mprIsStopped() || mprIsIdle(0)) {
                /*
                    Don't return yet if GC paused. Could be an outside event pending
                 */
                if (!mprGCPaused()) {
                    break;
                }
                timeout = 1;
            }
        }
    }
    MPR->eventing = 0;
    mprSignalCond(MPR->cond);
    return abs(es->eventCount - beginEventCount);
}


PUBLIC void mprSuspendThread(MprTicks timeout)
{
    mprWaitForMultiCond(MPR->stopCond, timeout);
}


PUBLIC int64 mprGetEventMark(MprDispatcher *dispatcher)
{
    int64   result;

    /*
        Ensure all writes are flushed so user state will be valid across all threads
     */
    result = dispatcher->mark;
    mprAtomicBarrier();
    return result;
}


/*
    Wait for an event to occur on the dispatcher and service the event. This is not called by mprServiceEvents.
    The dispatcher may be "started" and owned by the thread, or it may be unowned.
    WARNING: the event may have already happened by the time this API is invoked.
    WARNING: this will enable GC while sleeping.
 */
PUBLIC int mprWaitForEvent(MprDispatcher *dispatcher, MprTicks timeout, int64 mark)
{
    MprEventService     *es;
    MprTicks            expires, delay;
    int                 runEvents, changed;

    if (dispatcher == NULL) {
        dispatcher = MPR->dispatcher;
    }
    if ((runEvents = (dispatcher->owner == mprGetCurrentOsThread())) != 0) {
        /* Called from an event on a running dispatcher */
        assert(isRunning(dispatcher));
        if (dispatchEvents(dispatcher)) {
            return 0;
        }
    }
    es = MPR->eventService;
    es->now = mprGetTicks();
    expires = timeout < 0 ? MPR_MAX_TIMEOUT : (es->now + timeout);
    delay = expires - es->now;

    lock(es);
    delay = getDispatcherIdleTicks(dispatcher, delay);
    dispatcher->flags |= MPR_DISPATCHER_WAITING;
    changed = dispatcher->mark != mark && mark != -1;
    unlock(es);

    if (changed) {
        return 0;
    }
    mprYield(MPR_YIELD_STICKY);
    mprWaitForCond(dispatcher->cond, delay);
    mprResetYield();
    es->now = mprGetTicks();

    lock(es);
    dispatcher->flags &= ~MPR_DISPATCHER_WAITING;
    unlock(es);

    if (runEvents) {
        dispatchEvents(dispatcher);
        assert(isRunning(dispatcher));
    }
    return 0;
}


PUBLIC void mprSignalCompletion(MprDispatcher *dispatcher)
{
    if (dispatcher == 0) {
        dispatcher = MPR->dispatcher;
    }
    dispatcher->flags |= MPR_DISPATCHER_COMPLETE;
    mprSignalDispatcher(dispatcher);
}


/*
    Wait for an event to complete signified by the 'completion' flag being set.
    This will wait for events on the dispatcher.
    The completion flag will be reset on return.
 */
PUBLIC bool mprWaitForCompletion(MprDispatcher *dispatcher, MprTicks timeout)
{
    MprTicks    mark;
    bool        success;

    assert(timeout >= 0);

    if (dispatcher == 0) {
        dispatcher = MPR->dispatcher;
    }
    if (mprGetDebugMode()) {
        timeout *= 100;
    }
    for (mark = mprGetTicks(); !(dispatcher->flags & MPR_DISPATCHER_COMPLETE) && mprGetElapsedTicks(mark) < timeout; ) {
        mprWaitForEvent(dispatcher, 10, -1);
    }
    success = (dispatcher->flags & MPR_DISPATCHER_COMPLETE) ? 1 : 0;
    dispatcher->flags &= ~MPR_DISPATCHER_COMPLETE;
    return success;
}


PUBLIC void mprClearWaiting()
{
    MPR->eventService->waiting = 0;
}


PUBLIC void mprWakeEventService()
{
    if (MPR->eventService->waiting) {
        mprWakeNotifier();
    }
}


PUBLIC void mprWakeDispatchers()
{
    MprEventService     *es;
    MprDispatcher       *runQ, *dp;

    es = MPR->eventService;
    lock(es);
    runQ = es->runQ;
    for (dp = runQ->next; dp != runQ; dp = dp->next) {
        mprSignalCond(dp->cond);
    }
    unlock(es);
}


PUBLIC int mprDispatchersAreIdle()
{
    MprEventService     *es;
    MprDispatcher       *runQ, *dispatcher;
    int                 idle;

    es = MPR->eventService;
    runQ = es->runQ;
    lock(es);
    dispatcher = runQ->next;
    idle = (dispatcher == runQ) ? 1 : (dispatcher->eventQ == dispatcher->eventQ->next);
    unlock(es);
    return idle;
}


/*
    Start the dispatcher by putting it on the runQ. This prevents the event service from 
    starting any events in parallel. The invoking thread should service events directly by
    calling mprServiceEvents or mprWaitForEvent.
 */
PUBLIC int mprStartDispatcher(MprDispatcher *dispatcher)
{
    if (dispatcher->owner && dispatcher->owner != mprGetCurrentOsThread()) {
        mprLog("error mpr event", 0, "Cannot start dispatcher - owned by another thread");
        return MPR_ERR_BAD_STATE;
    }
    if (!isRunning(dispatcher)) {
        queueDispatcher(dispatcher->service->runQ, dispatcher);
    }
    dispatcher->owner = mprGetCurrentOsThread();
    return 0;
}


PUBLIC int mprStopDispatcher(MprDispatcher *dispatcher)
{
    if (dispatcher->owner != mprGetCurrentOsThread()) {
        assert(dispatcher->owner == mprGetCurrentOsThread());
        return MPR_ERR_BAD_STATE;
    }
    if (!isRunning(dispatcher)) {
        assert(isRunning(dispatcher));
        return MPR_ERR_BAD_STATE;
    }
    dispatcher->owner = 0;
    dequeueDispatcher(dispatcher);
    mprScheduleDispatcher(dispatcher);
    return 0;
}


/*
    Schedule a dispatcher to run but don't disturb an already running dispatcher. If the event queue is empty, 
    the dispatcher is moved to the idleQ. If there is a past-due event, it is moved to the readyQ. If there is a future 
    event pending, it is put on the waitQ.
 */
PUBLIC void mprScheduleDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    int                 mustWakeWaitService, mustWakeCond;

    assert(dispatcher);
    if (dispatcher->flags & MPR_DISPATCHER_DESTROYED) {
        return;
    }
    es = dispatcher->service;
    lock(es);
    mustWakeWaitService = es->waiting;

    if (isRunning(dispatcher)) {
        mustWakeCond = dispatcher->flags & MPR_DISPATCHER_WAITING;

    } else if (isEmpty(dispatcher)) {
        queueDispatcher(es->idleQ, dispatcher);
        mustWakeCond = dispatcher->flags & MPR_DISPATCHER_WAITING;

    } else {
        event = dispatcher->eventQ->next;
        mustWakeWaitService = mustWakeCond = 0;
        if (event->due > es->now) {
            queueDispatcher(es->waitQ, dispatcher);
            if (event->due < es->willAwake) {
                mustWakeWaitService = 1;
                mustWakeCond = dispatcher->flags & MPR_DISPATCHER_WAITING;
            }
        } else {
            queueDispatcher(es->readyQ, dispatcher);
            mustWakeWaitService = es->waiting;
            mustWakeCond = dispatcher->flags & MPR_DISPATCHER_WAITING;
        }
    }
    unlock(es);
    if (mustWakeCond) {
        mprSignalDispatcher(dispatcher);
    }
    if (mustWakeWaitService) {
        mprWakeEventService();
    }
}


PUBLIC void mprRescheduleDispatcher(MprDispatcher *dispatcher)
{
    if (dispatcher) {
        dequeueDispatcher(dispatcher);
        mprScheduleDispatcher(dispatcher);
    }
}


/*
    Run events for a dispatcher
 */
static int dispatchEvents(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    MprOsThread         priorOwner;
    int                 count;

    assert(isRunning(dispatcher));
    es = dispatcher->service;

    priorOwner = dispatcher->owner;
    assert(priorOwner == 0 || priorOwner == mprGetCurrentOsThread());

    dispatcher->owner = mprGetCurrentOsThread();

    /*
        Events are removed from the dispatcher queue and put onto the currentQ. This is so they will be marked for GC.
        If the callback calls mprRemoveEvent, it will not remove from the currentQ. If it was a continuous event, 
        mprRemoveEvent will clear the continuous flag.

        OPT - this could all be simpler if dispatchEvents was never called recursively. Then a currentQ would not be needed,
        and neither would a running flag. See mprRemoveEvent().
     */
    for (count = 0; (event = mprGetNextEvent(dispatcher)) != 0; count++) {
        assert(!(event->flags & MPR_EVENT_RUNNING));
        event->flags |= MPR_EVENT_RUNNING;

        assert(event->proc);
        mprAtomicAdd64(&dispatcher->mark, 1);

        (event->proc)(event->data, event);

        if (dispatcher->flags & MPR_DISPATCHER_DESTROYED) {
            break;
        }
        event->flags &= ~MPR_EVENT_RUNNING;

        lock(es);
        if (event->flags & MPR_EVENT_CONTINUOUS) {
            /* 
                Reschedule if continuous 
             */
            if (event->next) {
                mprDequeueEvent(event);
            }
            event->timestamp = dispatcher->service->now;
            event->due = event->timestamp + (event->period ? event->period : 1);
            mprQueueEvent(dispatcher, event);
        } else {
            /* Remove from currentQ - GC can then collect */
            mprDequeueEvent(event);
        }
        es->eventCount++;
        unlock(es);
        assert(dispatcher->owner == mprGetCurrentOsThread());
    }
    dispatcher->owner = priorOwner;
    return count;
}


/*
    Run events for a dispatcher in a worker thread. When complete, reschedule the dispatcher as required.
 */
static void dispatchEventsWorker(MprDispatcher *dispatcher)
{
    if (dispatcher->flags & MPR_DISPATCHER_DESTROYED) {
        /* Dispatcher destroyed after worker started */
        return;
    }
    dispatcher->owner = mprGetCurrentOsThread();
    dispatchEvents(dispatcher);
    dispatcher->owner = 0;

    if (!(dispatcher->flags & MPR_DISPATCHER_DESTROYED)) {
        dequeueDispatcher(dispatcher);
        mprScheduleDispatcher(dispatcher);
    }
}


PUBLIC void mprWakePendingDispatchers()
{
    MprEventService *es;
    int             mustWake;

    es = MPR->eventService;
    lock(es);
    mustWake = es->pendingQ->next != es->pendingQ;
    unlock(es);

    if (mustWake) {
        mprWakeEventService();
    }
}


/*
    Get the next (ready) dispatcher off given runQ and move onto the runQ
 */
static MprDispatcher *getNextReadyDispatcher(MprEventService *es)
{
    MprDispatcher   *dp, *next, *pendingQ, *readyQ, *waitQ, *dispatcher;
    MprEvent        *event;

    waitQ = es->waitQ;
    readyQ = es->readyQ;
    pendingQ = es->pendingQ;
    dispatcher = 0;

    lock(es);
    if (pendingQ->next != pendingQ && mprAvailableWorkers() > 0) {
        dispatcher = pendingQ->next;

    } else if (readyQ->next == readyQ) {
        /*
            ReadyQ is empty, try to transfer a dispatcher with due events onto the readyQ
         */
        for (dp = waitQ->next; dp != waitQ; dp = next) {
            next = dp->next;
            event = dp->eventQ->next;
            if (event->due <= es->now) {
                queueDispatcher(es->readyQ, dp);
                break;
            }
        }
    }
    if (!dispatcher && readyQ->next != readyQ) {
        dispatcher = readyQ->next;
    }
    /*
        Reserve the dispatcher. This may get transferred to a worker
     */
    if (dispatcher) {
        dispatcher->owner = mprGetCurrentOsThread();
    }
    unlock(es);
    return dispatcher;
}


/*
    Get the time to sleep till the next pending event. Must be called locked.
 */
static MprTicks getIdleTicks(MprEventService *es, MprTicks timeout)
{
    MprDispatcher   *readyQ, *waitQ, *dp;
    MprEvent        *event;
    MprTicks        delay;

    waitQ = es->waitQ;
    readyQ = es->readyQ;

    if (readyQ->next != readyQ) {
        delay = 0;
    } else if (mprIsStopping()) {
        delay = 10;
    } else {
        /*
            Examine all the dispatchers on the waitQ
         */
        delay = es->delay ? es->delay : MPR_MAX_TIMEOUT;
        for (dp = waitQ->next; dp != waitQ; dp = dp->next) {
            event = dp->eventQ->next;
            if (event != dp->eventQ) {
                delay = min(delay, (event->due - es->now));
                if (delay <= 0) {
                    break;
                }
            }
        }
        delay = min(delay, timeout);
        es->delay = 0;
    }
    return delay < 0 ? 0 : delay;
}


PUBLIC void mprSetEventServiceSleep(MprTicks delay)
{
    MPR->eventService->delay = delay;
}


static MprTicks getDispatcherIdleTicks(MprDispatcher *dispatcher, MprTicks timeout)
{
    MprEvent    *next;
    MprTicks    delay;

    if (timeout < 0) {
        timeout = 0;
    } else {
        next = dispatcher->eventQ->next;
        delay = MPR_MAX_TIMEOUT;
        if (next != dispatcher->eventQ) {
            delay = (next->due - dispatcher->service->now);
            if (delay < 0) {
                delay = 0;
            }
        }
        timeout = min(delay, timeout);
    }
    return timeout;
}


static void initDispatcher(MprDispatcher *dispatcher)
{
    dispatcher->next = dispatcher;
    dispatcher->prev = dispatcher;
    dispatcher->parent = dispatcher;
}


static void queueDispatcher(MprDispatcher *prior, MprDispatcher *dispatcher)
{
    assert(dispatcher->service == MPR->eventService);
    lock(dispatcher->service);

    if (dispatcher->parent) {
        dequeueDispatcher(dispatcher);
    }
    dispatcher->parent = prior->parent;
    dispatcher->prev = prior;
    dispatcher->next = prior->next;
    prior->next->prev = dispatcher;
    prior->next = dispatcher;
    unlock(dispatcher->service);
}


static void dequeueDispatcher(MprDispatcher *dispatcher)
{
    lock(dispatcher->service);
    if (dispatcher->next) {
        dispatcher->next->prev = dispatcher->prev;
        dispatcher->prev->next = dispatcher->next;
        dispatcher->next = dispatcher;
        dispatcher->prev = dispatcher;
        dispatcher->parent = dispatcher;
    } else {
        assert(dispatcher->parent == dispatcher);
        assert(dispatcher->next == dispatcher);
        assert(dispatcher->prev == dispatcher);
    }
    unlock(dispatcher->service);
}


PUBLIC void mprSignalDispatcher(MprDispatcher *dispatcher)
{
    if (dispatcher == NULL) {
        dispatcher = MPR->dispatcher;
    }
    mprSignalCond(dispatcher->cond);
}


PUBLIC bool mprDispatcherHasEvents(MprDispatcher *dispatcher)
{
    if (dispatcher == 0) {
        return 0;
    }
    return !isEmpty(dispatcher);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/encode.c ************/


/*
    encode.c - URI encode and decode routines

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/************************************ Locals **********************************/
/*
    Character escape/descape matching codes. Generated by charGen.
 */
static uchar charMatch[256] = {
    0x00,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x7e,0x3c,0x3c,0x7c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x7c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x0c,0x7f,0x28,0x2a,0x3c,0x2b,0x4f,0x0e,0x0e,0x0e,0x28,0x28,0x00,0x00,0x28,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x2a,0x3f,0x28,0x3f,0x2a,
    0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3a,0x7e,0x3a,0x3e,0x00,
    0x3e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x3e,0x3e,0x02,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c 
};

/*
    Max size of the port specification in a URL
 */
#define MAX_PORT_LEN 8

/************************************ Code ************************************/
/*
    Uri encode by encoding special characters with hex equivalents. Return an allocated string.
 */
PUBLIC char *mprUriEncode(cchar *inbuf, int map)
{
    static cchar    hexTable[] = "0123456789ABCDEF";
    uchar           c;
    cchar           *ip;
    char            *result, *op;
    int             len;

    assert(inbuf);
    assert(inbuf);

    if (!inbuf) {
        return MPR->emptyString;
    }
    for (len = 1, ip = inbuf; *ip; ip++, len++) {
        if (charMatch[(uchar) *ip] & map) {
            len += 2;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }
    op = result;

    while ((c = (uchar) (*inbuf++)) != 0) {
        if (c == ' ' && (map & MPR_ENCODE_URI_COMPONENT)) {
            *op++ = '+';
        } else if (charMatch[c] & map) {
            *op++ = '%';
            *op++ = hexTable[c >> 4];
            *op++ = hexTable[c & 0xf];
        } else {
            *op++ = c;
        }
    }
    assert(op < &result[len]);
    *op = '\0';
    return result;
}


/*
    Decode a string using URL encoding. Return an allocated string.
 */
PUBLIC char *mprUriDecode(cchar *inbuf)
{
    cchar   *ip;
    char    *result, *op;
    int     num, i, c;

    assert(inbuf);

    if ((result = sclone(inbuf)) == 0) {
        return 0;
    }
    for (op = result, ip = inbuf; ip && *ip; ip++, op++) {
        if (*ip == '+') {
            *op = ' ';

        } else if (*ip == '%' && isxdigit((uchar) ip[1]) && isxdigit((uchar) ip[2])) {
            ip++;
            num = 0;
            for (i = 0; i < 2; i++, ip++) {
                c = tolower((uchar) *ip);
                if (c >= 'a' && c <= 'f') {
                    num = (num * 16) + 10 + c - 'a';
                } else if (c >= '0' && c <= '9') {
                    num = (num * 16) + c - '0';
                } else {
                    /* Bad chars in URL */
                    return 0;
                }
            }
            *op = (char) num;
            ip--;

        } else {
            *op = *ip;
        }
    }
    *op = '\0';
    return result;
}


/*
    Decode a string using URL encoding. This decodes in situ.
 */
PUBLIC char *mprUriDecodeInSitu(char *inbuf)
{
    char    *ip, *op;
    int     num, i, c;

    assert(inbuf);

    for (op = ip = inbuf; ip && *ip; ip++, op++) {
        if (*ip == '+') {
            *op = ' ';

        } else if (*ip == '%' && isxdigit((uchar) ip[1]) && isxdigit((uchar) ip[2])) {
            ip++;
            num = 0;
            for (i = 0; i < 2; i++, ip++) {
                c = tolower((uchar) *ip);
                if (c >= 'a' && c <= 'f') {
                    num = (num * 16) + 10 + c - 'a';
                } else if (c >= '0' && c <= '9') {
                    num = (num * 16) + c - '0';
                } else {
                    /* Bad chars in URL */
                    return 0;
                }
            }
            *op = (char) num;
            ip--;

        } else {
            *op = *ip;
        }
    }
    *op = '\0';
    return inbuf;
}


/*
    Escape a shell command. Not really Http, but useful anyway for CGI
 */
PUBLIC char *mprEscapeCmd(cchar *cmd, int esc)
{
    uchar   c;
    cchar   *ip;
    char    *result, *op;
    int     len;

    assert(cmd);

    if (!cmd) {
        return MPR->emptyString;
    }
    for (len = 1, ip = cmd; *ip; ip++, len++) {
        if (charMatch[(uchar) *ip] & MPR_ENCODE_SHELL) {
            len++;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }

    if (esc == 0) {
        esc = '\\';
    }
    op = result;
    while ((c = (uchar) *cmd++) != 0) {
#if ME_WIN_LIKE
        if ((c == '\r' || c == '\n') && *cmd != '\0') {
            c = ' ';
            continue;
        }
#endif
        if (charMatch[c] & MPR_ENCODE_SHELL) {
            *op++ = esc;
        }
        *op++ = c;
    }
    assert(op < &result[len]);
    *op = '\0';
    return result;
}


/*
    Escape HTML to escape defined characters (prevent cross-site scripting)
 */
PUBLIC char *mprEscapeHtml(cchar *html)
{
    cchar   *ip;
    char    *result, *op;
    int     len;

    if (!html) {
        return MPR->emptyString;
    }
    for (len = 1, ip = html; *ip; ip++, len++) {
        if (charMatch[(uchar) *ip] & MPR_ENCODE_HTML) {
            len += 5;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }

    /*
        Leave room for the biggest expansion
     */
    op = result;
    while (*html != '\0') {
        if (charMatch[(uchar) *html] & MPR_ENCODE_HTML) {
            if (*html == '&') {
                strcpy(op, "&amp;");
                op += 5;
            } else if (*html == '<') {
                strcpy(op, "&lt;");
                op += 4;
            } else if (*html == '>') {
                strcpy(op, "&gt;");
                op += 4;
            } else if (*html == '#') {
                strcpy(op, "&#35;");
                op += 5;
            } else if (*html == '(') {
                strcpy(op, "&#40;");
                op += 5;
            } else if (*html == ')') {
                strcpy(op, "&#41;");
                op += 5;
            } else if (*html == '"') {
                strcpy(op, "&quot;");
                op += 6;
            } else if (*html == '\'') {
                strcpy(op, "&#39;");
                op += 5;
            } else {
                assert(0);
            }
            html++;
        } else {
            *op++ = *html++;
        }
    }
    assert(op < &result[len]);
    *op = '\0';
    return result;
}


PUBLIC char *mprEscapeSQL(cchar *cmd)
{
    uchar   c;
    cchar   *ip;
    char    *result, *op;
    int     len, esc;

    assert(cmd);

    if (!cmd) {
        return MPR->emptyString;
    }
    for (len = 1, ip = cmd; *ip; ip++, len++) {
        if (charMatch[(uchar) *ip] & MPR_ENCODE_SQL) {
            len++;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }
    esc = '\\';
    op = result;
    while ((c = (uchar) *cmd++) != 0) {
        if (charMatch[c] & MPR_ENCODE_SQL) {
            *op++ = esc;
        }
        *op++ = c;
    }
    assert(op < &result[len]);
    *op = '\0';
    return result;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/epoll.c ************/


/**
    epoll.c - Wait for I/O by using epoll on unix like systems.

    This module augments the mprWait wait services module by providing kqueue() based waiting support.
    Also see mprAsyncSelectWait and mprSelectWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_EVENT_NOTIFIER == MPR_EVENT_EPOLL
/********************************** Forwards **********************************/

static void serviceIO(MprWaitService *ws, struct epoll_event *events, int count);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    struct epoll_event  ev;

    if ((ws->handlerMap = mprCreateList(MPR_FD_MIN, 0)) == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    if ((ws->epoll = epoll_create(ME_MAX_EVENTS)) < 0) {
        mprLog("critical mpr event", 0, "Call to epoll failed");
        return MPR_ERR_CANT_INITIALIZE;
    }

#if defined(EFD_NONBLOCK)
    if ((ws->breakFd[MPR_READ_PIPE] = eventfd(0, 0)) < 0) {
        mprLog("critical mpr event", 0, "Cannot open breakout event");
        return MPR_ERR_CANT_INITIALIZE;
    }
#else
    /*
        Initialize the "wakeup" pipe. This is used to wakeup the service thread if other threads need to wait for I/O.
     */
    if (pipe(ws->breakFd) < 0) {
        mprLog("critical mpr event", 0, "Cannot open breakout pipe");
        return MPR_ERR_CANT_INITIALIZE;
    }
    fcntl(ws->breakFd[0], F_SETFL, fcntl(ws->breakFd[0], F_GETFL) | O_NONBLOCK);
    fcntl(ws->breakFd[1], F_SETFL, fcntl(ws->breakFd[1], F_GETFL) | O_NONBLOCK);
#endif
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.fd = ws->breakFd[MPR_READ_PIPE];
    epoll_ctl(ws->epoll, EPOLL_CTL_ADD, ws->breakFd[MPR_READ_PIPE], &ev);
    return 0;
}


PUBLIC void mprManageEpoll(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        /* Handlers are not marked here so they will auto-remove from the list */
        mprMark(ws->handlerMap);

    } else if (flags & MPR_MANAGE_FREE) {
        if (ws->epoll) {
            close(ws->epoll);
        }
        if (ws->breakFd[0] >= 0) {
            close(ws->breakFd[0]);
        }
        if (ws->breakFd[1] >= 0) {
            close(ws->breakFd[1]);
        }
    }
}


PUBLIC int mprNotifyOn(MprWaitHandler *wp, int mask)
{
    MprWaitService      *ws;
    struct epoll_event  ev;
    int                 fd, rc;

    assert(wp);
    fd = wp->fd;
    ws = wp->service;

    lock(ws);
    if (wp->desiredMask != mask) {
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;
        if (wp->desiredMask & MPR_READABLE) {
            ev.events |= EPOLLIN | EPOLLHUP;
        }
        if (wp->desiredMask & MPR_WRITABLE) {
            ev.events |= EPOLLOUT;
        }
        if (wp->desiredMask == (MPR_READABLE | MPR_WRITABLE)) {
            ev.events |= EPOLLHUP;
        }
        if (ev.events) {
            if ((rc = epoll_ctl(ws->epoll, EPOLL_CTL_DEL, fd, &ev)) != 0) {
                mprLog("error mpr event", 0, "Epoll delete error %d on fd %d", errno, fd);
            }
        }
        ev.events = 0;
        if (mask & MPR_READABLE) {
            ev.events |= (EPOLLIN | EPOLLHUP);
        }
        if (mask & MPR_WRITABLE) {
            ev.events |= EPOLLOUT | EPOLLHUP;
        }
        if (ev.events) {
            if ((rc = epoll_ctl(ws->epoll, EPOLL_CTL_ADD, fd, &ev)) != 0) {
                mprLog("error mpr event", 0, "Epoll add error %d on fd %d", errno, fd);
            }
        }
        wp->desiredMask = mask;
#if UNUSED
        /*
            Disabled because mprRemoteEvent schedules the dispatcher AND lockes the event service.
            This may cause deadlocks and specifically, mprRemoveEvent may crash while it races with event service
            on another thread.
         */
        if (wp->event) {
            mprRemoveEvent(wp->event);
            wp->event = 0;
        }
#endif
        mprSetItem(ws->handlerMap, fd, mask ? wp : 0);
    }
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single file descriptor. Return a mask of events found. Mask is the events of interest.
    timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTicks timeout)
{
    struct epoll_event  ev, events[2];
    int                 epfd, rc, result;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
    memset(&ev, 0, sizeof(ev));
    memset(events, 0, sizeof(events));
    ev.data.fd = fd;
    if ((epfd = epoll_create(ME_MAX_EVENTS)) < 0) {
        mprLog("error mpr event", 0, "Epoll_create failed, errno=%d", errno);
        return MPR_ERR_CANT_INITIALIZE;
    }
    ev.events = 0;
    if (mask & MPR_READABLE) {
        ev.events = EPOLLIN | EPOLLHUP;
    }
    if (mask & MPR_WRITABLE) {
        ev.events = EPOLLOUT | EPOLLHUP;
    }
    if (ev.events) {
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    mprYield(MPR_YIELD_STICKY);
    rc = epoll_wait(epfd, events, sizeof(events) / sizeof(struct epoll_event), timeout);
    mprResetYield();
    close(epfd);

    result = 0;
    if (rc < 0) {
        mprLog("error mpr event", 0, "Epoll returned %d, errno %d", rc, errno);

    } else if (rc > 0) {
        if (rc > 0) {
            if ((events[0].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) && (mask & MPR_READABLE)) {
                result |= MPR_READABLE;
            }
            if ((events[0].events & (EPOLLOUT | EPOLLHUP)) && (mask & MPR_WRITABLE)) {
                result |= MPR_WRITABLE;
            }
        }
    }
    return result;
}


/*
    Wait for I/O on all registered file descriptors. Timeout is in milliseconds. Return the number of events detected. 
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTicks timeout)
{
    struct epoll_event  events[ME_MAX_EVENTS];
    int                 nevents;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if ME_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    mprYield(MPR_YIELD_STICKY);

    if ((nevents = epoll_wait(ws->epoll, events, sizeof(events) / sizeof(struct epoll_event), timeout)) < 0) {
        if (errno != EINTR) {
            mprLog("error mpr event", 0, "epoll returned %d, errno %d", nevents, mprGetOsError());
        }
    }
    mprClearWaiting();
    mprResetYield();

    if (nevents > 0) {
        serviceIO(ws, events, nevents);
    }
    ws->wakeRequested = 0;
}


static void serviceIO(MprWaitService *ws, struct epoll_event *events, int count)
{
    MprWaitHandler      *wp;
    struct epoll_event  *ev;
    int                 fd, i, mask;

    lock(ws);
    for (i = 0; i < count; i++) {
        ev = &events[i];
        fd = ev->data.fd;
        if (fd == ws->breakFd[MPR_READ_PIPE]) {
            char buf[16];
            if (read(fd, buf, sizeof(buf)) < 0) {}
            continue;
        }
        if (fd < 0 || (wp = mprGetItem(ws->handlerMap, fd)) == 0) {
            /*
                This can happen if a writable event has been triggered (e.g. MprCmd command stdin pipe) and the pipe is closed.
                This thread may have waked from kevent before the pipe is closed and the wait handler removed from the map.

                mprLog("error mpr event", 0, "fd not in handler map. fd %d", fd);
             */
            continue;
        }
        mask = 0;
        if (ev->events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            mask |= MPR_READABLE;
        }
        if (ev->events & (EPOLLOUT | EPOLLHUP)) {
            mask |= MPR_WRITABLE;
        }
        wp->presentMask = mask & wp->desiredMask;

#if KEEP
        if (ev->events & EPOLLERR) {
            int error = 0;
            socklen_t errlen = sizeof(error);
            getsockopt(wp->fd, SOL_SOCKET, SO_ERROR, (void*) &error, &errlen);
            printf("error %d\n", error);
            /* Get EPOLLERR for broken pipe */
            mprRemoveWaitHandler(wp);
        }
#endif
        if (wp->presentMask) {
            if (wp->flags & MPR_WAIT_IMMEDIATE) {
                (wp->proc)(wp->handlerData, NULL);
            } else {
                /*
                    Suppress further events while this event is being serviced. User must re-enable.
                 */
                mprNotifyOn(wp, 0);
                mprQueueIOEvent(wp);
            }
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
    Must be async-safe.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;

    ws = MPR->waitService;
    if (!ws->wakeRequested) {
        /*
            This code works for both eventfds and for pipes. We must write a value of 0x1 for eventfds.
         */
        ws->wakeRequested = 1;
#if defined(EFD_NONBLOCK)
        uint64 c = 1;
        if (write(ws->breakFd[MPR_READ_PIPE], &c, sizeof(c)) != sizeof(c)) {
            mprLog("error mpr event", 0, "Cannot write to break port errno=%d", errno);
        }
#else
        int c = 1;
        if (write(ws->breakFd[MPR_WRITE_PIPE], &c, 1) < 0) {}
#endif
    }
}

#else
void epollDummy() {}
#endif /* MPR_EVENT_EPOLL */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/event.c ************/


/*
    event.c - Event and dispatch services

    This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/***************************** Forward Declarations ***************************/

static void initEvent(MprDispatcher *dispatcher, MprEvent *event, cchar *name, MprTicks period, void *proc, 
        void *data, int flgs);
static void initEventQ(MprEvent *q, cchar *name);
static void manageEvent(MprEvent *event, int flags);
static void queueEvent(MprEvent *prior, MprEvent *event);

/************************************* Code ***********************************/
/*
    Create and queue a new event for service. Period is used as the delay before running the event and as the period 
    between events for continuous events.
 */
PUBLIC MprEvent *mprCreateEventQueue()
{
    MprEvent    *queue;

    if ((queue = mprAllocObj(MprEvent, manageEvent)) == 0) {
        return 0;
    }
    initEventQ(queue, "eventq");
    return queue;
}


/*
    Create and queue a new event for service. Period is used as the delay before running the event and as the period 
    between events for continuous events.
 */
PUBLIC MprEvent *mprCreateEvent(MprDispatcher *dispatcher, cchar *name, MprTicks period, void *proc, void *data, int flags)
{
    MprEvent    *event;

    if ((event = mprAllocObj(MprEvent, manageEvent)) == 0) {
        return 0;
    }
    if (dispatcher == 0 || (dispatcher->flags & MPR_DISPATCHER_DESTROYED)) {
        dispatcher = (flags & MPR_EVENT_QUICK) ? MPR->nonBlock : MPR->dispatcher;
    }
    initEvent(dispatcher, event, name, period, proc, data, flags);
    if (!(flags & MPR_EVENT_DONT_QUEUE)) {
        mprQueueEvent(dispatcher, event);
    }
    return event;
}


static void manageEvent(MprEvent *event, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(event->name);
        mprMark(event->dispatcher);
        mprMark(event->handler);
        if (!(event->flags & MPR_EVENT_STATIC_DATA)) {
            mprMark(event->data);
        }
        mprMark(event->sock);
    }
}


static void initEvent(MprDispatcher *dispatcher, MprEvent *event, cchar *name, MprTicks period, void *proc, void *data, 
    int flags)
{
    assert(dispatcher);
    assert(event);
    assert(proc);
    assert(event->next == 0);
    assert(event->prev == 0);

    dispatcher->service->now = mprGetTicks();
    event->name = sclone(name);
    event->timestamp = dispatcher->service->now;
    event->proc = proc;
    event->period = period;
    event->due = event->timestamp + period;
    event->data = data;
    event->dispatcher = dispatcher;
    event->next = event->prev = 0;
    event->flags = flags;
}


/*
    Create an interval timer
 */
PUBLIC MprEvent *mprCreateTimerEvent(MprDispatcher *dispatcher, cchar *name, MprTicks period, void *proc, 
    void *data, int flags)
{
    return mprCreateEvent(dispatcher, name, period, proc, data, MPR_EVENT_CONTINUOUS | flags);
}


PUBLIC void mprQueueEvent(MprDispatcher *dispatcher, MprEvent *event)
{
    MprEventService     *es;
    MprEvent            *prior, *q;

    assert(dispatcher);
    assert(event);
    assert(event->timestamp);

    es = dispatcher->service;

    lock(es);
    q = dispatcher->eventQ;
    for (prior = q->prev; prior != q; prior = prior->prev) {
        if (event->due > prior->due) {
            break;
        } else if (event->due == prior->due) {
            break;
        }
    }
    assert(prior->next);
    assert(prior->prev);

    queueEvent(prior, event);
    event->dispatcher = dispatcher;
    es->eventCount++;
    mprScheduleDispatcher(dispatcher);
    unlock(es);
}


PUBLIC void mprRemoveEvent(MprEvent *event)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;

    dispatcher = event->dispatcher;
    if (dispatcher) {
        es = dispatcher->service;
        lock(es);
        if (event->next && !(event->flags & MPR_EVENT_RUNNING)) {
            mprDequeueEvent(event);
        }
        event->dispatcher = 0;
        event->flags &= ~MPR_EVENT_CONTINUOUS;
        if (event->due == es->willAwake && dispatcher->eventQ->next != dispatcher->eventQ) {
            mprScheduleDispatcher(dispatcher);
        }
        unlock(es);
    }
}


PUBLIC void mprRescheduleEvent(MprEvent *event, MprTicks period)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;
    int                 continuous;

    dispatcher = event->dispatcher;

    es = dispatcher->service;

    lock(es);
    event->period = period;
    event->timestamp = es->now;
    event->due = event->timestamp + period;
    if (event->next) {
        continuous = event->flags & MPR_EVENT_CONTINUOUS;
        mprRemoveEvent(event);
        event->flags |= continuous;
    }
    unlock(es);
    mprQueueEvent(dispatcher, event);
}


PUBLIC void mprStopContinuousEvent(MprEvent *event)
{
    lock(event->dispatcher->service);
    event->flags &= ~MPR_EVENT_CONTINUOUS;
    unlock(event->dispatcher->service);
}


PUBLIC void mprRestartContinuousEvent(MprEvent *event)
{
    lock(event->dispatcher->service);
    event->flags |= MPR_EVENT_CONTINUOUS;
    unlock(event->dispatcher->service);
    mprRescheduleEvent(event, event->period);
}


PUBLIC void mprEnableContinuousEvent(MprEvent *event, int enable)
{
    lock(event->dispatcher->service);
    event->flags &= ~MPR_EVENT_CONTINUOUS;
    if (enable) {
        event->flags |= MPR_EVENT_CONTINUOUS;
    }
    unlock(event->dispatcher->service);
}


/*
    Get the next due event from the front of the event queue.
 */
PUBLIC MprEvent *mprGetNextEvent(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event, *next;

    es = dispatcher->service;
    event = 0;
    lock(es);
    next = dispatcher->eventQ->next;
    if (next != dispatcher->eventQ) {
        if (next->due <= es->now) {
            /*
                Hold event while executing in the current queue
             */
            event = next;
            queueEvent(dispatcher->currentQ, event);
        }
    }
    unlock(es);
    return event;
}


PUBLIC int mprGetEventCount(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    int                 count;

    es = dispatcher->service;

    lock(es);
    count = 0;
    for (event = dispatcher->eventQ->next; event != dispatcher->eventQ; event = event->next) {
        count++;
    }
    unlock(es);
    return count;
}


static void initEventQ(MprEvent *q, cchar *name)
{
    assert(q);

    q->next = q;
    q->prev = q;
    q->name = sclone(name);
}


/*
    Append a new event. Must be locked when called.
 */
static void queueEvent(MprEvent *prior, MprEvent *event)
{
    assert(prior);
    assert(event);
    assert(prior->next);

    if (event->next) {
        mprDequeueEvent(event);
    }
    event->prev = prior;
    event->next = prior->next;
    prior->next->prev = event;
    prior->next = event;
}


/*
    Remove an event. Must be locked when called.
 */
PUBLIC void mprDequeueEvent(MprEvent *event)
{
    assert(event);

    /* If a continuous event is removed, next may already be null */
    if (event->next) {
        event->next->prev = event->prev;
        event->prev->next = event->next;
        event->next = 0;
        event->prev = 0;
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/file.c ************/


/**
    file.c - File services.

    This modules provides a simple cross platform file I/O abstraction. It uses the MprFileSystem to provide I/O services.
    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/****************************** Forward Declarations **************************/

static ssize fillBuf(MprFile *file);
static void manageFile(MprFile *file, int flags);

/************************************ Code ************************************/

PUBLIC MprFile *mprAttachFileFd(int fd, cchar *name, int omode)
{
    MprFileSystem   *fs;
    MprFile         *file;

    fs = mprLookupFileSystem("/");

    if ((file = mprAllocObj(MprFile, manageFile)) != 0) {
        file->fd = fd;
        file->fileSystem = fs;
        file->path = sclone(name);
        file->mode = omode;
        file->attached = 1;
    }
    return file;
}


static void manageFile(MprFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->buf);
        mprMark(file->path);
#if ME_ROM
        mprMark(file->inode);
#endif
    } else if (flags & MPR_MANAGE_FREE) {
        if (!file->attached) {
            /* Prevent flushing */
            file->buf = 0;
            mprCloseFile(file);
        }
    }
}


PUBLIC int mprFlushFile(MprFile *file)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           len, rc;

    assert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
    if (file->buf == 0) {
        return 0;
    }
    if (file->mode & (O_WRONLY | O_RDWR)) {
        fs = file->fileSystem;
        bp = file->buf;
        while (mprGetBufLength(bp) > 0) {
            len = mprGetBufLength(bp);
            rc = fs->writeFile(file, mprGetBufStart(bp), len);
            if (rc < 0) {
                return (int) rc;
            }
            mprAdjustBufStart(bp, rc);
        }
        mprFlushBuf(bp);
    }
    return 0;
}


PUBLIC MprOff mprGetFilePosition(MprFile *file)
{
    return file->pos;
}


PUBLIC MprOff mprGetFileSize(MprFile *file)
{
    return file->size;
}


PUBLIC MprFile *mprGetStderr()
{
    return MPR->stdError;
}


PUBLIC MprFile *mprGetStdin()
{
    return MPR->stdInput;
}


PUBLIC MprFile *mprGetStdout()
{
    return MPR->stdOutput;
}


/*
    Get a character from the file. This will put the file into buffered mode.
 */
PUBLIC int mprGetFileChar(MprFile *file)
{
    MprBuf      *bp;
    ssize     len;

    assert(file);

    if (file == 0) {
        return MPR_ERR;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(ME_MAX_BUFFER, ME_MAX_BUFFER);
    }
    bp = file->buf;

    if (mprGetBufLength(bp) == 0) {
        len = fillBuf(file);
        if (len <= 0) {
            return -1;
        }
    }
    if (mprGetBufLength(bp) == 0) {
        return 0;
    }
    file->pos++;
    return mprGetCharFromBuf(bp);
}


static char *findNewline(cchar *str, cchar *newline, ssize len, ssize *nlen)
{
    char    *start, *best;
    ssize   newlines;
    int     i;

    assert(str);
    assert(newline);
    assert(nlen);
    assert(len > 0);

    if (str == NULL || newline == NULL) {
        return NULL;
    }
    newlines = slen(newline);
    assert(newlines == 1 || newlines == 2);

    start = best = NULL;
    *nlen = 0;
    for (i = 0; i < newlines; i++) {
        if ((start = memchr(str, newline[i], len)) != 0) {
            if (best == NULL || start < best) {
                best = start;
                *nlen = 1;
                if (newlines == 2 && best[1] == newline[!i]) {
                    (*nlen)++;
                }
            }
        }
    }
    return best;
}


/*
    Read a line from the file. This will put the file into buffered mode.
    Return NULL on eof.
 */
PUBLIC char *mprReadLine(MprFile *file, ssize maxline, ssize *lenp)
{
    MprBuf          *bp;
    MprFileSystem   *fs;
    ssize           size, len, nlen, consumed;
    cchar           *eol, *newline, *start;
    char            *result;

    assert(file);

    if (file == 0) {
        return NULL;
    }
    if (lenp) {
        *lenp = 0;
    }
    if (maxline <= 0) {
        maxline = ME_MAX_BUFFER;
    }
    fs = file->fileSystem;
    newline = fs->newline;
    if (file->buf == 0) {
        file->buf = mprCreateBuf(maxline, maxline);
    }
    bp = file->buf;

    result = NULL;
    size = 0;
    do {
        if (mprGetBufLength(bp) == 0) {
            if (fillBuf(file) <= 0) {
                return result;
            }
        }
        start = mprGetBufStart(bp);
        len = mprGetBufLength(bp);
        if ((eol = findNewline(start, newline, len, &nlen)) != 0) {
            len = eol - start;
            consumed = len + nlen;
        } else {
            consumed = len;
        }
        file->pos += (MprOff) consumed;
        if (lenp) {
            *lenp += len;
        }
        if ((result = mprRealloc(result, size + len + 1)) == 0) {
            return NULL;
        }
        memcpy(&result[size], start, len);
        size += len;
        result[size] = '\0';
        mprAdjustBufStart(bp, consumed);
    } while (!eol);

    return result;
}


PUBLIC MprFile *mprOpenFile(cchar *path, int omode, int perms)
{
    MprFileSystem   *fs;
    MprFile         *file;
    MprPath         info;

    fs = mprLookupFileSystem(path);

    file = fs->openFile(fs, path, omode, perms);
    if (file) {
        file->fileSystem = fs;
        file->path = sclone(path);
        if (omode & (O_WRONLY | O_RDWR)) {
            /*
                OPT. Should compute this lazily.
             */
            fs->getPathInfo(fs, path, &info);
            file->size = (MprOff) info.size;
        }
        file->mode = omode;
        file->perms = perms;
    }
    return file;
}


PUBLIC int mprCloseFile(MprFile *file)
{
    MprFileSystem   *fs;

    if (file == 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    fs = mprLookupFileSystem(file->path);
    return fs->closeFile(file);
}


/*
    Put a string to the file. This will put the file into buffered mode.
 */
PUBLIC ssize mprPutFileString(MprFile *file, cchar *str)
{
    MprBuf  *bp;
    ssize   total, bytes, count;
    char    *buf;

    assert(file);
    count = slen(str);

    /*
        Buffer output and flush when full.
     */
    if (file->buf == 0) {
        file->buf = mprCreateBuf(ME_MAX_BUFFER, 0);
        if (file->buf == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    bp = file->buf;

    if (mprGetBufLength(bp) > 0 && mprGetBufSpace(bp) < count) {
        mprFlushFile(file);
    }
    total = 0;
    buf = (char*) str;

    while (count > 0) {
        bytes = mprPutBlockToBuf(bp, buf, count);
        if (bytes < 0) {
            return MPR_ERR_CANT_ALLOCATE;

        } else if (bytes == 0) {
            if (mprFlushFile(file) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            continue;
        }
        count -= bytes;
        buf += bytes;
        total += bytes;
        file->pos += (MprOff) bytes;
    }
    return total;
}


/*
    Peek at a character from the file without disturbing the read position. This will put the file into buffered mode.
 */
PUBLIC int mprPeekFileChar(MprFile *file)
{
    MprBuf      *bp;
    ssize       len;

    assert(file);

    if (file == 0) {
        return MPR_ERR;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(ME_MAX_BUFFER, ME_MAX_BUFFER);
    }
    bp = file->buf;

    if (mprGetBufLength(bp) == 0) {
        len = fillBuf(file);
        if (len <= 0) {
            return -1;
        }
    }
    if (mprGetBufLength(bp) == 0) {
        return 0;
    }
    return ((uchar*) mprGetBufStart(bp))[0];
}


/*
    Put a character to the file. This will put the file into buffered mode.
 */
PUBLIC ssize mprPutFileChar(MprFile *file, int c)
{
    assert(file);

    if (file == 0) {
        return -1;
    }
    if (file->buf) {
        if (mprPutCharToBuf(file->buf, c) != 1) {
            return MPR_ERR_CANT_WRITE;
        }
        file->pos++;
        return 1;

    }
    return mprWriteFile(file, &c, 1);
}


PUBLIC ssize mprReadFile(MprFile *file, void *buf, ssize size)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           bytes, totalRead;
    void            *bufStart;

    assert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
    fs = file->fileSystem;
    bp = file->buf;
    if (bp == 0) {
        totalRead = fs->readFile(file, buf, size);

    } else {
        bufStart = buf;
        while (size > 0) {
            if (mprGetBufLength(bp) == 0) {
                bytes = fillBuf(file);
                if (bytes <= 0) {
                    return -1;
                }
            }
            bytes = min(size, mprGetBufLength(bp));
            memcpy(buf, mprGetBufStart(bp), bytes);
            mprAdjustBufStart(bp, bytes);
            buf = (void*) (((char*) buf) + bytes);
            size -= bytes;
        }
        totalRead = ((char*) buf - (char*) bufStart);
    }
    file->pos += (MprOff) totalRead;
    return totalRead;
}


PUBLIC MprOff mprSeekFile(MprFile *file, int seekType, MprOff pos)
{
    MprFileSystem   *fs;

    assert(file);
    fs = file->fileSystem;

    if (file->buf) {
        if (! (seekType == SEEK_CUR && pos == 0)) {
            /*
                Discard buffering as we may be seeking outside the buffer.
                OPT. Could be smarter about this and preserve the buffer.
             */
            if (file->mode & (O_WRONLY | O_RDWR)) {
                if (mprFlushFile(file) < 0) {
                    return MPR_ERR_CANT_WRITE;
                }
            }
            if (file->buf) {
                mprFlushBuf(file->buf);
            }
        }
    }
    if (seekType == SEEK_SET) {
        file->pos = pos;
    } else if (seekType == SEEK_CUR) {
        file->pos += pos;
    } else {
        file->pos = fs->seekFile(file, SEEK_END, 0);
    }
    if (fs->seekFile(file, SEEK_SET, file->pos) != file->pos) {
        return MPR_ERR;
    }
    if (file->mode & (O_WRONLY | O_RDWR)) {
        if (file->pos > file->size) {
            file->size = file->pos;
        }
    }
    return file->pos;
}


PUBLIC int mprTruncateFile(cchar *path, MprOff size)
{
    MprFileSystem   *fs;

    assert(path && *path);

    if ((fs = mprLookupFileSystem(path)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    return fs->truncateFile(fs, path, size);
}


PUBLIC ssize mprWriteFile(MprFile *file, cvoid *buf, ssize count)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           bytes, written;

    assert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }

    fs = file->fileSystem;
    bp = file->buf;
    if (bp == 0) {
        if ((written = fs->writeFile(file, buf, count)) < 0) {
            return written;
        }
    } else {
        written = 0;
        while (count > 0) {
            bytes = mprPutBlockToBuf(bp, buf, count);
            if (bytes < 0) {
                return bytes;
            } 
            if (bytes != count) {
                mprFlushFile(file);
            }
            count -= bytes;
            written += bytes;
            buf = (char*) buf + bytes;
        }
    }
    file->pos += (MprOff) written;
    if (file->pos > file->size) {
        file->size = file->pos;
    }
    return written;
}


PUBLIC ssize mprWriteFileString(MprFile *file, cchar *str)
{
    return mprWriteFile(file, str, slen(str));
}


PUBLIC ssize mprWriteFileFmt(MprFile *file, cchar *fmt, ...)
{
    va_list     ap;
    char        *buf;
    ssize       rc;

    rc = -1;
    va_start(ap, fmt);
    if ((buf = sfmtv(fmt, ap)) != NULL) {
        rc = mprWriteFileString(file, buf);
    }
    va_end(ap);
    return rc;
}


/*
    Fill the read buffer. Return the new buffer length. Only called when the buffer is empty.
 */
static ssize fillBuf(MprFile *file)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           len;

    bp = file->buf;
    fs = file->fileSystem;

    assert(mprGetBufLength(bp) == 0);
    mprFlushBuf(bp);

    len = fs->readFile(file, mprGetBufStart(bp), mprGetBufSpace(bp));
    if (len <= 0) {
        return len;
    }
    mprAdjustBufEnd(bp, len);
    return len;
}


/*
    Enable and control file buffering
 */
PUBLIC int mprEnableFileBuffering(MprFile *file, ssize initialSize, ssize maxSize)
{
    assert(file);

    if (file == 0) {
        return MPR_ERR_BAD_STATE;
    }
    if (initialSize <= 0) {
        initialSize = ME_MAX_BUFFER;
    }
    if (maxSize <= 0) {
        maxSize = ME_MAX_BUFFER;
    }
    if (maxSize <= initialSize) {
        maxSize = initialSize;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(initialSize, maxSize);
    }
    return 0;
}


PUBLIC void mprDisableFileBuffering(MprFile *file)
{
    mprFlushFile(file);
    file->buf = 0;
}


PUBLIC int mprGetFileFd(MprFile *file)
{
    return file->fd;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/fs.c ************/


/**
    fs.c - File system services.

    This module provides a simple cross platform file system abstraction. File systems provide a file system switch and 
    underneath a file system provider that implements actual I/O.
    This module is not thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/************************************ Code ************************************/

PUBLIC MprFileSystem *mprCreateFileSystem(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    /*
        FUTURE: evolve this to support multiple file systems in a single system
     */
#if ME_ROM
    fs = (MprFileSystem*) mprCreateRomFileSystem(path);
#else
    fs = (MprFileSystem*) mprCreateDiskFileSystem(path);
#endif

#if ME_WIN_LIKE
    fs->separators = sclone("\\/");
    fs->newline = sclone("\r\n");
#elif CYGWIN
    fs->separators = sclone("/\\");
    fs->newline = sclone("\n");
#else
    fs->separators = sclone("/");
    fs->newline = sclone("\n");
#endif

#if ME_WIN_LIKE || MACOSX || CYGWIN
    fs->caseSensitive = 0;
#else
    fs->caseSensitive = 1;
#endif

#if ME_WIN_LIKE || VXWORKS || CYGWIN
    fs->hasDriveSpecs = 1;
#endif

    if (MPR->fileSystem == NULL) {
        MPR->fileSystem = fs;
    }
    fs->root = mprGetAbsPath(path);
    if ((cp = strpbrk(fs->root, fs->separators)) != 0) {
        *++cp = '\0';
    }
#if ME_WIN_LIKE || CYGWIN
    fs->cygwin = mprReadRegistry("HKEY_LOCAL_MACHINE\\SOFTWARE\\Cygwin\\setup", "rootdir");
    fs->cygdrive = sclone("/cygdrive");
#endif
    return fs;
}


PUBLIC void mprAddFileSystem(MprFileSystem *fs)
{
    assert(fs);

    /* NOTE: this does not currently add a file system. It merely replaces the existing file system. */
    MPR->fileSystem = fs;
}


/*
    Note: path can be null
 */
PUBLIC MprFileSystem *mprLookupFileSystem(cchar *path)
{
    return MPR->fileSystem;
}


PUBLIC cchar *mprGetPathNewline(cchar *path)
{
    MprFileSystem   *fs;

    assert(path);

    fs = mprLookupFileSystem(path);
    return fs->newline;
}


PUBLIC cchar *mprGetPathSeparators(cchar *path)
{
    MprFileSystem   *fs;

    assert(path);

    fs = mprLookupFileSystem(path);
    return fs->separators;
}


PUBLIC char mprGetPathSeparator(cchar *path)
{
    MprFileSystem   *fs;

    assert(path);
    fs = mprLookupFileSystem(path);
    return fs->separators[0];
}


PUBLIC void mprSetPathSeparators(cchar *path, cchar *separators)
{
    MprFileSystem   *fs;

    assert(path);
    assert(separators);

    fs = mprLookupFileSystem(path);
    fs->separators = sclone(separators);
}


PUBLIC void mprSetPathNewline(cchar *path, cchar *newline)
{
    MprFileSystem   *fs;

    assert(path);
    assert(newline);

    fs = mprLookupFileSystem(path);
    fs->newline = sclone(newline);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/hash.c ************/


/*
    hash.c - Fast hashing hash lookup module

    This hash hash uses a fast key lookup mechanism. Keys may be C strings or unicode strings. The hash value entries 
    are arbitrary pointers. The keys are hashed into a series of buckets which then have a chain of hash entries.
    The chain in in collating sequence so search time through the chain is on average (N/hashSize)/2.

    This module is not thread-safe. It is the callers responsibility to perform all thread synchronization.
    There is locking solely for the purpose of synchronization with the GC marker()

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/

#ifndef ME_MAX_HASH
    #define ME_MAX_HASH 23           /* Default initial hash size */
#endif

/********************************** Forwards **********************************/

static void *dupKey(MprHash *hash, cvoid *key);
static MprKey *lookupHash(int *index, MprKey **prevSp, MprHash *hash, cvoid *key);
static void manageHashTable(MprHash *hash, int flags);

/*********************************** Code *************************************/
/*
    Create a new hash hash of a given size. Caller should provide a size that is a prime number for the greatest efficiency.
    Can use hashSize -1, 0 to get a default hash.
 */
PUBLIC MprHash *mprCreateHash(int hashSize, int flags)
{
    MprHash     *hash;

    if ((hash = mprAllocObjNoZero(MprHash, manageHashTable)) == 0) {
        return 0;
    }
    if (hashSize < ME_MAX_HASH) {
        hashSize = ME_MAX_HASH;
    }
    if ((hash->buckets = mprAllocZeroed(sizeof(MprKey*) * hashSize)) == 0) {
        return NULL;
    }
    hash->flags = flags | MPR_OBJ_HASH;
    hash->size = hashSize;
    hash->length = 0;
    if (!(flags & MPR_HASH_STABLE)) {
        hash->mutex = mprCreateLock();
    } else {
        hash->mutex = 0;
    }
#if ME_CHAR_LEN > 1 && KEEP
    if (hash->flags & MPR_HASH_UNICODE) {
        if (hash->flags & MPR_HASH_CASELESS) {
            hash->fn = (MprHashProc) whashlower;
        } else {
            hash->fn = (MprHashProc) whash;
        }
    } else 
#endif
    {
        if (hash->flags & MPR_HASH_CASELESS) {
            hash->fn = (MprHashProc) shashlower;
        } else {
            hash->fn = (MprHashProc) shash;
        }
    }
    return hash;
}


static void manageHashTable(MprHash *hash, int flags)
{
    MprKey      *sp;
    int         i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(hash->mutex);
        mprMark(hash->buckets);
        lock(hash);
        for (i = 0; i < hash->size; i++) {
            for (sp = (MprKey*) hash->buckets[i]; sp; sp = sp->next) {
                mprMark(sp);
                if (!(hash->flags & MPR_HASH_STATIC_VALUES)) {
#if ME_DEBUG
                    if (sp->data && !mprIsValid(sp->data)) {
                        mprDebug("error mpr hash", 0, "Data in key %s is not valid", sp->key);
                    }
                    assert(sp->data == 0 || mprIsValid(sp->data));
#endif
                    mprMark(sp->data);
                }
                if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
                    assert(mprIsValid(sp->key));
                    mprMark(sp->key);
                }
            }
        }
        unlock(hash);
    }
}


/*
    Insert an entry into the hash hash. If the entry already exists, update its value.  Order of insertion is not preserved.
 */
PUBLIC MprKey *mprAddKey(MprHash *hash, cvoid *key, cvoid *ptr)
{
    MprKey      *sp, *prevSp;
    int         index;

    if (hash == 0 || key == 0) {
        assert(hash && key);
        return 0;
    }
    lock(hash);
    if ((sp = lookupHash(&index, &prevSp, hash, key)) != 0) {
        if (hash->flags & MPR_HASH_UNIQUE) {
            unlock(hash);
            return 0;
        }
        /*
            Already exists. Just update the data.
         */
        sp->data = ptr;
        unlock(hash);
        return sp;
    }
    /*
        Hash entries are managed by manageHashTable
     */
    if ((sp = mprAllocStructNoZero(MprKey)) == 0) {
        unlock(hash);
        return 0;
    }
    sp->data = ptr;
    if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
        sp->key = dupKey(hash, key);
    } else {
        sp->key = (void*) key;
    }
    sp->type = 0;
    sp->bucket = index;
    sp->next = hash->buckets[index];
    hash->buckets[index] = sp;
    hash->length++;
    unlock(hash);
    return sp;
}


PUBLIC MprKey *mprAddKeyWithType(MprHash *hash, cvoid *key, cvoid *ptr, int type)
{
    MprKey  *kp;

    if ((kp = mprAddKey(hash, key, ptr)) != 0) {
        kp->type = type;
    }
    return kp;
}


PUBLIC MprKey *mprAddKeyFmt(MprHash *hash, cvoid *key, cchar *fmt, ...)
{
    va_list     ap;
    char        *value;

    va_start(ap, fmt);
    value = sfmtv(fmt, ap);
    va_end(ap);
    return mprAddKey(hash, key, value);
}


/*
    Multiple insertion. Insert an entry into the hash hash allowing for multiple entries with the same key.
    Order of insertion is not preserved. Lookup cannot be used to retrieve all duplicate keys, some will be shadowed. 
    Use enumeration to retrieve the keys.
 */
PUBLIC MprKey *mprAddDuplicateKey(MprHash *hash, cvoid *key, cvoid *ptr)
{
    MprKey      *sp;
    int         index;

    assert(hash);
    assert(key);

    if ((sp = mprAllocStructNoZero(MprKey)) == 0) {
        return 0;
    }
    sp->type = 0;
    sp->data = ptr;
    if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
        sp->key = dupKey(hash, key);
    } else {
        sp->key = (void*) key;
    }
    lock(hash);
    index = hash->fn(key, slen(key)) % hash->size;
    sp->bucket = index;
    sp->next = hash->buckets[index];
    hash->buckets[index] = sp;
    hash->length++;
    unlock(hash);
    return sp;
}


PUBLIC int mprRemoveKey(MprHash *hash, cvoid *key)
{
    MprKey      *sp, *prevSp;
    int         index;

#if KEEP
    assert(!(MPR->heap->sweeper == mprGetCurrentThread()));
#endif
    assert(hash);
    assert(key);

    lock(hash);
    if ((sp = lookupHash(&index, &prevSp, hash, key)) == 0) {
        unlock(hash);
        return MPR_ERR_CANT_FIND;
    }
    if (prevSp) {
        prevSp->next = sp->next;
    } else {
        hash->buckets[index] = sp->next;
    }
    hash->length--;
    unlock(hash);
    return 0;
}


PUBLIC MprHash *mprBlendHash(MprHash *hash, MprHash *extra)
{
    MprKey      *kp;

    if (hash == 0 || extra == 0) {
        return hash;
    }
    for (ITERATE_KEYS(extra, kp)) {
        mprAddKey(hash, kp->key, kp->data);
    }
    return hash;
}


PUBLIC MprHash *mprCloneHash(MprHash *master)
{
    MprKey      *kp;
    MprHash     *hash;

    assert(master);

    if ((hash = mprCreateHash(master->size, master->flags)) == 0) {
        return 0;
    }
    kp = mprGetFirstKey(master);
    while (kp) {
        mprAddKey(hash, kp->key, kp->data);
        kp = mprGetNextKey(master, kp);
    }
    return hash;
}


/*
    Lookup a key and return the hash entry
 */
PUBLIC MprKey *mprLookupKeyEntry(MprHash *hash, cvoid *key)
{
    return lookupHash(0, 0, hash, key);
}


/*
    Lookup a key and return the hash entry data
 */
PUBLIC void *mprLookupKey(MprHash *hash, cvoid *key)
{
    MprKey      *sp;

    if ((sp = lookupHash(0, 0, hash, key)) == 0) {
        return 0;
    }
    return (void*) sp->data;
}


/*
    Exponential primes
 */
static int hashSizes[] = {
     19, 29, 59, 79, 97, 193, 389, 769, 1543, 3079, 6151, 12289, 24593, 49157, 98317, 196613, 0
};


static int getHashSize(int numKeys)
{
    int     i;

    for (i = 0; hashSizes[i]; i++) {
        if (numKeys < hashSizes[i]) {
            return hashSizes[i];
        }
    }
    return hashSizes[i - 1];
}


/*
    This is unlocked because it is read-only
 */
static MprKey *lookupHash(int *bucketIndex, MprKey **prevSp, MprHash *hash, cvoid *key)
{
    MprKey      *sp, *prev, *next;
    MprKey      **buckets;
    int         hashSize, i, index, rc;

    if (key == 0 || hash == 0) {
        return 0;
    }
    if (hash->length > hash->size) {
        hashSize = getHashSize(hash->length * 4 / 3);
        if (hash->size < hashSize) {
            if ((buckets = mprAllocZeroed(sizeof(MprKey*) * hashSize)) != 0) {
                hash->length = 0;
                for (i = 0; i < hash->size; i++) {
                    for (sp = hash->buckets[i]; sp; sp = next) {
                        next = sp->next;
                        assert(next != sp);
                        index = hash->fn(sp->key, slen(sp->key)) % hashSize;
                        if (buckets[index]) {
                            sp->next = buckets[index];
                        } else {
                            sp->next = 0;
                        }
                        buckets[index] = sp;
                        sp->bucket = index;
                        hash->length++;
                    }
                }
                hash->size = hashSize;
                hash->buckets = buckets;
            }
        }
    }
    index = hash->fn(key, slen(key)) % hash->size;
    if (bucketIndex) {
        *bucketIndex = index;
    }
    sp = hash->buckets[index];
    prev = 0;

    while (sp) {
#if ME_CHAR_LEN > 1 && KEEP
        if (hash->flags & MPR_HASH_UNICODE) {
            wchar *u1, *u2;
            u1 = (wchar*) sp->key;
            u2 = (wchar*) key;
            rc = -1;
            if (hash->flags & MPR_HASH_CASELESS) {
                rc = wcasecmp(u1, u2);
            } else {
                rc = wcmp(u1, u2);
            }
        } else 
#endif
        if (hash->flags & MPR_HASH_CASELESS) {
            rc = scaselesscmp(sp->key, key);
        } else {
            rc = strcmp(sp->key, key);
        }
        if (rc == 0) {
            if (prevSp) {
                *prevSp = prev;
            }
            return sp;
        }
        prev = sp;
        assert(sp != sp->next);
        sp = sp->next;
    }
    return 0;
}


PUBLIC int mprGetHashLength(MprHash *hash)
{
    return hash->length;
}


/*
    Return the first entry in the hash.
 */
PUBLIC MprKey *mprGetFirstKey(MprHash *hash)
{
    MprKey      *sp;
    int         i;

    if (!hash) {
        return 0;
    }
    for (i = 0; i < hash->size; i++) {
        if ((sp = (MprKey*) hash->buckets[i]) != 0) {
            return sp;
        }
    }
    return 0;
}


/*
    Return the next entry in the hash
 */
PUBLIC MprKey *mprGetNextKey(MprHash *hash, MprKey *last)
{
    MprKey      *sp;
    int         i;

    if (hash == 0) {
        return 0;
    }
    if (last == 0) {
        return mprGetFirstKey(hash);
    }
    if (last->next) {
        return last->next;
    }
    for (i = last->bucket + 1; i < hash->size; i++) {
        if ((sp = (MprKey*) hash->buckets[i]) != 0) {
            return sp;
        }
    }
    return 0;
}


static void *dupKey(MprHash *hash, cvoid *key)
{
#if ME_CHAR_LEN > 1 && KEEP
    if (hash->flags & MPR_HASH_UNICODE) {
        return wclone((wchar*) key);
    } else
#endif
        return sclone(key);
}


PUBLIC MprHash *mprCreateHashFromWords(cchar *str)
{
    MprHash     *hash;
    char        *word, *next;

    hash = mprCreateHash(0, 0);
    word = stok(sclone(str), ", \t\n\r", &next);
    while (word) {
        mprAddKey(hash, word, word);
        word = stok(NULL, ", \t\n\r", &next);
    }
    return hash;
}


PUBLIC char *mprHashToString(MprHash *hash, cchar *join)
{
    MprBuf  *buf;
    MprKey  *kp;
    cchar   *item;

    if (!join) {
        join = ",";
    }
    buf = mprCreateBuf(0, 0);
    for (ITERATE_KEY_DATA(hash, kp, item)) {
        mprPutStringToBuf(buf, item);
        mprPutStringToBuf(buf, join);
    }
    mprAdjustBufEnd(buf, -1);
    mprAddNullToBuf(buf);
    return mprGetBufStart(buf);
}


PUBLIC char *mprHashKeysToString(MprHash *hash, cchar *join)
{
    MprBuf  *buf;
    MprKey  *kp;

    if (!join) {
        join = ",";
    }
    buf = mprCreateBuf(0, 0);
    for (ITERATE_KEYS(hash, kp)) {
        mprPutStringToBuf(buf, kp->key);
        mprPutStringToBuf(buf, join);
    }
    mprAdjustBufEnd(buf, -1);
    mprAddNullToBuf(buf);
    return mprGetBufStart(buf);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/json.c ************/


/**
    json.c - A JSON parser, serializer and query language.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */
/********************************** Includes **********************************/



/*********************************** Locals ***********************************/
/*
    JSON parse tokens
 */
#define JTOK_LBRACE     1
#define JTOK_RBRACE     2
#define JTOK_LBRACKET   3
#define JTOK_RBRACKET   4
#define JTOK_COMMA      5
#define JTOK_COLON      6
#define JTOK_STRING     7
#define JTOK_EOF        8
#define JTOK_ERR        9

#define JSON_EXPR_CHARS "<>=!~"

/****************************** Forward Declarations **************************/

static void adoptChildren(MprJson *obj, MprJson *other);
static void appendItem(MprJson *obj, MprJson *child);
static void appendProperty(MprJson *obj, MprJson *child);
static int checkBlockCallback(MprJsonParser *parser, cchar *name, bool leave);
static void formatValue(MprBuf *buf, MprJson *obj, int flags);
static int gettok(MprJsonParser *parser);
static MprJson *jsonParse(MprJsonParser *parser, MprJson *obj);
static void jsonErrorCallback(MprJsonParser *parser, cchar *msg);
static int peektok(MprJsonParser *parser);
static void puttok(MprJsonParser *parser);
static MprJson *queryCore(MprJson *obj, cchar *key, MprJson *value, int flags);
static MprJson *queryLeaf(MprJson *obj, cchar *property, MprJson *value, int flags);
static MprJson *setProperty(MprJson *obj, cchar *name, MprJson *child);
static void setValue(MprJson *obj, cchar *value);
static int setValueCallback(MprJsonParser *parser, MprJson *obj, cchar *name, MprJson *child);
static void spaces(MprBuf *buf, int count);

/************************************ Code ************************************/

static void manageJson(MprJson *obj, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(obj->name);
        mprMark(obj->value);
        mprMark(obj->prev);
        mprMark(obj->next);
        mprMark(obj->children);
    }
}


/*
    If value is null, return null so query can detect "set" operations
 */
static MprJson *createJsonValue(cchar *value)
{
    MprJson  *obj;

    if (!value) {
        return 0;
    }
    if ((obj = mprAllocObj(MprJson, manageJson)) == 0) {
        return 0;
    }
    setValue(obj, value);
    return obj;
}


PUBLIC MprJson *mprCreateJson(int type)
{
    MprJson  *obj;

    if ((obj = mprAllocObj(MprJson, manageJson)) == 0) {
        return 0;
    }
    obj->type = type ? type : MPR_JSON_OBJ;
    return obj;
}


static MprJson *createObjCallback(MprJsonParser *parser, int type)
{
    return mprCreateJson(type);
}


static void manageJsonParser(MprJsonParser *parser, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(parser->token);
        mprMark(parser->putback);
        mprMark(parser->path);
        mprMark(parser->errorMsg);
        mprMark(parser->buf);
    }
}


/*
    Extended parse. The str and data args are unmanged.
 */
PUBLIC MprJson *mprParseJsonEx(cchar *str, MprJsonCallback *callback, void *data, MprJson *obj, cchar **errorMsg)
{
    MprJsonParser   *parser;
    MprJson         *result, *child, *next;
    int             i;

    if ((parser = mprAllocObj(MprJsonParser, manageJsonParser)) == 0) {
        return 0;
    }
    parser->input = str ? str : "";
    if (callback) {
        parser->callback = *callback;
    }
    if (parser->callback.checkBlock == 0) {
        parser->callback.checkBlock = checkBlockCallback;
    }
    if (parser->callback.createObj == 0) {
        parser->callback.createObj = createObjCallback;
    }
    if (parser->callback.parseError == 0) {
        parser->callback.parseError = jsonErrorCallback;
    }
    if (parser->callback.setValue == 0) {
        parser->callback.setValue = setValueCallback;
    }
    parser->data = data;
    parser->state = MPR_JSON_STATE_VALUE;
    parser->tolerant = 1;
    parser->buf = mprCreateBuf(128, 0); 
    parser->lineNumber = 1;

    if ((result = jsonParse(parser, 0)) == 0) {
        if (errorMsg) {
            *errorMsg = parser->errorMsg;
        }
        return 0;
    }
    if (obj) {
        for (i = 0, child = result->children; child && i < result->length; child = next, i++) {
            next = child->next;
            setProperty(obj, child->name, child);
        }
    } else {
        obj = result;
    }
    return obj;
}


PUBLIC MprJson *mprParseJsonInto(cchar *str, MprJson *obj)
{
    return mprParseJsonEx(str, 0, 0, obj, 0);
}


PUBLIC MprJson *mprParseJson(cchar *str)
{
    return mprParseJsonEx(str, 0, 0, 0, 0);
}


/*
    Inner parse routine. This is called recursively.
 */
static MprJson *jsonParse(MprJsonParser *parser, MprJson *obj)
{
    MprJson      *child;
    cchar       *name;
    int         tokid;

    name = 0;
    while (1) {
        tokid = gettok(parser);
        switch (parser->state) {
        case MPR_JSON_STATE_ERR:
            return 0;

        case MPR_JSON_STATE_EOF:
            return obj;

        case MPR_JSON_STATE_NAME:
            if (tokid == JTOK_RBRACE) {
                puttok(parser);
                return obj;
            } else if (tokid != JTOK_STRING) {
                mprSetJsonError(parser, "Expected property name");
                return 0;
            }
            name = sclone(parser->token);
            if (gettok(parser) != JTOK_COLON) {
                mprSetJsonError(parser, "Expected colon");
                return 0;
            }
            parser->state = MPR_JSON_STATE_VALUE;
            break;

        case MPR_JSON_STATE_VALUE:
            if (tokid == JTOK_STRING) {
                child = createJsonValue(parser->token);

            } else if (tokid == JTOK_LBRACE) {
                parser->state = MPR_JSON_STATE_NAME;
                if (name && parser->callback.checkBlock(parser, name, 0) < 0) {
                    return 0;
                }
                child = jsonParse(parser, parser->callback.createObj(parser, MPR_JSON_OBJ));
                if (gettok(parser) != JTOK_RBRACE) {
                    mprSetJsonError(parser, "Missing closing brace");
                    return 0;
                }
                if (name && parser->callback.checkBlock(parser, name, 1) < 0) {
                    return 0;
                }

            } else if (tokid == JTOK_LBRACKET) {
                if (parser->callback.checkBlock(parser, name, 0) < 0) {
                    return 0;
                }
                child = jsonParse(parser, parser->callback.createObj(parser, MPR_JSON_ARRAY));
                if (gettok(parser) != JTOK_RBRACKET) {
                    mprSetJsonError(parser, "Missing closing bracket");
                    return 0;
                }
                if (parser->callback.checkBlock(parser, name, 1) < 0) {
                    return 0;
                }

            } else if (tokid == JTOK_RBRACKET || tokid == JTOK_RBRACE) {
                puttok(parser);
                return obj;

            } else if (tokid == JTOK_EOF) {
                return obj;

            } else {
                mprSetJsonError(parser, "Unexpected input");
                return 0;
            }
            if (child == 0) {
                return 0;
            }
            if (obj) {
                if (parser->callback.setValue(parser, obj, name, child) < 0) {
                    return 0;
                }
            } else {
                /* Becomes root object */
                obj = child;
            }
            tokid = peektok(parser);
            if (tokid == JTOK_COMMA) {
                gettok(parser);
                if (parser->tolerant) {
                    tokid = peektok(parser); 
                    if (tokid == JTOK_RBRACE || parser->tokid == JTOK_RBRACKET) {
                        return obj;
                    }
                }
                if (obj->type & MPR_JSON_OBJ) {
                    parser->state = MPR_JSON_STATE_NAME;
                }
            } else if (tokid == JTOK_RBRACE || parser->tokid == JTOK_RBRACKET || tokid == JTOK_EOF) {
                return obj;
            } else {
                mprSetJsonError(parser, "Unexpected input. Missing comma.");
                return 0;
            }
            break;
        }
    }
}


static void eatRestOfComment(MprJsonParser *parser)
{
    cchar   *cp;

    cp = parser->input;
    if (*cp == '/') {
        for (cp++; *cp && *cp != '\n'; cp++) {}
        parser->lineNumber++;

    } else if (*cp == '*') {
        for (cp++; cp[0] && (cp[0] != '*' || cp[1] != '/'); cp++) {
            if (*cp == '\n') {
                parser->lineNumber++;
            }
        }
        cp += 2;
    }
    parser->input = cp;
}


/*
    Peek at the next token without consuming it
 */
static int peektok(MprJsonParser *parser)
{
    int     tokid;

    tokid = gettok(parser);
    puttok(parser);
    return tokid;
}


/*
    Put back the token so it can be refetched via gettok
 */
static void puttok(MprJsonParser *parser)
{
    parser->putid = parser->tokid;
    parser->putback = sclone(parser->token);
}


/*
    Get the next token. Returns the token ID and also stores it in parser->tokid.
    Residuals: parser->token set to the token text. parser->errorMsg for parse error diagnostics.
    Note: parser->token is a reference into the parse buffer and will be overwritten on the next call to gettok.
 */
static int gettok(MprJsonParser *parser)
{
    cchar   *cp;
    ssize   len;
    int     c;

    assert(parser);
    assert(parser->input);
    mprFlushBuf(parser->buf);

    if (parser->state == MPR_JSON_STATE_EOF || parser->state == MPR_JSON_STATE_ERR) {
        return parser->tokid = JTOK_EOF;
    }
    if (parser->putid) {
        parser->tokid = parser->putid;
        parser->putid = 0;
        mprPutStringToBuf(parser->buf, parser->putback);

    } else {
        for (parser->tokid = 0; !parser->tokid; ) {
            c = *parser->input++;
            switch (c) {
            case '\0':
                parser->state = MPR_JSON_STATE_EOF;
                parser->tokid = JTOK_EOF;
                parser->input--;
                break;
            case ' ':
            case '\t':
                break;
            case '\n':
                parser->lineNumber++;
                break;
            case '{':
                parser->tokid = JTOK_LBRACE;
                mprPutCharToBuf(parser->buf, c);
                break;
            case '}':
                parser->tokid = JTOK_RBRACE;
                mprPutCharToBuf(parser->buf, c);
                break;
            case '[':
                 parser->tokid = JTOK_LBRACKET;
                mprPutCharToBuf(parser->buf, c);
                 break;
            case ']':
                parser->tokid = JTOK_RBRACKET;
                mprPutCharToBuf(parser->buf, c);
                break;
            case ',':
                parser->tokid = JTOK_COMMA;
                mprPutCharToBuf(parser->buf, c);
                break;
            case ':':
                parser->tokid = JTOK_COLON;
                mprPutCharToBuf(parser->buf, c);
                break;
            case '/':
                c = *parser->input;
                if (c == '*' || c == '/') {
                    eatRestOfComment(parser);
                } else {
                    mprSetJsonError(parser, "Unexpected input");
                }
                break;

            case '\\':
                mprSetJsonError(parser, "Bad input state");
                break;

            case '"':
            case '\'':
                if (parser->state == MPR_JSON_STATE_NAME || parser->state == MPR_JSON_STATE_VALUE) {
                    for (cp = parser->input; *cp; cp++) {
                        if (*cp == '\\' && cp[1]) {
                            cp++;
                        } else if (*cp == c) {
                            parser->tokid = JTOK_STRING;
                            parser->input = cp + 1;
                            break;
                        }
                        mprPutCharToBuf(parser->buf, *cp);
                    }
                    if (*cp != c) {
                        mprSetJsonError(parser, "Missing closing quote");
                    }
                } else {
                    mprSetJsonError(parser, "Unexpected quote");
                }
                break;

            default:
                parser->input--;
                if (parser->state == MPR_JSON_STATE_NAME) {
                    if (parser->tolerant) {
                        /* Allow unquoted names */
                        for (cp = parser->input; *cp; cp++) {
                            c = *cp;
                            if (c == '\\' && cp[1]) {
                                if (isxdigit((uchar) cp[1]) && isxdigit((uchar) cp[2]) && 
                                    isxdigit((uchar) cp[3]) && isxdigit((uchar) cp[4])) {
                                    c = (int) stoiradix(cp, 16, NULL);
                                    cp += 3;
                                } else {
                                    c = *cp++;
                                }
                            } else if (isspace((uchar) c) || c == ':') {
                                break;
                            }
                            mprPutCharToBuf(parser->buf, c);
                        }
                        parser->tokid = JTOK_STRING;
                        parser->input = cp;
                    }

                } else if (parser->state == MPR_JSON_STATE_VALUE) {
                    if ((cp = strpbrk(parser->input, " \t\n\r:,}]")) == 0) {
                        cp = &parser->input[slen(parser->input)];
                    }
                    len = cp - parser->input;
                    mprPutBlockToBuf(parser->buf, parser->input, len);
                    parser->tokid = JTOK_STRING;
                    parser->input += len;

                } else {
                    mprSetJsonError(parser, "Unexpected input");
                }
                break;
            }
        }
    }
    mprAddNullToBuf(parser->buf);
    parser->token = mprGetBufStart(parser->buf);
    return parser->tokid;
}


/*
    Supports hashes where properties are strings or hashes of strings. N-level nest is supported.
 */
static char *objToString(MprBuf *buf, MprJson *obj, int indent, int flags)
{
    MprJson  *child;
    int     quotes, pretty, index;

    pretty = flags & MPR_JSON_PRETTY;
    quotes = flags & MPR_JSON_QUOTES;

    if (obj->type & MPR_JSON_ARRAY) {
        mprPutCharToBuf(buf, '[');
        indent++;
        if (pretty) mprPutCharToBuf(buf, '\n');

        for (ITERATE_JSON(obj, child, index)) {
            if (pretty) spaces(buf, indent);
            objToString(buf, child, indent, flags);
            if (child->next != obj->children) {
                mprPutCharToBuf(buf, ',');
            }
            if (pretty) mprPutCharToBuf(buf, '\n');
        }
        if (pretty) spaces(buf, --indent);
        mprPutCharToBuf(buf, ']');

    } else if (obj->type & MPR_JSON_OBJ) {
        mprPutCharToBuf(buf, '{');
        indent++;
        if (pretty) mprPutCharToBuf(buf, '\n');
        for (ITERATE_JSON(obj, child, index)) {
            if (pretty) spaces(buf, indent);
            if (quotes) mprPutCharToBuf(buf, '"');
            mprPutStringToBuf(buf, child->name);
            if (quotes) mprPutCharToBuf(buf, '"');
            if (pretty) {
                mprPutStringToBuf(buf, ": ");
            } else {
                mprPutCharToBuf(buf, ':');
            }
            objToString(buf, child, indent, flags);
            if (child->next != obj->children) {
                mprPutCharToBuf(buf, ',');
            }
            if (pretty) mprPutCharToBuf(buf, '\n');
        }
        if (pretty) spaces(buf, --indent);
        mprPutCharToBuf(buf, '}');
        
    } else {
        formatValue(buf, obj, flags);
    }
    return sclone(mprGetBufStart(buf));
}


/*
    Serialize into JSON format.
 */
PUBLIC char *mprJsonToString(MprJson *obj, int flags)
{
    if (!obj) {
        return 0;
    }
    return objToString(mprCreateBuf(0, 0), obj, 0, flags);
}


static void setValue(MprJson *obj, cchar *value)
{
    if (value == 0) {
        value = "";
    }
    obj->type = MPR_JSON_VALUE;
    if (scaselessmatch(value, "false")) {
        obj->type |= MPR_JSON_FALSE;
    } else if (scaselessmatch(value, "null")) {
        obj->type |= MPR_JSON_NULL;
        value = 0;
    } else if (scaselessmatch(value, "true")) {
        obj->type |= MPR_JSON_TRUE;
    } else if (scaselessmatch(value, "undefined")) {
        obj->type |= MPR_JSON_UNDEFINED;
    } else if (sfnumber(value)) {
        obj->type |= MPR_JSON_NUMBER;
    } else if (*value == '/' && value[slen(value) - 1] == '/') {
        obj->type |= MPR_JSON_REGEXP;
    } else {
        obj->type |= MPR_JSON_STRING;
    }
    obj->value = value ? sclone(value) : 0;
}


static void formatValue(MprBuf *buf, MprJson *obj, int flags)
{
    cchar   *cp;

    if (!(obj->type & MPR_JSON_STRING) && !(flags & MPR_JSON_STRINGS)) {
        if (obj->value == 0) {
            mprPutStringToBuf(buf, "null");
        } else if (obj->type & MPR_JSON_REGEXP) {
            mprPutToBuf(buf, "\"/%s/\"", obj->value);
        } else {
            mprPutStringToBuf(buf, obj->value);
        }
        return;
    }
    mprPutCharToBuf(buf, '"');
    for (cp = obj->value; *cp; cp++) {
        if (*cp == '\"' || *cp == '\\') {
            mprPutCharToBuf(buf, '\\');
            mprPutCharToBuf(buf, *cp);
        } else if (*cp == '\r') {
            mprPutStringToBuf(buf, "\\\\r");
        } else if (*cp == '\n') {
            mprPutStringToBuf(buf, "\\\\n");
        } else {
            mprPutCharToBuf(buf, *cp);
        }
    }
    mprPutCharToBuf(buf, '"');
}


static void spaces(MprBuf *buf, int count)
{
    int     i;

    for (i = 0; i < count; i++) {
        mprPutStringToBuf(buf, "    ");
    }
}


static void jsonErrorCallback(MprJsonParser *parser, cchar *msg)
{
    if (!parser->errorMsg) {
        if (parser->path) {
            parser->errorMsg = sfmt("JSON Parse Error: %s\nIn file '%s' at line %d. Token \"%s\"",
                msg, parser->path, parser->lineNumber + 1, parser->token);
        } else {
            parser->errorMsg = sfmt("JSON Parse Error: %s\nAt line %d. Token \"%s\"",
                msg, parser->lineNumber + 1, parser->token);
        }
        mprDebug("mpr json", 4, "%s", parser->errorMsg);
    }
}


PUBLIC void mprSetJsonError(MprJsonParser *parser, cchar *fmt, ...)
{
    va_list     args;
    cchar       *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    (parser->callback.parseError)(parser, msg);
    va_end(args);
    parser->state = MPR_JSON_STATE_ERR;
    parser->tokid = JTOK_ERR;
}

/*
 **************** JSON object query API -- only works for MprJson implementations *****************
 */

PUBLIC int mprBlendJson(MprJson *dest, MprJson *src, int flags)
{
    MprJson     *dp, *sp, *child;
    cchar       *trimmedName;
    int         kind, si, pflags;

    if (src == 0) {
        return 0;
    }
    if (dest == 0) {
        dest = mprCreateJson(MPR_JSON_OBJ);
    }
    if ((MPR_JSON_TYPE_MASK & dest->type) != (MPR_JSON_TYPE_MASK & src->type)) {
        if (flags & (MPR_JSON_APPEND | MPR_JSON_REPLACE)) {
            return 0;
        }
    }
    if (src->type & MPR_JSON_OBJ) {
        if (flags & MPR_JSON_CREATE) {
            /* Already present */
        } else {
            /* Examine each property for: MPR_JSON_APPEND (default) | MPR_JSON_REPLACE) */
            pflags = flags;
            for (ITERATE_JSON(src, sp, si)) {
                trimmedName = sp->name;
                pflags = flags;
                if (flags & MPR_JSON_COMBINE && sp->name) {
                    kind = sp->name[0];
                    if (kind == '+') {
                        pflags = MPR_JSON_APPEND | (flags & MPR_JSON_COMBINE);
                        trimmedName = &sp->name[1];
                    } else if (kind == '-') {
                        pflags = MPR_JSON_REPLACE | (flags & MPR_JSON_COMBINE);
                        trimmedName = &sp->name[1];
                    } else if (kind == '?') {
                        pflags = MPR_JSON_CREATE | (flags & MPR_JSON_COMBINE);
                        trimmedName = &sp->name[1];
                    } else if (kind == '=') {
                        pflags = MPR_JSON_OVERWRITE | (flags & MPR_JSON_COMBINE);
                        trimmedName = &sp->name[1];
                    }
                }
                if ((dp = mprReadJsonObj(dest, trimmedName)) == 0) {
                    /* Absent in destination */
                    if (pflags & MPR_JSON_COMBINE && sp->type == MPR_JSON_OBJ) {
                        dp = mprCreateJson(sp->type);
                        if (trimmedName == &sp->name[1]) {
                            trimmedName = sclone(trimmedName);
                        }
                        setProperty(dest, trimmedName, dp);
                        mprBlendJson(dp, sp, pflags);
                    } else if (!(pflags & MPR_JSON_REPLACE)) {
                        if (trimmedName == &sp->name[1]) {
                            trimmedName = sclone(trimmedName);
                        }
                        setProperty(dest, trimmedName, mprCloneJson(sp));
                    }
                } else if (!(pflags & MPR_JSON_CREATE)) {
                    /* Already present in destination */
                    if (sp->type & MPR_JSON_OBJ && (MPR_JSON_TYPE_MASK & dp->type) != (MPR_JSON_TYPE_MASK & sp->type)) {
                        dp = setProperty(dest, dp->name, mprCreateJson(sp->type));
                    }
                    mprBlendJson(dp, sp, pflags);

                    if (pflags & MPR_JSON_REPLACE && 
                            !(sp->type & (MPR_JSON_OBJ | MPR_JSON_ARRAY)) && sspace(dp->value)) {
                        mprRemoveJsonChild(dest, dp);
                    }
                }
            }
        }
    } else if (src->type & MPR_JSON_ARRAY) {
        if (flags & MPR_JSON_REPLACE) {
            for (ITERATE_JSON(src, sp, si)) {
                if ((child = mprReadJsonValue(dest, sp->value)) != 0) {
                    mprRemoveJsonChild(dest, child);
                }
            }
        } else if (flags & MPR_JSON_CREATE) {
            ;
        } else if (flags & MPR_JSON_APPEND) {
            for (ITERATE_JSON(src, sp, si)) {
                if ((child = mprReadJsonValue(dest, sp->value)) == 0) {
                    appendProperty(dest, mprCloneJson(sp));
                }
            }
        } else {
            /* Default is to MPR_JSON_OVERWRITE */
            if ((sp = mprCloneJson(src)) != 0) {
                adoptChildren(dest, sp);
            }
        }

    } else {
        /* Ordinary string value */
        if (src->value) {
            if (flags & MPR_JSON_APPEND) {
                setValue(dest, sjoin(dest->value, " ", src->value, NULL));
            } else if (flags & MPR_JSON_REPLACE) {
                setValue(dest, sreplace(dest->value, src->value, NULL));
            } else if (flags & MPR_JSON_CREATE) {
                /* Do nothing */
            } else {
                /* MPR_JSON_OVERWRITE (default) */
                dest->value = sclone(src->value);
            }
        }
    }
    return 0;
}


/*
    Simple one-level lookup. Returns the actual JSON object and not a clone.
 */
PUBLIC MprJson *mprReadJsonObj(MprJson *obj, cchar *name)
{
    MprJson      *child;
    int         i, index;

    if (!obj || !name) {
        return 0;
    }
    if (obj->type & MPR_JSON_OBJ) {
        for (ITERATE_JSON(obj, child, i)) {
            if (smatch(child->name, name)) {
                return child;
            }
        }
    } else if (obj->type & MPR_JSON_ARRAY) {
        /*
            Note this does a linear traversal counting array elements. Not the fastest.
            This code is not optimized for huge arrays.
         */
        if (*name == '$') {
            return 0;
        }
        index = (int) stoi(name);
        for (ITERATE_JSON(obj, child, i)) {
            if (i == index) {
                return child;
            }
        }
    }
    return 0;
}


PUBLIC cchar *mprReadJson(MprJson *obj, cchar *name)
{
    MprJson     *item;

    if ((item = mprReadJsonObj(obj, name)) != 0 && item->type & MPR_JSON_VALUE) {
        return item->value;
    }
    return 0;
}


PUBLIC MprJson *mprReadJsonValue(MprJson *obj, cchar *value)
{
    MprJson     *child;
    int         i;

    if (!obj || !value) {
        return 0;
    }
    for (ITERATE_JSON(obj, child, i)) {
        if (smatch(child->value, value)) {
            return child;
        }
    }
    return 0;
}


/*
    JSON expression operators
 */
#define JSON_OP_EQ          1
#define JSON_OP_NE          2
#define JSON_OP_LT          3
#define JSON_OP_LE          4
#define JSON_OP_GT          5
#define JSON_OP_GE          6
#define JSON_OP_MATCH       7
#define JSON_OP_NMATCH      8

#define JSON_PROP_CONTENTS  0x1         /* property has "@" for 'contains'. Only for array contents */
#define JSON_PROP_ELIPSIS   0x2         /* Property was after elipsis:  ..name */
#define JSON_PROP_EXPR      0x4         /* property has expression. Only for objects */
#define JSON_PROP_RANGE     0x8         /* Property is a range N:M */
#define JSON_PROP_WILD      0x10        /* Property is wildcard "*" */
#define JSON_PROP_COMPOUND  0xff        /* property is not just a simple string */
#define JSON_PROP_ARRAY     0x100       /* Hint that an array should be created */

/*
    Split a mulitpart property string and extract the token, deliminator and remaining portion.
    Format expected is: [delimitor] property [delimitor2] rest
    Delimitor characters are: . .. [ ]
    Properties may be simple expressions (field OP value)
    Returns the next property token.
    If value is set, the operation is a "set"
 */
PUBLIC char *getNextTerm(MprJson *obj, MprJson *value, char *str, char **rest, int *termType)
{
    char    *start, *end, *seps, *dot, *expr;
    ssize   i;

    *termType = 0;
    seps = ".[]";
    start = (str || !rest) ? str : *rest;
    if (start == 0) {
        if (rest) {
            *rest = 0;
        }
        return 0;
    }
    while (isspace((int) *start)) start++;
    if (termType && *start == '.') {
        *termType |= JSON_PROP_ELIPSIS;
    }
    if ((i = strspn(start, seps)) > 0) {
        start += i;
    }
    if (*start == '\0') {
        if (rest) {
            *rest = 0;
        }
        return 0;
    }
    if (*start == '*' && (start[1] == '\0' || start[1] == '.' || start[1] == ']')) {
        *termType |= JSON_PROP_WILD;
    } else if (*start == '@' && obj->type & MPR_JSON_ARRAY) {
        *termType |= JSON_PROP_CONTENTS;
    } else if (schr(start, ':') && obj->type & MPR_JSON_ARRAY) {
        *termType |= JSON_PROP_RANGE;
    }
    dot = strpbrk(start, ".[");
    expr = strpbrk(start, " \t]" JSON_EXPR_CHARS);

    if (expr && (!dot || (expr < dot))) {
        /* Assume in [FIELD OP VALUE] */
        end = strpbrk(start, "]");
    } else {
        end = strpbrk(start, seps);
    }
    if (end != 0) {
        if (*end == '[') {
            /* Hint that an array vs object should be created if required */
            *termType |= JSON_PROP_ARRAY;
            *end++ = '\0';
        } else if (*end == '.') {
            *end++ = '\0';
        } else {
            *end++ = '\0';
            i = strspn(end, seps);
            end += i;
            if (*end == '\0') {
                end = 0;
            }
        }
    }
    if (spbrk(start, JSON_EXPR_CHARS) && !(*termType & JSON_PROP_CONTENTS)) {
        *termType |= JSON_PROP_EXPR;
    }
    *rest = end;
    return start;
}


static char *splitExpression(char *property, int *operator, char **value)
{
    char    *seps, *op, *end, *vp;
    ssize   i;

    assert(property);
    assert(operator);
    assert(value);

    seps = JSON_EXPR_CHARS " \t";
    *value = 0;

    if ((op = spbrk(property, seps)) == 0) {
        return 0;
    }
    end = op;
    while (isspace((int) *op)) op++;
    if (end < op) {
        *end = '\0';
    }
    switch (op[0]) {
    case '<':
        *operator = (op[1] == '=') ? JSON_OP_LE: JSON_OP_LT;
        break;
    case '>':
        *operator = (op[1] == '=') ? JSON_OP_GE: JSON_OP_GT;
        break;
    case '=':
        *operator = JSON_OP_EQ;
        break;
    case '!':
        if (op[1] == '~') {
            *operator = JSON_OP_NMATCH;
        } else if (op[1] == '=') {
            *operator = JSON_OP_NE;
        } else {
            *operator = 0;
            return 0;
        }
        break;
    case '~':
        *operator = JSON_OP_MATCH;
        break;
    default:
        *operator = 0;
        return 0;
    }
    if ((vp = spbrk(op, "<>=! \t")) != 0) {
        *vp++ = '\0';
        i = sspn(vp, seps);
        vp += i;
        if (*vp == '\'' || *vp == '"') {
            for (end = &vp[1]; *end; end++) {
                if (*end == '\\' && end[1]) {
                    end++;
                } else if (*end == *vp) {
                    *end = '\0';
                    vp++;
                }
            }
        }
        *value = vp;
    }
    return property;
}


/*
    Note: value is modified
 */
static bool matchExpression(MprJson *obj, int operator, char *value)
{
    if (!(obj->type & MPR_JSON_VALUE)) {
        return 0;
    }
    if ((value = stok(value, "'\"", NULL)) == 0) {
        return 0;
    }
    switch (operator) {
    case JSON_OP_EQ:
        return smatch(obj->value, value);
    case JSON_OP_NE:
        return !smatch(obj->value, value);
    case JSON_OP_LT:
        return scmp(obj->value, value) < 0;
    case JSON_OP_LE:
        return scmp(obj->value, value) <= 0;
    case JSON_OP_GT:
        return scmp(obj->value, value) > 0;
    case JSON_OP_GE:
        return scmp(obj->value, value) >= 0;
    case JSON_OP_MATCH:
        return scontains(obj->value, value) != 0;
    case JSON_OP_NMATCH:
        return scontains(obj->value, value) == 0;
    default:
        return 0;
    }
}


static void appendProperty(MprJson *obj, MprJson *child)
{
    if (child) {
        setProperty(obj, child->name, child);
    }
}


static void appendItem(MprJson *obj, MprJson *child)
{
    if (child) {
        setProperty(obj, 0, child);
    }
}


/*
    WARNING: this steals properties from items
 */
static void appendItems(MprJson *obj, MprJson *items)
{
    MprJson  *child, *next;
    int     index;

    for (index = 0, child = items ? items->children: 0; items && index < items->length; child = next, index++) {
        next = child->next;
        appendItem(obj, child);
    }
}


/*
    Search all descendants down multiple levels: ".."
 */
static MprJson *queryElipsis(MprJson *obj, cchar *property, cchar *rest, MprJson *value, int flags)
{
    MprJson     *child, *result;
    cchar       *subkey;
    int         index;

    result = mprCreateJson(MPR_JSON_ARRAY);
    for (ITERATE_JSON(obj, child, index)) {
        if (smatch(child->name, property)) {
            if (rest == 0) {
                appendItem(result, queryLeaf(obj, property, value, flags));
            } else {
                appendItems(result, queryCore(child, rest, value, flags));
            }
        } else if (child->type & (MPR_JSON_ARRAY | MPR_JSON_OBJ)) {
            if (rest) {
                subkey = sjoin("..", property, ".", rest, NULL);
            } else {
                subkey = sjoin("..", property, NULL);
            }
            appendItems(result, queryCore(child, subkey, value, flags));
        }
    }
    return result;
}


/*
    Search wildcard values: "*"
 */
static MprJson *queryWild(MprJson *obj, cchar *property, cchar *rest, MprJson *value, int flags)
{
    MprJson     *child, *result;
    int         index;

    result = mprCreateJson(MPR_JSON_ARRAY);
    for (ITERATE_JSON(obj, child, index)) {
        if (rest == 0) {
            appendItem(result, queryLeaf(obj, child->name, value, flags));
        } else {
            appendItems(result, queryCore(child, rest, value, flags));
        }
    } 
    return result;
}


/*
    Array contents match: [@ EXPR value]
 */
static MprJson *queryContents(MprJson *obj, char *property, cchar *rest, MprJson *value, int flags)
{
    MprJson     *child, *result;
    char        *v, ibuf[16];
    int         operator, index;

    result = mprCreateJson(MPR_JSON_ARRAY);
    if (!(obj->type & MPR_JSON_ARRAY)) {
        /* Cannot get here */
        assert(0);
        return result;
    }
    if (splitExpression(property, &operator, &v) == 0) {
        return result;
    }
    for (ITERATE_JSON(obj, child, index)) {
        if (matchExpression(child, operator, v)) {
            if (rest == 0) {
                if (flags & MPR_JSON_REMOVE) {
                    appendItem(result, mprRemoveJsonChild(obj, child));
                } else {
                    appendItem(result, queryLeaf(obj, itosbuf(ibuf, sizeof(ibuf), index, 10), value, flags));
                }
            } else {
                assert(0);
                /*  Should never get here as this means the array has objects instead of simple values */
                appendItems(result, queryCore(child, rest, value, flags));
            }
        }
    }
    return result;
}


/*
    Array range of elements
 */
static MprJson *queryRange(MprJson *obj, char *property, cchar *rest, MprJson *value, int flags)
{
    MprJson     *child, *result;
    ssize       start, end;
    char        *e, *s, ibuf[16];
    int         index;

    result = mprCreateJson(MPR_JSON_ARRAY);
    if (!(obj->type & MPR_JSON_ARRAY)) {
        return result;
    }
    if ((s = stok(property, ": \t", &e)) == 0) {
        return result;
    }
    start = (ssize) stoi(s);
    end = (ssize) stoi(e);
    if (start < 0) {
        start = obj->length + start;
    }
    if (end < 0) {
        end = obj->length + end;
    }
    for (ITERATE_JSON(obj, child, index)) {
        if (index < start) continue;
        if (index > end) break;
        if (rest == 0) {
            if (flags & MPR_JSON_REMOVE) {
                appendItem(result, mprRemoveJsonChild(obj, child));
            } else {
                appendItem(result, queryLeaf(obj, itosbuf(ibuf, sizeof(ibuf), index, 10), value, flags));
            }
        } else {
            appendItems(result, queryCore(child, rest, value, flags));
        }
    }
    return result;
}


/*
    Object property match: property EXPR value
 */
static MprJson *queryExpr(MprJson *obj, char *property, cchar *rest, MprJson *value, int flags)
{
    MprJson     *child, *result, *prop;
    char        *v;
    int         index, operator, pi;

    result = mprCreateJson(MPR_JSON_ARRAY);
    if ((property = splitExpression(property, &operator, &v)) == 0) {
        /* Expression does not parse and so does not match */
        return result;
    }
    for (ITERATE_JSON(obj, child, index)) {
        for (ITERATE_JSON(child, prop, pi)) {
            if (matchExpression(prop, operator, v)) {
                if (rest == 0) {
                    if (flags & MPR_JSON_REMOVE) {
                        appendItem(result, mprRemoveJsonChild(obj, child));
                    } else {
                        appendItem(result, queryLeaf(obj, property, value, flags));
                    }
                } else {
                    appendItems(result, queryCore(child, rest, value, flags));
                }
            }
        }
    }
    return result;
}


static MprJson *queryCompound(MprJson *obj, char *property, cchar *rest, MprJson *value, int flags, int termType)
{
    if (termType & JSON_PROP_ELIPSIS) {
        return queryElipsis(obj, property, rest, value, flags);

    } else if (termType & JSON_PROP_WILD) {
        return queryWild(obj, property, rest, value, flags);

    } else if (termType & JSON_PROP_CONTENTS) {
        return queryContents(obj, property, rest, value, flags);

    } else if (termType & JSON_PROP_RANGE) {
        return queryRange(obj, property, rest, value, flags);

    } else if (termType & JSON_PROP_EXPR) {
        return queryExpr(obj, property, rest, value, flags);

    } else {
        assert(0);
    }
    return 0;
}


/*
    Property must be a managed reference
    Value must be cloned so it can be freely linked
 */
static MprJson *queryLeaf(MprJson *obj, cchar *property, MprJson *value, int flags)
{
    MprJson     *child;

    assert(obj);
    assert(property && *property);

    if (value) {
        setProperty(obj, sclone(property), value);
        return 0;

    } else if (flags & MPR_JSON_REMOVE) {
        if ((child = mprReadJsonObj(obj, property)) != 0) {
            return mprRemoveJsonChild(obj, child);
        }
        return 0;

    } else {
        return mprCloneJson(mprReadJsonObj(obj, property));
    }   
}


/*
    Query a JSON object for a property key path and execute the given command.
    The object may be a string, array or object.
    The path is a multipart property. Examples are:
        user.name
        user['name']
        users[2]
        users[2:4]
        users[-4:-1]                //  Range from end of array
        users[name == 'john']
        users[age >= 50]
        users[phone ~ ^206]         //  Starts with 206
        colors[@ != 'red']          //  Array element not 'red'
        people..[name == 'john']    //  Elipsis descends down multiple levels

    If a value is provided, the property described by the keyPath is set to the value.
    If flags includes MPR_JSON_REMOVE, the property described by the keyPath is removed.
    If doing a get, the properties described by the keyPath are cloned and returned as the result.
    If doing a set, ....
    If doing a remove, the removed properties are returned.

    This routine recurses for query expressions. Normal property references are handled without recursion.

    For get, returns list of matching properties. These are cloned.
    For set, returns empty list if successful, else null.

    For remove, returns list of removed elements
 */
static MprJson *queryCore(MprJson *obj, cchar *key, MprJson *value, int flags)
{
    MprJson     *result, *child;
    char        *property, *rest;
    int         termType;

    if (obj == 0 || key == 0 || *key == '\0' || obj->type & MPR_JSON_VALUE) {
        return 0;
    }
    result = 0;
    for (property = getNextTerm(obj, value, sclone(key), &rest, &termType); property; ) {
        if (termType & JSON_PROP_COMPOUND) {
            result = queryCompound(obj, property, rest, value, flags, termType);
            break;

        } else if (rest == 0) {
            if (!result && !value) {
                result = mprCreateJson(MPR_JSON_ARRAY);
            }
            appendItem(result, queryLeaf(obj, property, value, flags));
            break;

        } else if ((child = mprReadJsonObj(obj, property)) == 0) {
            if (value) {
                child = mprCreateJson(termType & JSON_PROP_ARRAY ? MPR_JSON_ARRAY : MPR_JSON_OBJ);
                setProperty(obj, sclone(property), child);
                obj = (MprJson*) child;
            } else {
                break;
            }
        }
        obj = (MprJson*) child;
        property = getNextTerm(obj, value, 0, &rest, &termType);
    }
    return result ? result : mprCreateJson(MPR_JSON_ARRAY);
}


PUBLIC MprJson *mprQueryJson(MprJson *obj, cchar *key, cchar *value, int flags)
{
    return queryCore(obj, key, createJsonValue(value), flags);
}


PUBLIC MprJson *mprGetJsonObj(MprJson *obj, cchar *key)
{
    MprJson      *result;

    if (key && !strpbrk(key, ".[]*")) {
        return mprReadJsonObj(obj, key);
    }
    if ((result = mprQueryJson(obj, key, 0, 0)) != 0 && result->children) {
        return result->children;
    }
    return 0;
}


PUBLIC cchar *mprGetJson(MprJson *obj, cchar *key)
{
    MprJson      *result;

    if (key && !strpbrk(key, ".[]*")) {
        return mprReadJson(obj, key);
    }
    if ((result = mprQueryJson(obj, key, 0, 0)) != 0) {
        if (result->length == 1 && result->children->type & MPR_JSON_VALUE) {
            return result->children->value;
        } else if (result->length > 1) {
            return mprJsonToString(result, 0);
        }
    }
    return 0;
}


PUBLIC int mprSetJsonObj(MprJson *obj, cchar *key, MprJson *value)
{
    if (key && !strpbrk(key, ".[]*")) {
        if (setProperty(obj, sclone(key), value) == 0) {
            return MPR_ERR_CANT_WRITE;
        }
    } else if (queryCore(obj, key, value, 0) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


PUBLIC int mprSetJson(MprJson *obj, cchar *key, cchar *value)
{
    if (key && !strpbrk(key, ".[]*")) {
        if (setProperty(obj, sclone(key), createJsonValue(value)) == 0) {
            return MPR_ERR_CANT_WRITE;
        }
    } else if (queryCore(obj, key, createJsonValue(value), 0) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


PUBLIC MprJson *mprRemoveJson(MprJson *obj, cchar *key)
{
    return mprQueryJson(obj, key, 0, MPR_JSON_REMOVE);
}


MprJson *mprLoadJson(cchar *path)
{
    char    *str;

    if ((str = mprReadPathContents(path, NULL)) != 0) {
        return mprParseJson(str);
    }
    return 0;
}


PUBLIC int mprSaveJson(MprJson *obj, cchar *path, int flags)
{
    MprFile     *file;
    ssize       len;
    char        *buf;

    if (flags == 0) {
        flags = MPR_JSON_PRETTY | MPR_JSON_QUOTES;
    }
    if ((buf = mprJsonToString(obj, flags)) == 0) {
        return MPR_ERR_BAD_FORMAT;
    }
    len = slen(buf);
    if ((file = mprOpenFile(path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, 0644)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if (mprWriteFile(file, buf, len) != len) {
        mprCloseFile(file);
        return MPR_ERR_CANT_WRITE;
    }
    mprWriteFileString(file, "\n");
    mprCloseFile(file);
    return 0;
}


PUBLIC void mprLogJson(int level, MprJson *obj, cchar *fmt, ...)
{
    va_list     ap;
    char        *msg;

    va_start(ap, fmt);
    msg = sfmtv(fmt, ap);
    va_end(ap);
    mprLog("info mpr json", level, "%s: %s", msg, mprJsonToString(obj, MPR_JSON_PRETTY));
}


/*
    Add the child as property in the given object. The child is not cloned and is dedicated to this object.
    NOTE: name must be a managed reference. For arrays, name can be a string index value. If name is null or empty,
    then the property will be appended. This is the typical pattern for appending to an array.
 */
static MprJson *setProperty(MprJson *obj, cchar *name, MprJson *child)
{
    MprJson      *prior, *existing;

    if (!obj || !child) {
        return 0;
    }
    if ((existing = mprReadJsonObj(obj, name)) != 0) {
        existing->value = child->value;
        existing->children = child->children;
        existing->type = child->type;
        existing->length = child->length;
        return existing;
    } 
    if (obj->children) {
        prior = obj->children->prev;
        child->next = obj->children;
        child->prev = prior;
        prior->next->prev = child;
        prior->next = child;
    } else {
        child->next = child->prev = child;
        obj->children = child;
    }
    child->name = name;
    obj->length++;
    return child;
}


static void adoptChildren(MprJson *obj, MprJson *other)
{
    if (obj && other) {
        obj->children = other->children;
        obj->length = other->length;
    }
}


static int checkBlockCallback(MprJsonParser *parser, cchar *name, bool leave)
{
    return 0;
}


/*  
    Note: name is allocated 
 */
static int setValueCallback(MprJsonParser *parser, MprJson *obj, cchar *name, MprJson *child)
{
    return setProperty(obj, name, child) != 0;
}


PUBLIC MprJson *mprRemoveJsonChild(MprJson *obj, MprJson *child)
{
    MprJson      *dep;
    int         index;

    for (ITERATE_JSON(obj, dep, index)) {
        if (dep == child) {
            if (--obj->length == 0) {
                obj->children = 0;
            } else if (obj->children == dep) {
                if (dep->next == dep) {
                    obj->children = 0;
                } else {
                    obj->children = dep->next;
                }
            }
            dep->prev->next = dep->next;
            dep->next->prev = dep->prev;
            child->next = child->prev = 0;
            return child;
        }
    }
    return 0;
}


/*
    Deep copy of an object
 */
PUBLIC MprJson *mprCloneJson(MprJson *obj) 
{
    MprJson     *result, *child;
    int         index;

    if (obj == 0) {
        return 0;
    }
    result = mprCreateJson(obj->type);
    result->name = obj->name;
    result->value = obj->value;
    result->type = obj->type;
    for (ITERATE_JSON(obj, child, index)) {
        setProperty(result, child->name, mprCloneJson(child));
    }
    return result;
}


PUBLIC ssize mprGetJsonLength(MprJson *obj)
{
    if (!obj) {
        return 0;
    }
    return obj->length;
}


PUBLIC MprHash *mprDeserializeInto(cchar *str, MprHash *hash)
{
    MprJson     *obj, *child;
    int         index;

    obj = mprParseJson(str);
    for (ITERATE_JSON(obj, child, index)) {
        mprAddKey(hash, child->name, child->value);
    }
    return hash;
}


PUBLIC MprHash *mprDeserialize(cchar *str)
{
    return mprDeserializeInto(str, mprCreateHash(0, 0));
}


PUBLIC char *mprSerialize(MprHash *hash, int flags)
{
    MprJson  *obj;
    MprKey   *kp;
    cchar    *key;

    obj = mprCreateJson(MPR_JSON_OBJ);
    for (ITERATE_KEYS(hash, kp)) {
        key = (hash->flags & MPR_HASH_STATIC_KEYS) ? sclone(kp->key) : kp->key;
        setProperty(obj, key, createJsonValue(kp->data));
    }
    return mprJsonToString(obj, flags);
}


PUBLIC MprJson *mprHashToJson(MprHash *hash)
{
    MprJson     *obj;
    MprKey      *kp;
    cchar       *key;

    obj = mprCreateJson(0);
    for (ITERATE_KEYS(hash, kp)) {
        key = (hash->flags & MPR_HASH_STATIC_KEYS) ? sclone(kp->key) : kp->key;
        setProperty(obj, key, createJsonValue(kp->data));
    }
    return obj;
}


PUBLIC MprHash *mprJsonToHash(MprJson *json)
{
    MprHash     *hash;
    MprJson     *obj;
    int         index;

    hash = mprCreateHash(0, 0);
    for (ITERATE_JSON(json, obj, index)) {
        if (obj->type & MPR_JSON_VALUE) {
            mprAddKey(hash, obj->name, obj->value);
        }
    }
    return hash;
}


PUBLIC int mprWriteJson(MprJson *obj, cchar *key, cchar *value)
{
    if (setProperty(obj, sclone(key), createJsonValue(value)) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


PUBLIC int mprWriteJsonObj(MprJson *obj, cchar *key, MprJson *value)
{
    if (setProperty(obj, sclone(key), value) == 0) {
        return MPR_ERR_CANT_WRITE;
    }
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/kqueue.c ************/


/**
    kevent.c - Wait for I/O by using kevent on MacOSX Unix systems.

    This module augments the mprWait wait services module by providing kqueue() based waiting support.
    Also see mprAsyncSelectWait and mprSelectWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_EVENT_NOTIFIER == MPR_EVENT_KQUEUE

/********************************** Forwards **********************************/

static void serviceIO(MprWaitService *ws, struct kevent *events, int count);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    struct kevent   ev;

    if ((ws->kq = kqueue()) < 0) {
        mprLog("critical mpr event", 0, "Call to kqueue failed, errno=%d", errno);
        return MPR_ERR_CANT_INITIALIZE;
    }
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(ws->kq, &ev, 1, NULL, 0, NULL) < 0) {
        mprLog("critical mpr event", 0, "Cannot issue notifier wakeup event, errno=%d", errno);
        return MPR_ERR_CANT_INITIALIZE;
    }
    if ((ws->handlerMap = mprCreateList(MPR_FD_MIN, 0)) == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    return 0;
}


PUBLIC void mprManageKqueue(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->handlerMap);

    } else if (flags & MPR_MANAGE_FREE) {
        if (ws->kq >= 0) {
            close(ws->kq);
        }
    }
}


PUBLIC int mprNotifyOn(MprWaitHandler *wp, int mask)
{
    MprWaitService  *ws;
    struct kevent   interest[4], *kp;
    int             fd;

    assert(wp);
    ws = wp->service;
    fd = wp->fd;
    assert(fd >= 0);
    kp = &interest[0];

    lock(ws);
    if (wp->desiredMask != mask) {
        assert(fd >= 0);
        if (wp->desiredMask & MPR_READABLE && !(mask & MPR_READABLE)) {
            EV_SET(kp, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
            kp++;
        }
        if (wp->desiredMask & MPR_WRITABLE && !(mask & MPR_WRITABLE)) {
            EV_SET(kp, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
            kp++;
        }
        if (mask & MPR_READABLE) {
            EV_SET(kp, fd, EVFILT_READ, EV_ADD, 0, 0, 0);
            kp++;
        }
        if (mask & MPR_WRITABLE) {
            EV_SET(kp, fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
            kp++;
        }
        wp->desiredMask = mask;
#if UNUSED
        /*
            Disabled because mprRemoteEvent schedules the dispatcher AND lockes the event service.
            This may cause deadlocks and specifically, mprRemoveEvent may crash while it races with event service
            on another thread.
         */
        if (wp->event) {
            mprRemoveEvent(wp->event);
            wp->event = 0;
        }
#endif
        if (kevent(ws->kq, interest, (int) (kp - interest), NULL, 0, NULL) < 0) {
            /*
                Reissue and get results. Test for broken pipe case.
             */
            if (mask) {
                int rc = kevent(ws->kq, interest, 1, interest, 1, NULL);
                if (rc == 1 && interest[0].flags & EV_ERROR && interest[0].data == EPIPE) {
                    /* Broken PIPE - just ignore */
                } else {
                    mprLog("error mpr event", 0, "Cannot issue notifier wakeup event, errno=%d", errno);
                }
            }
        }
        mprSetItem(ws->handlerMap, fd, mask ? wp : 0);
    }
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single file descriptor. Return a mask of events found. Mask is the events of interest.
    timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTicks timeout)
{
    struct timespec ts;
    struct kevent   interest[2], events[1];
    int             kq, interestCount, rc, result;

    if (timeout < 0) {
        timeout = MAXINT;
    }
    interestCount = 0; 
    if (mask & MPR_READABLE) {
        EV_SET(&interest[interestCount++], fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    }
    if (mask & MPR_WRITABLE) {
        EV_SET(&interest[interestCount++], fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
    }
    if ((kq = kqueue()) < 0) {
        mprLog("error mpr event", 0, "Kqueue returned %d, errno=%d", kq, errno);
        return MPR_ERR_CANT_OPEN;
    }
    ts.tv_sec = ((int) (timeout / 1000));
    ts.tv_nsec = ((int) (timeout % 1000)) * 1000 * 1000;

    mprYield(MPR_YIELD_STICKY);
    rc = kevent(kq, interest, interestCount, events, 1, &ts);
    mprResetYield();

    result = 0;
    if (rc < 0) {
        mprLog("error mpr event", 0, "Kevent returned %d, errno=%d", rc, errno);
    } else if (rc > 0) {
        if (events[0].filter & EVFILT_READ) {
            result |= MPR_READABLE;
        }
        if (events[0].filter == EVFILT_WRITE) {
            result |= MPR_WRITABLE;
        }
    }
    close(kq);
    return result;
}


/*
    Wait for I/O on all registered file descriptors. Timeout is in milliseconds. Return the number of events detected.
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTicks timeout)
{
    struct timespec ts;
    struct kevent   events[ME_MAX_EVENTS];
    int             nevents;

    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if ME_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    ts.tv_sec = ((int) (timeout / 1000));
    ts.tv_nsec = ((int) ((timeout % 1000) * 1000 * 1000));

    mprYield(MPR_YIELD_STICKY);

    if ((nevents = kevent(ws->kq, NULL, 0, events, ME_MAX_EVENTS, &ts)) < 0) {
        if (errno != EINTR) {
            mprLog("error mpr event", 0, "Kevent returned %d, errno %d", nevents, mprGetOsError());
        }
    }
    mprClearWaiting();
    mprResetYield();

    if (nevents > 0) {
        serviceIO(ws, events, nevents);
    }
    ws->wakeRequested = 0;
}


static void serviceIO(MprWaitService *ws, struct kevent *events, int count)
{
    MprWaitHandler      *wp;
    struct kevent       *kev;
    int                 fd, i, mask, prior, err;

    lock(ws);
    for (i = 0; i < count; i++) {
        kev = &events[i];
        fd = (int) kev->ident;
        if (kev->filter == EVFILT_USER) {
            continue;
        }
        if (fd < 0 || (wp = mprGetItem(ws->handlerMap, fd)) == 0) {
            /*
                This can happen if a writable event has been triggered (e.g. MprCmd command stdin pipe) and the 
                pipe is closed. This thread may have waked from kevent before the pipe is closed and the wait 
                handler removed from the map.
             */
            continue;
        }
        assert(mprIsValid(wp));
        mask = 0;
        if (kev->filter == EVFILT_READ) {
            mask |= MPR_READABLE;
        }
        if (kev->filter == EVFILT_WRITE) {
            mask |= MPR_WRITABLE;
        }
        assert(mprIsValid(wp));
        wp->presentMask = mask & wp->desiredMask;

        if (kev->flags & EV_ERROR) {
            err = (int) kev->data;
            if (err == ENOENT) {
                prior = wp->desiredMask;
                mprNotifyOn(wp, 0);
                wp->desiredMask = 0;
                mprNotifyOn(wp, prior);
                mprLog("error mpr event", 0, "Kqueue file descriptor may have been closed and reopened, fd %d", wp->fd);
                continue;

            } else if (err == EBADF || err == EINVAL) {
                mprLog("error mpr event", 0, "Kqueue invalid file descriptor fd %d", wp->fd);
                mprRemoveWaitHandler(wp);
                wp->presentMask = 0;
            }
        }
        if (wp->presentMask) {
            if (wp->flags & MPR_WAIT_IMMEDIATE) {
                (wp->proc)(wp->handlerData, NULL);
            } else {
                /* 
                    Suppress further events while this event is being serviced. User must re-enable.
                 */
                mprNotifyOn(wp, 0);
                mprQueueIOEvent(wp);
            }
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
    Must be async-safe.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;
    struct kevent   ev;

    ws = MPR->waitService;
    if (!ws->wakeRequested) {
        ws->wakeRequested = 1;
        EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        if (kevent(ws->kq, &ev, 1, NULL, 0, NULL) < 0) {
            mprLog("error mpr event", 0, "Cannot issue notifier wakeup event, errno=%d", errno);
        }
    }
}

#else
void kqueueDummy() {}
#endif /* MPR_EVENT_KQUEUE */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/list.c ************/


/**
    list.c - Simple list type.

    The list supports two modes of operation. Compact mode where the list is compacted after removing list items, 
    and no-compact mode where removed items are zeroed. No-compact mode implies that all valid list entries must 
    be non-zero.

    This module is not thread-safe. It is the callers responsibility to perform all thread synchronization.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/

#ifndef ME_MAX_LIST
    #define ME_MAX_LIST   8
#endif

/********************************** Forwards **********************************/

static int growList(MprList *lp, int incr);
static void manageList(MprList *lp, int flags);

/************************************ Code ************************************/
/*
    Create a general growable list structure
 */
PUBLIC MprList *mprCreateList(int size, int flags)
{
    MprList     *lp;

    if ((lp = mprAllocObjNoZero(MprList, manageList)) == 0) {
        return 0;
    }
    lp->flags = flags | MPR_OBJ_LIST;
    lp->size = 0;
    lp->length = 0;
    lp->maxSize = MAXINT;
    if (!(flags & MPR_LIST_STABLE)) {
        lp->mutex = mprCreateLock();
    } else {
        lp->mutex = 0;
    }
    lp->items = 0;
    if (size != 0) {
        mprSetListLimits(lp, size, -1);
    }
    return lp;
}


static void manageList(MprList *lp, int flags)
{
    int     i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(lp->mutex);
        /* OPT - no need to lock as this is running solo */
        lock(lp);
        mprMark(lp->items);
        if (!(lp->flags & MPR_LIST_STATIC_VALUES)) {
            for (i = 0; i < lp->length; i++) {
#if ME_DEBUG
                assert(lp->items[i] == 0 || mprIsValid(lp->items[i]));
#endif
                mprMark(lp->items[i]);
            }
        }
        unlock(lp);
    }
}

/*
    Initialize a list which may not be a memory context.
 */
PUBLIC void mprInitList(MprList *lp, int flags)
{
    lp->flags = 0;
    lp->size = 0;
    lp->length = 0;
    lp->maxSize = MAXINT;
    lp->items = 0;
    lp->mutex = (flags & MPR_LIST_STABLE) ? 0 : mprCreateLock();
}


/*
    Define the list maximum size. If the list has not yet been written to, the initialSize will be observed.
 */
PUBLIC int mprSetListLimits(MprList *lp, int initialSize, int maxSize)
{
    ssize   size;

    if (initialSize <= 0) {
        initialSize = ME_MAX_LIST;
    }
    if (maxSize <= 0) {
        maxSize = MAXINT;
    }
    size = initialSize * sizeof(void*);

    lock(lp);
    if (lp->items == 0) {
        if ((lp->items = mprAlloc(size)) == 0) {
            assert(!MPR_ERR_MEMORY);
            unlock(lp);
            return MPR_ERR_MEMORY;
        }
        memset(lp->items, 0, size);
        lp->size = initialSize;
    }
    lp->maxSize = maxSize;
    unlock(lp);
    return 0;
}


PUBLIC int mprCopyListContents(MprList *dest, MprList *src)
{
    void        *item;
    int         next;

    mprClearList(dest);

    lock(src);
    if (mprSetListLimits(dest, src->size, src->maxSize) < 0) {
        assert(!MPR_ERR_MEMORY);
        unlock(src);
        return MPR_ERR_MEMORY;
    }
    for (next = 0; (item = mprGetNextItem(src, &next)) != 0; ) {
        if (mprAddItem(dest, item) < 0) {
            assert(!MPR_ERR_MEMORY);
            unlock(src);
            return MPR_ERR_MEMORY;
        }
    }
    unlock(src);
    return 0;
}


PUBLIC MprList *mprCloneList(MprList *src)
{
    MprList     *lp;

    if ((lp = mprCreateList(src->size, src->flags)) == 0) {
        return 0;
    }
    if (mprCopyListContents(lp, src) < 0) {
        return 0;
    }
    return lp;
}


PUBLIC MprList *mprCreateListFromWords(cchar *str)
{
    MprList     *list;
    char        *word, *next;

    list = mprCreateList(0, 0);
    word = stok(sclone(str), ", \t\n\r", &next);
    while (word) {
        mprAddItem(list, word);
        word = stok(NULL, ", \t\n\r", &next);
    }
    return list;
}


PUBLIC MprList *mprAppendList(MprList *lp, MprList *add)
{
    void        *item;
    int         next;

    assert(lp);

    for (next = 0; ((item = mprGetNextItem(add, &next)) != 0); ) {
        if (mprAddItem(lp, item) < 0) {
            return 0;
        }
    }
    return lp;
}


/*
    Change the item in the list at index. Return the old item.
 */
PUBLIC void *mprSetItem(MprList *lp, int index, cvoid *item)
{
    void    *old;
    int     length;

    assert(lp);
    assert(lp->size >= 0);
    assert(lp->length >= 0);
    assert(index >= 0);

    length = lp->length;

    if (index >= length) {
        length = index + 1;
    }
    lock(lp);
    if (length > lp->size) {
        if (growList(lp, length - lp->size) < 0) {
            unlock(lp);
            return 0;
        }
    }
    old = lp->items[index];
    lp->items[index] = (void*) item;
    lp->length = length;
    unlock(lp);
    return old;
}



/*
    Add an item to the list and return the item index.
 */
PUBLIC int mprAddItem(MprList *lp, cvoid *item)
{
    int     index;

    assert(lp);
    assert(lp->size >= 0);
    assert(lp->length >= 0);

    lock(lp);
    if (lp->length >= lp->size) {
        if (growList(lp, 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }
    }
    index = lp->length++;
    lp->items[index] = (void*) item;
    unlock(lp);
    return index;
}


PUBLIC int mprAddNullItem(MprList *lp)
{
    int     index;

    assert(lp);
    assert(lp->size >= 0);
    assert(lp->length >= 0);

    lock(lp);
    if (lp->length != 0 && lp->items[lp->length - 1] == 0) {
        index = lp->length - 1;
    } else {
        if (lp->length >= lp->size) {
            if (growList(lp, 1) < 0) {
                unlock(lp);
                return MPR_ERR_TOO_MANY;
            }
        }
        index = lp->length;
        lp->items[index] = 0;
    }
    unlock(lp);
    return index;
}


/*
    Insert an item to the list at a specified position. We insert before the item at "index".
    ie. The inserted item will go into the "index" location and the other elements will be moved up.
 */
PUBLIC int mprInsertItemAtPos(MprList *lp, int index, cvoid *item)
{
    void    **items;
    int     i;

    assert(lp);
    assert(lp->size >= 0);
    assert(lp->length >= 0);
    assert(index >= 0);

    if (index < 0) {
        index = 0;
    }
    lock(lp);
    if (index >= lp->size) {
        if (growList(lp, index - lp->size + 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }

    } else if (lp->length >= lp->size) {
        if (growList(lp, 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }
    }
    if (index >= lp->length) {
        lp->length = index + 1;
    } else {
        /*
            Copy up items to make room to insert
         */
        items = lp->items;
        for (i = lp->length; i > index; i--) {
            items[i] = items[i - 1];
        }
        lp->length++;
    }
    lp->items[index] = (void*) item;
    unlock(lp);
    return index;
}


/*
    Remove an item from the list. Return the index where the item resided.
 */
PUBLIC int mprRemoveItem(MprList *lp, cvoid *item)
{
    int     index;

#if KEEP
    assert(!(MPR->heap->sweeper == mprGetCurrentThread()));
#endif
    if (!lp) {
        return -1;
    }
    lock(lp);
    index = mprLookupItem(lp, item);
    if (index < 0) {
        unlock(lp);
        return index;
    }
    index = mprRemoveItemAtPos(lp, index);
    assert(index >= 0);
    unlock(lp);
    return index;
}


PUBLIC int mprRemoveLastItem(MprList *lp)
{
    assert(lp);
    assert(lp->size > 0);
    assert(lp->length > 0);

    if (lp->length <= 0) {
        return MPR_ERR_CANT_FIND;
    }
    return mprRemoveItemAtPos(lp, lp->length - 1);
}


/*
    Remove an index from the list. Return the index where the item resided.
    The list is compacted.
 */
PUBLIC int mprRemoveItemAtPos(MprList *lp, int index)
{
    void    **items;

    assert(lp);
    assert(lp->size > 0);
    assert(index >= 0 && index < lp->size);
    assert(lp->length > 0);

    if (index < 0 || index >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    lock(lp);
    items = lp->items;
    memmove(&items[index], &items[index + 1], (lp->length - index - 1) * sizeof(void*));
    lp->length--;
    lp->items[lp->length] = 0;
    assert(lp->length >= 0);
    unlock(lp);
    return index;
}


/*
    Remove a set of items. Return 0 if successful.
 */
PUBLIC int mprRemoveRangeOfItems(MprList *lp, int start, int end)
{
    void    **items;
    int     i, count;

    assert(lp);
    assert(lp->size > 0);
    assert(lp->length > 0);
    assert(start > end);

    if (start < 0 || start >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    if (end < 0 || end >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    if (start > end) {
        return MPR_ERR_BAD_ARGS;
    }
    /*
        Copy down to compress
     */
    items = lp->items;
    count = end - start;
    lock(lp);
    for (i = start; i < (lp->length - count); i++) {
        items[i] = items[i + count];
    }
    lp->length -= count;
    for (i = lp->length; i < lp->size; i++) {
        items[i] = 0;
    }
    unlock(lp);
    return 0;
}


/*
    Remove a string item from the list. Return the index where the item resided.
 */
PUBLIC int mprRemoveStringItem(MprList *lp, cchar *str)
{
    int     index;

    assert(lp);

    lock(lp);
    index = mprLookupStringItem(lp, str);
    if (index < 0) {
        unlock(lp);
        return index;
    }
    index = mprRemoveItemAtPos(lp, index);
    assert(index >= 0);
    unlock(lp);
    return index;
}


PUBLIC void *mprGetItem(MprList *lp, int index)
{
    assert(lp);

    if (index < 0 || index >= lp->length) {
        return 0;
    }
    return lp->items[index];
}


PUBLIC void *mprGetFirstItem(MprList *lp)
{
    assert(lp);

    if (lp == 0) {
        return 0;
    }
    if (lp->length == 0) {
        return 0;
    }
    return lp->items[0];
}


PUBLIC void *mprGetLastItem(MprList *lp)
{
    assert(lp);

    if (lp == 0) {
        return 0;
    }
    if (lp->length == 0) {
        return 0;
    }
    return lp->items[lp->length - 1];
}


PUBLIC void *mprGetNextItem(MprList *lp, int *next)
{
    void    *item;
    int     index;

    assert(next);
    assert(*next >= 0);

    if (lp == 0) {
        return 0;
    }
    lock(lp);
    index = *next;
    if (index < lp->length) {
        item = lp->items[index];
        *next = ++index;
        unlock(lp);
        return item;
    }
    unlock(lp);
    return 0;
}


PUBLIC void *mprGetNextStableItem(MprList *lp, int *next)
{
    void    *item;
    int     index;

    assert(next);
    assert(*next >= 0);

    if (lp == 0) {
        return 0;
    }
    assert(lp->flags & MPR_LIST_STABLE);
    index = *next;
    if (index < lp->length) {
        item = lp->items[index];
        *next = ++index;
        return item;
    }
    return 0;
}


PUBLIC void *mprGetPrevItem(MprList *lp, int *next)
{
    void    *item;
    int     index;

    assert(next);

    if (lp == 0) {
        return 0;
    }
    lock(lp);
    if (*next < 0) {
        *next = lp->length;
    }
    index = *next;
    if (--index < lp->length && index >= 0) {
        *next = index;
        item = lp->items[index];
        unlock(lp);
        return item;
    }
    unlock(lp);
    return 0;
}


PUBLIC int mprPushItem(MprList *lp, cvoid *item)
{
    return mprAddItem(lp, item);
}


PUBLIC void *mprPopItem(MprList *lp)
{
    void    *item;
    int     index;

    item = NULL;
    if (lp->length > 0) {
        lock(lp);
        index = lp->length - 1;
        item = mprGetItem(lp, index);
        mprRemoveItemAtPos(lp, index);
        unlock(lp);
    }
    return item;
}


#ifndef mprGetListLength
PUBLIC int mprGetListLength(MprList *lp)
{
    if (lp == 0) {
        return 0;
    }
    return lp->length;
}
#endif


PUBLIC int mprGetListCapacity(MprList *lp)
{
    assert(lp);

    if (lp == 0) {
        return 0;
    }
    return lp->size;
}


PUBLIC void mprClearList(MprList *lp)
{
    int     i;

    assert(lp);

    lock(lp);
    for (i = 0; i < lp->length; i++) {
        lp->items[i] = 0;
    }
    lp->length = 0;
    unlock(lp);
}


PUBLIC int mprLookupItem(MprList *lp, cvoid *item)
{
    int     i;

    assert(lp);

    lock(lp);
    for (i = 0; i < lp->length; i++) {
        if (lp->items[i] == item) {
            unlock(lp);
            return i;
        }
    }
    unlock(lp);
    return MPR_ERR_CANT_FIND;
}


PUBLIC int mprLookupStringItem(MprList *lp, cchar *str)
{
    int     i;

    assert(lp);

    lock(lp);
    for (i = 0; i < lp->length; i++) {
        if (smatch(lp->items[i], str)) {
            unlock(lp);
            return i;
        }
    }
    unlock(lp);
    return MPR_ERR_CANT_FIND;
}


/*
    Grow the list by the requried increment
 */
static int growList(MprList *lp, int incr)
{
    ssize       memsize;
    int         len;

    if (lp->maxSize <= 0) {
        lp->maxSize = MAXINT;
    }
    /*
        Need to grow the list
     */
    if (lp->size >= lp->maxSize) {
        assert(lp->size < lp->maxSize);
        return MPR_ERR_TOO_MANY;
    }
    /*
        If growing by 1, then use the default increment which exponentially grows. Otherwise, assume the caller knows exactly
        how much the list needs to grow.
     */
    if (incr <= 1) {
        len = ME_MAX_LIST + (lp->size * 2);
    } else {
        len = lp->size + incr;
    }
    memsize = len * sizeof(void*);

    if ((lp->items = mprRealloc(lp->items, memsize)) == NULL) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    lp->size = len;
    return 0;
}


static int defaultSort(char **q1, char **q2, void *ctx)
{
    return scmp(*q1, *q2);
}


PUBLIC MprList *mprSortList(MprList *lp, MprSortProc cmp, void *ctx)
{
    if (!lp) {
        return 0;
    }
    lock(lp);
    mprSort(lp->items, lp->length, sizeof(void*), cmp, ctx);
    unlock(lp);
    return lp;
}


static void manageKeyValue(MprKeyValue *pair, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(pair->key);
        mprMark(pair->value);
    }
}


PUBLIC MprKeyValue *mprCreateKeyPair(cchar *key, cchar *value, int flags)
{
    MprKeyValue     *pair;

    if ((pair = mprAllocObjNoZero(MprKeyValue, manageKeyValue)) == 0) {
        return 0;
    }
    pair->key = sclone(key);
    pair->value = sclone(value);
    pair->flags = flags;
    return pair;
}


static void swapElt(char *a, char *b, ssize width)
{
    char    tmp;

    if (a == b) {
        return;
    }
    while (width--) {
        tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}


PUBLIC void mprSort(void *base, ssize nelt, ssize esize, MprSortProc cmp, void *ctx) 
{
    char    *array, *pivot, *left, *right;

    if (nelt < 2 || esize <= 0) {
        return;
    }
    if (!cmp) {
        cmp = (MprSortProc) defaultSort;
    }
    array = base;
    left = array;
    right = array + ((nelt - 1) * esize);
    pivot = array + ((nelt / 2) * esize);

    while (left <= right) {
        while (cmp(left, pivot, ctx) < 0) {
            left += esize;
        }
        while (cmp(right, pivot, ctx) > 0) {
            right -= esize;
        }
        if (left <= right) {
            swapElt(left, right, esize);
            left += esize;
            right -= esize;
        }
    }
    /* left and right are swapped */
    mprSort(array, (right - array) / esize + 1, esize, cmp, ctx);
    mprSort(left, nelt - ((left - array) / esize), esize, cmp, ctx);
}


PUBLIC char *mprListToString(MprList *list, cchar *join)
{
    MprBuf  *buf;
    cchar   *s;
    int     next;

    if (!join) {
        join = ",";
    }
    buf = mprCreateBuf(0, 0);
    for (ITERATE_ITEMS(list, s, next)) {
        mprPutStringToBuf(buf, s);
        mprPutStringToBuf(buf, join);
    }
    if (next > 0) {
        mprAdjustBufEnd(buf, -1);
    }
    mprAddNullToBuf(buf);
    return mprGetBufStart(buf);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/lock.c ************/


/**
    lock.c - Thread Locking Support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/*********************************** Includes *********************************/



/***************************** Forward Declarations ***************************/

static void manageLock(MprMutex *lock, int flags);
static void manageSpinLock(MprSpin *lock, int flags);

/************************************ Code ************************************/

PUBLIC MprMutex *mprCreateLock()
{
    MprMutex    *lock;

    if ((lock = mprAllocObjNoZero(MprMutex, manageLock)) == 0) {
        return 0;
    }
    return mprInitLock(lock);
}


static void manageLock(MprMutex *lock, int flags)
{
    if (flags & MPR_MANAGE_FREE) {
        assert(lock);
#if ME_UNIX_LIKE
        pthread_mutex_destroy(&lock->cs);
#elif ME_WIN_LIKE
        lock->freed = 1;
        DeleteCriticalSection(&lock->cs);
#elif VXWORKS
        semDelete(lock->cs);
#endif
    }
}


PUBLIC MprMutex *mprInitLock(MprMutex *lock)
{
#if ME_UNIX_LIKE
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&lock->cs, &attr);
    pthread_mutexattr_destroy(&attr);

#elif ME_WIN_LIKE && !ME_DEBUG && CRITICAL_SECTION_NO_DEBUG_INFO && ME_64 && _WIN32_WINNT >= 0x0600
    InitializeCriticalSectionEx(&lock->cs, ME_MPR_SPIN_COUNT, CRITICAL_SECTION_NO_DEBUG_INFO);
    lock->freed = 0;

#elif ME_WIN_LIKE
    InitializeCriticalSectionAndSpinCount(&lock->cs, ME_MPR_SPIN_COUNT);
    lock->freed = 0;

#elif VXWORKS
    lock->cs = semMCreate(SEM_Q_PRIORITY | SEM_DELETE_SAFE);
#endif
    return lock;
}


/*
    Try to attain a lock. Do not block! Returns true if the lock was attained.
 */
PUBLIC bool mprTryLock(MprMutex *lock)
{
    int     rc;

    if (lock == 0) return 0;

#if ME_UNIX_LIKE
    rc = pthread_mutex_trylock(&lock->cs) != 0;
#elif ME_WIN_LIKE
    rc = TryEnterCriticalSection(&lock->cs) == 0;
#elif VXWORKS
    rc = semTake(lock->cs, NO_WAIT) != OK;
#endif
#if ME_DEBUG
    lock->owner = mprGetCurrentOsThread();
#endif
    return (rc) ? 0 : 1;
}


PUBLIC MprSpin *mprCreateSpinLock()
{
    MprSpin    *lock;

    if ((lock = mprAllocObjNoZero(MprSpin, manageSpinLock)) == 0) {
        return 0;
    }
    return mprInitSpinLock(lock);
}


static void manageSpinLock(MprSpin *lock, int flags)
{
    if (flags & MPR_MANAGE_FREE) {
        assert(lock);
#if USE_MPR_LOCK || MACOSX
        ;
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
        pthread_spin_destroy(&lock->cs);
#elif ME_UNIX_LIKE
        pthread_mutex_destroy(&lock->cs);
#elif ME_WIN_LIKE
        lock->freed = 1;
        DeleteCriticalSection(&lock->cs);
#elif VXWORKS
        semDelete(lock->cs);
#endif
    }
}


/*
    Static version just for mprAlloc which needs locks that don't allocate memory.
 */
PUBLIC MprSpin *mprInitSpinLock(MprSpin *lock)
{
#if USE_MPR_LOCK
    mprInitLock(&lock->cs);

#elif MACOSX
    lock->cs = OS_SPINLOCK_INIT;

#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    pthread_spin_init(&lock->cs, 0);

#elif ME_UNIX_LIKE
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&lock->cs, &attr);
    pthread_mutexattr_destroy(&attr);

#elif ME_WIN_LIKE && !ME_DEBUG && CRITICAL_SECTION_NO_DEBUG_INFO && ME_64 && _WIN32_WINNT >= 0x0600
    InitializeCriticalSectionEx(&lock->cs, ME_MPR_SPIN_COUNT, CRITICAL_SECTION_NO_DEBUG_INFO);
    lock->freed = 0;

#elif ME_WIN_LIKE
    InitializeCriticalSectionAndSpinCount(&lock->cs, ME_MPR_SPIN_COUNT);
    lock->freed = 0;

#elif VXWORKS
    lock->cs = semMCreate(SEM_Q_PRIORITY | SEM_DELETE_SAFE);
#endif /* VXWORKS */

#if ME_DEBUG
    lock->owner = 0;
#endif
    return lock;
}


/*
    Try to attain a lock. Do not block! Returns true if the lock was attained.
 */
PUBLIC bool mprTrySpinLock(MprSpin *lock)
{
    int     rc;

    if (lock == 0) return 0;

#if USE_MPR_LOCK
    mprTryLock(&lock->cs);
#elif MACOSX
    rc = !OSSpinLockTry(&lock->cs);
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    rc = pthread_spin_trylock(&lock->cs) != 0;
#elif ME_UNIX_LIKE
    rc = pthread_mutex_trylock(&lock->cs) != 0;
#elif ME_WIN_LIKE
    rc = (lock->freed) ? 0 : (TryEnterCriticalSection(&lock->cs) == 0);
#elif VXWORKS
    rc = semTake(lock->cs, NO_WAIT) != OK;
#endif
#if ME_DEBUG && COSTLY
    if (rc == 0) {
        assert(lock->owner != mprGetCurrentOsThread());
        lock->owner = mprGetCurrentOsThread();
    }
#endif
    return (rc) ? 0 : 1;
}


/*
    Big global lock. Avoid using this.
 */
PUBLIC void mprGlobalLock()
{
    if (MPR && MPR->mutex) {
        mprLock(MPR->mutex);
    }
}


PUBLIC void mprGlobalUnlock()
{
    if (MPR && MPR->mutex) {
        mprUnlock(MPR->mutex);
    }
}


#if ME_USE_LOCK_MACROS
/*
    Still define these even if using macros to make linking with *.def export files easier
 */
#undef mprLock
#undef mprUnlock
#undef mprSpinLock
#undef mprSpinUnlock
#endif

/*
    Lock a mutex
 */
PUBLIC void mprLock(MprMutex *lock)
{
    if (lock == 0) return;
#if ME_UNIX_LIKE
    pthread_mutex_lock(&lock->cs);
#elif ME_WIN_LIKE
    if (!lock->freed) {
        EnterCriticalSection(&lock->cs);
    }
#elif VXWORKS
    semTake(lock->cs, WAIT_FOREVER);
#endif
#if ME_DEBUG
    /* Store last locker only */ 
    lock->owner = mprGetCurrentOsThread();
#endif
}


PUBLIC void mprUnlock(MprMutex *lock)
{
    if (lock == 0) return;
#if ME_UNIX_LIKE
    pthread_mutex_unlock(&lock->cs);
#elif ME_WIN_LIKE
    LeaveCriticalSection(&lock->cs);
#elif VXWORKS
    semGive(lock->cs);
#endif
}


/*
    Use functions for debug mode. Production release uses macros
 */
/*
    Lock a mutex
 */
PUBLIC void mprSpinLock(MprSpin *lock)
{
    if (lock == 0) return;

#if ME_DEBUG
    /*
        Spin locks don't support recursive locking on all operating systems.
     */
    assert(lock->owner != mprGetCurrentOsThread());
#endif

#if USE_MPR_LOCK
    mprTryLock(&lock->cs);
#elif MACOSX
    OSSpinLockLock(&lock->cs);
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    pthread_spin_lock(&lock->cs);
#elif ME_UNIX_LIKE
    pthread_mutex_lock(&lock->cs);
#elif ME_WIN_LIKE
    if (!lock->freed) {
        EnterCriticalSection(&lock->cs);
    }
#elif VXWORKS
    semTake(lock->cs, WAIT_FOREVER);
#endif
#if ME_DEBUG
    assert(lock->owner != mprGetCurrentOsThread());
    lock->owner = mprGetCurrentOsThread();
#endif
}


PUBLIC void mprSpinUnlock(MprSpin *lock)
{
    if (lock == 0) return;

#if ME_DEBUG
    lock->owner = 0;
#endif

#if USE_MPR_LOCK
    mprUnlock(&lock->cs);
#elif MACOSX
    OSSpinLockUnlock(&lock->cs);
#elif ME_UNIX_LIKE && ME_COMPILER_HAS_SPINLOCK
    pthread_spin_unlock(&lock->cs);
#elif ME_UNIX_LIKE
    pthread_mutex_unlock(&lock->cs);
#elif ME_WIN_LIKE
    LeaveCriticalSection(&lock->cs);
#elif VXWORKS
    semGive(lock->cs);
#endif
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/log.c ************/


/**
    log.c - Multithreaded Portable Runtime (MPR) Logging and error reporting.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/

#ifndef ME_MAX_LOGLINE
    #define ME_MAX_LOGLINE 8192           /* Max size of a log line */
#endif

/********************************** Forwards **********************************/

static void logOutput(cchar *tags, int level, cchar *msg);

/************************************ Code ************************************/
/*
    Put first in file so it is easy to locate in a debugger
 */
PUBLIC void mprBreakpoint()
{
#if DEBUG_PAUSE
    {
        static int  paused = 1;
        int         i;
        printf("Paused to permit debugger to attach - will awake in 2 minutes\n");
        fflush(stdout);
        for (i = 0; i < 120 && paused; i++) {
            mprNap(1000);
        }
    }
#endif
}


PUBLIC void mprCreateLogService()
{
    MPR->logFile = MPR->stdError;
}


PUBLIC int mprStartLogging(cchar *logSpec, int flags)
{
    MprFile     *file;
    char        *levelSpec, *path;
    int         level;

    if (logSpec == 0 || strcmp(logSpec, "none") == 0) {
        return 0;
    }
    level = -1;
    file = 0;
    MPR->logPath = path = sclone(logSpec);
    if ((levelSpec = strrchr(path, ':')) != 0 && isdigit((uchar) levelSpec[1])) {
        *levelSpec++ = '\0';
        level = atoi(levelSpec);
    }
    if (strcmp(path, "stdout") == 0) {
        file = MPR->stdOutput;
    } else if (strcmp(path, "stderr") == 0) {
        file = MPR->stdError;
#if !ME_ROM
    } else {
        MprPath     info;
        int         mode;
        mode = (flags & MPR_LOG_ANEW) ? O_TRUNC : O_APPEND;
        mode |= O_CREAT | O_WRONLY | O_TEXT;
        if (MPR->logBackup > 0) {
            mprGetPathInfo(path, &info);
            if (MPR->logSize <= 0 || (info.valid && info.size > MPR->logSize) || (flags & MPR_LOG_ANEW)) {
                mprBackupLog(path, MPR->logBackup);
            }
        }
        if ((file = mprOpenFile(path, mode, 0664)) == 0) {
            mprLog("error mpr log", 0, "Cannot open log file %s, errno=%d", path, errno);
            return MPR_ERR_CANT_OPEN;
        }
#endif
    }
    MPR->flags |= (flags & (MPR_LOG_DETAILED | MPR_LOG_ANEW | MPR_LOG_CONFIG | MPR_LOG_CMDLINE | MPR_LOG_TAGGED));

    if (level >= 0) {
        mprSetLogLevel(level);
    }
    if (file) {
        mprSetLogFile(file);
    }
    if (flags & MPR_LOG_CONFIG) {
        mprLogConfig();
    }
    return 0;
}


PUBLIC void mprLogConfig()
{
    cchar   *name;

    name = MPR->name;
    mprLog(name, 2, "Configuration for %s", mprGetAppTitle());
    mprLog(name, 2, "----------------------------------");
    mprLog(name, 2, "Version:            %s", ME_VERSION);
    mprLog(name, 2, "BuildType:          %s", ME_DEBUG ? "Debug" : "Release");
    mprLog(name, 2, "CPU:                %s", ME_CPU);
    mprLog(name, 2, "OS:                 %s", ME_OS);
    mprLog(name, 2, "Host:               %s", mprGetHostName());
    mprLog(name, 2, "Configure:          %s", ME_CONFIG_CMD);
    mprLog(name, 2, "----------------------------------");
}


PUBLIC int mprBackupLog(cchar *path, int count)
{
    char    *from, *to;
    int     i;

    for (i = count - 1; i > 0; i--) {
        from = sfmt("%s.%d", path, i - 1);
        to = sfmt("%s.%d", path, i);
        unlink(to);
        rename(from, to);
    }
    from = sfmt("%s", path);
    to = sfmt("%s.0", path);
    unlink(to);
    if (rename(from, to) < 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


PUBLIC void mprSetLogBackup(ssize size, int backup, int flags)
{
    MPR->logBackup = backup;
    MPR->logSize = size;
    MPR->flags |= (flags & MPR_LOG_ANEW);
}


/*
    Legacy error messages
 */
PUBLIC void mprError(cchar *format, ...)
{
    va_list     args;
    char        buf[ME_MAX_LOGLINE], tagbuf[128];

    va_start(args, format);
    fmt(tagbuf, sizeof(tagbuf), "%s error", MPR->name);
    logOutput(tagbuf, 0, fmtv(buf, sizeof(buf), format, args));
    va_end(args);
}


PUBLIC void mprLogProc(cchar *tags, int level, cchar *fmt, ...)
{
    va_list     args;
    char        buf[ME_MAX_LOGLINE];

    va_start(args, fmt);
    logOutput(tags, level, fmtv(buf, sizeof(buf), fmt, args));
    va_end(args);
}


PUBLIC void mprAssert(cchar *loc, cchar *msg)
{
#if ME_MPR_DEBUG_LOGGING
    char    buf[ME_MAX_LOGLINE];

    if (loc) {
#if ME_UNIX_LIKE
        snprintf(buf, sizeof(buf), "Assertion %s, failed at %s", msg, loc);
#else
        sprintf(buf, "Assertion %s, failed at %s", msg, loc);
#endif
        msg = buf;
    }
    mprLogProc("debug assert", 0, "%s", buf);
#if ME_DEBUG_WATSON
    fprintf(stderr, "Pause for debugger to attach\n");
    mprSleep(24 * 3600 * 1000);
#endif
#endif
}


/*
    Output a log message to the log handler
 */
static void logOutput(cchar *tags, int level, cchar *msg)
{
    MprLogHandler   handler;

    if (level < 0 || level > mprGetLogLevel()) {
        return;
    }
    handler = MPR->logHandler;
    if (handler != 0) {
        (handler)(tags, level, msg);
        return;
    }
    mprDefaultLogHandler(tags, level, msg);
}


static void backupLog()
{
#if !ME_ROM
    MprPath     info;
    MprFile     *file;
    int         mode;

    mprGetPathInfo(MPR->logPath, &info);
    if (info.valid && info.size > MPR->logSize) {
        lock(MPR);
        mprSetLogFile(0);
        mprBackupLog(MPR->logPath, MPR->logBackup);
        mode = O_CREAT | O_WRONLY | O_TEXT;
        if ((file = mprOpenFile(MPR->logPath, mode, 0664)) == 0) {
            mprLog("error mpr log", 0, "Cannot open log file %s, errno=%d", MPR->logPath, errno);
            MPR->logSize = MAXINT;
            unlock(MPR);
            return;
        }
        mprSetLogFile(file);
        unlock(MPR);
    }
#endif
}


/*
    If MPR_LOG_DETAILED with tags, the format is:
        MM/DD/YY HH:MM:SS LEVEL TAGS, Message
    Otherwise just the message is output
 */
PUBLIC void mprDefaultLogHandler(cchar *tags, int level, cchar *msg)
{
    MprFile     *file;
    char        tbuf[128];
    static int  check = 0;

    if ((file = MPR->logFile) == 0) {
        return;
    }
    if (MPR->logBackup && MPR->logSize && (check++ % 1000) == 0) {
        backupLog();
    }
    if (tags && *tags) {
        if (MPR->flags & MPR_LOG_DETAILED) {
            fmt(tbuf, sizeof(tbuf), "%s %d %s, ", mprGetDate(MPR_LOG_DATE), level, tags);
            mprWriteFileString(file, tbuf);
        } else if (MPR->flags & MPR_LOG_TAGGED) {
            if (schr(tags, ' ')) {
                tags = ssplit(sclone(tags), " ", NULL);
            }
            if (!isupper((uchar) *tags)) {
                tags = stitle(tags);
            }
            mprWriteFileFmt(file, "%12s ", sfmt("[%s]", tags));
        }
    }
    mprWriteFileString(file, msg);
    mprWriteFileString(file, "\n");
#if ME_MPR_OSLOG
    if (level == 0) {
        mprWriteToOsLog(sfmt("%s: %d %s: %s", MPR->name, level, tags, msg), level);
    }
#endif
}


/*
    Return the raw O/S error code
 */
PUBLIC int mprGetOsError()
{
#if ME_WIN_LIKE
    int     rc;
    rc = GetLastError();

    /*
        Client has closed the pipe
     */
    if (rc == ERROR_NO_DATA) {
        return EPIPE;
    }
    return rc;
#elif ME_UNIX_LIKE || VXWORKS
    return errno;
#else
    return 0;
#endif
}


PUBLIC void mprSetOsError(int error)
{
#if ME_WIN_LIKE
    SetLastError(error);
#elif ME_UNIX_LIKE || VXWORKS
    errno = error;
#endif
}


/*
    Return the mapped (portable, Posix) error code
 */
PUBLIC int mprGetError()
{
#if !ME_WIN_LIKE
    return mprGetOsError();
#else
    int     err;

    err = mprGetOsError();
    switch (err) {
    case ERROR_SUCCESS:
        return 0;
    case ERROR_FILE_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EPERM;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
        return ENOMEM;
    case ERROR_PATH_BUSY:
    case ERROR_BUSY_DRIVE:
    case ERROR_NETWORK_BUSY:
    case ERROR_PIPE_BUSY:
    case ERROR_BUSY:
        return EBUSY;
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_BAD_PATHNAME:
    case ERROR_BAD_ARGUMENTS:
        return EINVAL;
    case WSAENOTSOCK:
        return ENOENT;
    case WSAEINTR:
        return EINTR;
    case WSAEBADF:
        return EBADF;
    case WSAEACCES:
        return EACCES;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEALREADY:
        return EALREADY;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAENETDOWN:
        return ENETDOWN;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    }
    return MPR_ERR;
#endif
}


/*
    Set the mapped (portable, Posix) error code
 */
PUBLIC void mprSetError(error)
{
#if !ME_WIN_LIKE
    mprSetOsError(error);
    return;
#else
    switch (error) {
    case ENOENT:
        error = ERROR_FILE_NOT_FOUND;
        break;

    case EPERM:
        error = ERROR_ACCESS_DENIED;
        break;

    case EBADF:
        error = ERROR_INVALID_HANDLE;
        break;

    case ENOMEM:
        error = ERROR_NOT_ENOUGH_MEMORY;
        break;

    case EBUSY:
        error = ERROR_BUSY;
        break;

    case EEXIST:
        error = ERROR_FILE_EXISTS;
        break;

    case EINVAL:
        error = ERROR_BAD_ARGUMENTS;
        break;

    case EINTR:
        error = WSAEINTR;
        break;

    case EACCES:
        error = WSAEACCES;
        break;

    case EINPROGRESS:
        error = WSAEINPROGRESS;
        break;

    case EALREADY:
        error = WSAEALREADY;
        break;

    case EADDRINUSE:
        error = WSAEADDRINUSE;
        break;

    case EADDRNOTAVAIL:
        error = WSAEADDRNOTAVAIL;
        break;

    case ENETDOWN:
        error = WSAENETDOWN;
        break;

    case ENETUNREACH:
        error = WSAENETUNREACH;
        break;

    case ECONNABORTED:
        error = WSAECONNABORTED;
        break;

    case ECONNRESET:
        error = WSAECONNRESET;
        break;

    case ECONNREFUSED:
        error = WSAECONNREFUSED;
        break;

    case EAGAIN:
        error = WSAEWOULDBLOCK;
        break;
    }
    mprSetOsError(error);
#endif
}


PUBLIC int mprGetLogLevel()
{
    Mpr     *mpr;

    /* Leave the code like this so debuggers can patch logLevel before returning */
    mpr = MPR;
    return mpr->logLevel;
}


PUBLIC MprLogHandler mprGetLogHandler()
{
    return MPR->logHandler;
}


PUBLIC int mprUsingDefaultLogHandler()
{
    return MPR->logHandler == mprDefaultLogHandler;
}


PUBLIC MprFile *mprGetLogFile()
{
    return MPR->logFile;
}


PUBLIC MprLogHandler mprSetLogHandler(MprLogHandler handler)
{
    MprLogHandler   priorHandler;

    priorHandler = MPR->logHandler;
    MPR->logHandler = handler;
    return priorHandler;
}


PUBLIC void mprSetLogFile(MprFile *file)
{
    if (file != MPR->logFile && MPR->logFile != MPR->stdOutput && MPR->logFile != MPR->stdError) {
        mprCloseFile(MPR->logFile);
    }
    MPR->logFile = file;
}


PUBLIC void mprSetLogLevel(int level)
{
    MPR->logLevel = level;
}


PUBLIC bool mprSetCmdlineLogging(bool on)
{
    bool    wasLogging;

    wasLogging = MPR->flags & MPR_LOG_CMDLINE;
    MPR->flags &= ~MPR_LOG_CMDLINE;
    if (on) {
        MPR->flags |= MPR_LOG_CMDLINE;
    }
    return wasLogging;
}


PUBLIC bool mprGetCmdlineLogging()
{
    return MPR->flags & MPR_LOG_CMDLINE ? 1 : 0;
}


#if MACOSX
/*
    Just for conditional breakpoints when debugging in Xcode
 */
PUBLIC int _cmp(char *s1, char *s2)
{
    return !strcmp(s1, s2);
}
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/mime.c ************/


/* 
    mime.c - Mime type handling

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Code *************************************/
/*
    Inbuilt mime type support
 */
static char *standardMimeTypes[] = {
    "ai",    "application/postscript",
    "asc",   "text/plain",
    "au",    "audio/basic",
    "avi",   "video/x-msvideo",
    "bin",   "application/octet-stream",
    "bmp",   "image/bmp",
    "class", "application/octet-stream",
    "css",   "text/css",
    "deb",   "application/octet-stream",
    "dll",   "application/octet-stream",
    "dmg",   "application/octet-stream",
    "doc",   "application/msword",
    "ejs",   "text/html",
    "eof",   "application/vnd.ms-fontobject",
    "esp",   "text/html",
    "eps",   "application/postscript",
    "es",    "application/x-javascript",
    "exe",   "application/octet-stream",
    "gif",   "image/gif",
    "gz",    "application/x-gzip",
    "htm",   "text/html",
    "html",  "text/html",
    "ico",   "image/x-icon",
    "jar",   "application/octet-stream",
    "jpeg",  "image/jpeg",
    "jpg",   "image/jpeg",
    "js",    "application/javascript",
    "json",  "application/json",
    "less",  "text/css",
    "mp3",   "audio/mpeg",
    "mp4",   "video/mp4",
    "mov",   "video/quicktime",
    "mpg",   "video/mpeg",
    "mpeg",  "video/mpeg",
    "otf",   "application/x-font-opentype",
    "pdf",   "application/pdf",
    "php",   "application/x-php",
    "pl",    "application/x-perl",
    "png",   "image/png",
    "ppt",   "application/vnd.ms-powerpoint",
    "ps",    "application/postscript",
    "py",    "application/x-python",
    "py",    "application/x-python",
    "ra",    "audio/x-realaudio",
    "ram",   "audio/x-pn-realaudio",
    "rmm",   "audio/x-pn-realaudio",
    "rtf",   "text/rtf",
    "rv",    "video/vnd.rn-realvideo",
    "so",    "application/octet-stream",
    "svg",   "image/svg+xml",
    "swf",   "application/x-shockwave-flash",
    "tar",   "application/x-tar",
    "tgz",   "application/x-gzip",
    "tiff",  "image/tiff",
    "ttf",   "application/x-font-ttf",
    "txt",   "text/plain",
    "wav",   "audio/x-wav",
    "woff",  "application/font-woff",
    "xls",   "application/vnd.ms-excel",
    "xml",   "application/xml",
    "zip",   "application/zip",
    0,       0,
};

#define MIME_HASH_SIZE 67

/********************************** Forward ***********************************/

static void addStandardMimeTypes(MprHash *table);
static void manageMimeType(MprMime *mt, int flags);

/*********************************** Code *************************************/

PUBLIC MprHash *mprCreateMimeTypes(cchar *path)
{
    MprHash     *table;
#if !ME_ROM
    MprFile     *file;
    char        *buf, *tok, *ext, *type;
    int         line;

    if (path) {
        if ((file = mprOpenFile(path, O_RDONLY | O_TEXT, 0)) == 0) {
            return 0;
        }
        if ((table = mprCreateHash(MIME_HASH_SIZE, MPR_HASH_CASELESS)) == 0) {
            mprCloseFile(file);
            return 0;
        }
        line = 0;
        while ((buf = mprReadLine(file, 0, NULL)) != 0) {
            line++;
            if (buf[0] == '#' || isspace((uchar) buf[0])) {
                continue;
            }
            type = stok(buf, " \t\n\r", &tok);
            ext = stok(0, " \t\n\r", &tok);
            if (type == 0 || ext == 0) {
                mprLog("error mpr", 0, "Bad mime type in %s at line %d", path, line);
                continue;
            }
            while (ext) {
                mprAddMime(table, ext, type);
                ext = stok(0, " \t\n\r", &tok);
            }
        }
        mprCloseFile(file);

    } else 
#endif
    {
        if ((table = mprCreateHash(MIME_HASH_SIZE, MPR_HASH_CASELESS)) == 0) {
            return 0;
        }
        addStandardMimeTypes(table);
    }
    return table;
}


static void addStandardMimeTypes(MprHash *table)
{
    char    **cp;

    for (cp = standardMimeTypes; cp[0]; cp += 2) {
        mprAddMime(table, cp[0], cp[1]);
    }
}


static void manageMimeType(MprMime *mt, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mt->type);
        mprMark(mt->program);
    }
}


PUBLIC MprMime *mprAddMime(MprHash *table, cchar *ext, cchar *mimeType)
{
    MprMime  *mt;

    if ((mt = mprAllocObj(MprMime, manageMimeType)) == 0) {
        return 0;
    }
    mt->type = sclone(mimeType);
    if (*ext == '.') {
        ext++;
    }
    mprAddKey(table, ext, mt);
    return mt;
}


PUBLIC int mprSetMimeProgram(MprHash *table, cchar *mimeType, cchar *program)
{
    MprKey      *kp;
    MprMime     *mt;

    kp = 0;
    mt = 0;
    while ((kp = mprGetNextKey(table, kp)) != 0) {
        mt = (MprMime*) kp->data;
        if (mt->type[0] == mimeType[0] && strcmp(mt->type, mimeType) == 0) {
            break;
        }
    }
    if (mt == 0) {
        return MPR_ERR_CANT_FIND;
    }
    mt->program = sclone(program);
    return 0;
}


PUBLIC cchar *mprGetMimeProgram(MprHash *table, cchar *mimeType)
{
    MprMime      *mt;

    if (mimeType == 0 || *mimeType == '\0') {
        return 0;
    }
    if ((mt = mprLookupKey(table, mimeType)) == 0) {
        return 0;
    }
    return mt->program;
}


PUBLIC cchar *mprLookupMime(MprHash *table, cchar *ext)
{
    MprMime     *mt;
    cchar       *ep;

    if (ext == 0 || *ext == '\0') {
        return 0;
    }
    if ((ep = strrchr(ext, '.')) != 0) {
        ext = &ep[1];
    }
    if (table == 0) {
        table = MPR->mimeTypes;
    }
    if ((mt = mprLookupKey(table, ext)) == 0) {
        return 0;
    }
    return mt->type;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/mixed.c ************/


/**
    mixed.c - Mixed mode strings. Unicode results with ascii args.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_CHAR_LEN > 1 && KEEP
/********************************** Forwards **********************************/

PUBLIC int mcaselesscmp(wchar *str1, cchar *str2)
{
    return mncaselesscmp(str1, str2, -1);
}


PUBLIC int mcmp(wchar *s1, cchar *s2)
{
    return mncmp(s1, s2, -1);
}


PUBLIC wchar *mncontains(wchar *str, cchar *pattern, ssize limit)
{
    wchar   *cp, *s1;
    cchar   *s2;
    ssize   lim;

    assert(0 <= limit && limit < MAXSSIZE);

    if (limit < 0) {
        limit = MAXINT;
    }
    if (str == 0) {
        return 0;
    }
    if (pattern == 0 || *pattern == '\0') {
        return (wchar*) str;
    }
    for (cp = str; *cp && limit > 0; cp++, limit--) {
        s1 = cp;
        s2 = pattern;
        for (lim = limit; *s1 && *s2 && (*s1 == (uchar) *s2) && lim > 0; lim--) {
            s1++;
            s2++;
        }
        if (*s2 == '\0') {
            return cp;
        }
    }
    return 0;
}


PUBLIC wchar *mcontains(wchar *str, cchar *pattern)
{
    return mncontains(str, pattern, -1);
}


/*
    destMax and len are character counts, not sizes in bytes
 */
PUBLIC ssize mcopy(wchar *dest, ssize destMax, cchar *src)
{
    ssize       len;

    assert(src);
    assert(dest);
    assert(0 < destMax && destMax < MAXINT);

    len = slen(src);
    if (destMax <= len) {
        assert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    return mtow(dest, len + 1, src, len);
}


PUBLIC int mends(wchar *str, cchar *suffix)
{
    wchar   *cp;
    cchar   *sp;

    if (str == NULL || suffix == NULL) {
        return 0;
    }
    cp = &str[wlen(str) - 1];
    sp = &suffix[slen(suffix)];
    for (; cp > str && sp > suffix; ) {
        if (*cp-- != *sp--) {
            return 0;
        }
    }
    if (sp > suffix) {
        return 0;
    }
    return 1;
}


PUBLIC wchar *mfmt(cchar *fmt, ...)
{
    va_list     ap;
    char        *mresult;

    assert(fmt);

    va_start(ap, fmt);
    mresult = sfmtv(fmt, ap);
    va_end(ap);
    return amtow(mresult, NULL);
}


PUBLIC wchar *mfmtv(cchar *fmt, va_list arg)
{
    char    *mresult;

    assert(fmt);
    mresult = sfmtv(fmt, arg);
    return amtow(mresult, NULL);
}


/*
    Sep is ascii, args are wchar
 */
PUBLIC wchar *mjoin(wchar *str, ...)
{
    wchar       *result;
    va_list     ap;

    assert(str);

    va_start(ap, str);
    result = mjoinv(str, ap);
    va_end(ap);
    return result;
}


PUBLIC wchar *mjoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp;
    int         required, len;

    assert(buf);

    va_copy(ap, args);
    required = 1;
    if (buf) {
        required += wlen(buf);
    }
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprAlloc(required)) == 0) {
        return 0;
    }
    dp = dest;
    if (buf) {
        wcopy(dp, -1, buf);
        dp += wlen(buf);
    }
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        len = wlen(str);
        dp += len;
        required -= len;
        str = va_arg(ap, wchar*);
    }
    *dp = '\0';
    return dest;
}


/*
    Case insensitive string comparison. Limited by length
 */
PUBLIC int mncaselesscmp(wchar *s1, cchar *s2, ssize n)
{
    int     rc;

    assert(0 <= n && n < MAXSSIZE);

    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = tolower((uchar) *s1) - tolower((uchar) *s2);
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}



PUBLIC int mncmp(wchar *s1, cchar *s2, ssize n)
{
    assert(0 <= n && n < MAXSSIZE);

    if (s1 == 0 && s2 == 0) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = *s1 - (uchar) *s2;
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


PUBLIC ssize mncopy(wchar *dest, ssize destMax, cchar *src, ssize len)
{
    assert(0 <= len && len < MAXSSIZE);
    assert(0 < destMax && destMax < MAXSSIZE);

    return mtow(dest, destMax, src, len);
}


PUBLIC wchar *mpbrk(wchar *str, cchar *set)
{
    cchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return str;
            }
        }
    }
    return 0;
}


/*
    Sep is ascii, args are wchar
 */
PUBLIC wchar *mrejoin(wchar *buf, ...)
{
    va_list     ap;
    wchar       *result;

    va_start(ap, buf);
    result = mrejoinv(buf, ap);
    va_end(ap);
    return result;
}


PUBLIC wchar *mrejoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp;
    int         required, len;

    va_copy(ap, args);
    required = 1;
    if (buf) {
        required += wlen(buf);
    }
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprRealloc(buf, required)) == 0) {
        return 0;
    }
    dp = dest;
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        len = wlen(str);
        dp += len;
        required -= len;
        str = va_arg(ap, wchar*);
    }
    *dp = '\0';
    return dest;
}


PUBLIC ssize mspn(wchar *str, cchar *set)
{
    cchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return break;
            }
        }
        if (*str != *sp) {
            break;
        }
    }
    return count;
}
 

PUBLIC int mstarts(wchar *str, cchar *prefix)
{
    if (str == NULL || prefix == NULL) {
        return 0;
    }
    if (mncmp(str, prefix, slen(prefix)) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC wchar *mtok(wchar *str, cchar *delim, wchar **last)
{
    wchar   *start, *end;
    ssize   i;

    start = str ? str : *last;

    if (start == 0) {
        *last = 0;
        return 0;
    }
    i = mspn(start, delim);
    start += i;
    if (*start == '\0') {
        *last = 0;
        return 0;
    }
    end = mpbrk(start, delim);
    if (end) {
        *end++ = '\0';
        i = mspn(end, delim);
        end += i;
    }
    *last = end;
    return start;
}


PUBLIC wchar *mtrim(wchar *str, cchar *set, int where)
{
    wchar   s;
    ssize   len, i;

    if (str == NULL || set == NULL) {
        return str;
    }
    s = wclone(str);
    if (where & MPR_TRIM_START) {
        i = mspn(s, set);
    } else {
        i = 0;
    }
    s += i;
    if (where & MPR_TRIM_END) {
        len = wlen(s);
        while (len > 0 && mspn(&s[len - 1], set) > 0) {
            s[len - 1] = '\0';
            len--;
        }
    }
    return s;
}

#else
PUBLIC void dummyWide() {}
#endif /* ME_CHAR_LEN > 1 */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/module.c ************/


/**
    module.c - Dynamic module loading support.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void manageModule(MprModule *mp, int flags);
static void manageModuleService(MprModuleService *ms, int flags);

/************************************* Code ***********************************/
/*
    Open the module service
 */
PUBLIC MprModuleService *mprCreateModuleService()
{
    MprModuleService    *ms;

    if ((ms = mprAllocObj(MprModuleService, manageModuleService)) == 0) {
        return 0;
    }
    ms->modules = mprCreateList(-1, 0);
    ms->mutex = mprCreateLock();
    MPR->moduleService = ms;
    mprSetModuleSearchPath(NULL);
    return ms;
}


static void manageModuleService(MprModuleService *ms, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ms->modules);
        mprMark(ms->searchPath);
        mprMark(ms->mutex);
    }
}


/*
    Call the start routine for each module
 */
PUBLIC int mprStartModuleService()
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    ms = MPR->moduleService;
    assert(ms);

    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        if (mprStartModule(mp) < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
#if VXWORKS && ME_DEBUG && SYM_SYNC_INCLUDED
    symSyncLibInit();
#endif
    return 0;
}


PUBLIC void mprStopModuleService()
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    ms = MPR->moduleService;
    assert(ms);
    mprLock(ms->mutex);
    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        mprStopModule(mp);
    }
    mprUnlock(ms->mutex);
}


PUBLIC MprModule *mprCreateModule(cchar *name, cchar *path, cchar *entry, void *data)
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 index;

    ms = MPR->moduleService;
    assert(ms);

    if ((mp = mprAllocObj(MprModule, manageModule)) == 0) {
        return 0;
    }
    mp->name = sclone(name);
    mp->path = sclone(path);
    if (entry && *entry) {
        mp->entry = sclone(entry);
    }
    mp->moduleData = data;
    mp->lastActivity = mprGetTicks();
    index = mprAddItem(ms->modules, mp);
    if (index < 0 || mp->name == 0) {
        return 0;
    }
    return mp;
}


static void manageModule(MprModule *mp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mp->name);
        mprMark(mp->path);
        mprMark(mp->entry);
        mprMark(mp->moduleData);
    }
}


PUBLIC int mprStartModule(MprModule *mp)
{
    assert(mp);

    if (mp->start && !(mp->flags & MPR_MODULE_STARTED)) {
        if (mp->start(mp) < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    mp->flags |= MPR_MODULE_STARTED;
    return 0;
}


PUBLIC int mprStopModule(MprModule *mp)
{
    assert(mp);

    if (mp->stop && (mp->flags & MPR_MODULE_STARTED) && !(mp->flags & MPR_MODULE_STOPPED)) {
        if (mp->stop(mp) < 0) {
            return MPR_ERR_NOT_READY;
        }
        mp->flags |= MPR_MODULE_STOPPED;
    }
    return 0;
}


/*
    See if a module is already loaded
 */
PUBLIC MprModule *mprLookupModule(cchar *name)
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    assert(name && name);

    ms = MPR->moduleService;
    assert(ms);

    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        assert(mp->name);
        if (mp && strcmp(mp->name, name) == 0) {
            return mp;
        }
    }
    return 0;
}


PUBLIC void *mprLookupModuleData(cchar *name)
{
    MprModule   *module;

    if ((module = mprLookupModule(name)) == NULL) {
        return NULL;
    }
    return module->moduleData;
}


PUBLIC void mprSetModuleTimeout(MprModule *module, MprTicks timeout)
{
    assert(module);
    if (module) {
        module->timeout = timeout;
    }
}


PUBLIC void mprSetModuleFinalizer(MprModule *module, MprModuleProc stop)
{
    assert(module);
    if (module) {
        module->stop = stop;
    }
}


PUBLIC void mprSetModuleSearchPath(char *searchPath)
{
    MprModuleService    *ms;

    ms = MPR->moduleService;
    if (searchPath == 0) {
#ifdef ME_VAPP_PREFIX
        ms->searchPath = sjoin(mprGetAppDir(), MPR_SEARCH_SEP, mprGetAppDir(), MPR_SEARCH_SEP, ME_VAPP_PREFIX "/bin", NULL);
#else
        ms->searchPath = sjoin(mprGetAppDir(), MPR_SEARCH_SEP, mprGetAppDir(), NULL);
#endif
    } else {
        ms->searchPath = sclone(searchPath);
    }
}


PUBLIC cchar *mprGetModuleSearchPath()
{
    return MPR->moduleService->searchPath;
}


/*
    Load a module. The module is located by searching for the filename by optionally using the module search path.
 */
PUBLIC int mprLoadModule(MprModule *mp)
{
#if ME_COMPILER_HAS_DYN_LOAD
    assert(mp);

    if (mprLoadNativeModule(mp) < 0) {
        return MPR_ERR_CANT_READ;
    }
    mprStartModule(mp);
    return 0;
#else
    mprLog("error mpr", 0, "mprLoadModule: %s failed", mp->name);
    mprLog("error mpr", 0, "Product built without the ability to load modules dynamically");
    return MPR_ERR_BAD_STATE;
#endif
}


PUBLIC int mprUnloadModule(MprModule *mp)
{
    mprDebug("mpr", 5, "Unloading native module %s from %s", mp->name, mp->path);
    if (mprStopModule(mp) < 0) {
        return MPR_ERR_NOT_READY;
    }
#if ME_COMPILER_HAS_DYN_LOAD
    if (mp->handle) {
        if (mprUnloadNativeModule(mp) != 0) {
            mprLog("error mpr", 0, "Cannot unload module %s", mp->name);
        }
        mp->handle = 0;
    }
#endif
    mprRemoveItem(MPR->moduleService->modules, mp);
    return 0;
}


#if ME_COMPILER_HAS_DYN_LOAD
/*
    Return true if the shared library in "file" can be found. Return the actual path in *path. The filename
    may not have a shared library extension which is typical so calling code can be cross platform.
 */
static char *probe(cchar *filename)
{
    char    *path;

    assert(filename && *filename);

    if (mprPathExists(filename, R_OK)) {
        return sclone(filename);
    }
    if (strstr(filename, ME_SHOBJ) == 0) {
        path = sjoin(filename, ME_SHOBJ, NULL);
        if (mprPathExists(path, R_OK)) {
            return path;
        }
    }
    return 0;
}
#endif


/*
    Search for a module "filename" in the modulePath. Return the result in "result"
 */
PUBLIC char *mprSearchForModule(cchar *filename)
{
#if ME_COMPILER_HAS_DYN_LOAD
    char    *path, *f, *searchPath, *dir, *tok;

    filename = mprNormalizePath(filename);

    /*
        Search for the path directly
     */
    if ((path = probe(filename)) != 0) {
        return path;
    }

    /*
        Search in the searchPath
     */
    searchPath = sclone(mprGetModuleSearchPath());
    tok = 0;
    dir = stok(searchPath, MPR_SEARCH_SEP, &tok);
    while (dir && *dir) {
        f = mprJoinPath(dir, filename);
        if ((path = probe(f)) != 0) {
            return path;
        }
        dir = stok(0, MPR_SEARCH_SEP, &tok);
    }
#endif /* ME_COMPILER_HAS_DYN_LOAD */
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/path.c ************/


/**
    path.c - Path (filename) services.

    This modules provides cross platform path name services.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/
/*
    Find the first separator in the path
 */
#if ME_UNIX_LIKE
    #define firstSep(fs, path)  strchr(path, fs->separators[0])
#else
    #define firstSep(fs, path)  strpbrk(path, fs->separators)
#endif

#define defaultSep(fs)          (fs->separators[0])

/*********************************** Forwards *********************************/

static MprList *globPathFiles(MprList *results, cchar *path, char *pattern, cchar *relativeTo, cchar *exclude, int flags);
static bool matchFile(cchar *filename, cchar *pattern);
static char *ptok(char *str, cchar *delim, char **last);
static char *rewritePattern(cchar *pattern, int flags);

/************************************* Code ***********************************/

static ME_INLINE bool isSep(MprFileSystem *fs, int c) 
{
    char    *separators;

    assert(fs);
    for (separators = fs->separators; *separators; separators++) {
        if (*separators == c)
            return 1;
    }
    return 0;
}


static ME_INLINE bool hasDrive(MprFileSystem *fs, cchar *path) 
{
    char    *cp, *endDrive;

    assert(fs);
    assert(path);

    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && (cp == NULL || endDrive < cp)) {
            return 1;
        }
    }
    return 0;
}


/*
    Return true if the path is absolute.
    This means the path portion after an optional drive specifier must begin with a directory speparator charcter.
    Cygwin returns true for "/abc" and "C:/abc".
 */
static ME_INLINE bool isAbsPath(MprFileSystem *fs, cchar *path) 
{
    char    *cp, *endDrive;

    assert(fs);
    assert(path);

    if (path == NULL || *path == '\0') {
        return 0;
    }
    if (fs->hasDriveSpecs) {
        if ((cp = firstSep(fs, path)) != 0) {
            if ((endDrive = strchr(path, ':')) != 0) {
                if (&endDrive[1] == cp) {
                    return 1;
                }
            }
            if (cp == path) {
                return 1;
            }
        }
    } else {
        if (isSep(fs, path[0])) {
            return 1;
        }
    }
    return 0;
}


/*
    Return true if the path is a fully qualified absolute path.
    On windows, this means it must have a drive specifier.
    On cygwin, this means it must not have a drive specifier.
 */
static ME_INLINE bool isFullPath(MprFileSystem *fs, cchar *path) 
{
    assert(fs);
    assert(path);

#if ME_WIN_LIKE || VXWORKS
{
    char    *cp, *endDrive;

    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && cp && &endDrive[1] == cp) {
            return 1;
        }
        return 0;
    }
}
#endif
    if (isSep(fs, path[0])) {
        return 1;
    }
    return 0;
}


/*
    Return true if the directory is the root directory on a file system
 */
static ME_INLINE bool isRoot(MprFileSystem *fs, cchar *path) 
{
    char    *cp;

    if (isAbsPath(fs, path)) {
        cp = firstSep(fs, path);
        if (cp && cp[1] == '\0') {
            return 1;
        }
    }
    return 0;
}


static ME_INLINE char *lastSep(MprFileSystem *fs, cchar *path) 
{
    char    *cp;

    for (cp = (char*) &path[slen(path)] - 1; cp >= path; cp--) {
        if (isSep(fs, *cp)) {
            return cp;
        }
    }
    return 0;
}

/************************************ Code ************************************/
/*
    This copies a file.
 */
PUBLIC int mprCopyPath(cchar *fromName, cchar *toName, int mode)
{
    MprFile     *from, *to;
    ssize       count;
    char        buf[ME_MAX_BUFFER];

    if ((from = mprOpenFile(fromName, O_RDONLY | O_BINARY, 0)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if ((to = mprOpenFile(toName, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, mode)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    while ((count = mprReadFile(from, buf, sizeof(buf))) > 0) {
        mprWriteFile(to, buf, count);
    }
    mprCloseFile(from);
    mprCloseFile(to);
    return 0;
}


PUBLIC int mprDeletePath(cchar *path)
{
    MprFileSystem   *fs;

    if (path == NULL || *path == '\0') {
        return MPR_ERR_CANT_ACCESS;
    }
    fs = mprLookupFileSystem(path);
    return fs->deletePath(fs, path);
}


/*
    Return an absolute (normalized) path.
    On CYGWIN, this is a cygwin path with forward-slashes and without drive specs. 
    Use mprGetWinPath for a windows style path with a drive specifier and back-slashes.
 */
PUBLIC char *mprGetAbsPath(cchar *path)
{
    MprFileSystem   *fs;
    char            *result;

    if (path == 0 || *path == '\0') {
        path = ".";
    }
#if ME_ROM
    result =  mprNormalizePath(path);
    mprMapSeparators(result, defaultSep(fs));
    return result;
#elif CYGWIN
    {
        ssize   len;
        /*
            cygwin_conf_path has a bug for paths that attempt to address a directory above the root. ie. "../../../.."
            So must convert to a windows path first.
         */
        if (strncmp(path, "../", 3) == 0) {
            path = mprGetWinPath(path);
        }
        if ((len = cygwin_conv_path(CCP_WIN_A_TO_POSIX | CCP_ABSOLUTE, path, NULL, 0)) >= 0) {
            /* Len includes room for the null */
            if ((result = mprAlloc(len)) == 0) {
                return 0;
            }
            cygwin_conv_path(CCP_WIN_A_TO_POSIX | CCP_ABSOLUTE, path, result, len);
            if (len > 3 && result[len - 2] == '/' && result[len - 3] != ':') {
                /* Trim trailing "/" */
                result[len - 2] = '\0';
            }
            return result;
        }
    }
#endif
    fs = mprLookupFileSystem(path);
    if (isFullPath(fs, path)) {
        /* Already absolute. On windows, must contain a drive specifier */
        result = mprNormalizePath(path);
        mprMapSeparators(result, defaultSep(fs));
        return result;
    }

#if ME_WIN_LIKE
{
    wchar    buf[ME_MAX_PATH];
    GetFullPathName(wide(path), sizeof(buf) - 1, buf, NULL);
    buf[sizeof(buf) - 1] = '\0';
    result = mprNormalizePath(multi(buf));
}
#elif VXWORKS
{
    char    *dir;
    if (hasDrive(fs, path)) {
        dir = mprGetCurrentPath();
        result = mprJoinPath(dir, &strchr(path, ':')[1]);

    } else {
        if (isAbsPath(fs, path)) {
            /*
                Path is absolute, but without a drive. Use the current drive.
             */
            dir = mprGetCurrentPath();
            result = mprJoinPath(dir, path);
        } else {
            dir = mprGetCurrentPath();
            result = mprJoinPath(dir, path);
        }
    }
}
#else
{
    char   *dir;
    dir = mprGetCurrentPath();
    result = mprJoinPath(dir, path);
}
#endif
    return result;
}


/*
    Get the directory containing the application executable. Tries to return an absolute path.
 */
PUBLIC char *mprGetAppDir()
{ 
    if (MPR->appDir == 0) {
        MPR->appDir = mprGetPathDir(mprGetAppPath());
    }
    return sclone(MPR->appDir); 
} 


/*
    Get the path for the application executable. Tries to return an absolute path.
 */
PUBLIC char *mprGetAppPath()
{ 
    if (MPR->appPath) {
        return sclone(MPR->appPath);
    }

#if MACOSX
{
    MprPath info;
    char    path[ME_MAX_PATH], pbuf[ME_MAX_PATH];
    uint    size;
    ssize   len;

    size = sizeof(path) - 1;
    if (_NSGetExecutablePath(path, &size) < 0) {
        return mprGetAbsPath(".");
    }
    path[size] = '\0';
    if (mprGetPathInfo(path, &info) == 0 && info.isLink) {
        len = readlink(path, pbuf, sizeof(pbuf) - 1);
        if (len > 0) {
            pbuf[len] = '\0';
            MPR->appPath = mprJoinPath(mprGetPathDir(path), pbuf);
        } else {
            MPR->appPath = mprGetAbsPath(path);
        }
    } else {
        MPR->appPath = mprGetAbsPath(path);
    }
}
#elif ME_BSD_LIKE 
{
    MprPath info;
    char    pbuf[ME_MAX_PATH];
    int     len;

    len = readlink("/proc/curproc/file", pbuf, sizeof(pbuf) - 1);
    if (len < 0) {
        return mprGetAbsPath(".");
     }
     pbuf[len] = '\0';
     MPR->appPath = mprGetAbsPath(pbuf);
}
#elif ME_UNIX_LIKE 
{
    MprPath info;
    char    pbuf[ME_MAX_PATH], *path;
    int     len;
#if SOLARIS
    path = sfmt("/proc/%i/path/a.out", getpid()); 
#else
    path = sfmt("/proc/%i/exe", getpid()); 
#endif
    if (mprGetPathInfo(path, &info) == 0 && info.isLink) {
        len = readlink(path, pbuf, sizeof(pbuf) - 1);
        if (len > 0) {
            pbuf[len] = '\0';
            MPR->appPath = mprGetAbsPath(pbuf);
            MPR->appPath = mprJoinPath(mprGetPathDir(path), pbuf);
        } else {
            MPR->appPath = mprGetAbsPath(path);
        }
    } else {
        MPR->appPath = mprGetAbsPath(path);
    }
}
#elif ME_WIN_LIKE
{
    wchar    pbuf[ME_MAX_PATH];

    if (GetModuleFileName(0, pbuf, sizeof(pbuf) - 1) <= 0) {
        return 0;
    }
    MPR->appPath = mprGetAbsPath(multi(pbuf));
}
#else
    if (mprIsPathAbs(MPR->argv[0])) {
        MPR->appPath = sclone(MPR->argv[0]);
    } else {
        MPR->appPath = mprGetCurrentPath();
    }
#endif
    return sclone(MPR->appPath);
}

 
/*
    This will return a fully qualified absolute path for the current working directory.
 */
PUBLIC char *mprGetCurrentPath()
{
    char    dir[ME_MAX_PATH];

    if (getcwd(dir, sizeof(dir)) == 0) {
        return mprGetAbsPath("/");
    }

#if VXWORKS
{
    MprFileSystem   *fs;
    char            sep[2];

    fs = mprLookupFileSystem(dir);

    /*
        Vx will sometimes just return a drive with no path.
     */
    if (firstSep(fs, dir) == NULL) {
        sep[0] = defaultSep(fs);
        sep[1] = '\0';
        return sjoin(dir, sep, NULL);
    }
}
#elif ME_WIN_LIKE || CYGWIN
{
    MprFileSystem   *fs;
    fs = mprLookupFileSystem(dir);
    mprMapSeparators(dir, fs->separators[0]);
}
#endif
    return sclone(dir);
}


PUBLIC cchar *mprGetFirstPathSeparator(cchar *path) 
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return firstSep(fs, path);
}


/*
    Return a pointer into the path at the last path separator or null if none found
 */
PUBLIC cchar *mprGetLastPathSeparator(cchar *path) 
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return lastSep(fs, path);
}


/*
    Return a path with native separators. This means "\\" on windows and cygwin
 */
PUBLIC char *mprGetNativePath(cchar *path)
{
    return mprTransformPath(path, MPR_PATH_NATIVE_SEP);
}


/*
    Return the last portion of a pathname. The separators are not mapped and the path is not cleaned.
 */
PUBLIC char *mprGetPathBase(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    if (path == 0) {
        return sclone("");
    }
    fs = mprLookupFileSystem(path);
    cp = (char*) lastSep(fs, path);
    if (cp == 0) {
        return sclone(path);
    } 
    if (cp == path) {
        if (cp[1] == '\0') {
            return sclone(path);
        }
    } else if (cp[1] == '\0') {
        return sclone("");
    }
    return sclone(&cp[1]);
}


/*
    Return the last portion of a pathname. The separators are not mapped and the path is not cleaned.
    This returns a reference into the original string
 */
PUBLIC cchar *mprGetPathBaseRef(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    if (path == 0) {
        return sclone("");
    }
    fs = mprLookupFileSystem(path);
    if ((cp = (char*) lastSep(fs, path)) == 0) {
        return path;
    } 
    if (cp == path) {
        if (cp[1] == '\0') {
            return path;
        }
    }
    return &cp[1];
}


/*
    Return the directory portion of a pathname.
 */
PUBLIC char *mprGetPathDir(cchar *path)
{
    MprFileSystem   *fs;
    cchar           *cp, *start;
    char            *result;
    ssize          len;

    assert(path);

    if (path == 0 || *path == '\0') {
        return sclone(path);
    }

    fs = mprLookupFileSystem(path);
    len = slen(path);
    cp = &path[len - 1];
    start = hasDrive(fs, path) ? strchr(path, ':') + 1 : path;

    /*
        Step back over trailing slashes
     */
    while (cp > start && isSep(fs, *cp)) {
        cp--;
    }
    for (; cp > start && !isSep(fs, *cp); cp--) { }

    if (cp == start) {
        if (!isSep(fs, *cp)) {
            /* No slashes found, parent is current dir */
            return sclone(".");
        }
        cp++;
    }
    len = (cp - path);
    result = mprAlloc(len + 1);
    mprMemcpy(result, len + 1, path, len);
    result[len] = '\0';
    return result;
}


/*
    Return the extension portion of a pathname.
    Return the extension without the "."
 */
PUBLIC char *mprGetPathExt(cchar *path)
{
    MprFileSystem  *fs;
    char            *cp;

    if ((cp = srchr(path, '.')) != NULL) {
        fs = mprLookupFileSystem(path);
        /*
            If there is no separator ("/") after the extension, then use it.
         */
        if (firstSep(fs, cp) == 0) {
            return sclone(++cp);
        }
    } 
    return 0;
}


static void manageDirEntry(MprDirEntry *dp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dp->name);
    }
}


#if ME_ROM
static MprList *getDirFiles(cchar *path)
{
    MprRomFileSystem    *rfs;
    MprRomInode         *ri;
    MprPath             fileInfo;
    MprList             *list;
    MprDirEntry         *dp;
    ssize               len;

    rfs = (MprRomFileSystem*) MPR->fileSystem;
    list = mprCreateList(256, 0);
    len = slen(path);

    for (ri = rfs->romInodes; ri->path; ri++) {
        if (!sstarts(ri->path, path) || !schr(&ri->path[len], '/')) {
            continue;
        }
        fileInfo.isDir = (ri->size == 0);
        fileInfo.isLink = 0;
        if ((dp = mprAllocObj(MprDirEntry, manageDirEntry)) == 0) {
            return list;
        }
        dp->name = sclone(ri->path);
        dp->size = ri->size;
        dp->isDir = (ri->data == 0);
        dp->isLink = 0;
        dp->lastModified = 0;
        mprAddItem(list, &ri->path[len]);
    }
    return list;
}

#else /* !ME_ROM */
/*
    This returns a list of MprDirEntry objects
 */
#if ME_WIN_LIKE
static MprList *getDirFiles(cchar *dir)
{
    HANDLE          h;
    MprDirEntry     *dp;
    MprPath         fileInfo;
    MprList         *list;
    cchar           *seps;
    char            *path, pbuf[ME_MAX_PATH];
    WIN32_FIND_DATA findData;

    list = mprCreateList(-1, 0);
    dp = 0;

    if ((path = mprJoinPath(dir, "*.*")) == 0) {
        return list;
    }
    seps = mprGetPathSeparators(dir);

    h = FindFirstFile(wide(path), &findData);
    if (h == INVALID_HANDLE_VALUE) {
        return list;
    }
    do {
        if (findData.cFileName[0] == '.' && (findData.cFileName[1] == '\0' || findData.cFileName[1] == '.')) {
            continue;
        }
        if ((dp = mprAlloc(sizeof(MprDirEntry))) == 0) {
            return list;
        }
        dp->name = awtom(findData.cFileName, 0);
        if (dp->name == 0) {
            return list;
        }
        /* dp->lastModified = (uint) findData.ftLastWriteTime.dwLowDateTime; */

        if (fmt(pbuf, sizeof(pbuf), "%s%c%s", dir, seps[0], dp->name) < 0) {
            dp->lastModified = 0;
        } else {
            mprGetPathInfo(pbuf, &fileInfo);
            dp->lastModified = fileInfo.mtime;
        }
        dp->isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        dp->isLink = 0;

#if KEEP_64_BIT
        if (findData.nFileSizeLow < 0) {
            dp->size = (((uint64) findData.nFileSizeHigh) * INT64(4294967296)) + (4294967296L - 
                (uint64) findData.nFileSizeLow);
        } else {
            dp->size = (((uint64) findData.nFileSizeHigh * INT64(4294967296))) + (uint64) findData.nFileSizeLow;
        }
#else
        dp->size = (uint) findData.nFileSizeLow;
#endif
        mprAddItem(list, dp);
    } while (FindNextFile(h, &findData) != 0);

    FindClose(h);
    return list;
}

#else /* !WIN */
static MprList *getDirFiles(cchar *path)
{
    DIR             *dir;
    MprPath         fileInfo;
    MprList         *list;
    struct dirent   *dirent;
    MprDirEntry     *dp;
    char            *fileName;
    int             rc;

    list = mprCreateList(256, 0);
    if ((dir = opendir((char*) path)) == 0) {
        return list;
    }
    while ((dirent = readdir(dir)) != 0) {
        if (dirent->d_name[0] == '.' && (dirent->d_name[1] == '\0' || dirent->d_name[1] == '.')) {
            continue;
        }
        fileName = mprJoinPath(path, dirent->d_name);
        /* workaround for if target of symlink does not exist */
        fileInfo.isLink = 0;
        fileInfo.isDir = 0;
        rc = mprGetPathInfo(fileName, &fileInfo);
        if ((dp = mprAllocObj(MprDirEntry, manageDirEntry)) == 0) {
            return list;
        }
        dp->name = sclone(dirent->d_name);
        if (dp->name == 0) {
            return list;
        }
        if (rc == 0 || fileInfo.isLink) {
            dp->lastModified = fileInfo.mtime;
            dp->size = fileInfo.size;
            dp->isDir = fileInfo.isDir;
            dp->isLink = fileInfo.isLink;
        } else {
            dp->lastModified = 0;
            dp->size = 0;
            dp->isDir = 0;
            dp->isLink = 0;
        }
        mprAddItem(list, dp);
    }
    closedir(dir);
    return list;
}
#endif /* !WIN */
#endif /* !ME_ROM */

#if LINUX
static int sortFiles(MprDirEntry **dp1, MprDirEntry **dp2)
{
    return strcmp((*dp1)->name, (*dp2)->name);
}
#endif


/*
    Find files in the directory "dir". If base is set, use that as the prefix for returned files.
    Returns a list of MprDirEntry objects.
 */
static MprList *findFiles(MprList *list, cchar *dir, cchar *base, int flags)
{
    MprDirEntry     *dp;
    MprList         *files;
    char            *name;
    int             next;

    if ((files = getDirFiles(dir)) == 0) {
        return 0;
    }
    for (next = 0; (dp = mprGetNextItem(files, &next)) != 0; ) {
        if (dp->name[0] == '.') {
            if (dp->name[1] == '\0' || (dp->name[1] == '.' && dp->name[2] == '\0')) {
                continue;
            }
            if (!(flags & MPR_PATH_INC_HIDDEN)) {
                continue;
            }
        }
        name = dp->name;
        dp->name = mprJoinPath(base, name);

        if (!(flags & MPR_PATH_DEPTH_FIRST) && !(dp->isDir && flags & MPR_PATH_NO_DIRS)) {
            mprAddItem(list, dp);
        }
        if (dp->isDir) {
            if (flags & MPR_PATH_DESCEND) {
                findFiles(list, mprJoinPath(dir, name), mprJoinPath(base, name), flags);
            } 
        }
        if ((flags & MPR_PATH_DEPTH_FIRST) && (!(dp->isDir && flags & MPR_PATH_NO_DIRS))) {
            mprAddItem(list, dp);
        }
    }
#if LINUX
    /* Linux returns directories not sorted */
    mprSortList(list, (MprSortProc) sortFiles, 0);
#endif
    return list;
}


/*
    Get the files in a directory. Returns a list of MprDirEntry objects.

    MPR_PATH_DESCEND        to traverse subdirectories
    MPR_PATH_DEPTH_FIRST    to do a depth-first traversal
    MPR_PATH_INC_HIDDEN     to include hidden files
    MPR_PATH_NO_DIRS        to exclude subdirectories
    MPR_PATH_RELATIVE       to return paths relative to the initial directory
 */
PUBLIC MprList *mprGetPathFiles(cchar *dir, int flags)
{
    MprList *list;
    cchar   *base;

    if (dir == 0 || *dir == '\0') {
        dir = ".";
    }
    base = (flags & MPR_PATH_RELATIVE) ? 0 : dir;
    if ((list = findFiles(mprCreateList(-1, 0), dir, base, flags)) == 0) {
        return 0;
    }
    return list;
}


/* 
    Skip over double wilds to the next non-double wild segment
    Return the first pattern segment as a result.
    Return in reference arg the following pattern and set *dwild if a double wild was skipped.
    This routine clones the original pattern to preserve it.
 */
static char *getNextPattern(char *pattern, char **nextPat, bool *dwild)
{
    MprFileSystem   *fs;
    char            *thisPat;

    fs = mprLookupFileSystem(pattern);
    pattern = sclone(pattern);
    *dwild = 0; 

    while (1) {
        thisPat = ptok(pattern, fs->separators, &pattern); 
        if (smatch(thisPat, "**") == 0) {
            break;
        }
        *dwild = 1;
    }
    if (nextPat) {
        *nextPat = pattern;
    }
    return thisPat;
}


/*
    Glob a full multi-segment path and return a list of matching files

    MOB relativeTo  - Relative files are relative to this directory.
    path        - Directory to search. Will be a physical directory path.
    pattern     - Search pattern with optional wildcards.
    exclude     - Exclusion pattern (not currently implemented as there is no API to pass in an exluded pattern).

    As this routine recurses, 'relativeTo' does not change, but path and pattern will.
 */
static MprList *globPathFiles(MprList *results, cchar *path, char *pattern, cchar *relativeTo, cchar *exclude, int flags)
{
    MprDirEntry     *dp;
    MprList         *list;
    cchar           *filename;
    char            *thisPat, *nextPat;
    bool            dwild;
    int             add, matched, next;

    if ((list = mprGetPathFiles(path, (flags & ~MPR_PATH_NO_DIRS) | MPR_PATH_RELATIVE)) == 0) {
        return results;
    }
    thisPat = getNextPattern(pattern, &nextPat, &dwild);

    for (next = 0; (dp = mprGetNextItem(list, &next)) != 0; ) {
        if (flags & MPR_PATH_RELATIVE) {
            filename = mprGetRelPath(mprJoinPath(path, dp->name), relativeTo);
        } else {
            filename = mprJoinPath(path, dp->name);
        }
        if ((matched = matchFile(dp->name, thisPat)) == 0) {
            if (dwild) {
                if (thisPat == 0) {
                    matched = 1;
                } else {
                    /* Match failed, so backup the pattern and try the double wild for this filename (only) */
                    globPathFiles(results, mprJoinPath(path, dp->name), pattern, relativeTo, exclude, flags);
                    continue;
                }
            }
        }
        add = (matched && (!nextPat || smatch(nextPat, "**")));
        //  MOB _ is this right using filename?  surely need to use dp->name?
        if (add && exclude && matchFile(filename, exclude)) {
            continue;
        }
        if (add && dp->isDir && (flags & MPR_PATH_NO_DIRS)) {
            add = 0;
        }
        if (add && !(flags & MPR_PATH_DEPTH_FIRST)) {
            mprAddItem(results, filename);
        }
        if (dp->isDir) {
            //  MOB -- same
            if (dwild) {
                globPathFiles(results, mprJoinPath(path, dp->name), pattern, relativeTo, exclude, flags);
            } else if (matched && nextPat) {
                globPathFiles(results, mprJoinPath(path, dp->name), nextPat, relativeTo, exclude, flags);
            }
        }
        if (add && (flags & MPR_PATH_DEPTH_FIRST)) {
            mprAddItem(results, filename);
        }
    }
    return results;
}


/*
    Get the files in a directory and subdirectories using glob-style matching
 */
PUBLIC MprList *mprGlobPathFiles(cchar *path, cchar *pattern, int flags)
{
    MprFileSystem   *fs;
    MprList         *result;
    cchar           *exclude, *relativeTo;
    char            *pat, *special, *start;

    result = mprCreateList(0, 0);
    if (path && pattern) {
        fs = mprLookupFileSystem(pattern);
        exclude = 0;
        pat = 0;
        relativeTo = (flags & MPR_PATH_RELATIVE) ? path : 0;
        /*
            Adjust path to include any fixed segements from the pattern
         */
        start = sclone(pattern);
        if ((special = strpbrk(start, "*?")) != 0) {
            if (special > start) {
                for (pat = special; pat > start && !strchr(fs->separators, *pat); pat--) { }
                if (pat > start) {
                    *pat++ = '\0';
                    path = mprJoinPath(path, start);
                }
                pattern = pat;
            }
        } else {
            pat = (char*) mprGetPathBaseRef(start);
            if (pat > start) {
                pat[-1] = '\0';
                path = mprJoinPath(path, start);
            }
            pattern = pat;
        }
        if (*pattern == '!') {
            exclude = &pattern[1];
        }
        globPathFiles(result, path, rewritePattern(pattern, flags), relativeTo, exclude, flags);
    }
    return result;
}


/*
    Special version of stok that does not skip leading delimiters.
    Need this to handle leading "/path". This is handled as an empty "" filename segment
    This then works (automagically) for windows drives "C:/"
 */
static char *ptok(char *str, cchar *delim, char **last)
{
    char    *start, *end;
    ssize   i;

    assert(delim);
    start = (str || !last) ? str : *last;
    if (start == 0) {
        if (last) {
            *last = 0;
        }
        return 0;
    }
    /* Don't skip delimiters at the start 
    i = strspn(start, delim);
    start += i;
    */
    if (*start == '\0') {
        if (last) {
            *last = 0;
        }
        return 0;
    }
    end = strpbrk(start, delim);
    if (end) {
        *end++ = '\0';
        i = strspn(end, delim);
        end += i;
    }
    if (last) {
        *last = end;
    }
    return start;
}


/*
    Convert pattern to canonical form:
    abc** => abc* / **
    **abc => ** / *abc
 */
static char *rewritePattern(cchar *pat, int flags)
{
    MprFileSystem   *fs;
    MprBuf          *buf;
    char            *cp, *pattern;

    fs = mprLookupFileSystem(pat);
    pattern = sclone(pat);
    if (flags & MPR_PATH_DESCEND) {
        pattern = mprJoinPath(pattern, "**");
    }
    if (!scontains(pattern, "**")) {
        return pattern;
    }
    buf = mprCreateBuf(0, 0);
    for (cp = pattern; *cp; cp++) {
        if (cp[0] == '*' && cp[1] == '*') {
            if (isSep(fs, cp[2]) && cp[3] == '*' && cp[4] == '*') {
                /* Remove redundant ** */
                cp += 3;
            }
            if (cp > pattern && !isSep(fs, cp[-1])) {
                // abc** => abc*/**
                mprPutCharToBuf(buf, '*');
                mprPutCharToBuf(buf, fs->separators[0]);
            } 
            mprPutCharToBuf(buf, '*');
            mprPutCharToBuf(buf, '*');
            if (cp[2] && !isSep(fs, cp[2])) {
                // **abc  => **/*abc
                mprPutCharToBuf(buf, fs->separators[0]);
                mprPutCharToBuf(buf, '*');
            }
            cp++;
        } else {
            mprPutCharToBuf(buf, *cp);
        }
    }
    mprAddNullToBuf(buf);
    return mprGetBufStart(buf);
}



/*
    Match a single filename (without separators) to a pattern (without separators).
    This supports the wildcards '?' and '*'. This routine does not handle double wild.
    If filename or pattern are null, returns false.
    Pattern may be an empty string -- will only match an empty filename. Used for matching leading "/".
 */
static bool matchFile(cchar *filename, cchar *pattern)
{
    MprFileSystem   *fs;
    cchar           *fp, *pp;

    if (filename == pattern) {
        return 1;
    }
    if (!filename || !pattern) {
        return 0;
    }
    fs = mprLookupFileSystem(filename);
    for (fp = filename, pp = pattern; *fp && *pp; fp++, pp++) {
        if (*pp == '?') {
            continue;
        } else if (*pp == '*') {
            if (matchFile(&fp[1], pp)) {
                return 1;
            }
            fp--;
            continue;
        } else {
            if (fs->caseSensitive) {
                if (*fp != *pp) {
                    return 0;
                }
            } else if (tolower((uchar) *fp) != tolower((uchar) *pp)) {
                return 0;
            }
        }
    }
    if (*fp) {
        return 0;
    }
    if (*pp) {
        /* Trailing '*' or '**' */
        if (!((pp[0] == '*' && pp[1] == '\0') || (pp[0] == '*' && pp[1] == '*' && pp[2] == '\0'))) {
            return 0;
        }
    }
    return 1;
}


/*
    Pattern is in canonical form where "**" is always a segment by itself
 */
static bool matchPath(MprFileSystem *fs, char *path, char *pattern)
{
    char    *nextPat, *thisFile, *thisPat;
    bool    dwild;

    assert(path);
    assert(pattern);

    while (pattern && path) {
        thisFile = ptok(path, fs->separators, &path);
        thisPat = getNextPattern(pattern, &nextPat, &dwild);
        if (!matchFile(thisFile, thisPat)) {
            if (dwild) {
                if (path) {
                    return matchPath(fs, path, pattern);
                } else {
                    return thisPat ? 0 : 1;
                }
            }
            return 0;
        }
        pattern = nextPat;
    }
    return (pattern && *pattern) ? 0 : 1;
}


PUBLIC bool mprMatchPath(cchar *path, cchar *pattern)
{
    MprFileSystem   *fs;

    if (!path || !pattern) {
        return 0;
    }
    fs = mprLookupFileSystem(path);
    return matchPath(fs, sclone(path), rewritePattern(pattern, 0));
}



/*
    Return the first directory of a pathname
 */
PUBLIC char *mprGetPathFirstDir(cchar *path)
{
    MprFileSystem   *fs;
    cchar           *cp;
    int             len;

    assert(path);

    fs = mprLookupFileSystem(path);
    if (isAbsPath(fs, path)) {
        len = hasDrive(fs, path) ? 2 : 1;
        return snclone(path, len);
    } else {
        if ((cp = firstSep(fs, path)) != 0) {
            return snclone(path, cp - path);
        }
        return sclone(path);
    }
}


PUBLIC int mprGetPathInfo(cchar *path, MprPath *info)
{
    MprFileSystem  *fs;

    fs = mprLookupFileSystem(path);
    return fs->getPathInfo(fs, path, info);
}


PUBLIC char *mprGetPathLink(cchar *path)
{
    MprFileSystem  *fs;

    fs = mprLookupFileSystem(path);
    return fs->getPathLink(fs, path);
}


/*
    GetPathParent is smarter than GetPathDir which operates purely textually on the path. GetPathParent will convert
    relative paths to absolute to determine the parent directory.
 */
PUBLIC char *mprGetPathParent(cchar *path)
{
    MprFileSystem   *fs;
    char            *dir;

    fs = mprLookupFileSystem(path);

    if (path == 0 || path[0] == '\0') {
        return mprGetAbsPath(".");
    }
    if (firstSep(fs, path) == NULL) {
        /*
            No parents in the path, so convert to absolute
         */
        dir = mprGetAbsPath(path);
        return mprGetPathDir(dir);
    }
    return mprGetPathDir(path);
}


PUBLIC char *mprGetPortablePath(cchar *path)
{
    char    *result, *cp;

    result = mprTransformPath(path, 0);
    for (cp = result; *cp; cp++) {
        if (*cp == '\\') {
            *cp = '/';
        }
    }
    return result;
}


/*
    Get a relative path path from an origin path to a destination. If a relative path cannot be obtained,
    an absolute path to the destination will be returned. This happens if the paths cross drives.
    Returns the supplied destArg modified to be relative to originArg.
 */
PUBLIC char *mprGetRelPath(cchar *destArg, cchar *originArg)
{
    MprFileSystem   *fs;
    char            originBuf[ME_MAX_FNAME], *dp, *result, *dest, *lastdp, *origin, *op, *lastop;
    int             originSegments, i, commonSegments, sep;

    fs = mprLookupFileSystem(destArg);

    if (destArg == 0 || *destArg == '\0') {
        return sclone(".");
    }
    dest = mprNormalizePath(destArg);

    if (!isAbsPath(fs, dest) && (originArg == 0 || *originArg == '\0')) {
        return dest;
    }
    sep = (dp = firstSep(fs, dest)) ? *dp : defaultSep(fs);

    if (originArg == 0 || *originArg == '\0') {
        /*
            Get the working directory. Ensure it is null terminated and leave room to append a trailing separators
            On cygwin, this will be a cygwin style path (starts with "/" and no drive specifier).
         */
        if (getcwd(originBuf, sizeof(originBuf)) == 0) {
            strcpy(originBuf, ".");
        }
        originBuf[sizeof(originBuf) - 2] = '\0';
        origin = originBuf;
    } else {
        origin = mprGetAbsPath(originArg);
    }
    dest = mprGetAbsPath(dest);

    /*
        Count segments in origin working directory. Ignore trailing separators.
     */
    for (originSegments = 0, dp = origin; *dp; dp++) {
        if (isSep(fs, *dp) && dp[1]) {
            originSegments++;
        }
    }

    /*
        Find portion of dest that matches the origin directory, if any. Start at -1 because matching root doesn't count.
     */
    commonSegments = -1;
    for (lastop = op = origin, lastdp = dp = dest; *op && *dp; op++, dp++) {
        if (isSep(fs, *op)) {
            lastop = op + 1;
            if (isSep(fs, *dp)) {
                lastdp = dp + 1;
                commonSegments++;
            }
        } else if (fs->caseSensitive) {
            if (*op != *dp) {
                break;
            }
        } else if (*op != *dp && tolower((uchar) *op) != tolower((uchar) *dp)) {
            break;
        }
    }
    if (commonSegments < 0) {
        /* Different drives - must return absolute path */
        return dest;
    }

    if ((*op && *dp) || (*op && *dp && !isSep(fs, *op) && !isSep(fs, *dp))) {
        /*
            Cases:
            /seg/abc>   Path('/seg/xyz').relative       # differing trailing segment
            /seg/abc>   Path('/seg/abcd)                # common last segment prefix, dest longer
            /seg/abc>   Path('/seg/ab')                 # common last segment prefix, origin longer
        */
        op = lastop;
        dp = lastdp;
    }

    /*
        Add one more segment if the last segment matches. Handle trailing separators.
     */
    if ((isSep(fs, *op) || *op == '\0') && (isSep(fs, *dp) || *dp == '\0')) {
        commonSegments++;
    }
    if (isSep(fs, *dp)) {
        dp++;
    }
    op = result = mprAlloc(originSegments * 3 + slen(dest) + 2);
    for (i = commonSegments; i < originSegments; i++) {
        *op++ = '.';
        *op++ = '.';
        *op++ = defaultSep(fs);
    }
    if (*dp) {
        strcpy(op, dp);
    } else if (op > result) {
        /*
            Cleanup trailing separators ("../" is the end of the new path)
         */
        op[-1] = '\0';
    } else {
        strcpy(result, ".");
    }
    mprMapSeparators(result, sep);
    return result;
}


/*
    Get a temporary file name. The file is created in the system temp location.
 */
PUBLIC char *mprGetTempPath(cchar *tempDir)
{
    MprFile         *file;
    char            *dir, *path;
    int             i, now;
    static int      tempSeed = 0;

    if (tempDir == 0 || *tempDir == '\0') {
#if ME_WIN_LIKE
{
        MprFileSystem   *fs;
        fs = mprLookupFileSystem(tempDir ? tempDir : (cchar*) "/");
        dir = sclone(getenv("TEMP"));
        mprMapSeparators(dir, defaultSep(fs));
}
#elif VXWORKS
        dir = sclone(".");
#else
        dir = sclone("/tmp");
#endif
    } else {
        dir = sclone(tempDir);
    }
    now = ((int) mprGetTime() & 0xFFFF) % 64000;
    file = 0;
    path = 0;
    for (i = 0; i < 128; i++) {
        path = sfmt("%s/MPR_%s_%d_%d_%d.tmp", dir, mprGetPathBase(MPR->name), getpid(), now, ++tempSeed);
        file = mprOpenFile(path, O_CREAT | O_EXCL | O_BINARY, 0664);
        if (file) {
            mprCloseFile(file);
            break;
        }
    }
    if (file == 0) {
        return 0;
    }
    return path;
}


/*
    Return a windows path.
    On CYGWIN, this is a cygwin path without drive specs.
 */
PUBLIC char *mprGetWinPath(cchar *path)
{
    char            *result;

    if (path == 0 || *path == '\0') {
        path = ".";
    }
#if ME_ROM
    result = mprNormalizePath(path);
#elif CYGWIN
{
    ssize   len;
    if ((len = cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, path, NULL, 0)) >= 0) {
        if ((result = mprAlloc(len)) == 0) {
            return 0;
        }
        cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, path, result, len);
        return result;
    } else {
        result = mprGetAbsPath(path);
    }
}
#else
    result = mprNormalizePath(path);
    mprMapSeparators(result, '\\');
#endif
    return result;
}


PUBLIC bool mprIsPathContained(cchar *path, cchar *dir)
{
    ssize   len;
    char    *base;

    dir = mprGetAbsPath(dir);
    path = mprGetAbsPath(path);
    len = slen(dir);
    if (len <= slen(path)) {
        base = sclone(path);
        base[len] = '\0';
        if (mprSamePath(dir, base)) {
            return 1;
        }
    }
    return 0;
}


PUBLIC bool mprIsAbsPathContained(cchar *path, cchar *dir)
{
    MprFileSystem   *fs;
    ssize            len;

    assert(mprIsPathAbs(path));
    assert(mprIsPathAbs(dir));
    len = slen(dir);
    if (len <= slen(path)) {
        fs = mprLookupFileSystem(path);
        if (mprSamePathCount(dir, path, len) && (path[len] == '\0' || isSep(fs, path[len]))) {
            return 1;
        }
    }
    return 0;
}


PUBLIC bool mprIsPathAbs(cchar *path)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return isAbsPath(fs, path);
}


PUBLIC bool mprIsPathDir(cchar *path)
{
    MprPath     info;

    return (mprGetPathInfo(path, &info) == 0 && info.isDir);
}


PUBLIC bool mprIsPathRel(cchar *path)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return !isAbsPath(fs, path);
}


PUBLIC bool mprIsPathSeparator(cchar *path, cchar c)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return isSep(fs, c);
}


/*
    Join paths. Returns a joined (normalized) path.
    If other is absolute, then return other. If other is null, empty or "." then return path.
    The separator is chosen to match the first separator found in either path. If none, it uses the default separator.
 */
PUBLIC char *mprJoinPath(cchar *path, cchar *other)
{
    MprFileSystem   *fs;
    char            *join, *drive, *cp;
    int             sep;

    fs = mprLookupFileSystem(path);
    if (other == NULL || *other == '\0' || strcmp(other, ".") == 0) {
        return sclone(path);
    }
    if (isAbsPath(fs, other)) {
        if (fs->hasDriveSpecs && !isFullPath(fs, other) && isFullPath(fs, path)) {
            /*
                Other is absolute, but without a drive. Use the drive from path.
             */
            drive = sclone(path);
            if ((cp = strchr(drive, ':')) != 0) {
                *++cp = '\0';
            }
            return sjoin(drive, other, NULL);
        } else {
            return mprNormalizePath(other);
        }
    }
    if (path == NULL || *path == '\0') {
        return mprNormalizePath(other);
    }
    if ((cp = firstSep(fs, path)) != 0) {
        sep = *cp;
    } else if ((cp = firstSep(fs, other)) != 0) {
        sep = *cp;
    } else {
        sep = defaultSep(fs);
    }
    if ((join = sfmt("%s%c%s", path, sep, other)) == 0) {
        return 0;
    }
    return mprNormalizePath(join);
}


PUBLIC char *mprJoinPaths(cchar *base, ...)
{
    va_list     args;
    cchar       *path;

    va_start(args, base);
    while ((path = va_arg(args, char*)) != 0) {
        base = mprJoinPath(base, path);
    }
    va_end(args);
    return (char*) base;
}


/*
    Join an extension to a path. If path already has an extension, this call does nothing.
    The extension should not have a ".", but this routine is tolerant if it does.
 */
PUBLIC char *mprJoinPathExt(cchar *path, cchar *ext)
{
    MprFileSystem   *fs;
    char            *cp;

    fs = mprLookupFileSystem(path);
    if (ext == NULL || *ext == '\0') {
        return sclone(path);
    }
    cp = srchr(path, '.');
    if (cp && firstSep(fs, cp) == 0) {
        return sclone(path);
    }
    if (ext[0] == '.') {
        return sjoin(path, ext, NULL);
    } else {
        return sjoin(path, ".", ext, NULL);
    }
}


/*
    Make a directory with all necessary intervening directories.
 */
PUBLIC int mprMakeDir(cchar *path, int perms, int owner, int group, bool makeMissing)
{
    MprFileSystem   *fs;
    char            *parent;
    int             rc;

    fs = mprLookupFileSystem(path);

    if (mprPathExists(path, X_OK)) {
        return 0;
    }
    if (fs->makeDir(fs, path, perms, owner, group) == 0) {
        return 0;
    }
    if (makeMissing && !isRoot(fs, path)) {
        parent = mprGetPathParent(path);
        if (!mprPathExists(parent, X_OK)) {
            if ((rc = mprMakeDir(parent, perms, owner, group, makeMissing)) < 0) {
                return rc;
            }
        }
        return fs->makeDir(fs, path, perms, owner, group);
    }
    return MPR_ERR_CANT_CREATE;
}


PUBLIC int mprMakeLink(cchar *path, cchar *target, bool hard)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    if (mprPathExists(path, X_OK)) {
        return 0;
    }
    return fs->makeLink(fs, path, target, hard);
}


/*
    Normalize a path to remove redundant "./" and cleanup "../" and make separator uniform. Does not make an abs path.
    It does not map separators, change case, nor add drive specifiers.
 */
PUBLIC char *mprNormalizePath(cchar *pathArg)
{
    MprFileSystem   *fs;
    char            *path, *sp, *dp, *mark, **segments;
    ssize           len;
    int             addSep, i, segmentCount, hasDot, last, sep;

    if (pathArg == 0 || *pathArg == '\0') {
        return sclone("");
    }
    fs = mprLookupFileSystem(pathArg);

    /*
        Allocate one spare byte incase we need to break into segments. If so, will add a trailing "/" to make 
        parsing easier later.
     */
    len = slen(pathArg);
    if ((path = mprAlloc(len + 2)) == 0) {
        return NULL;
    }
    strcpy(path, pathArg);
    sep = (sp = firstSep(fs, path)) ? *sp : defaultSep(fs);

    /*
        Remove multiple path separators. Check if we have any "." characters and count the number of path segments
        Map separators to the first separator found
     */
    hasDot = segmentCount = 0;
    for (sp = dp = path; *sp; ) {
        if (isSep(fs, *sp)) {
            *sp = sep;
            segmentCount++;
            while (isSep(fs, sp[1])) {
                sp++;
            }
        } 
        if (*sp == '.') {
            hasDot++;
        }
        *dp++ = *sp++;
    }
    *dp = '\0';
    if (!sep) {
        sep = defaultSep(fs);
    }
    if (!hasDot && segmentCount == 0) {
        if (fs->hasDriveSpecs) {
            last = path[slen(path) - 1];
            if (last == ':') {
                path = sjoin(path, ".", NULL);
            }
        }
        return path;
    }

    if (dp > path && !isSep(fs, dp[-1])) {
        *dp++ = sep;
        *dp = '\0';
        segmentCount++;
    }

    /*
        Have dots to process so break into path segments. Add one incase we need have an absolute path with a drive-spec.
     */
    assert(segmentCount > 0);
    if ((segments = mprAlloc(sizeof(char*) * (segmentCount + 1))) == 0) {
        return NULL;
    }

    /*
        NOTE: The root "/" for absolute paths will be stored as empty.
     */
    len = 0;
    for (i = 0, mark = sp = path; *sp; sp++) {
        if (isSep(fs, *sp)) {
            *sp = '\0';
            if (*mark == '.' && mark[1] == '\0' && segmentCount > 1) {
                /* Remove "."  However, preserve lone "." */
                mark = sp + 1;
                segmentCount--;
                continue;
            }
            if (*mark == '.' && mark[1] == '.' && mark[2] == '\0' && i > 0 && strcmp(segments[i-1], "..") != 0) {
                /* Erase ".." and previous segment */
                if (*segments[i - 1] == '\0' ) {
                    assert(i == 1);
                    /* Previous segement is "/". Prevent escape from root */
                    segmentCount--;
                } else {
                    i--;
                    segmentCount -= 2;
                }
                assert(segmentCount >= 0);
                mark = sp + 1;
                continue;
            }
            segments[i++] = mark;
            len += (sp - mark);
#if KEEP
            if (i == 1 && segmentCount == 1 && fs->hasDriveSpecs && strchr(mark, ':') != 0) {
                /*
                    Normally we truncate a trailing "/", but this is an absolute path with a drive spec (c:/). 
                 */
                segments[i++] = "";
                segmentCount++;
            }
#endif
            mark = sp + 1;
        }
    }

    if (--sp > mark) {
        segments[i++] = mark;
        len += (sp - mark);
    }
    assert(i <= segmentCount);
    segmentCount = i;

    if (segmentCount <= 0) {
        return sclone(".");
    }

    addSep = 0;
    sp = segments[0];
    if (fs->hasDriveSpecs && *sp != '\0') {
        last = sp[slen(sp) - 1];
        if (last == ':') {
            /* This matches an original path of: "c:/" but not "c:filename" */
            addSep++;
        }
    }
#if ME_WIN_LIKE
    if (strcmp(segments[segmentCount - 1], " ") == 0) {
        segmentCount--;
    }
#endif
    if ((path = mprAlloc(len + segmentCount + 1)) == 0) {
        return NULL;
    }
    assert(segmentCount > 0);

    /*
        First segment requires special treatment due to drive specs
     */
    dp = path;
    strcpy(dp, segments[0]);
    dp += slen(segments[0]);

    if (segmentCount == 1 && (addSep || (*segments[0] == '\0'))) {
        *dp++ = sep;
    }

    for (i = 1; i < segmentCount; i++) {
        *dp++ = sep;
        strcpy(dp, segments[i]);
        dp += slen(segments[i]);
    }
    *dp = '\0';
    return path;
}


PUBLIC void mprMapSeparators(char *path, int separator)
{
    MprFileSystem   *fs;
    char            *cp;

    fs = mprLookupFileSystem(path);
    for (cp = path; *cp; cp++) {
        if (isSep(fs, *cp)) {
            *cp = separator;
        }
    }
}


PUBLIC bool mprPathExists(cchar *path, int omode)
{
    MprFileSystem  *fs;

    if (path == 0 || *path == '\0') {
        return 0;
    }
    fs = mprLookupFileSystem(path);
    return fs->accessPath(fs, path, omode);
}


PUBLIC char *mprReadPathContents(cchar *path, ssize *lenp)
{
    MprFile     *file;
    MprPath     info;
    ssize       len;
    char        *buf;

    if ((file = mprOpenFile(path, O_RDONLY | O_BINARY, 0)) == 0) {
        return 0;
    }
    if (mprGetPathInfo(path, &info) < 0) {
        mprCloseFile(file);
        return 0;
    }
    len = (ssize) info.size;
    if ((buf = mprAlloc(len + 1)) == 0) {
        mprCloseFile(file);
        return 0;
    }
    if (mprReadFile(file, buf, len) != len) {
        mprCloseFile(file);
        return 0;
    }
    buf[len] = '\0';
    if (lenp) {
        *lenp = len;
    }
    mprCloseFile(file);
    return buf;
}


PUBLIC int mprRenamePath(cchar *from, cchar *to)
{
    return rename(from, to);
}


PUBLIC char *mprReplacePathExt(cchar *path, cchar *ext)
{
    if (ext == NULL || *ext == '\0') {
        return sclone(path);
    }
    path = mprTrimPathExt(path);
    /*
        Don't use mprJoinPathExt incase path has an embedded "."
     */
    if (ext[0] == '.') {
        return sjoin(path, ext, NULL);
    } else {
        return sjoin(path, ".", ext, NULL);
    }
}


/*
    Resolve paths in the neighborhood of this path. Resolve operates like join, except that it joins the 
    given paths to the directory portion of the current ("this") path. For example: 
    Path("/usr/bin/ejs/bin").resolve("lib") will return "/usr/lib/ejs/lib". i.e. it will return the
    sibling directory "lib".

    Resolve operates by determining a virtual current directory for this Path object. It then successively 
    joins the given paths to the directory portion of the current result. If the next path is an absolute path, 
    it is used unmodified.  The effect is to find the given paths with a virtual current directory set to the 
    directory containing the prior path.

    Resolve is useful for creating paths in the region of the current path and gracefully handles both 
    absolute and relative path segments.

    Returns a joined (normalized) path.
    If path is absolute, then return path. If path is null, empty or "." then return path.
 */
PUBLIC char *mprResolvePath(cchar *base, cchar *path)
{
    MprFileSystem   *fs;
    char            *join, *drive, *cp, *dir;

    fs = mprLookupFileSystem(base);
    if (path == NULL || *path == '\0' || strcmp(path, ".") == 0) {
        return sclone(base);
    }
    if (isAbsPath(fs, path)) {
        if (fs->hasDriveSpecs && !isFullPath(fs, path) && isFullPath(fs, base)) {
            /*
                Other is absolute, but without a drive. Use the drive from base.
             */
            drive = sclone(base);
            if ((cp = strchr(drive, ':')) != 0) {
                *++cp = '\0';
            }
            return sjoin(drive, path, NULL);
        }
        return mprNormalizePath(path);
    }
    if (base == NULL || *base == '\0') {
        return mprNormalizePath(path);
    }
    dir = mprGetPathDir(base);
    if ((join = sfmt("%s/%s", dir, path)) == 0) {
        return 0;
    }
    return mprNormalizePath(join);
}


/*
    Compare two file path to determine if they point to the same file.
 */
PUBLIC int mprSamePath(cchar *path1, cchar *path2)
{
    MprFileSystem   *fs;
    cchar           *p1, *p2;

    fs = mprLookupFileSystem(path1);

    /*
        Convert to absolute (normalized) paths to compare. 
     */
    if (!isFullPath(fs, path1)) {
        path1 = mprGetAbsPath(path1);
    } else {
        path1 = mprNormalizePath(path1);
    }
    if (!isFullPath(fs, path2)) {
        path2 = mprGetAbsPath(path2);
    } else {
        path2 = mprNormalizePath(path2);
    }
    if (fs->caseSensitive) {
        for (p1 = path1, p2 = path2; *p1 && *p2; p1++, p2++) {
            if (*p1 != *p2 && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    } else {
        for (p1 = path1, p2 = path2; *p1 && *p2; p1++, p2++) {
            if (tolower((uchar) *p1) != tolower((uchar) *p2) && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    }
    return (*p1 == *p2);
}


/*
    Compare two file path to determine if they point to the same file.
 */
PUBLIC int mprSamePathCount(cchar *path1, cchar *path2, ssize len)
{
    MprFileSystem   *fs;
    cchar           *p1, *p2;

    fs = mprLookupFileSystem(path1);

    /*
        Convert to absolute paths to compare. 
     */
    if (!isFullPath(fs, path1)) {
        path1 = mprGetAbsPath(path1);
    }
    if (!isFullPath(fs, path2)) {
        path2 = mprGetAbsPath(path2);
    }
    if (fs->caseSensitive) {
        for (p1 = path1, p2 = path2; *p1 && *p2 && len > 0; p1++, p2++, len--) {
            if (*p1 != *p2 && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    } else {
        for (p1 = path1, p2 = path2; *p1 && *p2 && len > 0; p1++, p2++, len--) {
            if (tolower((uchar) *p1) != tolower((uchar) *p2) && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    }
    return len == 0;
}


PUBLIC void mprSetAppPath(cchar *path)
{ 
    MPR->appPath = sclone(path);
    MPR->appDir = mprGetPathDir(MPR->appPath);
}


static char *checkPath(cchar *path, int flags) 
{
    MprPath     info;
    int         access;

    access = (flags & (MPR_SEARCH_EXE | MPR_SEARCH_DIR)) ? X_OK : R_OK;

    if (mprPathExists(path, access)) {
        mprGetPathInfo(path, &info);
        if (flags & MPR_SEARCH_DIR && info.isDir) {
            return sclone(path);
        }
        if (info.isReg) {
            return sclone(path);
        }
    }
    return 0;
}


PUBLIC char *mprSearchPath(cchar *file, int flags, cchar *search, ...)
{
    va_list     args;
    char        *result, *path, *dir, *nextDir, *tok;

    va_start(args, search);

    if ((result = checkPath(file, flags)) != 0) {
        return result;
    }
    if ((flags & MPR_SEARCH_EXE) && *ME_EXE) {
        if ((result = checkPath(mprJoinPathExt(file, ME_EXE), flags)) != 0) {
            return result;
        }
    }
    for (nextDir = (char*) search; nextDir; nextDir = va_arg(args, char*)) {
        tok = NULL;
        nextDir = sclone(nextDir);
        dir = stok(nextDir, MPR_SEARCH_SEP, &tok);
        while (dir && *dir) {
            path = mprJoinPath(dir, file);
            if ((result = checkPath(path, flags)) != 0) {
                return mprNormalizePath(result);
            }
            if ((flags & MPR_SEARCH_EXE) && *ME_EXE) {
                if ((result = checkPath(mprJoinPathExt(path, ME_EXE), flags)) != 0) {
                    return mprNormalizePath(result);
                }
            }
            dir = stok(0, MPR_SEARCH_SEP, &tok);
        }
    }
    va_end(args);
    return 0;
}


/*
    This normalizes a path. Returns a normalized path according to flags. Default is absolute. 
    if MPR_PATH_NATIVE_SEP is specified in the flags, map separators to the native format.
 */
PUBLIC char *mprTransformPath(cchar *path, int flags)
{
    char    *result;

#if CYGWIN
    if (flags & MPR_PATH_ABS) {
        if (flags & MPR_PATH_WIN) {
            result = mprGetWinPath(path);
        } else {
            result = mprGetAbsPath(path);
        }
#else
    if (flags & MPR_PATH_ABS) {
        result = mprGetAbsPath(path);

#endif
    } else if (flags & MPR_PATH_REL) {
        result = mprGetRelPath(path, 0);

    } else {
        result = mprNormalizePath(path);
    }
    if (flags & MPR_PATH_NATIVE_SEP) {
#if ME_WIN_LIKE
        mprMapSeparators(result, '\\');
#elif CYGWIN
        mprMapSeparators(result, '/');
#endif
    }
    return result;
}

//  TODO - should these really return cchar

PUBLIC char *mprTrimPathComponents(cchar *path, int count)
{
    MprFileSystem   *fs;
    cchar           *cp;
    int             sep;

    fs = mprLookupFileSystem(path);

    if (count == 0) {
        return sclone(path);

    } else if (count > 0) {
        do {
            if ((path = firstSep(fs, path)) == 0) {
                return sclone("");
            }
            path++;
        } while (--count > 0);
        return sclone(path);

    } else {
        sep = (cp = firstSep(fs, path)) ? *cp : defaultSep(fs);
        for (cp = &path[slen(path) - 1]; cp >= path && count < 0; cp--) {
            if (*cp == sep) {
                count++;
            }
        }
        if (count == 0) {
            return snclone(path, cp - path + 1);
        }
    }
    return sclone("");
}


PUBLIC char *mprTrimPathExt(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp, *result;

    fs = mprLookupFileSystem(path);
    result = sclone(path);
    if ((cp = srchr(result, '.')) != NULL) {
        if (firstSep(fs, cp) == 0) {
            *cp = '\0';
        }
    } 
    return result;
}


PUBLIC char *mprTrimPathDrive(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp, *endDrive;

    fs = mprLookupFileSystem(path);
    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && (cp == NULL || endDrive < cp)) {
            return sclone(++endDrive);
        }
    }
    return sclone(path);
}


PUBLIC ssize mprWritePathContents(cchar *path, cchar *buf, ssize len, int mode)
{
    MprFile     *file;

    if (mode == 0) {
        mode = 0644;
    }
    if (len < 0) {
        len = slen(buf);
    }
    if ((file = mprOpenFile(path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, mode)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    if (mprWriteFile(file, buf, len) != len) {
        mprCloseFile(file);
        return MPR_ERR_CANT_WRITE;
    }
    mprCloseFile(file);
    return len;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/posix.c ************/


/**
    posix.c - Posix specific adaptions

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_UNIX_LIKE
/*********************************** Code *************************************/

PUBLIC int mprCreateOsService()
{
    umask(022);

    /*
        Cleanup the environment. IFS is often a security hole
     */
    putenv("IFS=\t ");
    return 0;
}


PUBLIC int mprStartOsService()
{
    /* 
        Open a syslog connection
     */
#if SOLARIS
    openlog(mprGetAppName(), LOG_LOCAL0);
#else
    openlog(mprGetAppName(), 0, LOG_LOCAL0);
#endif
    return 0;
}


PUBLIC void mprStopOsService()
{
    closelog();
}


PUBLIC int mprGetRandomBytes(char *buf, ssize length, bool block)
{
    ssize   sofar, rc;
    int     fd;

    if ((fd = open((block) ? "/dev/random" : "/dev/urandom", O_RDONLY, 0666)) < 0) {
        return MPR_ERR_CANT_OPEN;
    }
    sofar = 0;
    do {
        rc = read(fd, &buf[sofar], length);
        if (rc < 0) {
            assert(0);
            return MPR_ERR_CANT_READ;
        }
        length -= rc;
        sofar += rc;
    } while (length > 0);
    close(fd);
    return 0;
}


#if ME_COMPILER_HAS_DYN_LOAD
PUBLIC int mprLoadNativeModule(MprModule *mp)
{
    MprModuleEntry  fn;
    void            *handle;

    assert(mp);

    /*
        Search the image incase the module has been statically linked
     */
#ifdef RTLD_DEFAULT
    handle = RTLD_DEFAULT;
#else
#ifdef RTLD_MAIN_ONLY
    handle = RTLD_MAIN_ONLY;
#else
    handle = 0;
#endif
#endif
    if (!mp->entry || !dlsym(handle, mp->entry)) {
#if ME_STATIC
        mprLog("error mpr", 0, "Cannot load module %s, product built static", mp->name);
        return MPR_ERR_BAD_STATE;
#else
        MprPath info;
        char    *at;
        if ((at = mprSearchForModule(mp->path)) == 0) {
            mprLog("error mpr", 0, "Cannot find module \"%s\", cwd: \"%s\", search path \"%s\"", mp->path, mprGetCurrentPath(),
                mprGetModuleSearchPath());
            return MPR_ERR_CANT_ACCESS;
        }
        mp->path = at;
        mprGetPathInfo(mp->path, &info);
        mp->modified = info.mtime;
        mprLog("info mpr", 4, "Loading native module %s", mprGetPathBase(mp->path));
        if ((handle = dlopen(mp->path, RTLD_LAZY | RTLD_GLOBAL)) == 0) {
            mprLog("error mpr", 0, "Cannot load module %s, reason: \"%s\"", mp->path, dlerror());
            return MPR_ERR_CANT_OPEN;
        } 
        mp->handle = handle;
#endif /* !ME_STATIC */

    } else if (mp->entry) {
        mprLog("info mpr", 4, "Activating native module %s", mp->name);
    }
    if (mp->entry) {
        if ((fn = (MprModuleEntry) dlsym(handle, mp->entry)) != 0) {
            if ((fn)(mp->moduleData, mp) < 0) {
                mprLog("error mpr", 0, "Initialization for module %s failed", mp->name);
                dlclose(handle);
                return MPR_ERR_CANT_INITIALIZE;
            }
        } else {
            mprLog("error mpr", 0, "Cannot load module %s, reason: cannot find function \"%s\"", mp->path, mp->entry);
            dlclose(handle);
            return MPR_ERR_CANT_READ;
        }
    }
    return 0;
}


PUBLIC int mprUnloadNativeModule(MprModule *mp)
{
    return dlclose(mp->handle);
}
#endif


/*
    This routine does not yield
 */
PUBLIC void mprNap(MprTicks timeout)
{
    MprTicks        remaining, mark;
    struct timespec t;
    int             rc;

    assert(timeout >= 0);

    mark = mprGetTicks();
    remaining = timeout;
    do {
        /* MAC OS X corrupts the timeout if using the 2nd paramater, so recalc each time */
        t.tv_sec = ((int) (remaining / 1000));
        t.tv_nsec = ((int) ((remaining % 1000) * 1000000));
        rc = nanosleep(&t, NULL);
        remaining = mprGetRemainingTicks(mark, timeout);
    } while (rc < 0 && errno == EINTR && remaining > 0);
}


/*
    This routine yields
 */
PUBLIC void mprSleep(MprTicks timeout)
{
    mprYield(MPR_YIELD_STICKY);
    mprNap(timeout);
    mprResetYield();
}


/*
    Write a message in the O/S native log (syslog in the case of linux)
 */
PUBLIC void mprWriteToOsLog(cchar *message, int level)
{
    syslog((level == 0) ? LOG_ERR : LOG_WARNING, "%s", message);
}


PUBLIC void mprSetFilesLimit(int limit)
{
    struct rlimit r;
    int           i;

    if (limit == 0 || limit == MAXINT) {
        /*
            We need to determine a reasonable maximum possible limit value.
            There is no #define we can use for this, so we test to determine it empirically
         */
        for (limit = 0x40000000; limit > 0; limit >>= 1) {
            r.rlim_cur = r.rlim_max = limit;
            if (setrlimit(RLIMIT_NOFILE, &r) == 0) {
                for (i = (limit >> 4) * 15; i > 0; i--) {
                    r.rlim_max = r.rlim_cur = limit + i;
                    if (setrlimit(RLIMIT_NOFILE, &r) == 0) {
                        limit = 0;
                        break;
                    }
                }
                break;
            }
        }
    } else {
        r.rlim_cur = r.rlim_max = limit;
        if (setrlimit(RLIMIT_NOFILE, &r) < 0) {
            mprLog("error mpr", 0, "Cannot set file limit to %d", limit);
        }
    }
    getrlimit(RLIMIT_NOFILE, &r);
}

#endif /* ME_UNIX_LIKE */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/printf.c ************/


/**
    printf.c - Printf routines safe for embedded programming

    This module provides safe replacements for the standard printf formatting routines. Most routines in this file 
    are not thread-safe. It is the callers responsibility to perform all thread synchronization.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/*********************************** Defines **********************************/
/*
    Class definitions
 */
#define CLASS_NORMAL    0               /* [All other]       Normal characters */
#define CLASS_PERCENT   1               /* [%]               Begin format */
#define CLASS_MODIFIER  2               /* [-+ #,']          Modifiers */
#define CLASS_ZERO      3               /* [0]               Special modifier - zero pad */
#define CLASS_STAR      4               /* [*]               Width supplied by arg */
#define CLASS_DIGIT     5               /* [1-9]             Field widths */
#define CLASS_DOT       6               /* [.]               Introduce precision */
#define CLASS_BITS      7               /* [hlLz]            Length bits */
#define CLASS_TYPE      8               /* [cdefginopsSuxX]  Type specifiers */

#define STATE_NORMAL    0               /* Normal chars in format string */
#define STATE_PERCENT   1               /* "%" */
#define STATE_MODIFIER  2               /* -+ #,' */
#define STATE_WIDTH     3               /* Width spec */
#define STATE_DOT       4               /* "." */
#define STATE_PRECISION 5               /* Precision spec */
#define STATE_BITS      6               /* Size spec */
#define STATE_TYPE      7               /* Data type */
#define STATE_COUNT     8

static char stateMap[] = {
    /*     STATES:  Normal Percent Modifier Width  Dot  Prec Bits Type */
    /* CLASS           0      1       2       3     4     5    6    7  */
    /* Normal   0 */   0,     0,      0,      0,    0,    0,   0,   0,
    /* Percent  1 */   1,     0,      1,      1,    1,    1,   1,   1,
    /* Modifier 2 */   0,     2,      2,      0,    0,    0,   0,   0,
    /* Zero     3 */   0,     2,      2,      3,    5,    5,   0,   0,
    /* Star     4 */   0,     3,      3,      0,    5,    0,   0,   0,
    /* Digit    5 */   0,     3,      3,      3,    5,    5,   0,   0,
    /* Dot      6 */   0,     4,      4,      4,    0,    0,   0,   0,
    /* Bits     7 */   0,     6,      6,      6,    6,    6,   6,   0,
    /* Types    8 */   0,     7,      7,      7,    7,    7,   7,   0,
};

/*
    Format:         %[modifier][width][precision][bits][type]

    The Class map will map from a specifier letter to a state.
 */
static char classMap[] = {
    /*   0  ' '    !     "     #     $     %     &     ' */
             2,    0,    0,    2,    0,    1,    0,    2,
    /*  07   (     )     *     +     ,     -     .     / */
             0,    0,    4,    2,    2,    2,    6,    0,
    /*  10   0     1     2     3     4     5     6     7 */
             3,    5,    5,    5,    5,    5,    5,    5,
    /*  17   8     9     :     ;     <     =     >     ? */
             5,    5,    0,    0,    0,    0,    0,    0,
    /*  20   @     A     B     C     D     E     F     G */
             8,    0,    0,    0,    0,    0,    0,    0,
    /*  27   H     I     J     K     L     M     N     O */
             0,    0,    0,    0,    7,    0,    8,    0,
    /*  30   P     Q     R     S     T     U     V     W */
             0,    0,    0,    8,    0,    0,    0,    0,
    /*  37   X     Y     Z     [     \     ]     ^     _ */
             8,    0,    0,    0,    0,    0,    0,    0,
    /*  40   '     a     b     c     d     e     f     g */
             0,    0,    0,    8,    8,    8,    8,    8,
    /*  47   h     i     j     k     l     m     n     o */
             7,    8,    0,    0,    7,    0,    8,    8,
    /*  50   p     q     r     s     t     u     v     w */
             8,    0,    0,    8,    0,    8,    0,    8,
    /*  57   x     y     z  */
             8,    0,    7,
};

/*
    Flags
 */
#define SPRINTF_LEFT        0x1         /* Left align */
#define SPRINTF_SIGN        0x2         /* Always sign the result */
#define SPRINTF_LEAD_SPACE  0x4         /* put leading space for +ve numbers */
#define SPRINTF_ALTERNATE   0x8         /* Alternate format */
#define SPRINTF_LEAD_ZERO   0x10        /* Zero pad */
#define SPRINTF_SHORT       0x20        /* 16-bit */
#define SPRINTF_LONG        0x40        /* 32-bit */
#define SPRINTF_INT64       0x80        /* 64-bit */
#define SPRINTF_COMMA       0x100       /* Thousand comma separators */
#define SPRINTF_UPPER_CASE  0x200       /* As the name says for numbers */
#define SPRINTF_SSIZE       0x400       /* Size of ssize */

typedef struct Format {
    uchar   *buf;
    uchar   *endbuf;
    uchar   *start;
    uchar   *end;
    ssize   growBy;
    ssize   maxsize;
    int     precision;
    int     radix;
    int     width;
    int     flags;
    int     len;
} Format;

#define BPUT(fmt, c) \
    if (1) { \
        /* Less one to allow room for the null */ \
        if ((fmt)->end >= ((fmt)->endbuf - sizeof(char))) { \
            if (growBuf(fmt) > 0) { \
                *(fmt)->end++ = (c); \
            } \
        } else { \
            *(fmt)->end++ = (c); \
        } \
    } else

#define BPUTNULL(fmt) \
    if (1) { \
        if ((fmt)->end > (fmt)->endbuf) { \
            if (growBuf(fmt) > 0) { \
                *(fmt)->end = '\0'; \
            } \
        } else { \
            *(fmt)->end = '\0'; \
        } \
    } else 

/*
    Just for Ejscript to be able to do %N and %S. THIS MUST MATCH EjsString in ejs.h
 */
typedef struct MprEjsString {
    void            *xtype;
#if ME_DEBUG
    char            *kind;
    void            *type;
    MprMem          *mem;
#endif
    void            *next;
    void            *prev;
    ssize           length;
    wchar         value[0];
} MprEjsString;

typedef struct MprEjsName {
    MprEjsString    *name;
    MprEjsString    *space;
} MprEjsName;

/********************************** Defines ***********************************/

#ifndef ME_MAX_FMT
    #define ME_MAX_FMT 256           /* Initial size of a printf buffer */
#endif

/***************************** Forward Declarations ***************************/

static int  getState(char c, int state);
static int  growBuf(Format *fmt);
PUBLIC char *mprPrintfCore(char *buf, ssize maxsize, cchar *fmt, va_list arg);
static void outNum(Format *fmt, cchar *prefix, uint64 val);
static void outString(Format *fmt, cchar *str, ssize len);
#if ME_CHAR_LEN > 1 && KEEP
static void outWideString(Format *fmt, wchar *str, ssize len);
#endif
#if ME_FLOAT
static void outFloat(Format *fmt, char specChar, double value);
#endif

/************************************* Code ***********************************/

PUBLIC ssize mprPrintf(cchar *fmt, ...)
{
    va_list     ap;
    char        *buf;
    ssize       len;

    /* No asserts here as this is used as part of assert reporting */

    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    if (buf != 0 && MPR->stdOutput) {
        len = mprWriteFileString(MPR->stdOutput, buf);
    } else {
        len = -1;
    }
    return len;
}


PUBLIC ssize mprEprintf(cchar *fmt, ...)
{
    va_list     ap;
    ssize       len;
    char        *buf;

    /* No asserts here as this is used as part of assert reporting */

    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    if (buf && MPR->stdError) {
        len = mprWriteFileString(MPR->stdError, buf);
    } else {
        len = -1;
    }
    return len;
}


PUBLIC ssize mprFprintf(MprFile *file, cchar *fmt, ...)
{
    ssize       len;
    va_list     ap;
    char        *buf;

    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    if (buf) {
        len = mprWriteFileString(file, buf);
    } else {
        len = -1;
    }
    return len;
}


#if KEEP
/*
    Printf with a static buffer. Used internally only. WILL NOT MALLOC.
 */
PUBLIC int mprStaticPrintf(cchar *fmt, ...)
{
    MprFileSystem   *fs;
    va_list         ap;
    char            buf[ME_MAX_BUFFER];

    fs = mprLookupFileSystem(NULL, "/");

    va_start(ap, fmt);
    mprPrintfCore(buf, ME_MAX_BUFFER, fmt, ap);
    va_end(ap);
    return mprWriteFile(fs->stdOutput, buf, slen(buf));
}


/*
    Printf with a static buffer. Used internally only. WILL NOT MALLOC.
 */
PUBLIC int mprStaticPrintfError(cchar *fmt, ...)
{
    MprFileSystem   *fs;
    va_list         ap;
    char            buf[ME_MAX_BUFFER];

    fs = mprLookupFileSystem(NULL, "/");

    va_start(ap, fmt);
    mprPrintfCore(buf, ME_MAX_BUFFER, fmt, ap);
    va_end(ap);
    return mprWriteFile(fs->stdError, buf, slen(buf));
}
#endif


PUBLIC char *fmt(char *buf, ssize bufsize, cchar *fmt, ...)
{
    va_list     ap;
    char        *result;

    assert(buf);
    assert(fmt);
    assert(bufsize > 0);

    va_start(ap, fmt);
    result = mprPrintfCore(buf, bufsize, fmt, ap);
    va_end(ap);
    return result;
}


PUBLIC char *fmtv(char *buf, ssize bufsize, cchar *fmt, va_list arg)
{
    assert(buf);
    assert(fmt);
    assert(bufsize > 0);

    return mprPrintfCore(buf, bufsize, fmt, arg);
}


static int getState(char c, int state)
{
    int     chrClass;

    if (c < ' ' || c > 'z') {
        chrClass = CLASS_NORMAL;
    } else {
        assert((c - ' ') < (int) sizeof(classMap));
        chrClass = classMap[(c - ' ')];
    }
    assert((chrClass * STATE_COUNT + state) < (int) sizeof(stateMap));
    state = stateMap[chrClass * STATE_COUNT + state];
    return state;
}


PUBLIC char *mprPrintfCore(char *buf, ssize maxsize, cchar *spec, va_list args)
{
    Format        fmt;
    MprEjsString  *es;
    MprEjsName    qname;
    ssize         len;
    int64         iValue;
    uint64        uValue;
    int           state;
    char          c, *safe;

    if (spec == 0) {
        spec = "";
    }
    if (buf != 0) {
        assert(maxsize > 0);
        fmt.buf = (uchar*) buf;
        fmt.endbuf = &fmt.buf[maxsize];
        fmt.growBy = -1;
    } else {
        if (maxsize <= 0) {
            maxsize = MAXINT;
        }
        len = min(ME_MAX_FMT, maxsize);
        if ((buf = mprAlloc(len)) == 0) {
            return 0;
        }
        fmt.buf = (uchar*) buf;
        fmt.endbuf = &fmt.buf[len];
        fmt.growBy = min(len * 2, maxsize - len);
    }
    fmt.maxsize = maxsize;
    fmt.start = fmt.buf;
    fmt.end = fmt.buf;
    fmt.len = 0;
    *fmt.start = '\0';

    state = STATE_NORMAL;

    while ((c = *spec++) != '\0') {
        state = getState(c, state);

        switch (state) {
        case STATE_NORMAL:
            BPUT(&fmt, c);
            break;

        case STATE_PERCENT:
            fmt.precision = -1;
            fmt.width = 0;
            fmt.flags = 0;
            break;

        case STATE_MODIFIER:
            switch (c) {
            case '+':
                fmt.flags |= SPRINTF_SIGN;
                break;
            case '-':
                fmt.flags |= SPRINTF_LEFT;
                break;
            case '#':
                fmt.flags |= SPRINTF_ALTERNATE;
                break;
            case '0':
                fmt.flags |= SPRINTF_LEAD_ZERO;
                break;
            case ' ':
                fmt.flags |= SPRINTF_LEAD_SPACE;
                break;
            case ',':
            case '\'':
                fmt.flags |= SPRINTF_COMMA;
                break;
            }
            break;

        case STATE_WIDTH:
            if (c == '*') {
                fmt.width = va_arg(args, int);
                if (fmt.width < 0) {
                    fmt.width = -fmt.width;
                    fmt.flags |= SPRINTF_LEFT;
                }
            } else {
                while (isdigit((uchar) c)) {
                    fmt.width = fmt.width * 10 + (c - '0');
                    c = *spec++;
                }
                spec--;
            }
            break;

        case STATE_DOT:
            fmt.precision = 0;
            break;

        case STATE_PRECISION:
            if (c == '*') {
                fmt.precision = va_arg(args, int);
            } else {
                while (isdigit((uchar) c)) {
                    fmt.precision = fmt.precision * 10 + (c - '0');
                    c = *spec++;
                }
                spec--;
            }
            break;

        case STATE_BITS:
            switch (c) {
            case 'L':
                fmt.flags |= SPRINTF_INT64;
                break;

            case 'h':
                fmt.flags |= SPRINTF_SHORT;
                break;

            case 'l':
                if (fmt.flags & SPRINTF_LONG) {
                    fmt.flags &= ~SPRINTF_LONG;
                    fmt.flags |= SPRINTF_INT64;
                } else {
                    fmt.flags |= SPRINTF_LONG;
                }
                break;

            case 'z':
                fmt.flags |= SPRINTF_SSIZE;
                break;
            }
            break;

        case STATE_TYPE:
            switch (c) {
            case 'e':
#if ME_FLOAT
            case 'g':
            case 'f':
                fmt.radix = 10;
                outFloat(&fmt, c, (double) va_arg(args, double));
                break;
#endif /* ME_FLOAT */

            case 'c':
                BPUT(&fmt, (char) va_arg(args, int));
                break;

            case 'N':
                /* Name */
                qname = va_arg(args, MprEjsName);
                if (qname.name) {
#if ME_CHAR_LEN > 1 && KEEP
                    outWideString(&fmt, (wchar*) qname.space->value, qname.space->length);
                    BPUT(&fmt, ':');
                    BPUT(&fmt, ':');
                    outWideString(&fmt, (wchar*) qname.name->value, qname.name->length);
#else
                    outString(&fmt, (char*) qname.space->value, qname.space->length);
                    BPUT(&fmt, ':');
                    BPUT(&fmt, ':');
                    outString(&fmt, (char*) qname.name->value, qname.name->length);
#endif
                } else {
                    outString(&fmt, NULL, 0);
                }
                break;

            case 'S':
                /* Safe string */
#if ME_CHAR_LEN > 1 && KEEP
                if (fmt.flags & SPRINTF_LONG) {
                    //  UNICODE - not right wchar
                    safe = mprEscapeHtml(va_arg(args, wchar*));
                    outWideString(&fmt, safe, -1);
                } else
#endif
                {
                    safe = mprEscapeHtml(va_arg(args, char*));
                    outString(&fmt, safe, -1);
                }
                break;

            case '@':
                /* MprEjsString */
                es = va_arg(args, MprEjsString*);
                if (es) {
#if ME_CHAR_LEN > 1 && KEEP
                    outWideString(&fmt, es->value, es->length);
#else
                    outString(&fmt, (char*) es->value, es->length);
#endif
                } else {
                    outString(&fmt, NULL, 0);
                }
                break;

            case 'w':
                /* Wide string of wchar characters (Same as %ls"). Null terminated. */
#if ME_CHAR_LEN > 1 && KEEP
                outWideString(&fmt, va_arg(args, wchar*), -1);
                break;
#else
                /* Fall through */
#endif

            case 's':
                /* Standard string */
#if ME_CHAR_LEN > 1 && KEEP
                if (fmt.flags & SPRINTF_LONG) {
                    outWideString(&fmt, va_arg(args, wchar*), -1);
                } else
#endif
                    outString(&fmt, va_arg(args, char*), -1);
                break;

            case 'i':
                ;

            case 'd':
                fmt.radix = 10;
                if (fmt.flags & SPRINTF_SHORT) {
                    iValue = (short) va_arg(args, int);
                } else if (fmt.flags & SPRINTF_LONG) {
                    iValue = (long) va_arg(args, long);
                } else if (fmt.flags & SPRINTF_SSIZE) {
                    iValue = (ssize) va_arg(args, ssize);
                } else if (fmt.flags & SPRINTF_INT64) {
                    iValue = (int64) va_arg(args, int64);
                } else {
                    iValue = (int) va_arg(args, int);
                }
                if (iValue >= 0) {
                    if (fmt.flags & SPRINTF_LEAD_SPACE) {
                        outNum(&fmt, " ", iValue);
                    } else if (fmt.flags & SPRINTF_SIGN) {
                        outNum(&fmt, "+", iValue);
                    } else {
                        outNum(&fmt, 0, iValue);
                    }
                } else {
                    outNum(&fmt, "-", -iValue);
                }
                break;

            case 'X':
                fmt.flags |= SPRINTF_UPPER_CASE;
                /*  Fall through  */
            case 'o':
            case 'x':
            case 'u':
                if (fmt.flags & SPRINTF_SHORT) {
                    uValue = (ushort) va_arg(args, uint);
                } else if (fmt.flags & SPRINTF_LONG) {
                    uValue = (ulong) va_arg(args, ulong);
                } else if (fmt.flags & SPRINTF_SSIZE) {
                    uValue = (ssize) va_arg(args, ssize);
                } else if (fmt.flags & SPRINTF_INT64) {
                    uValue = (uint64) va_arg(args, uint64);
                } else {
                    uValue = va_arg(args, uint);
                }
                if (c == 'u') {
                    fmt.radix = 10;
                    outNum(&fmt, 0, uValue);
                } else if (c == 'o') {
                    fmt.radix = 8;
                    if (fmt.flags & SPRINTF_ALTERNATE && uValue != 0) {
                        outNum(&fmt, "0", uValue);
                    } else {
                        outNum(&fmt, 0, uValue);
                    }
                } else {
                    fmt.radix = 16;
                    if (fmt.flags & SPRINTF_ALTERNATE && uValue != 0) {
                        if (c == 'X') {
                            outNum(&fmt, "0X", uValue);
                        } else {
                            outNum(&fmt, "0x", uValue);
                        }
                    } else {
                        outNum(&fmt, 0, uValue);
                    }
                }
                break;

            case 'n':       /* Count of chars seen thus far */
                if (fmt.flags & SPRINTF_SHORT) {
                    short *count = va_arg(args, short*);
                    *count = (int) (fmt.end - fmt.start);
                } else if (fmt.flags & SPRINTF_LONG) {
                    long *count = va_arg(args, long*);
                    *count = (int) (fmt.end - fmt.start);
                } else {
                    int *count = va_arg(args, int *);
                    *count = (int) (fmt.end - fmt.start);
                }
                break;

            case 'p':       /* Pointer */
#if ME_64
                uValue = (uint64) va_arg(args, void*);
#else
                uValue = (uint) PTOI(va_arg(args, void*));
#endif
                fmt.radix = 16;
                outNum(&fmt, "0x", uValue);
                break;

            default:
                BPUT(&fmt, c);
            }
        }
    }
    /*
        Return the buffer as the result. Prevents a double alloc.
     */
    BPUTNULL(&fmt);
    return (char*) fmt.buf;
}


static void outString(Format *fmt, cchar *str, ssize len)
{
    cchar   *cp;
    ssize   i;

    if (str == NULL) {
        str = "null";
        len = 4;
    } else if (fmt->flags & SPRINTF_ALTERNATE) {
        str++;
        len = (ssize) *str;
    } else if (fmt->precision >= 0) {
        for (cp = str, len = 0; len < fmt->precision; len++) {
            if (*cp++ == '\0') {
                break;
            }
        }
    } else if (len < 0) {
        len = slen(str);
    }
    if (!(fmt->flags & SPRINTF_LEFT)) {
        for (i = len; i < fmt->width; i++) {
            BPUT(fmt, (char) ' ');
        }
    }
    for (i = 0; i < len && *str; i++) {
        BPUT(fmt, *str++);
    }
    if (fmt->flags & SPRINTF_LEFT) {
        for (i = len; i < fmt->width; i++) {
            BPUT(fmt, (char) ' ');
        }
    }
}


#if ME_CHAR_LEN > 1 && KEEP
static void outWideString(Format *fmt, wchar *str, ssize len)
{
    wchar     *cp;
    int         i;

    if (str == 0) {
        BPUT(fmt, (char) 'n');
        BPUT(fmt, (char) 'u');
        BPUT(fmt, (char) 'l');
        BPUT(fmt, (char) 'l');
        return;
    } else if (fmt->flags & SPRINTF_ALTERNATE) {
        str++;
        len = (ssize) *str;
    } else if (fmt->precision >= 0) {
        for (cp = str, len = 0; len < fmt->precision; len++) {
            if (*cp++ == 0) {
                break;
            }
        }
    } else if (len < 0) {
        for (cp = str, len = 0; *cp++ == 0; len++) ;
    }
    if (!(fmt->flags & SPRINTF_LEFT)) {
        for (i = len; i < fmt->width; i++) {
            BPUT(fmt, (char) ' ');
        }
    }
    for (i = 0; i < len && *str; i++) {
        BPUT(fmt, *str++);
    }
    if (fmt->flags & SPRINTF_LEFT) {
        for (i = len; i < fmt->width; i++) {
            BPUT(fmt, (char) ' ');
        }
    }
}
#endif


static void outNum(Format *fmt, cchar *prefix, uint64 value)
{
    char    numBuf[64];
    char    *cp;
    char    *endp;
    char    c;
    int     letter, len, leadingZeros, i, fill;

    endp = &numBuf[sizeof(numBuf) - 1];
    *endp = '\0';
    cp = endp;

    /*
     *  Convert to ascii
     */
    if (fmt->radix == 16) {
        do {
            letter = (int) (value % fmt->radix);
            if (letter > 9) {
                if (fmt->flags & SPRINTF_UPPER_CASE) {
                    letter = 'A' + letter - 10;
                } else {
                    letter = 'a' + letter - 10;
                }
            } else {
                letter += '0';
            }
            *--cp = letter;
            value /= fmt->radix;
        } while (value > 0);

    } else if (fmt->flags & SPRINTF_COMMA) {
        i = 1;
        do {
            *--cp = '0' + (int) (value % fmt->radix);
            value /= fmt->radix;
            if ((i++ % 3) == 0 && value > 0) {
                *--cp = ',';
            }
        } while (value > 0);
    } else {
        do {
            *--cp = '0' + (int) (value % fmt->radix);
            value /= fmt->radix;
        } while (value > 0);
    }

    len = (int) (endp - cp);
    fill = fmt->width - len;

    if (prefix != 0) {
        fill -= (int) slen(prefix);
    }
    leadingZeros = (fmt->precision > len) ? fmt->precision - len : 0;
    fill -= leadingZeros;

    if (!(fmt->flags & SPRINTF_LEFT)) {
        c = (fmt->flags & SPRINTF_LEAD_ZERO) ? '0': ' ';
        for (i = 0; i < fill; i++) {
            BPUT(fmt, c);
        }
    }
    if (prefix != 0) {
        while (*prefix) {
            BPUT(fmt, *prefix++);
        }
    }
    for (i = 0; i < leadingZeros; i++) {
        BPUT(fmt, '0');
    }
    while (*cp) {
        BPUT(fmt, *cp);
        cp++;
    }
    if (fmt->flags & SPRINTF_LEFT) {
        for (i = 0; i < fill; i++) {
            BPUT(fmt, ' ');
        }
    }
}


#if ME_FLOAT
static void outFloat(Format *fmt, char specChar, double value)
{
    char    result[256], *cp;
    int     c, fill, i, len, index;

    result[0] = '\0';
    if (specChar == 'f') {
        sprintf(result, "%.*f", fmt->precision, value);
    } else if (specChar == 'g') {
        sprintf(result, "%*.*g", fmt->width, fmt->precision, value);
    } else if (specChar == 'e') {
        sprintf(result, "%*.*e", fmt->width, fmt->precision, value);
    }
    len = (int) slen(result);
    fill = fmt->width - len;
    if (fmt->flags & SPRINTF_COMMA) {
        if (((len - 1) / 3) > 0) {
            fill -= (len - 1) / 3;
        }
    }

    if (fmt->flags & SPRINTF_SIGN && value > 0) {
        BPUT(fmt, '+');
        fill--;
    }
    if (!(fmt->flags & SPRINTF_LEFT)) {
        c = (fmt->flags & SPRINTF_LEAD_ZERO) ? '0': ' ';
        for (i = 0; i < fill; i++) {
            BPUT(fmt, c);
        }
    }
    index = len;
    for (cp = result; *cp; cp++) {
        BPUT(fmt, *cp);
        if (fmt->flags & SPRINTF_COMMA) {
            if ((--index % 3) == 0 && index > 0) {
                BPUT(fmt, ',');
            }
        }
    }
    if (fmt->flags & SPRINTF_LEFT) {
        for (i = 0; i < fill; i++) {
            BPUT(fmt, ' ');
        }
    }
    BPUTNULL(fmt);
}


PUBLIC int mprIsNan(double value) {
#if WINDOWS
    return _fpclass(value) & (_FPCLASS_SNAN | _FPCLASS_QNAN);
#elif VXWORKS
    return value == (0.0 / 0.0);
#else
    return fpclassify(value) == FP_NAN;
#endif
}


PUBLIC int mprIsInfinite(double value) {
#if WINDOWS
    return _fpclass(value) & (_FPCLASS_PINF | _FPCLASS_NINF);
#elif VXWORKS
    return value == (1.0 / 0.0) || value == (-1.0 / 0.0);
#else
    return fpclassify(value) == FP_INFINITE;
#endif
}

PUBLIC int mprIsZero(double value) {
#if WINDOWS
    return _fpclass(value) & (_FPCLASS_NZ | _FPCLASS_PZ);
#elif VXWORKS
    return value == 0.0 || value == -0.0;
#else
    return fpclassify(value) == FP_ZERO;
#endif
}
#endif /* ME_FLOAT */


/*
    Grow the buffer to fit new data. Return 1 if the buffer can grow. 
    Grow using the growBy size specified when creating the buffer. 
 */
static int growBuf(Format *fmt)
{
    uchar   *newbuf;
    ssize   buflen;

    buflen = (int) (fmt->endbuf - fmt->buf);
    if (fmt->maxsize >= 0 && buflen >= fmt->maxsize) {
        return 0;
    }
    if (fmt->growBy <= 0) {
        /*
            User supplied buffer
         */
        return 0;
    }
    newbuf = mprAlloc(buflen + fmt->growBy);
    if (newbuf == 0) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    if (fmt->buf) {
        memcpy(newbuf, fmt->buf, buflen);
    }
    buflen += fmt->growBy;
    fmt->end = newbuf + (fmt->end - fmt->buf);
    fmt->start = newbuf + (fmt->start - fmt->buf);
    fmt->buf = newbuf;
    fmt->endbuf = &fmt->buf[buflen];

    /*
        Increase growBy to reduce overhead
     */
    if ((buflen + (fmt->growBy * 2)) < fmt->maxsize) {
        fmt->growBy *= 2;
    }
    return 1;
}


PUBLIC ssize print(cchar *fmt, ...) 
{
    va_list     ap;
    char        *buf;
    ssize       len;

    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    if (buf != 0 && MPR->stdOutput) {
        len = mprWriteFileString(MPR->stdOutput, buf);
        len += mprWriteFileString(MPR->stdOutput, "\n");
    } else {
        len = -1;
    }
    return len;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/rom.c ************/


/*
    rom.c - ROM File system

    ROM support for systems without disk or flash based file systems. This module provides read-only file retrieval 
    from compiled file images. Use the mprRomComp program to compile files into C code and then link them into your 
    application. This module uses a hashed symbol table for fast file lookup.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_ROM 
/********************************** Defines ***********************************/

#ifndef ME_MAX_ROMFS
    #define ME_MAX_ROMFS 37           /* Size of the ROMFS hash lookup */
#endif

/********************************** Forwards **********************************/

static void manageRomFile(MprFile *file, int flags);
static int getPathInfo(MprRomFileSystem *rfs, cchar *path, MprPath *info);
static MprRomInode *lookup(MprRomFileSystem *rfs, cchar *path);

/*********************************** Code *************************************/

static MprFile *openFile(MprFileSystem *fileSystem, cchar *path, int flags, int omode)
{
    MprRomFileSystem    *rfs;
    MprFile             *file;

    assert(path && *path);

    rfs = (MprRomFileSystem*) fileSystem;
    file = mprAllocObj(MprFile, manageRomFile);
    file->fileSystem = fileSystem;
    file->mode = omode;
    file->fd = -1;
    file->path = sclone(path);
    if ((file->inode = lookup(rfs, path)) == 0) {
        return 0;
    }
    return file;
}


static void manageRomFile(MprFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->path);
        mprMark(file->buf);
        mprMark(file->fileSystem);
        mprMark(file->inode);
    }
}


static int closeFile(MprFile *file)
{
    return 0;
}


static ssize readFile(MprFile *file, void *buf, ssize size)
{
    MprRomInode     *inode;
    ssize           len;

    assert(buf);

    if (file->fd == 0) {
        return read(file->fd, buf, size);
    }
    inode = file->inode;
    len = min(inode->size - file->iopos, size);
    assert(len >= 0);
    memcpy(buf, &inode->data[file->iopos], len);
    file->iopos += len;
    return len;
}


static ssize writeFile(MprFile *file, cvoid *buf, ssize size)
{
    if (file->fd == 1 || file->fd == 2) {
        return write(file->fd, buf, size);
    }
    return MPR_ERR_CANT_WRITE;
}


static long seekFile(MprFile *file, int seekType, long distance)
{
    MprRomInode     *inode;

    assert(seekType == SEEK_SET || seekType == SEEK_CUR || seekType == SEEK_END);
    inode = file->inode;

    switch (seekType) {
    case SEEK_CUR:
        file->iopos += distance;
        break;

    case SEEK_END:
        file->iopos = inode->size + distance;
        break;

    default:
        file->iopos = distance;
        break;
    }
    if (file->iopos < 0) {
        errno = EBADF;
        return MPR_ERR_BAD_STATE;
    }
    return file->iopos;
}


static bool accessPath(MprRomFileSystem *fileSystem, cchar *path, int omode)
{
    MprPath     info;
    return getPathInfo(fileSystem, path, &info) == 0 ? 1 : 0;
}


static int deletePath(MprRomFileSystem *fileSystem, cchar *path)
{
    return MPR_ERR_CANT_WRITE;
}
 

static int makeDir(MprRomFileSystem *fileSystem, cchar *path, int perms, int owner, int group)
{
    return MPR_ERR_CANT_WRITE;
}


static int makeLink(MprRomFileSystem *fileSystem, cchar *path, cchar *target, int hard)
{
    return MPR_ERR_CANT_WRITE;
}


static int getPathInfo(MprRomFileSystem *rfs, cchar *path, MprPath *info)
{
    MprRomInode *ri;

    assert(path && *path);
    memset(info, 0, sizeof(MprPath));
    info->checked = 1;

    if ((ri = (MprRomInode*) lookup(rfs, path)) == 0) {
        return MPR_ERR_CANT_FIND;
    }
    info->valid = 1;
    info->size = ri->size;
    info->mtime = 0;
    info->inode = ri->num;

    if (ri->data == 0) {
        info->isDir = 1;
        info->isReg = 0;
    } else {
        info->isReg = 1;
        info->isDir = 0;
    }
    return 0;
}


static int getPathLink(MprRomFileSystem *rfs, cchar *path)
{
    /* Links not supported on ROMfs */
    return -1;
}


static MprRomInode *lookup(MprRomFileSystem *rfs, cchar *path)
{
    if (path == 0) {
        return 0;
    }
    /*
        Remove "./" segments
     */
    while (*path == '.') {
        if (path[1] == '\0') {
            path++;
        } else if (path[1] == '/') {
            path += 2;
        } else {
            break;
        }
    }
    /*
        Skip over the leading "/"
     */
    if (*path == '/') {
        path++;
    }
    return (MprRomInode*) mprLookupKey(rfs->fileIndex, path);
}


PUBLIC int mprSetRomFileSystem(MprRomInode *inodeList)
{
    MprRomFileSystem    *rfs;
    MprRomInode         *ri;

    rfs = (MprRomFileSystem*) MPR->fileSystem;
    rfs->romInodes = inodeList;
    rfs->fileIndex = mprCreateHash(ME_MAX_ROMFS, MPR_HASH_STATIC_KEYS | MPR_HASH_STATIC_VALUES);

    for (ri = inodeList; ri->path; ri++) {
        if (mprAddKey(rfs->fileIndex, ri->path, ri) < 0) {
            assert(!MPR_ERR_MEMORY);
            return MPR_ERR_MEMORY;
        }
    }
    return 0;
}


PUBLIC void manageRomFileSystem(MprRomFileSystem *rfs, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        MprFileSystem *fs = (MprFileSystem*) rfs;
        mprMark(fs->separators);
        mprMark(fs->newline);
        mprMark(fs->root);
#if ME_WIN_LIKE || CYGWIN
        mprMark(fs->cygdrive);
        mprMark(fs->cygwin);
#endif
        mprMark(rfs->fileIndex);
    }
}


PUBLIC MprRomFileSystem *mprCreateRomFileSystem(cchar *path)
{
    MprFileSystem      *fs;
    MprRomFileSystem   *rfs;

    if ((rfs = mprAllocObj(MprRomFileSystem, manageRomFileSystem)) == 0) {
        return rfs;
    }
    fs = &rfs->fileSystem;
    fs->accessPath = (MprAccessFileProc) accessPath;
    fs->deletePath = (MprDeleteFileProc) deletePath;
    fs->getPathInfo = (MprGetPathInfoProc) getPathInfo;
    fs->getPathLink = (MprGetPathLinkProc) getPathLink;
    fs->makeDir = (MprMakeDirProc) makeDir;
    fs->makeLink = (MprMakeLinkProc) makeLink;
    fs->openFile = (MprOpenFileProc) openFile;
    fs->closeFile = (MprCloseFileProc) closeFile;
    fs->readFile = (MprReadFileProc) readFile;
    fs->seekFile = (MprSeekFileProc) seekFile;
    fs->writeFile = (MprWriteFileProc) writeFile;

    if ((MPR->stdError = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdError, "stderr");
    MPR->stdError->fd = 2;
    MPR->stdError->fileSystem = fs;
    MPR->stdError->mode = O_WRONLY;

    if ((MPR->stdInput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdInput, "stdin");
    MPR->stdInput->fd = 0;
    MPR->stdInput->fileSystem = fs;
    MPR->stdInput->mode = O_RDONLY;

    if ((MPR->stdOutput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdOutput, "stdout");
    MPR->stdOutput->fd = 1;
    MPR->stdOutput->fileSystem = fs;
    MPR->stdOutput->mode = O_WRONLY;
    return rfs;
}

#else
void romDummy() {}

#endif /* ME_ROM */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/select.c ************/


/**
    select.c - Wait for I/O by using select.

    This module provides I/O wait management for sockets on VxWorks and systems that use select(). Windows and Unix
    uses different mechanisms. See mprAsyncSelectWait and mprPollWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */
/********************************* Includes ***********************************/



#if ME_EVENT_NOTIFIER == MPR_EVENT_SELECT

/********************************** Forwards **********************************/

static void serviceIO(MprWaitService *ws, fd_set *readMask, fd_set *writeMask, int maxfd);
static void readPipe(MprWaitService *ws);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    int     rc, retries, breakPort, breakSock, maxTries;

    ws->highestFd = 0;
    if ((ws->handlerMap = mprCreateList(MPR_FD_MIN, 0)) == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    FD_ZERO(&ws->readMask);
    FD_ZERO(&ws->writeMask);

    /*
        Try to find a good port to use to break out of the select wait
     */ 
    maxTries = 100;
    breakPort = ME_WAKEUP_PORT;
    for (rc = retries = 0; retries < maxTries; retries++) {
        breakSock = socket(AF_INET, SOCK_DGRAM, 0);
        if (breakSock < 0) {
            mprLog("critical mpr select", 0, "Cannot open port %d to use for select. Retrying.");
        }
#if ME_UNIX_LIKE
        fcntl(breakSock, F_SETFD, FD_CLOEXEC);
#endif
        ws->breakAddress.sin_family = AF_INET;
#if CYGWIN || VXWORKS
        /*
            Cygwin & VxWorks don't work with INADDR_ANY
         */
        ws->breakAddress.sin_addr.s_addr = inet_addr("127.0.0.1");
#else
        ws->breakAddress.sin_addr.s_addr = INADDR_ANY;
#endif
        ws->breakAddress.sin_port = htons((short) breakPort);
        rc = bind(breakSock, (struct sockaddr *) &ws->breakAddress, sizeof(ws->breakAddress));
        if (breakSock >= 0 && rc == 0) {
#if VXWORKS
            /* VxWorks 6.0 bug workaround */
            ws->breakAddress.sin_port = htons((short) breakPort);
#endif
            break;
        }
        if (breakSock >= 0) {
            closesocket(breakSock);
        }
        breakPort++;
    }
    if (breakSock < 0 || rc < 0) {
        mprLog("critical mpr select", 0, "Cannot bind any port to use for select. Tried %d-%d", breakPort, breakPort - maxTries);
        return MPR_ERR_CANT_OPEN;
    }
    ws->breakSock = breakSock;
    FD_SET(breakSock, &ws->readMask);
    ws->highestFd = breakSock;
    return 0;
}


PUBLIC void mprManageSelect(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->handlerMap);

    } else if (flags & MPR_MANAGE_FREE) {
        if (ws->breakSock >= 0) {
            close(ws->breakSock);
        }
    }
}


PUBLIC int mprNotifyOn(MprWaitHandler *wp, int mask)
{
    MprWaitService  *ws;
    int     fd;

    ws = wp->service;
    fd = wp->fd;
    assert(fd >= 0);

    if (fd >= FD_SETSIZE) {
        mprLog("error mpr event", 0, "File descriptor exceeds configured maximum in FD_SETSIZE (%d vs %d)", fd, FD_SETSIZE);
        return MPR_ERR_CANT_INITIALIZE;
    }
    lock(ws);
    if (wp->desiredMask != mask) {
        if (wp->desiredMask & MPR_READABLE && !(mask & MPR_READABLE)) {
            FD_CLR(fd, &ws->readMask);
        }
        if (wp->desiredMask & MPR_WRITABLE && !(mask & MPR_WRITABLE)) {
            FD_CLR(fd, &ws->writeMask);
        }
        if (mask & MPR_READABLE) {
            FD_SET(fd, &ws->readMask);
        }
        if (mask & MPR_WRITABLE) {
            FD_SET(fd, &ws->writeMask);
        }
        wp->desiredMask = mask;
        ws->highestFd = max(fd, ws->highestFd);
        if (mask == 0 && fd == ws->highestFd) {
            while (--fd > 0) {
                if (FD_ISSET(fd, &ws->readMask) || FD_ISSET(fd, &ws->writeMask)) {
                    break;
                }
            }
            ws->highestFd = fd;
        }
#if UNUSED
        /*
            Disabled because mprRemoteEvent schedules the dispatcher AND lockes the event service.
            This may cause deadlocks and specifically, mprRemoveEvent may crash while it races with event service
            on another thread.
         */
        if (wp->event) {
            mprRemoveEvent(wp->event);
            wp->event = 0;
        }
#endif
        mprSetItem(ws->handlerMap, fd, mask ? wp : 0);
    }
    mprWakeEventService();
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single file descriptor. Return a mask of events found. Mask is the events of interest.
    timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTicks timeout)
{
    struct timeval  tval;
    fd_set          readMask, writeMask;
    int             rc, result;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
    tval.tv_sec = (int) (timeout / 1000);
    tval.tv_usec = (int) ((timeout % 1000) * 1000);

    FD_ZERO(&readMask);
    if (mask & MPR_READABLE) {
        FD_SET(fd, &readMask);
    }
    FD_ZERO(&writeMask);
    if (mask & MPR_WRITABLE) {
        FD_SET(fd, &writeMask);
    }
    mprYield(MPR_YIELD_STICKY);
    rc = select(fd + 1, &readMask, &writeMask, NULL, &tval);
    mprResetYield();

    result = 0;
    if (rc < 0) {
        mprLog("error mpr event", 0, "Select returned %d, errno %d", rc, mprGetOsError());

    } else if (rc > 0) {
        if (FD_ISSET(fd, &readMask)) {
            result |= MPR_READABLE;
        }
        if (FD_ISSET(fd, &writeMask)) {
            result |= MPR_WRITABLE;
        }
    }
    return result;
}


/*
    Wait for I/O on all registered file descriptors. Timeout is in milliseconds. Return the number of events detected.
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTicks timeout)
{
    struct timeval  tval;
    fd_set          readMask, writeMask;
    int             rc, maxfd;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if ME_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
#if VXWORKS
    /* Minimize worst-case VxWorks task starvation */
    timeout = max(timeout, 50);
#endif
    tval.tv_sec = (int) (timeout / 1000);
    tval.tv_usec = (int) ((timeout % 1000) * 1000);

    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    lock(ws);
    readMask = ws->readMask;
    writeMask = ws->writeMask;
    maxfd = ws->highestFd + 1;
    unlock(ws);

    mprYield(MPR_YIELD_STICKY);
    rc = select(maxfd, &readMask, &writeMask, NULL, &tval);
    mprClearWaiting();
    mprResetYield();

    if (rc > 0) {
        serviceIO(ws, &readMask, &writeMask, maxfd);
    }
    ws->wakeRequested = 0;
}


static void serviceIO(MprWaitService *ws, fd_set *readMask, fd_set *writeMask, int maxfd)
{
    MprWaitHandler      *wp;
    int                 fd, mask;

    lock(ws);
    for (fd = 0; fd < maxfd; fd++) {
        mask = 0;
        if (FD_ISSET(fd, readMask)) {
            mask |= MPR_READABLE;
        }
        if (FD_ISSET(fd, writeMask)) {
            mask |= MPR_WRITABLE;
        }
        if (mask) {
            if (fd == ws->breakSock) {
                readPipe(ws);
                continue;
            }
            if (fd < 0 || (wp = mprGetItem(ws->handlerMap, fd)) == 0) {
                /*
                    This can happen if a writable event has been triggered (e.g. MprCmd command stdin pipe) and the pipe is closed.
                    Also may happen if fd == ws->breakSock and breakSock is the highest fd.
                    This thread may have waked before the pipe is closed and the wait handler removed from the map.
                 */
                continue;
            }
            wp->presentMask = mask & wp->desiredMask;
            if (wp->presentMask) {
                if (wp->flags & MPR_WAIT_IMMEDIATE) {
                    (wp->proc)(wp->handlerData, NULL);
                } else {
                    mprNotifyOn(wp, 0);
                    mprQueueIOEvent(wp);
                }
            }
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
    Must be async-safe.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;
    ssize           rc;
    int             c;

    ws = MPR->waitService;
    if (!ws->wakeRequested) {
        ws->wakeRequested = 1;
        c = 0;
        rc = sendto(ws->breakSock, (char*) &c, 1, 0, (struct sockaddr*) &ws->breakAddress, (int) sizeof(ws->breakAddress));
        if (rc < 0) {
            static int warnOnce = 0;
            if (warnOnce++ == 0) {
                mprLog("error mpr event", 0, "Cannot send wakeup to breakout socket: errno %d", errno);
            }
        }
    }
}


static void readPipe(MprWaitService *ws)
{
    char        buf[128];

#if VXWORKS
    int len = sizeof(ws->breakAddress);
    (void) recvfrom(ws->breakSock, buf, (int) sizeof(buf), 0, (struct sockaddr*) &ws->breakAddress, (int*) &len);
#else
    socklen_t   len = sizeof(ws->breakAddress);
    (void) recvfrom(ws->breakSock, buf, (int) sizeof(buf), 0, (struct sockaddr*) &ws->breakAddress, (socklen_t*) &len);
#endif
}

#else
void selectDummy() {}

#endif /* MPR_EVENT_SELECT */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/signal.c ************/


/**
    signal.c - Signal handling for Unix systems

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/*********************************** Includes *********************************/



/*********************************** Forwards *********************************/
#if ME_UNIX_LIKE

static void manageSignal(MprSignal *sp, int flags);
static void manageSignalService(MprSignalService *ssp, int flags);
static void signalEvent(MprSignal *sp, MprEvent *event);
static void signalHandler(int signo, siginfo_t *info, void *arg);
static void standardSignalHandler(void *ignored, MprSignal *sp);
static void unhookSignal(int signo);

/************************************ Code ************************************/

PUBLIC MprSignalService *mprCreateSignalService()
{
    MprSignalService    *ssp;

    if ((ssp = mprAllocObj(MprSignalService, manageSignalService)) == 0) {
        return 0;
    }
    ssp->mutex = mprCreateLock();
    ssp->signals = mprAllocZeroed(sizeof(MprSignal*) * MPR_MAX_SIGNALS);
    ssp->standard = mprCreateList(-1, 0);
    return ssp;
}


static void manageSignalService(MprSignalService *ssp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ssp->signals);
        mprMark(ssp->standard);
        mprMark(ssp->mutex);
        /* Don't mark signals elements as it will prevent signal handlers being reclaimed */
    }
}


PUBLIC void mprStopSignalService()
{
    int     i;

    for (i = 1; i < MPR_MAX_SIGNALS; i++) {
        unhookSignal(i);
    }
}


/*
    Signals are hooked on demand and remain till the Mpr is destroyed
 */
static void hookSignal(int signo, MprSignal *sp)
{
    MprSignalService    *ssp;
    struct sigaction    act, old;
    int                 rc;

    assert(0 < signo && signo < MPR_MAX_SIGNALS);
    ssp = MPR->signalService;
    lock(ssp);
    rc = sigaction(signo, 0, &old);
    if (rc == 0 && old.sa_sigaction != signalHandler) {
        sp->sigaction = old.sa_sigaction;
        ssp->prior[signo] = old;
        memset(&act, 0, sizeof(act));
        act.sa_sigaction = signalHandler;
        act.sa_flags |= SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
        act.sa_flags &= ~SA_NODEFER;
        sigemptyset(&act.sa_mask);
        if (sigaction(signo, &act, 0) != 0) {
            mprLog("error mpr", 0, "Cannot hook signal %d, errno %d", signo, mprGetOsError());
        }
    }
    unlock(ssp);
}


static void unhookSignal(int signo)
{
    MprSignalService    *ssp;
    struct sigaction    act;
    int                 rc;

    ssp = MPR->signalService;
    lock(ssp);
    rc = sigaction(signo, 0, &act);
    if (rc == 0 && act.sa_sigaction == signalHandler) {
        if (sigaction(signo, &ssp->prior[signo], 0) != 0) {
            mprLog("error mpr", 0, "Cannot unhook signal %d, errno %d", signo, mprGetOsError());
        }
    }
    unlock(ssp);
}


/*
    Actual signal handler - must be async-safe. Do very, very little here. Just set a global flag and wakeup the wait
    service (mprWakeEventService is async-safe). WARNING: Don't put memory allocation, logging or printf here.

    NOTES: The problems here are several fold. The signalHandler may be invoked re-entrantly for different threads for
    the same signal (SIGCHLD). Masked signals are blocked by a single bit and so siginfo will only store one such instance, 
    so you cannot use siginfo to get the pid for SIGCHLD. So you really cannot save state here, only set an indication that
    a signal has occurred. MprServiceSignals will then process. Signal handlers must then all be invoked and they must
    test if the signal is valid for them. 
 */
static void signalHandler(int signo, siginfo_t *info, void *arg)
{
    MprSignalService    *ssp;
    MprSignalInfo       *ip;
    int                 saveErrno;

    if (signo == SIGINT) {
        /* Fixes command line recall to complete the line */
        printf("\n");
        exit(1);
    }
    if (signo <= 0 || signo >= MPR_MAX_SIGNALS || MPR == 0 || mprIsStopped()) {
        return;
    }
    /*
        Cannot save siginfo, because there is no reliable and scalable way to save siginfo state for multiple threads.
     */
    ssp = MPR->signalService;
    ip = &ssp->info[signo];
    ip->triggered = 1;
    ssp->hasSignals = 1;
    saveErrno = errno;
    mprWakeNotifier();
    errno = saveErrno;
}

/*
    Called by mprServiceEvents after a signal has been received. Create an event and queue on the appropriate dispatcher.
 */
PUBLIC void mprServiceSignals()
{
    MprSignalService    *ssp;
    MprSignal           *sp;
    MprSignalInfo       *ip;
    int                 signo;

    ssp = MPR->signalService;
    if (ssp->hasSignals) {
        lock(ssp);
        ssp->hasSignals = 0;
        for (ip = ssp->info; ip < &ssp->info[MPR_MAX_SIGNALS]; ip++) {
            if (ip->triggered) {
                ip->triggered = 0;
                /*
                    Create events for all registered handlers
                 */
                signo = (int) (ip - ssp->info);
                for (sp = ssp->signals[signo]; sp; sp = sp->next) {
                    mprCreateEvent(sp->dispatcher, "signalEvent", 0, signalEvent, sp, 0);
                }
            }
        }
        unlock(ssp);
    }
}


/*
    Process the signal event. Runs from the dispatcher so signal handlers don't have to be async-safe.
 */
static void signalEvent(MprSignal *sp, MprEvent *event)
{
    assert(sp);
    assert(event);

    mprDebug("mpr signal", 5, "Received signal %d, flags %x", sp->signo, sp->flags);

    /*
        Return if the handler has been removed since it the event was created
     */
    if (sp->signo == 0) {
        return;
    }
    if (sp->flags & MPR_SIGNAL_BEFORE) {
        (sp->handler)(sp->data, sp);
    } 
    if (sp->sigaction && (sp->sigaction != SIG_IGN && sp->sigaction != SIG_DFL)) {
        /*
            Call the original (foreign) action handler. Cannot pass on siginfo, because there is no reliable and scalable
            way to save siginfo state when the signalHandler is reentrant across multiple threads.
         */
        (sp->sigaction)(sp->signo, NULL, NULL);
    }
    if (sp->flags & MPR_SIGNAL_AFTER) {
        (sp->handler)(sp->data, sp);
    }
}


static void linkSignalHandler(MprSignal *sp)
{
    MprSignalService    *ssp;

    ssp = MPR->signalService;
    lock(ssp);
    sp->next = ssp->signals[sp->signo];
    ssp->signals[sp->signo] = sp;
    unlock(ssp);
}


static void unlinkSignalHandler(MprSignal *sp)
{
    MprSignalService    *ssp;
    MprSignal           *np, *prev;

    ssp = MPR->signalService;
    lock(ssp);
    for (prev = 0, np = ssp->signals[sp->signo]; np; np = np->next) {
        if (sp == np) {
            if (prev) {
                prev->next = sp->next;
            } else {
                ssp->signals[sp->signo] = sp->next;
            }
            sp->signo = 0;
            break;
        }
        prev = np;
    }
    unlock(ssp);
}


/*
    Add a safe-signal handler. This creates a signal handler that will run from a dispatcher without the
    normal async-safe strictures of normal signal handlers. This manages a next of signal handlers and ensures
    that prior handlers will be called appropriately.
 */
PUBLIC MprSignal *mprAddSignalHandler(int signo, void *handler, void *data, MprDispatcher *dispatcher, int flags)
{
    MprSignal           *sp;

    if (signo <= 0 || signo >= MPR_MAX_SIGNALS) {
        mprLog("error mpr", 0, "Bad signal: %d", signo);
        return 0;
    }
    if (!(flags & MPR_SIGNAL_BEFORE)) {
        flags |= MPR_SIGNAL_AFTER;
    }
    if ((sp = mprAllocObj(MprSignal, manageSignal)) == 0) {
        return 0;
    }
    sp->signo = signo;
    sp->flags = flags;
    sp->handler = handler;
    sp->dispatcher = dispatcher;
    sp->data = data;
    linkSignalHandler(sp);
    hookSignal(signo, sp);
    return sp;
}


static void manageSignal(MprSignal *sp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        /* Don't mark next as it will prevent other signal handlers being reclaimed */
        mprMark(sp->data);
        mprMark(sp->dispatcher);

    } else if (flags & MPR_MANAGE_FREE) {
        if (sp->signo) {
            unlinkSignalHandler(sp);
        }
    }
}


PUBLIC void mprRemoveSignalHandler(MprSignal *sp)
{
    if (sp && sp->signo) {
        unlinkSignalHandler(sp);
    }
}


/*
    Standard signal handler. The following signals are handled:
        SIGINT - immediate exit
        SIGTERM - graceful shutdown
        SIGPIPE - ignore
        SIGXFZ - ignore
        SIGUSR1 - graceful shutdown, then restart
        SIGUSR2 - toggle trace level (Appweb)
        All others - default exit
 */
PUBLIC void mprAddStandardSignals()
{
    MprSignalService    *ssp;

    ssp = MPR->signalService;
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGINT,  standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGQUIT, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGTERM, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGPIPE, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGUSR1, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
#if SIGXFSZ
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGXFSZ, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
#endif
#if MACOSX && ME_DEBUG && KEEP
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGBUS, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
    mprAddItem(ssp->standard, mprAddSignalHandler(SIGSEGV, standardSignalHandler, 0, 0, MPR_SIGNAL_AFTER));
#endif
}


static void standardSignalHandler(void *ignored, MprSignal *sp)
{
    if (sp->signo == SIGTERM) {
        mprShutdown(MPR_EXIT_NORMAL, -1, MPR_EXIT_TIMEOUT);

    } else if (sp->signo == SIGINT || sp->signo == SIGQUIT) {
#if ME_UNIX_LIKE
        /*  Ensure shell input goes to a new line */
        if (isatty(1)) {
            if (write(1, "\n", 1) < 0) {}
        }
#endif
        mprShutdown(MPR_EXIT_ABORT, -1, 0);

    } else if (sp->signo == SIGUSR1) {
        mprShutdown(MPR_EXIT_RESTART, 0, 0);

    } else if (sp->signo == SIGPIPE || sp->signo == SIGXFSZ) {
        /* Ignore */

#if KEEP
    } else if (sp->signo == SIGSEGV || sp->signo == SIGBUS) {
#if EMBEDTHIS && KEEP
        printf("PAUSED for watson to debug\n");
        sleep(120);
#else
        exit(255);
#endif
#endif
    } else {
        mprShutdown(MPR_EXIT_ABORT, -1, 0);
    }
}


#else /* ME_UNIX_LIKE */
    void mprAddStandardSignals() {}
    MprSignalService *mprCreateSignalService() { return mprAlloc(0); }
    void mprStopSignalService() {};
    void mprRemoveSignalHandler(MprSignal *sp) { }
    void mprServiceSignals() {}
#endif /* ME_UNIX_LIKE */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/socket.c ************/


/**
    socket.c - Convenience class for the management of sockets

    This module provides a higher interface to interact with the standard sockets API. It does not perform buffering.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



#if !VXWORKS
/*
    On MAC OS X, getaddrinfo is not thread-safe and crashes when called by a 2nd thread at any time. ie. locking wont help.
 */
#define ME_COMPILER_HAS_GETADDRINFO 1
#endif

/********************************** Defines ***********************************/

#ifndef ME_MAX_IP
    #define ME_MAX_IP 1024
#endif

/********************************** Forwards **********************************/

static void closeSocket(MprSocket *sp, bool gracefully);
static int connectSocket(MprSocket *sp, cchar *ipAddr, int port, int initialFlags);
static MprSocketProvider *createStandardProvider(MprSocketService *ss);
static void disconnectSocket(MprSocket *sp);
static ssize flushSocket(MprSocket *sp);
static int getSocketIpAddr(struct sockaddr *addr, int addrlen, char *ip, int size, int *port);
static int ipv6(cchar *ip);
static void manageSocket(MprSocket *sp, int flags);
static void manageSocketService(MprSocketService *ss, int flags);
static void manageSsl(MprSsl *ssl, int flags);
static ssize readSocket(MprSocket *sp, void *buf, ssize bufsize);
static char *socketState(MprSocket *sp);
static ssize writeSocket(MprSocket *sp, cvoid *buf, ssize bufsize);

/************************************ Code ************************************/
/*
    Open the socket service
 */

PUBLIC MprSocketService *mprCreateSocketService()
{
    MprSocketService    *ss;
    char                hostName[ME_MAX_IP], serverName[ME_MAX_IP], domainName[ME_MAX_IP], *dp;
    Socket              fd;

    if ((ss = mprAllocObj(MprSocketService, manageSocketService)) == 0) {
        return 0;
    }
    ss->maxAccept = MAXINT;
    ss->numAccept = 0;

    if ((ss->standardProvider = createStandardProvider(ss)) == 0) {
        return 0;
    }
    if ((ss->mutex = mprCreateLock()) == 0) {
        return 0;
    }
    serverName[0] = '\0';
    domainName[0] = '\0';
    hostName[0] = '\0';
    if (gethostname(serverName, sizeof(serverName)) < 0) {
        scopy(serverName, sizeof(serverName), "localhost");
        mprLog("error mpr", 0, "Cannot get host name. Using \"localhost\".");
        /* Keep going */
    }
    if ((dp = strchr(serverName, '.')) != 0) {
        scopy(hostName, sizeof(hostName), serverName);
        *dp++ = '\0';
        scopy(domainName, sizeof(domainName), dp);

    } else {
        scopy(hostName, sizeof(hostName), serverName);
    }
    mprSetServerName(serverName);
    mprSetDomainName(domainName);
    mprSetHostName(hostName);
    ss->secureSockets = mprCreateList(0, 0);

    if ((fd = socket(AF_INET6, SOCK_STREAM, 0)) != -1) {
        ss->hasIPv6 = 1;
        closesocket(fd);
    } else {
        mprLog("info mpr socket", 1, "This system does not have IPv6 support");
    }
    return ss;
}


static void manageSocketService(MprSocketService *ss, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ss->standardProvider);
        mprMark(ss->providers);
        mprMark(ss->sslProvider);
        mprMark(ss->secureSockets);
        mprMark(ss->mutex);
    }
}


static void manageSocketProvider(MprSocketProvider *provider, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(provider->name);
    }
}


static MprSocketProvider *createStandardProvider(MprSocketService *ss)
{
    MprSocketProvider   *provider;

    if ((provider = mprAllocObj(MprSocketProvider, manageSocketProvider)) == 0) {
        return 0;
    }
    provider->name = sclone("standard");;
    provider->closeSocket = closeSocket;
    provider->disconnectSocket = disconnectSocket;
    provider->flushSocket = flushSocket;
    provider->readSocket = readSocket;
    provider->writeSocket = writeSocket;
    provider->socketState = socketState;
    return provider;
}


PUBLIC void mprAddSocketProvider(cchar *name, MprSocketProvider *provider)
{
    MprSocketService    *ss;

    ss = MPR->socketService;

    if (ss->providers == 0 && (ss->providers = mprCreateHash(0, 0)) == 0) {
        return;
    }
    provider->name = sclone(name);
    mprAddKey(ss->providers, name, provider);
}


PUBLIC bool mprHasSecureSockets()
{
    return (MPR->socketService->providers != 0);
}


PUBLIC int mprSetMaxSocketAccept(int max)
{
    assert(max >= 0);

    MPR->socketService->maxAccept = max;
    return 0;
}


PUBLIC MprSocket *mprCreateSocket()
{
    MprSocketService    *ss;
    MprSocket           *sp;

    ss = MPR->socketService;
    if ((sp = mprAllocObj(MprSocket, manageSocket)) == 0) {
        return 0;
    }
    sp->port = -1;
    sp->fd = INVALID_SOCKET;

    sp->provider = ss->standardProvider;
    sp->service = ss;
    sp->mutex = mprCreateLock();
    return sp;
}


PUBLIC MprSocket *mprCloneSocket(MprSocket *sp)
{
    MprSocket   *newsp;

    if ((newsp = mprCreateSocket()) == 0) {
       return 0;
    }
    newsp->handler = sp->handler;
    newsp->acceptIp = sp->acceptIp;
    newsp->ip = sp->ip;
    newsp->errorMsg = sp->errorMsg;
    newsp->acceptPort = sp->acceptPort;
    newsp->port = sp->port;
    newsp->fd = sp->fd;
    newsp->flags = sp->flags;
    newsp->provider = sp->provider;
    newsp->listenSock = sp->listenSock;
    newsp->sslSocket = sp->sslSocket;
    newsp->ssl = sp->ssl;
    newsp->mutex = mprCreateLock();
    return newsp;
}    


static void manageSocket(MprSocket *sp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(sp->handler);
        mprMark(sp->acceptIp);
        mprMark(sp->ip);
        mprMark(sp->errorMsg);
        mprMark(sp->provider);
        mprMark(sp->listenSock);
        mprMark(sp->sslSocket);
        mprMark(sp->ssl);
        mprMark(sp->cipher);
        mprMark(sp->peerName);
        mprMark(sp->peerCert);
        mprMark(sp->peerCertIssuer);
        mprMark(sp->service);
        mprMark(sp->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        if (sp->fd != INVALID_SOCKET) {
            if (sp->handler) {
                mprRemoveWaitHandler(sp->handler);
            }
            closesocket(sp->fd);
            if (sp->flags & MPR_SOCKET_SERVER) {
                mprAtomicAdd(&sp->service->numAccept, -1);
            }
        }
    }
}


/*
    Re-initialize all socket variables so the socket can be reused. This closes the socket and removes all wait handlers.
 */
static void resetSocket(MprSocket *sp)
{
    if (sp->fd != INVALID_SOCKET) {
        mprCloseSocket(sp, 0);
    }
    if (sp->flags & MPR_SOCKET_CLOSED) {
        sp->flags = 0;
        sp->port = -1;
        sp->fd = INVALID_SOCKET;
        sp->ip = 0;
    }
    assert(sp->provider);
}


PUBLIC bool mprHasDualNetworkStack() 
{
    bool dual;

#if defined(ME_COMPILER_HAS_SINGLE_STACK) || VXWORKS
    dual = 0;
#else
    dual = MPR->socketService->hasIPv6;
#endif
    return dual;
}


PUBLIC bool mprHasIPv6() 
{
    return MPR->socketService->hasIPv6;
}


/*
    Open a server connection
 */
PUBLIC Socket mprListenOnSocket(MprSocket *sp, cchar *ip, int port, int flags)
{
    struct sockaddr     *addr;
    Socklen             addrlen;
    cchar               *sip;
    int                 datagram, family, protocol, rc, only;

    lock(sp);
    resetSocket(sp);

    sp->ip = sclone(ip);
    sp->fd = INVALID_SOCKET;
    sp->port = port;
    sp->flags = (flags & (MPR_SOCKET_BROADCAST | MPR_SOCKET_DATAGRAM | MPR_SOCKET_BLOCK |
         MPR_SOCKET_NOREUSE | MPR_SOCKET_NODELAY | MPR_SOCKET_THREAD));
    datagram = sp->flags & MPR_SOCKET_DATAGRAM;

    /*
        Change null IP address to be an IPv6 endpoint if the system is dual-stack. That way we can listen on 
        both IPv4 and IPv6
     */
    sip = ((ip == 0 || *ip == '\0') && mprHasDualNetworkStack()) ? "::" : ip;

    if (mprGetSocketInfo(sip, port, &family, &protocol, &addr, &addrlen) < 0) {
        unlock(sp);
        return SOCKET_ERROR;
    }
    if ((sp->fd = (int) socket(family, datagram ? SOCK_DGRAM: SOCK_STREAM, protocol)) == SOCKET_ERROR) {
        unlock(sp);
        assert(sp->fd == INVALID_SOCKET);
        return SOCKET_ERROR;
    }

#if !ME_WIN_LIKE && !VXWORKS
    /*
        Children won't inherit this fd
     */
    fcntl(sp->fd, F_SETFD, FD_CLOEXEC);
#endif

    if (!(sp->flags & MPR_SOCKET_NOREUSE)) {
        rc = 1;
#if ME_UNIX_LIKE || VXWORKS
        setsockopt(sp->fd, SOL_SOCKET, SO_REUSEADDR, (char*) &rc, sizeof(rc));
#elif ME_WIN_LIKE && defined(SO_EXCLUSIVEADDRUSE)
        setsockopt(sp->fd, SOL_SOCKET, SO_REUSEADDR | SO_EXCLUSIVEADDRUSE, (char*) &rc, sizeof(rc));
#endif
    }
    /*
        By default, most stacks listen on both IPv6 and IPv4 if ip == 0, except windows which inverts this.
        So we explicitly control.
     */
#if defined(IPV6_V6ONLY)
    if (MPR->socketService->hasIPv6) {
        if (ip == 0 || *ip == '\0') {
            only = 0;
            setsockopt(sp->fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*) &only, sizeof(only));
        } else if (ipv6(ip)) {
            only = 1;
            setsockopt(sp->fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*) &only, sizeof(only));
        }
    }
#endif
    if (sp->service->prebind) {
        if ((sp->service->prebind)(sp) < 0) {
            closesocket(sp->fd);
            sp->fd = INVALID_SOCKET;
            unlock(sp);
            return SOCKET_ERROR;
        }
    }
    if ((rc = bind(sp->fd, addr, addrlen)) < 0) {
        if (errno == EADDRINUSE) {
            mprLog("error mpr socket", 3, "Cannot bind, address %s:%d already in use", ip, port);
        } else {
            mprLog("error mpr socket", 3, "Cannot bind, address %s:%d errno %d", ip, port, errno);
        }
        rc = mprGetOsError();
        closesocket(sp->fd);
        mprSetOsError(rc);
        sp->fd = INVALID_SOCKET;
        unlock(sp);
        return SOCKET_ERROR;
    }

    /* NOTE: Datagrams have not been used in a long while. Maybe broken */
    if (!datagram) {
        sp->flags |= MPR_SOCKET_LISTENER;
        if (listen(sp->fd, SOMAXCONN) < 0) {
            mprLog("error mpr socket", 3, "Listen error %d", mprGetOsError());
            closesocket(sp->fd);
            sp->fd = INVALID_SOCKET;
            unlock(sp);
            return SOCKET_ERROR;
        }
    }

#if ME_WIN_LIKE
    /*
        Delay setting reuse until now so that we can be assured that we have exclusive use of the port.
     */
    if (!(sp->flags & MPR_SOCKET_NOREUSE)) {
        int rc = 1;
        setsockopt(sp->fd, SOL_SOCKET, SO_REUSEADDR, (char*) &rc, sizeof(rc));
    }
#endif
    mprSetSocketBlockingMode(sp, (bool) (sp->flags & MPR_SOCKET_BLOCK));

    /*
        TCP/IP stacks have the No delay option (nagle algorithm) on by default.
     */
    if (sp->flags & MPR_SOCKET_NODELAY) {
        mprSetSocketNoDelay(sp, 1);
    }
    unlock(sp);
    return sp->fd;
}


PUBLIC MprWaitHandler *mprAddSocketHandler(MprSocket *sp, int mask, MprDispatcher *dispatcher, void *proc, 
    void *data, int flags)
{
    assert(sp);
    assert(sp->fd != INVALID_SOCKET);
    assert(proc);

    if (sp->fd == INVALID_SOCKET) {
        return 0;
    }
    if (sp->handler) {
        mprDestroyWaitHandler(sp->handler);
    }
    if (sp->flags & MPR_SOCKET_BUFFERED_READ) {
        mask |= MPR_READABLE;
    }
    if (sp->flags & MPR_SOCKET_BUFFERED_WRITE) {
        mask |= MPR_WRITABLE;
    }
    sp->handler = mprCreateWaitHandler((int) sp->fd, mask, dispatcher, proc, data, flags);
    return sp->handler;
}


PUBLIC void mprRemoveSocketHandler(MprSocket *sp)
{
    if (sp && sp->handler) {
        mprDestroyWaitHandler(sp->handler);
        sp->handler = 0;
    }
}


PUBLIC void mprSetSocketDispatcher(MprSocket *sp, MprDispatcher *dispatcher)
{
    if (sp && sp->handler) {
        sp->handler->dispatcher = dispatcher;
    }
}


PUBLIC void mprHiddenSocketData(MprSocket *sp, ssize len, int dir)
{
    lock(sp);
    if (len > 0) {
        sp->flags |= (dir == MPR_READABLE) ? MPR_SOCKET_BUFFERED_READ : MPR_SOCKET_BUFFERED_WRITE;
        if (sp->handler) {
            mprRecallWaitHandler(sp->handler);
        }
    } else {
        sp->flags &= ~((dir == MPR_READABLE) ? MPR_SOCKET_BUFFERED_READ : MPR_SOCKET_BUFFERED_WRITE);
    }
    unlock(sp);
}


//  FUTURE rename to mprWaitOnSocket

PUBLIC void mprEnableSocketEvents(MprSocket *sp, int mask)
{
    assert(sp->handler);
    if (sp->handler) {
        if (sp->flags & MPR_SOCKET_BUFFERED_READ) {
            mask |= MPR_READABLE;
        }
        if (sp->flags & MPR_SOCKET_BUFFERED_WRITE) {
            mask |= MPR_WRITABLE;
        }
        if (sp->flags & (MPR_SOCKET_BUFFERED_READ | MPR_SOCKET_BUFFERED_WRITE)) {
            if (sp->handler) {
                mprRecallWaitHandler(sp->handler);
            }
        }
        mprWaitOn(sp->handler, mask);
    }
}


/*
    Open a client socket connection
 */
PUBLIC int mprConnectSocket(MprSocket *sp, cchar *ip, int port, int flags)
{
    if (sp->provider == 0) {
        return MPR_ERR_NOT_INITIALIZED;
    }
    return connectSocket(sp, ip, port, flags);
}


static int connectSocket(MprSocket *sp, cchar *ip, int port, int initialFlags)
{
    struct sockaddr     *addr;
    Socklen             addrlen;
    int                 broadcast, datagram, family, protocol, rc;

    lock(sp);
    resetSocket(sp);

    sp->port = port;
    sp->flags = (initialFlags &
        (MPR_SOCKET_BROADCAST | MPR_SOCKET_DATAGRAM | MPR_SOCKET_BLOCK |
         MPR_SOCKET_LISTENER | MPR_SOCKET_NOREUSE | MPR_SOCKET_NODELAY | MPR_SOCKET_THREAD));
    sp->ip = sclone(ip);

    broadcast = sp->flags & MPR_SOCKET_BROADCAST;
    if (broadcast) {
        sp->flags |= MPR_SOCKET_DATAGRAM;
    }
    datagram = sp->flags & MPR_SOCKET_DATAGRAM;

    if (mprGetSocketInfo(ip, port, &family, &protocol, &addr, &addrlen) < 0) {
        closesocket(sp->fd);
        sp->fd = INVALID_SOCKET;
        unlock(sp);
        return MPR_ERR_CANT_ACCESS;
    }
    if ((sp->fd = (int) socket(family, datagram ? SOCK_DGRAM: SOCK_STREAM, protocol)) < 0) {
        unlock(sp);
        return MPR_ERR_CANT_OPEN;
    }
#if !ME_WIN_LIKE && !VXWORKS
    /*
        Children should not inherit this fd
     */
    fcntl(sp->fd, F_SETFD, FD_CLOEXEC);
#endif
    if (broadcast) {
        int flag = 1;
        if (setsockopt(sp->fd, SOL_SOCKET, SO_BROADCAST, (char *) &flag, sizeof(flag)) < 0) {
            closesocket(sp->fd);
            sp->fd = INVALID_SOCKET;
            unlock(sp);
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    if (!datagram) {
        sp->flags |= MPR_SOCKET_CONNECTING;
        do {
            rc = connect(sp->fd, addr, addrlen);
        } while (rc == -1 && errno == EINTR);
        if (rc < 0) {
            /* MAC/BSD returns EADDRINUSE */
            if (errno == EINPROGRESS || errno == EALREADY || errno == EADDRINUSE) {
#if ME_UNIX_LIKE
                do {
                    struct pollfd pfd;
                    pfd.fd = sp->fd;
                    pfd.events = POLLOUT;
                    rc = poll(&pfd, 1, 1000);
                } while (rc < 0 && errno == EINTR);
#endif
                if (rc > 0) {
                    errno = EISCONN;
                }
            } 
            if (errno != EISCONN) {
                closesocket(sp->fd);
                sp->fd = INVALID_SOCKET;
                unlock(sp);
                return MPR_ERR_CANT_COMPLETE;
            }
        }
    }
    mprSetSocketBlockingMode(sp, (bool) (sp->flags & MPR_SOCKET_BLOCK));

    /*
        TCP/IP stacks have the no delay option (nagle algorithm) on by default.
     */
    if (sp->flags & MPR_SOCKET_NODELAY) {
        mprSetSocketNoDelay(sp, 1);
    }
    unlock(sp);
    return 0;
}


/*
    Abortive disconnect. Thread-safe. (e.g. from a timeout or callback thread). This closes the underlying socket file
    descriptor but keeps the handler and socket object intact. It also forces a recall on the wait handler.
 */
PUBLIC void mprDisconnectSocket(MprSocket *sp)
{
    if (sp && sp->provider) {
        sp->provider->disconnectSocket(sp);
    }
}


static void disconnectSocket(MprSocket *sp)
{
    char    buf[ME_MAX_BUFFER];
    int     i;

    /*
        Defensive lock buster. Use try lock incase an operation is blocked somewhere with a lock asserted. 
        Should never happen.
     */
    if (!mprTryLock(sp->mutex)) {
        return;
    }
    if (!(sp->flags & MPR_SOCKET_EOF)) {
        /*
            Read a reasonable amount of outstanding data to minimize resets. Then do a shutdown to send a FIN and read 
            outstanding data.  All non-blocking.
         */
        mprSetSocketBlockingMode(sp, 0);
        for (i = 0; i < 16; i++) {
            if (recv(sp->fd, buf, sizeof(buf), 0) <= 0) {
                break;
            }
        }
        shutdown(sp->fd, SHUT_RDWR);
        for (i = 0; i < 16; i++) {
            if (recv(sp->fd, buf, sizeof(buf), 0) <= 0) {
                break;
            }
        }
    }
    if (sp->fd == INVALID_SOCKET || !(sp->flags & MPR_SOCKET_EOF)) {
        sp->flags |= MPR_SOCKET_EOF | MPR_SOCKET_DISCONNECTED;
        if (sp->handler) {
            mprRecallWaitHandler(sp->handler);
        }
    }
    unlock(sp);
}


PUBLIC void mprCloseSocket(MprSocket *sp, bool gracefully)
{
    if (sp == 0 || sp->provider == 0) {
        return;
    }
    mprRemoveSocketHandler(sp);
    sp->provider->closeSocket(sp, gracefully);
}


/*
    Standard (non-SSL) close. Permit multiple calls.
 */
static void closeSocket(MprSocket *sp, bool gracefully)
{
    MprSocketService    *ss;
    MprTime             timesUp;
    char                buf[16];

    ss = MPR->socketService;

    lock(sp);
    if (sp->flags & MPR_SOCKET_CLOSED) {
        unlock(sp);
        return;
    }
    sp->flags |= MPR_SOCKET_CLOSED | MPR_SOCKET_EOF;

    if (sp->fd != INVALID_SOCKET) {
        /*
            Read any outstanding read data to minimize resets. Then do a shutdown to send a FIN and read outstanding 
            data. All non-blocking.
         */
        if (gracefully) {
            mprSetSocketBlockingMode(sp, 0);
            while (recv(sp->fd, buf, sizeof(buf), 0) > 0) { }
        }
        if (shutdown(sp->fd, SHUT_RDWR) == 0) {
            if (gracefully) {
                timesUp = mprGetTime() + MPR_TIMEOUT_LINGER;
                do {
                    if (recv(sp->fd, buf, sizeof(buf), 0) <= 0) {
                        break;
                    }
                } while (mprGetTime() < timesUp);
            }
        }
        closesocket(sp->fd);
        sp->fd = INVALID_SOCKET;
    }
    if (sp->flags & MPR_SOCKET_SERVER) {
        mprAtomicAdd(&ss->numAccept, -1);
    }
    unlock(sp);
}


PUBLIC MprSocket *mprAcceptSocket(MprSocket *listen)
{
    MprSocketService            *ss;
    MprSocket                   *nsp;
    struct sockaddr_storage     addrStorage, saddrStorage;
    struct sockaddr             *addr, *saddr;
    char                        ip[ME_MAX_IP], acceptIp[ME_MAX_IP];
    Socklen                     addrlen, saddrlen;
    Socket                      fd;
    int                         port, acceptPort;

    ss = MPR->socketService;
    addr = (struct sockaddr*) &addrStorage;
    addrlen = sizeof(addrStorage);

    if (listen->flags & MPR_SOCKET_BLOCK) {
        mprYield(MPR_YIELD_STICKY);
    }
    fd = accept(listen->fd, addr, &addrlen);
    if (listen->flags & MPR_SOCKET_BLOCK) {
        mprResetYield();
    }
    if (fd == SOCKET_ERROR) {
        if (mprGetError() != EAGAIN) {
            mprDebug("mpr socket", 5, "Accept failed, errno %d", mprGetOsError());
        }
        return 0;
    }
    if ((nsp = mprCreateSocket()) == 0) {
        closesocket(fd);
        return 0;
    }
    nsp->fd = fd;
    nsp->listenSock = listen;
    nsp->port = listen->port;
    nsp->flags = ((listen->flags & ~MPR_SOCKET_LISTENER) | MPR_SOCKET_SERVER);

    /*
        Limit the number of simultaneous clients
     */
    lock(ss);
    if (++ss->numAccept >= ss->maxAccept) {
        unlock(ss);
        mprLog("error mpr socket", 2, "Rejecting connection, too many client connections (%d)", ss->numAccept);
        mprCloseSocket(nsp, 0);
        return 0;
    }
    unlock(ss);

#if !ME_WIN_LIKE && !VXWORKS
    /* Prevent children inheriting this socket */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

    mprSetSocketBlockingMode(nsp, (nsp->flags & MPR_SOCKET_BLOCK) ? 1: 0);
    if (nsp->flags & MPR_SOCKET_NODELAY) {
        mprSetSocketNoDelay(nsp, 1);
    }
    /*
        Get the remote client address
     */
    if (getSocketIpAddr(addr, addrlen, ip, sizeof(ip), &port) != 0) {
        assert(0);
        mprCloseSocket(nsp, 0);
        return 0;
    }
    nsp->ip = sclone(ip);
    nsp->port = port;

    /*
        Get the server interface address accepting the connection
     */
    saddr = (struct sockaddr*) &saddrStorage;
    saddrlen = sizeof(saddrStorage);
    getsockname(fd, saddr, &saddrlen);
    acceptPort = 0;
    getSocketIpAddr(saddr, saddrlen, acceptIp, sizeof(acceptIp), &acceptPort);
    nsp->acceptIp = sclone(acceptIp);
    nsp->acceptPort = acceptPort;
    return nsp;
}


/*
    Read data. Return -1 for EOF and errors. On success, return the number of bytes read.
 */
PUBLIC ssize mprReadSocket(MprSocket *sp, void *buf, ssize bufsize)
{
    assert(sp);
    assert(buf);
    assert(bufsize > 0);
    assert(sp->provider);

    if (sp->provider == 0) {
        return MPR_ERR_NOT_INITIALIZED;
    }
    return sp->provider->readSocket(sp, buf, bufsize);
}


/*
    Standard read from a socket (Non SSL)
    Return number of bytes read. Return -1 on errors and EOF.
 */
static ssize readSocket(MprSocket *sp, void *buf, ssize bufsize)
{
    struct sockaddr_storage server;
    Socklen                 len;
    ssize                   bytes;
    int                     errCode;

    assert(buf);
    assert(bufsize > 0);
    assert(~(sp->flags & MPR_SOCKET_CLOSED));

    lock(sp);
    if (sp->flags & MPR_SOCKET_EOF) {
        unlock(sp);
        return -1;
    }
again:
    if (sp->flags & MPR_SOCKET_BLOCK) {
        mprYield(MPR_YIELD_STICKY);
    }
    if (sp->flags & MPR_SOCKET_DATAGRAM) {
        len = sizeof(server);
        bytes = recvfrom(sp->fd, buf, (int) bufsize, MSG_NOSIGNAL, (struct sockaddr*) &server, (Socklen*) &len);
    } else {
        bytes = recv(sp->fd, buf, (int) bufsize, MSG_NOSIGNAL);
    }
    if (sp->flags & MPR_SOCKET_BLOCK) {
        mprResetYield();
    }
    if (bytes < 0) {
        errCode = mprGetSocketError(sp);
        if (errCode == EINTR) {
            goto again;

        } else if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
            bytes = 0;                          /* No data available */

        } else if (errCode == ECONNRESET) {
            sp->flags |= MPR_SOCKET_EOF;        /* Disorderly disconnect */
            bytes = -1;

        } else {
            sp->flags |= MPR_SOCKET_EOF;        /* Some other error */
            bytes = -errCode;
        }

    } else if (bytes == 0) {                    /* EOF */
        sp->flags |= MPR_SOCKET_EOF;
        bytes = -1;
    }
    unlock(sp);
    return bytes;
}


/*
    Write data. Return the number of bytes written or -1 on errors. NOTE: this routine will return with a
    short write if the underlying socket cannot accept any more data.
 */
PUBLIC ssize mprWriteSocket(MprSocket *sp, cvoid *buf, ssize bufsize)
{
    assert(sp);
    assert(buf);
    assert(bufsize > 0);
    assert(sp->provider);

    if (sp->provider == 0) {
        return MPR_ERR_NOT_INITIALIZED;
    }
    return sp->provider->writeSocket(sp, buf, bufsize);
}


/*
    Standard write to a socket (Non SSL)
    Return count of bytes written. mprGetError will return EAGAIN or EWOULDBLOCK if transport is saturated.
 */
static ssize writeSocket(MprSocket *sp, cvoid *buf, ssize bufsize)
{
    struct sockaddr     *addr;
    Socklen             addrlen;
    ssize               len, written, sofar;
    int                 family, protocol, errCode;

    assert(buf);
    assert(bufsize >= 0);
    assert((sp->flags & MPR_SOCKET_CLOSED) == 0);

    lock(sp);
    if (sp->flags & (MPR_SOCKET_BROADCAST | MPR_SOCKET_DATAGRAM)) {
        if (mprGetSocketInfo(sp->ip, sp->port, &family, &protocol, &addr, &addrlen) < 0) {
            unlock(sp);
            return MPR_ERR_CANT_FIND;
        }
    }
    if (sp->flags & MPR_SOCKET_EOF) {
        sofar = MPR_ERR_CANT_WRITE;
    } else {
        errCode = 0;
        len = bufsize;
        sofar = 0;
        while (len > 0) {
            unlock(sp);
            if (sp->flags & MPR_SOCKET_BLOCK) {
                mprYield(MPR_YIELD_STICKY);
            }
            if ((sp->flags & MPR_SOCKET_BROADCAST) || (sp->flags & MPR_SOCKET_DATAGRAM)) {
                written = sendto(sp->fd, &((char*) buf)[sofar], (int) len, MSG_NOSIGNAL, addr, addrlen);
            } else {
                written = send(sp->fd, &((char*) buf)[sofar], (int) len, MSG_NOSIGNAL);
            }
            /* Get the error code before calling mprResetYield to avoid clearing global error numbers */
            errCode = mprGetSocketError(sp);
            if (sp->flags & MPR_SOCKET_BLOCK) {
                mprResetYield();
            }
            lock(sp);
            if (written < 0) {
                assert(errCode != 0);
                if (errCode == EINTR) {
                    continue;
                } else if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
#if ME_WIN_LIKE
                    /*
                        Windows sockets don't support blocking I/O. So we simulate here
                        OPT - could wait for a writable event
                     */
                    if (sp->flags & MPR_SOCKET_BLOCK) {
                        mprNap(0);
                        continue;
                    }
#endif
                    unlock(sp);
                    if (sofar) {
                        return sofar;
                    }
                    return -errCode;
                }
                unlock(sp);
                return -errCode;
            }
            len -= written;
            sofar += written;
        }
    }
    unlock(sp);
    return sofar;
}


/*
    Write a string to the socket
 */
PUBLIC ssize mprWriteSocketString(MprSocket *sp, cchar *str)
{
    return mprWriteSocket(sp, str, slen(str));
}


PUBLIC ssize mprWriteSocketVector(MprSocket *sp, MprIOVec *iovec, int count)
{
    char        *start;
    ssize       total, len, written;
    int         i;

#if ME_UNIX_LIKE
    if (sp->sslSocket == 0) {
        return writev(sp->fd, (const struct iovec*) iovec, (int) count);
    } else
#endif
    {
        //  OPT - better to buffer and have fewer raw writes
        if (count <= 0) {
            return 0;
        }
        start = iovec[0].start;
        len = (int) iovec[0].len;
        assert(len > 0);

        for (total = i = 0; i < count; ) {
            written = mprWriteSocket(sp, start, len);
            if (written < 0) {
                if (total > 0) {
                    break;
                }
                return written;
            } else if (written == 0) {
                break;
            } else {
                len -= written;
                start += written;
                total += written;
                if (len <= 0) {
                    i++;
                    start = iovec[i].start;
                    len = (int) iovec[i].len;
                }
            }
        }
        return total;
    }
}


#if !ME_ROM
#if !LINUX || __UCLIBC__
static ssize localSendfile(MprSocket *sp, MprFile *file, MprOff offset, ssize len)
{
    char    buf[ME_MAX_BUFFER];

    mprSeekFile(file, SEEK_SET, (int) offset);
    len = min(len, sizeof(buf));
    if ((len = mprReadFile(file, buf, len)) < 0) {
        assert(0);
        return MPR_ERR_CANT_READ;
    }
    return mprWriteSocket(sp, buf, len);
}
#endif


/*
    Write data from a file to a socket. Includes the ability to write header before and after the file data.
    Works even with a null "file" to just output the headers.
 */
PUBLIC MprOff mprSendFileToSocket(MprSocket *sock, MprFile *file, MprOff offset, MprOff bytes, MprIOVec *beforeVec, 
    int beforeCount, MprIOVec *afterVec, int afterCount)
{
#if MACOSX && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1050
    struct sf_hdtr  def;
#endif
    MprOff          written, toWriteFile;
    ssize           i, rc, toWriteBefore, toWriteAfter, nbytes;
    int             done;

    rc = 0;

#if MACOSX && __MAC_OS_X_VERSION_MIN_REQUIRED >= 1050
    def.hdr_cnt = (int) beforeCount;
    def.headers = (beforeCount > 0) ? (struct iovec*) beforeVec: 0;
    def.trl_cnt = (int) afterCount;
    def.trailers = (afterCount > 0) ? (struct iovec*) afterVec: 0;

    if (file && file->fd >= 0) {
        written = bytes;
        if (sock->flags & MPR_SOCKET_BLOCK) {
            mprYield(MPR_YIELD_STICKY);
        }
        rc = sendfile(file->fd, sock->fd, offset, &written, &def, 0);
        if (sock->flags & MPR_SOCKET_BLOCK) {
            mprResetYield();
        }
    } else
#else
    if (1) 
#endif
    {
        /* Either !MACOSX or no file */
        done = 0;
        written = 0;
        for (i = toWriteBefore = 0; i < beforeCount; i++) {
            toWriteBefore += beforeVec[i].len;
        }
        for (i = toWriteAfter = 0; i < afterCount; i++) {
            toWriteAfter += afterVec[i].len;
        }
        toWriteFile = (bytes - toWriteBefore - toWriteAfter);
        assert(toWriteFile >= 0);

        /*
            Linux sendfile does not have the integrated ability to send headers. Must do it separately here.
            I/O requests may return short (write fewer than requested bytes).
         */
        if (beforeCount > 0) {
            rc = mprWriteSocketVector(sock, beforeVec, beforeCount);
            if (rc > 0) {
                written += rc;
            }
            if (rc != toWriteBefore) {
                done++;
            }
        }

        if (!done && toWriteFile > 0 && file->fd >= 0) {
#if LINUX && !__UCLIBC__ && !ME_COMPILER_HAS_OFF64
            off_t off = (off_t) offset;
#endif
            while (!done && toWriteFile > 0) {
                nbytes = (ssize) min(MAXSSIZE, toWriteFile);
                if (sock->flags & MPR_SOCKET_BLOCK) {
                    mprYield(MPR_YIELD_STICKY);
                }
#if LINUX && !__UCLIBC__
    #if ME_COMPILER_HAS_OFF64
                rc = sendfile64(sock->fd, file->fd, &offset, nbytes);
    #else
                rc = sendfile(sock->fd, file->fd, &off, nbytes);
    #endif
#else
                rc = localSendfile(sock, file, offset, nbytes);
#endif
                if (sock->flags & MPR_SOCKET_BLOCK) {
                    mprResetYield();
                }
                if (rc > 0) {
                    written += rc;
                    toWriteFile -= rc;
                }
                if (rc != nbytes) {
                    done++;
                    break;
                }
            }
        }
        if (!done && afterCount > 0) {
            rc = mprWriteSocketVector(sock, afterVec, afterCount);
            if (rc > 0) {
                written += rc;
            }
        }
    }
    if (rc < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return written;
        }
        return -1;
    }
    return written;
}
#endif /* !ME_ROM */


static ssize flushSocket(MprSocket *sp)
{
    return 0;
}


PUBLIC ssize mprFlushSocket(MprSocket *sp)
{
    if (sp->provider == 0) {
        return MPR_ERR_NOT_INITIALIZED;
    }
    return sp->provider->flushSocket(sp);
}


static char *socketState(MprSocket *sp)
{
    return MPR->emptyString;
}


PUBLIC char *mprGetSocketState(MprSocket *sp)
{
    if (sp->provider == 0) {
        return 0;
    }
    return sp->provider->socketState(sp);
}


PUBLIC bool mprSocketHasBuffered(MprSocket *sp)
{
    return (sp->flags & (MPR_SOCKET_BUFFERED_READ | MPR_SOCKET_BUFFERED_WRITE)) ? 1 : 0;
}


PUBLIC bool mprSocketHasBufferedRead(MprSocket *sp)
{
    return (sp->flags & MPR_SOCKET_BUFFERED_READ) ? 1 : 0;
}


PUBLIC bool mprSocketHasBufferedWrite(MprSocket *sp)
{
    return (sp->flags & MPR_SOCKET_BUFFERED_WRITE) ? 1 : 0;
}


PUBLIC bool mprSocketHandshaking(MprSocket *sp)
{
    return (sp->flags & MPR_SOCKET_HANDSHAKING) ? 1 : 0;
}


/*
    Return true if end of file
 */
PUBLIC bool mprIsSocketEof(MprSocket *sp)
{
    return (!sp || ((sp->flags & MPR_SOCKET_EOF) != 0));
}


/*
    Set the EOF condition
 */
PUBLIC void mprSetSocketEof(MprSocket *sp, bool eof)
{
    if (eof) {
        sp->flags |= MPR_SOCKET_EOF;
    } else {
        sp->flags &= ~MPR_SOCKET_EOF;
    }
}


/*
    Return the O/S socket handle
 */
PUBLIC Socket mprGetSocketHandle(MprSocket *sp)
{
    return sp->fd;
}


PUBLIC Socket mprStealSocketHandle(MprSocket *sp)
{
    Socket  fd;

    if (!sp) {
        return INVALID_SOCKET;
    }
    fd = sp->fd;
    sp->fd = INVALID_SOCKET;
    return fd;
}


/*
    Return the blocking mode of the socket
 */
PUBLIC bool mprGetSocketBlockingMode(MprSocket *sp)
{
    assert(sp);
    return sp && (sp->flags & MPR_SOCKET_BLOCK);
}


/*
    Get the socket flags
 */
PUBLIC int mprGetSocketFlags(MprSocket *sp)
{
    return sp->flags;
}


/*
    Set whether the socket blocks or not on read/write
 */
PUBLIC int mprSetSocketBlockingMode(MprSocket *sp, bool on)
{
    int     oldMode;

    assert(sp);

    lock(sp);
    oldMode = sp->flags & MPR_SOCKET_BLOCK;

    sp->flags &= ~(MPR_SOCKET_BLOCK);
    if (on) {
        sp->flags |= MPR_SOCKET_BLOCK;
    }
#if ME_WIN_LIKE
{
    int flag = (sp->flags & MPR_SOCKET_BLOCK) ? 0 : 1;
    ioctlsocket(sp->fd, FIONBIO, (ulong*) &flag);
}
#elif VXWORKS
{
    int flag = (sp->flags & MPR_SOCKET_BLOCK) ? 0 : 1;
    ioctl(sp->fd, FIONBIO, (int) &flag);
}
#else
    if (on) {
        fcntl(sp->fd, F_SETFL, fcntl(sp->fd, F_GETFL) & ~O_NONBLOCK);
    } else {
        fcntl(sp->fd, F_SETFL, fcntl(sp->fd, F_GETFL) | O_NONBLOCK);
    }
#endif
    unlock(sp);
    return oldMode;
}


/*
    Set the TCP delay behavior (nagle algorithm)
 */
PUBLIC int mprSetSocketNoDelay(MprSocket *sp, bool on)
{
    int     oldDelay;

    lock(sp);
    oldDelay = sp->flags & MPR_SOCKET_NODELAY;
    if (on) {
        sp->flags |= MPR_SOCKET_NODELAY;
    } else {
        sp->flags &= ~(MPR_SOCKET_NODELAY);
    }
#if ME_WIN_LIKE
    {
        BOOL    noDelay;
        noDelay = on ? 1 : 0;
        setsockopt(sp->fd, IPPROTO_TCP, TCP_NODELAY, (FAR char*) &noDelay, sizeof(BOOL));
    }
#else
    {
        int     noDelay;
        noDelay = on ? 1 : 0;
        setsockopt(sp->fd, IPPROTO_TCP, TCP_NODELAY, (char*) &noDelay, sizeof(int));
    }
#endif /* ME_WIN_LIKE */
    unlock(sp);
    return oldDelay;
}


/*
    Get the port number
 */
PUBLIC int mprGetSocketPort(MprSocket *sp)
{
    return sp->port;
}


/*
    Map the O/S error code to portable error codes.
 */
PUBLIC int mprGetSocketError(MprSocket *sp)
{
#if ME_WIN_LIKE
    int     rc;
    switch (rc = WSAGetLastError()) {
    case WSAEINTR:
        return EINTR;

    case WSAENETDOWN:
        return ENETDOWN;

    case WSAEWOULDBLOCK:
        return EWOULDBLOCK;

    case WSAEPROCLIM:
        return EAGAIN;

    case WSAECONNRESET:
    case WSAECONNABORTED:
        return ECONNRESET;

    case WSAECONNREFUSED:
        return ECONNREFUSED;

    case WSAEADDRINUSE:
        return EADDRINUSE;
    default:
        return EINVAL;
    }
#else
    return errno;
#endif
}


#if ME_COMPILER_HAS_GETADDRINFO
/*
    Get a socket address from a host/port combination. If a host provides both IPv4 and IPv6 addresses, 
    prefer the IPv4 address.
 */
PUBLIC int mprGetSocketInfo(cchar *ip, int port, int *family, int *protocol, struct sockaddr **addr, socklen_t *addrlen)
{
    MprSocketService    *ss;
    struct addrinfo     hints, *res, *r;
    char                *portStr;
    int                 v6;

    assert(addr);
    ss = MPR->socketService;

    lock(ss);
    memset((char*) &hints, '\0', sizeof(hints));

    /*
        Note that IPv6 does not support broadcast, there is no 255.255.255.255 equivalent.
        Multicast can be used over a specific link, but the user must provide that address plus %scope_id.
     */
    if (ip == 0 || ip[0] == '\0') {
        ip = 0;
        hints.ai_flags |= AI_PASSIVE;           /* Bind to 0.0.0.0 and :: if available */
    }
    v6 = ipv6(ip);
    hints.ai_socktype = SOCK_STREAM;
    if (ip) {
        hints.ai_family = v6 ? AF_INET6 : AF_INET;
    } else {
        hints.ai_family = AF_UNSPEC;
    }
    portStr = itos(port);

    /*
        Try to sleuth the address to avoid duplicate address lookups. Then try IPv4 first then IPv6.
     */
    res = 0;
    if (getaddrinfo(ip, portStr, &hints, &res) != 0) {
        unlock(ss);
        return MPR_ERR_CANT_OPEN;
    }
    /*
        Prefer IPv4 if IPv6 not requested
     */
    for (r = res; r; r = r->ai_next) {
        if (v6) {
            if (r->ai_family == AF_INET6) {
                break;
            }
        } else {
            if (r->ai_family == AF_INET) {
                break;
            }
        }
    }
    if (r == NULL) {
        r = res;
    }
    *addr = mprAlloc(sizeof(struct sockaddr_storage));
    mprMemcpy((char*) *addr, sizeof(struct sockaddr_storage), (char*) r->ai_addr, (int) r->ai_addrlen);

    *addrlen = (int) r->ai_addrlen;
    *family = r->ai_family;
    *protocol = r->ai_protocol;

    freeaddrinfo(res);
    unlock(ss);
    return 0;
}
#else

PUBLIC int mprGetSocketInfo(cchar *ip, int port, int *family, int *protocol, struct sockaddr **addr, Socklen *addrlen)
{
    MprSocketService    *ss;
    struct sockaddr_in  *sa;

    ss = MPR->socketService;

    if ((sa = mprAllocStruct(struct sockaddr_in)) == 0) {
        assert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    memset((char*) sa, '\0', sizeof(struct sockaddr_in));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((short) (port & 0xFFFF));

    if (strcmp(ip, "") != 0) {
        sa->sin_addr.s_addr = inet_addr((char*) ip);
    } else {
        sa->sin_addr.s_addr = INADDR_ANY;
    }

    /*
        gethostbyname is not thread safe on some systems
     */
    lock(ss);
    if (sa->sin_addr.s_addr == INADDR_NONE) {
#if VXWORKS
        /*
            VxWorks only supports one interface and this code only supports IPv4
         */
        sa->sin_addr.s_addr = (ulong) hostGetByName((char*) ip);
        if (sa->sin_addr.s_addr < 0) {
            unlock(ss);
            assert(0);
            return 0;
        }
#else
        struct hostent *hostent;
        hostent = gethostbyname2(ip, AF_INET);
        if (hostent == 0) {
            hostent = gethostbyname2(ip, AF_INET6);
            if (hostent == 0) {
                unlock(ss);
                return MPR_ERR_CANT_FIND;
            }
        }
        memcpy((char*) &sa->sin_addr, (char*) hostent->h_addr_list[0], (ssize) hostent->h_length);
#endif
    }
    *addr = (struct sockaddr*) sa;
    *addrlen = sizeof(struct sockaddr_in);
    *family = sa->sin_family;
    *protocol = 0;
    unlock(ss);
    return 0;
}
#endif


/*
    Return a numerical IP address and port for the given socket info
 */
static int getSocketIpAddr(struct sockaddr *addr, int addrlen, char *ip, int ipLen, int *port)
{
#if (ME_UNIX_LIKE || WIN)
    char    service[NI_MAXSERV];

#ifdef IN6_IS_ADDR_V4MAPPED
    if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*) addr;
        if (IN6_IS_ADDR_V4MAPPED(&addr6->sin6_addr)) {
            struct sockaddr_in addr4;
            memset(&addr4, 0, sizeof(addr4));
            addr4.sin_family = AF_INET;
            addr4.sin_port = addr6->sin6_port;
            memcpy(&addr4.sin_addr.s_addr, addr6->sin6_addr.s6_addr + 12, sizeof(addr4.sin_addr.s_addr));
            memcpy(addr, &addr4, sizeof(addr4));
            addrlen = sizeof(addr4);
        }
    }
#endif
    if (getnameinfo(addr, addrlen, ip, ipLen, service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV | NI_NOFQDN)) {
        return MPR_ERR_BAD_VALUE;
    }
    *port = atoi(service);

#else
    struct sockaddr_in  *sa;

#if HAVE_NTOA_R
    sa = (struct sockaddr_in*) addr;
    inet_ntoa_r(sa->sin_addr, ip, ipLen);
#else
    uchar   *cp;
    sa = (struct sockaddr_in*) addr;
    cp = (uchar*) &sa->sin_addr;
    fmt(ip, ipLen, "%d.%d.%d.%d", cp[0], cp[1], cp[2], cp[3]);
#endif
    *port = ntohs(sa->sin_port);
#endif
    return 0;
}


/*
    Looks like an IPv6 address if it has 2 or more colons
 */
static int ipv6(cchar *ip)
{
    cchar   *cp;
    int     colons;

    if (ip == 0 || *ip == 0) {
        /*
            Listening on just a bare port means IPv4 only.
         */
        return 0;
    }
    colons = 0;
    for (cp = (char*) ip; ((*cp != '\0') && (colons < 2)) ; cp++) {
        if (*cp == ':') {
            colons++;
        }
    }
    return colons >= 2;
}


/*
    Parse address and return the IP address and port components. Handles ipv4 and ipv6 addresses. 
    If the IP portion is absent, *pip is set to null. If the port portion is absent, port is set to the defaultPort.
    If a ":*" port specifier is used, *pport is set to -1;
    When an address contains an ipv6 port it should be written as:

        aaaa:bbbb:cccc:dddd:eeee:ffff:gggg:hhhh:iiii
    or
        [aaaa:bbbb:cccc:dddd:eeee:ffff:gggg:hhhh:iiii]:port

    If supplied an IPv6 address, the backets are stripped in the returned IP address.
    This routine parses any "https://" prefix.
 */
PUBLIC int mprParseSocketAddress(cchar *address, char **pip, int *pport, int *psecure, int defaultPort)
{
    char    *ip, *cp;
    int     port;

    ip = 0;
    if (defaultPort < 0) {
        defaultPort = 80;
    }
    if (psecure) {
        *psecure = sncmp(address, "https", 5) == 0;
    }
    ip = sclone(address);
    if ((cp = strchr(ip, ' ')) != 0) {
        *cp++ = '\0';
    }
    if ((cp = strstr(ip, "://")) != 0) {
        ip = sclone(&cp[3]);
    }
    if (ipv6(ip)) {
        /*
            IPv6. If port is present, it will follow a closing bracket ']'
         */
        if ((cp = strchr(ip, ']')) != 0) {
            cp++;
            if ((*cp) && (*cp == ':')) {
                port = (*++cp == '*') ? -1 : atoi(cp);

                /* Set ipAddr to ipv6 address without brackets */
                ip = sclone(ip + 1);
                cp = strchr(ip, ']');
                *cp = '\0';

            } else {
                /* Handles [a:b:c:d:e:f:g:h:i] case (no port)- should not occur */
                ip = sclone(ip + 1);
                if ((cp = strchr(ip, ']')) != 0) {
                    *cp = '\0';
                }
                if (*ip == '\0') {
                    ip = 0;
                }
                /* No port present, use callers default */
                port = defaultPort;
            }
        } else {
            /* Handles a:b:c:d:e:f:g:h:i case (no port) */
            /* No port present, use callers default */
            port = defaultPort;
        }

    } else {
        /*
            ipv4 
         */
        if ((cp = strchr(ip, ':')) != 0) {
            *cp++ = '\0';
            if (*cp == '*') {
                port = -1;
            } else {
                port = atoi(cp);
            }
            if (*ip == '*') {
                ip = 0;
            }

        } else if (strchr(ip, '.')) {
            if ((cp = strchr(ip, ' ')) != 0) {
                *cp++ = '\0';
            }
            port = defaultPort;

        } else {
            if (isdigit((uchar) *ip)) {
                port = atoi(ip);
                ip = 0;
            } else {
                /* No port present, use callers default */
                port = defaultPort;
            }
        }
    }
    if (pport) {
        *pport = port;
    }
    if (pip) {
        *pip = ip;
    }
    return 0;
}


PUBLIC bool mprIsSocketSecure(MprSocket *sp)
{
    return sp->sslSocket != 0;
}


PUBLIC bool mprIsSocketV6(MprSocket *sp)
{
    return sp->ip && ipv6(sp->ip);
}


PUBLIC bool mprIsIPv6(cchar *ip)
{
    return ip && ipv6(ip);
}


PUBLIC void mprSetSocketPrebindCallback(MprSocketPrebind callback)
{
    MPR->socketService->prebind = callback;
}


static void manageSsl(MprSsl *ssl, int flags) 
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ssl->providerName);
        mprMark(ssl->provider);
        mprMark(ssl->key);
        mprMark(ssl->keyFile);
        mprMark(ssl->certFile);
        mprMark(ssl->caFile);
        mprMark(ssl->caPath);
        mprMark(ssl->ciphers);
        mprMark(ssl->config);
        mprMark(ssl->mutex);
    }
}


/*
    Create a new SSL context object
 */
PUBLIC MprSsl *mprCreateSsl(int server)
{
    MprSsl      *ssl;
    cchar       *path;

    if ((ssl = mprAllocObj(MprSsl, manageSsl)) == 0) {
        return 0;
    }
    ssl->protocols = MPR_PROTO_TLSV1_1 | MPR_PROTO_TLSV1_2;

    /*
        The default for servers is not to verify client certificates.
        The default for clients is to verify unless MPR->verifySsl has been set to false
     */
    if (server) {
        ssl->verifyDepth = 10;
        ssl->verifyPeer = 0;
        ssl->verifyIssuer = 0;
    } else {
        ssl->verifyDepth = 10;
        if (MPR->verifySsl) {
            ssl->verifyPeer = MPR->verifySsl;
            ssl->verifyIssuer = MPR->verifySsl;
            path = mprJoinPath(mprGetAppDir(), MPR_CA_CERT);
            if (mprPathExists(path, R_OK)) {
                ssl->caFile = path;
            }
        }
    }
    ssl->mutex = mprCreateLock();
    return ssl;
}


/*
    Clone a SSL context object
 */
PUBLIC MprSsl *mprCloneSsl(MprSsl *src)
{
    MprSsl      *ssl;

    if ((ssl = mprAllocObj(MprSsl, manageSsl)) == 0) {
        return 0;
    }
    if (src) {
        *ssl = *src;
    }
    return ssl;
}


PUBLIC int mprLoadSsl()
{
#if ME_COM_SSL
    MprSocketService    *ss;
    MprModule           *mp;
    cchar               *path;

    ss = MPR->socketService;
    if (ss->providers) {
        return 0;
    }
    path = mprJoinPath(mprGetAppDir(), "libmprssl");
    if (!mprPathExists(path, R_OK)) {
        path = mprSearchForModule("libmprssl");
    }
    if (!path) {
        return MPR_ERR_CANT_FIND;
    }
    if ((mp = mprCreateModule("sslModule", path, "mprSslInit", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    if (mprLoadModule(mp) < 0) {
        mprLog("error mpr", 0, "Cannot load %s", path);
        return MPR_ERR_CANT_READ;
    }
    return 0;
#else
    mprLog("error mpr", 0, "SSL communications support not included in build");
    return MPR_ERR_BAD_STATE;
#endif
}


static int loadProviders()
{
    MprSocketService    *ss;

    ss = MPR->socketService;
    mprGlobalLock();
    if (!ss->providers && mprLoadSsl() < 0) {
        mprGlobalUnlock();
        return MPR_ERR_CANT_READ;
    }
    if (!ss->providers) {
        mprLog("error mpr", 0, "Cannot load SSL provider");
        mprGlobalUnlock();
        return MPR_ERR_CANT_INITIALIZE;
    }
    mprGlobalUnlock();
    return 0;
}


/*
    Upgrade a socket to use SSL
 */
PUBLIC int mprUpgradeSocket(MprSocket *sp, MprSsl *ssl, cchar *peerName)
{
    MprSocketService    *ss;
    cchar               *providerName;

    ss  = sp->service;
    assert(sp);

    if (!ssl) {
        return MPR_ERR_BAD_ARGS;
    }
    if (!ssl->provider) {
        if (loadProviders() < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
        providerName = (ssl->providerName) ? ssl->providerName : ss->sslProvider;
        if ((ssl->provider = mprLookupKey(ss->providers, providerName)) == 0) {
            sp->errorMsg = sfmt("Cannot use SSL, missing SSL provider %s", providerName);
            return MPR_ERR_CANT_INITIALIZE;
        }
        ssl->providerName = providerName;
    }
    sp->provider = ssl->provider;
#if KEEP
    /* session resumption can cause problems with Nagle. However, appweb opens sockets with nodelay by default */
    sp->flags |= MPR_SOCKET_NODELAY;
    mprSetSocketNoDelay(sp, 1);
#endif
    return sp->provider->upgradeSocket(sp, ssl, peerName);
}


PUBLIC void mprAddSslCiphers(MprSsl *ssl, cchar *ciphers)
{
    assert(ssl);
    if (ssl->ciphers) {
        ssl->ciphers = sjoin(ssl->ciphers, ":", ciphers, NULL);
    } else {
        ssl->ciphers = sclone(ciphers);
    }
    ssl->changed = 1;
}


PUBLIC void mprSetSslCiphers(MprSsl *ssl, cchar *ciphers)
{
    assert(ssl);
    ssl->ciphers = sclone(ciphers);
    ssl->changed = 1;
}


PUBLIC void mprSetSslKeyFile(MprSsl *ssl, cchar *keyFile)
{
    assert(ssl);
    ssl->keyFile = (keyFile && *keyFile) ? sclone(keyFile) : 0;
    ssl->changed = 1;
}


PUBLIC void mprSetSslCertFile(MprSsl *ssl, cchar *certFile)
{
    assert(ssl);
    ssl->certFile = (certFile && *certFile) ? sclone(certFile) : 0;
    ssl->changed = 1;
}


PUBLIC void mprSetSslCaFile(MprSsl *ssl, cchar *caFile)
{
    assert(ssl);
    ssl->caFile = (caFile && *caFile) ? sclone(caFile) : 0;
    ssl->changed = 1;
}


/* Only supported in OpenSSL */
PUBLIC void mprSetSslCaPath(MprSsl *ssl, cchar *caPath)
{
    assert(ssl);
    ssl->caPath = (caPath && *caPath) ? sclone(caPath) : 0;
    ssl->changed = 1;
}


/* Only supported in OpenSSL */
PUBLIC void mprSetSslProtocols(MprSsl *ssl, int protocols)
{
    assert(ssl);
    ssl->protocols = protocols;
    ssl->changed = 1;
}


PUBLIC void mprSetSslProvider(MprSsl *ssl, cchar *provider)
{
    assert(ssl);
    ssl->providerName = (provider && *provider) ? sclone(provider) : 0;
    ssl->changed = 1;
}


PUBLIC void mprVerifySslPeer(MprSsl *ssl, bool on)
{
    if (ssl) {
        ssl->verifyPeer = on;
        ssl->verifyIssuer = on;
        ssl->changed = 1;
    } else {
        MPR->verifySsl = on;
    }
}


PUBLIC void mprVerifySslIssuer(MprSsl *ssl, bool on)
{
    assert(ssl);
    ssl->verifyIssuer = on;
    ssl->changed = 1;
}


PUBLIC void mprVerifySslDepth(MprSsl *ssl, int depth)
{
    assert(ssl);
    ssl->verifyDepth = depth;
    ssl->changed = 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/string.c ************/


/**
    string.c - String routines safe for embedded programming

    This module provides safe replacements for the standard string library. 
    Most routines in this file are not thread-safe. It is the callers responsibility to perform all thread synchronization.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/*********************************** Locals ***********************************/

#define HASH_PRIME 0x01000193

/************************************ Code ************************************/

PUBLIC char *itos(int64 value)
{
    return itosradix(value, 10);
}


/*
    Format a number as a string. Support radix 10 and 16.
 */
PUBLIC char *itosradix(int64 value, int radix)
{
    char    numBuf[32];
    char    *cp;
    char    digits[] = "0123456789ABCDEF";
    int     negative;

    if (radix != 10 && radix != 16) {
        return 0;
    }
    cp = &numBuf[sizeof(numBuf)];
    *--cp = '\0';

    if (value < 0) {
        negative = 1;
        value = -value;
    } else {
        negative = 0;
    }
    do {
        *--cp = digits[value % radix];
        value /= radix;
    } while (value > 0);

    if (negative) {
        *--cp = '-';
    }
    return sclone(cp);
}


PUBLIC char *itosbuf(char *buf, ssize size, int64 value, int radix)
{
    char    *cp, *end;
    char    digits[] = "0123456789ABCDEF";
    int     negative;

    if ((radix != 10 && radix != 16) || size < 2) {
        return 0;
    }
    end = cp = &buf[size];
    *--cp = '\0';

    if (value < 0) {
        negative = 1;
        value = -value;
        size--;
    } else {
        negative = 0;
    }
    do {
        *--cp = digits[value % radix];
        value /= radix;
    } while (value > 0 && cp > buf);

    if (negative) {
        if (cp <= buf) {
            return 0;
        }
        *--cp = '-';
    }
    if (buf < cp) {
        /* Move the null too */
        memmove(buf, cp, end - cp + 1);
    }
    return buf;
}


PUBLIC char *scamel(cchar *str)
{
    char    *ptr;
    ssize   size, len;

    if (str == 0) {
        str = "";
    }
    len = slen(str);
    size = len + 1;
    if ((ptr = mprAlloc(size)) != 0) {
        memcpy(ptr, str, len);
        ptr[len] = '\0';
    }
    ptr[0] = (char) tolower((uchar) ptr[0]);
    return ptr;
}


/*
    Case insensitive string comparison. Limited by length
 */
PUBLIC int scaselesscmp(cchar *s1, cchar *s2)
{
    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    return sncaselesscmp(s1, s2, max(slen(s1), slen(s2)));
}


PUBLIC bool scaselessmatch(cchar *s1, cchar *s2)
{
    return scaselesscmp(s1, s2) == 0;
}


PUBLIC char *schr(cchar *s, int c)
{
    if (s == 0) {
        return 0;
    }
    return strchr(s, c);
}


PUBLIC char *sncontains(cchar *str, cchar *pattern, ssize limit)
{
    cchar   *cp, *s1, *s2;
    ssize   lim;

    if (limit < 0) {
        limit = MAXINT;
    }
    if (str == 0) {
        return 0;
    }
    if (pattern == 0 || *pattern == '\0') {
        return 0;
    }
    for (cp = str; *cp && limit > 0; cp++, limit--) {
        s1 = cp;
        s2 = pattern;
        for (lim = limit; *s1 && *s2 && (*s1 == *s2) && lim > 0; lim--) {
            s1++;
            s2++;
        }
        if (*s2 == '\0') {
            return (char*) cp;
        }
    }
    return 0;
}


PUBLIC char *scontains(cchar *str, cchar *pattern)
{
    return sncontains(str, pattern, -1);
}


/*
    Copy a string into a buffer. Always ensure it is null terminated
 */
PUBLIC ssize scopy(char *dest, ssize destMax, cchar *src)
{
    ssize      len;

    assert(src);
    assert(dest);
    assert(0 < dest && destMax < MAXINT);

    len = slen(src);
    /* Must ensure room for null */
    if (destMax <= len) {
        assert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    strcpy(dest, src);
    return len;
}


PUBLIC char *sclone(cchar *str)
{
    char    *ptr;
    ssize   size, len;

    if (str == 0) {
        str = "";
    }
    len = slen(str);
    size = len + 1;
    if ((ptr = mprAlloc(size)) != 0) {
        memcpy(ptr, str, len);
        ptr[len] = '\0';
    }
    return ptr;
}


PUBLIC int scmp(cchar *s1, cchar *s2)
{
    if (s1 == s2) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    return sncmp(s1, s2, max(slen(s1), slen(s2)));
}


PUBLIC bool sends(cchar *str, cchar *suffix)
{
    if (str == 0 || suffix == 0) {
        return 0;
    }
    if (strcmp(&str[slen(str) - slen(suffix)], suffix) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC char *sfmt(cchar *format, ...)
{
    va_list     ap;
    char        *buf;

    if (format == 0) {
        format = "%s";
    }
    va_start(ap, format);
    buf = mprPrintfCore(NULL, -1, format, ap);
    va_end(ap);
    return buf;
}


PUBLIC char *sfmtv(cchar *format, va_list arg)
{
    assert(format);
    return mprPrintfCore(NULL, -1, format, arg);
}


PUBLIC uint shash(cchar *cname, ssize len)
{
    uint    hash;

    assert(cname);
    assert(0 <= len && len < MAXINT);

    if (cname == 0) {
        return 0;
    }
    hash = (uint) len;
    while (len-- > 0) {
        hash ^= *cname++;
        hash *= HASH_PRIME;
    }
    return hash;
}


/*
    Hash the lower case name
 */
PUBLIC uint shashlower(cchar *cname, ssize len)
{
    uint    hash;

    assert(cname);
    assert(0 <= len && len < MAXINT);

    if (cname == 0) {
        return 0;
    }
    hash = (uint) len;
    while (len-- > 0) {
        hash ^= tolower((uchar) *cname++);
        hash *= HASH_PRIME;
    }
    return hash;
}


PUBLIC char *sjoin(cchar *str, ...)
{
    va_list     ap;
    char        *result;

    va_start(ap, str);
    result = sjoinv(str, ap);
    va_end(ap);
    return result;
}


PUBLIC char *sjoinv(cchar *buf, va_list args)
{
    va_list     ap;
    char        *dest, *str, *dp;
    ssize       required;

    va_copy(ap, args);
    required = 1;
    if (buf) {
        required += slen(buf);
    }
    str = va_arg(ap, char*);
    while (str) {
        required += slen(str);
        str = va_arg(ap, char*);
    }
    if ((dest = mprAlloc(required)) == 0) {
        return 0;
    }
    dp = dest;
    if (buf) {
        strcpy(dp, buf);
        dp += slen(buf);
    }
    va_copy(ap, args);
    str = va_arg(ap, char*);
    while (str) {
        strcpy(dp, str);
        dp += slen(str);
        str = va_arg(ap, char*);
    }
    *dp = '\0';
    return dest;
}


PUBLIC ssize slen(cchar *s)
{
    return s ? strlen(s) : 0;
}


/*
    Map a string to lower case. Allocates a new string.
 */
PUBLIC char *slower(cchar *str)
{
    char    *cp, *s;

    if (str) {
        s = sclone(str);
        for (cp = s; *cp; cp++) {
            if (isupper((uchar) *cp)) {
                *cp = (char) tolower((uchar) *cp);
            }
        }
        str = s;
    }
    return (char*) str;
}


PUBLIC bool smatch(cchar *s1, cchar *s2)
{
    return scmp(s1, s2) == 0;
}


PUBLIC int sncaselesscmp(cchar *s1, cchar *s2, ssize n)
{
    int     rc;

    assert(0 <= n && n < MAXINT);

    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = tolower((uchar) *s1) - tolower((uchar) *s2);
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


/*
    Clone a sub-string of a specified length. The null is added after the length. The given len can be longer than the
    source string.
 */
PUBLIC char *snclone(cchar *str, ssize len)
{
    char    *ptr;
    ssize   size, l;

    if (str == 0) {
        str = "";
    }
    l = slen(str);
    len = min(l, len);
    size = len + 1;
    if ((ptr = mprAlloc(size)) != 0) {
        memcpy(ptr, str, len);
        ptr[len] = '\0';
    }
    return ptr;
}


/*
    Case sensitive string comparison. Limited by length
 */
PUBLIC int sncmp(cchar *s1, cchar *s2, ssize n)
{
    int     rc;

    assert(0 <= n && n < MAXINT);

    if (s1 == 0 && s2 == 0) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = *s1 - *s2;
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


/*
    This routine copies at most "count" characters from a string. It ensures the result is always null terminated and 
    the buffer does not overflow. Returns MPR_ERR_WONT_FIT if the buffer is too small.
 */
PUBLIC ssize sncopy(char *dest, ssize destMax, cchar *src, ssize count)
{
    ssize      len;

    assert(dest);
    assert(src);
    assert(src != dest);
    assert(0 <= count && count < MAXINT);
    assert(0 < destMax && destMax < MAXINT);

    //  OPT use strnlen(src, count);
    len = slen(src);
    len = min(len, count);
    if (destMax <= len) {
        assert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    if (len > 0) {
        memcpy(dest, src, len);
        dest[len] = '\0';
    } else {
        *dest = '\0';
        len = 0;
    } 
    return len;
}


PUBLIC bool snumber(cchar *s)
{
    if (!s) {
        return 0;
    }
    if (*s == '-' || *s == '+') {
        s++;
    }
    return s && *s && strspn(s, "1234567890") == strlen(s);
} 


PUBLIC bool sspace(cchar *s)
{
    if (!s) {
        return 1;
    }
    while (isspace((uchar) *s)) {
        s++;
    }
    if (*s == '\0') {
        return 1;
    }
    return 0;
} 


/*
    Hex
 */
PUBLIC bool shnumber(cchar *s)
{
    return s && *s && strspn(s, "1234567890abcdefABCDEFxX") == strlen(s);
} 


/*
    Floating point
    Float:      [DIGITS].[DIGITS][(e|E)[+|-]DIGITS]
 */
PUBLIC bool sfnumber(cchar *s)
{
    cchar   *cp;
    int     dots, valid;

    valid = s && *s && strspn(s, "1234567890.+-eE") == strlen(s) && strspn(s, "1234567890") > 0;
    if (valid) {
        /*
            Some extra checks
         */
        for (cp = s, dots = 0; *cp; cp++) {
            if (*cp == '.') {
                if (dots++ > 0) {
                    valid = 0;
                    break;
                }
            }
        }
    }
    return valid;
} 


PUBLIC char *stitle(cchar *str)
{
    char    *ptr;
    ssize   size, len;

    if (str == 0) {
        str = "";
    }
    len = slen(str);
    size = len + 1;
    if ((ptr = mprAlloc(size)) != 0) {
        memcpy(ptr, str, len);
        ptr[len] = '\0';
    }
    ptr[0] = (char) toupper((uchar) ptr[0]);
    return ptr;
}


PUBLIC char *spbrk(cchar *str, cchar *set)
{
    cchar       *sp;
    int         count;

    if (str == 0 || set == 0) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return (char*) str;
            }
        }
    }
    return 0;
}


PUBLIC char *srchr(cchar *s, int c)
{
    if (s == 0) {
        return 0;
    }
    return strrchr(s, c);
}


PUBLIC char *srejoin(char *buf, ...)
{
    va_list     args;
    char        *result;

    va_start(args, buf);
    result = srejoinv(buf, args);
    va_end(args);
    return result;
}


PUBLIC char *srejoinv(char *buf, va_list args)
{
    va_list     ap;
    char        *dest, *str, *dp;
    ssize       len, required;

    va_copy(ap, args);
    len = slen(buf);
    required = len + 1;
    str = va_arg(ap, char*);
    while (str) {
        required += slen(str);
        str = va_arg(ap, char*);
    }
    if ((dest = mprRealloc(buf, required)) == 0) {
        return 0;
    }
    dp = &dest[len];
    va_copy(ap, args);
    str = va_arg(ap, char*);
    while (str) {
        strcpy(dp, str);
        dp += slen(str);
        str = va_arg(ap, char*);
    }
    *dp = '\0';
    return dest;
}


PUBLIC char *sreplace(cchar *str, cchar *pattern, cchar *replacement)
{
    MprBuf      *buf;
    cchar       *s;
    ssize       plen;

    if (!pattern || pattern[0] == '\0') {
        return sclone(str);
    }
    buf = mprCreateBuf(-1, -1);
    plen = slen(pattern);
    for (s = str; *s; s++) {
        if (sncmp(s, pattern, plen) == 0) {
            if (replacement) {
                mprPutStringToBuf(buf, replacement);
            }
            s += plen - 1;
        } else {
            mprPutCharToBuf(buf, *s);
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}


/*
    Split a string at a delimiter and return the parts.
    This differs from stok in that it never returns null. Also, stok eats leading deliminators, whereas 
    ssplit will return an empty string if there are leading deliminators.
    Note: Modifies the original string and returns the string for chaining.
 */
PUBLIC char *ssplit(char *str, cchar *delim, char **last)
{
    char    *end;

    if (last) {
        *last = MPR->emptyString;
    }
    if (str == 0) {
        return MPR->emptyString;
    }
    if (delim == 0 || *delim == '\0') {
        return str;
    }
    if ((end = strpbrk(str, delim)) != 0) {
        *end++ = '\0';
        end += strspn(end, delim);
    } else {
        end = MPR->emptyString;
    }
    if (last) {
        *last = end;
    }
    return str;
}


PUBLIC ssize sspn(cchar *str, cchar *set)
{
#if KEEP
    cchar       *sp;
    int         count;

    if (str == 0 || set == 0) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                break;
            }
        }
        if (*str != *sp) {
            break;
        }
    }
    return count;
#else
    if (str == 0 || set == 0) {
        return 0;
    }
    return strspn(str, set);
#endif
}
 

PUBLIC bool sstarts(cchar *str, cchar *prefix)
{
    if (str == 0 || prefix == 0) {
        return 0;
    }
    if (strncmp(str, prefix, slen(prefix)) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC int64 stoi(cchar *str)
{
    return stoiradix(str, 10, NULL);
}


PUBLIC double stof(cchar *str)
{
    if (str == 0 || *str == 0) {
        return 0.0;
    }
    return atof(str);
}


/*
    Parse a number and check for parse errors. Supports radix 8, 10 or 16. 
    If radix is <= 0, then the radix is sleuthed from the input.
    Supports formats:
        [(+|-)][0][OCTAL_DIGITS]
        [(+|-)][0][(x|X)][HEX_DIGITS]
        [(+|-)][DIGITS]
 */
PUBLIC int64 stoiradix(cchar *str, int radix, int *err)
{
    cchar   *start;
    int64   val;
    int     n, c, negative;

    if (err) {
        *err = 0;
    }
    if (str == 0) {
        if (err) {
            *err = MPR_ERR_BAD_SYNTAX;
        }
        return 0;
    }
    while (isspace((uchar) *str)) {
        str++;
    }
    val = 0;
    if (*str == '-') {
        negative = 1;
        str++;
    } else {
        negative = 0;
    }
    start = str;
    if (radix <= 0) {
        radix = 10;
        if (*str == '0') {
            if (tolower((uchar) str[1]) == 'x') {
                radix = 16;
                str += 2;
            } else {
                radix = 8;
                str++;
            }
        }

    } else if (radix == 16) {
        if (*str == '0' && tolower((uchar) str[1]) == 'x') {
            str += 2;
        }

    } else if (radix > 10) {
        radix = 10;
    }
    if (radix == 16) {
        while (*str) {
            c = tolower((uchar) *str);
            if (isdigit((uchar) c)) {
                val = (val * radix) + c - '0';
            } else if (c >= 'a' && c <= 'f') {
                val = (val * radix) + c - 'a' + 10;
            } else {
                break;
            }
            str++;
        }
    } else {
        while (*str && isdigit((uchar) *str)) {
            n = *str - '0';
            if (n >= radix) {
                break;
            }
            val = (val * radix) + n;
            str++;
        }
    }
    if (str == start) {
        /* No data */
        if (err) {
            *err = MPR_ERR_BAD_SYNTAX;
        }
        return 0;
    }
    return (negative) ? -val: val;
}


/*
    Note "str" is modifed as per strtok()
    WARNING:  this does not allocate
 */
PUBLIC char *stok(char *str, cchar *delim, char **last)
{
    char    *start, *end;
    ssize   i;

    assert(delim);
    start = (str || !last) ? str : *last;
    if (start == 0) {
        if (last) {
            *last = 0;
        }
        return 0;
    }
    i = strspn(start, delim);
    start += i;
    if (*start == '\0') {
        if (last) {
            *last = 0;
        }
        return 0;
    }
    end = strpbrk(start, delim);
    if (end) {
        *end++ = '\0';
        i = strspn(end, delim);
        end += i;
    }
    if (last) {
        *last = end;
    }
    return start;
}


PUBLIC char *ssub(cchar *str, ssize offset, ssize len)
{
    char    *result;
    ssize   size;

    assert(str);
    assert(offset >= 0);
    assert(0 <= len && len < MAXINT);

    if (str == 0) {
        return 0;
    }
    size = len + 1;
    if ((result = mprAlloc(size)) == 0) {
        return 0;
    }
    sncopy(result, size, &str[offset], len);
    return result;
}


/*
    Trim characters from the given set. Returns a newly allocated string.
 */
PUBLIC char *strim(cchar *str, cchar *set, int where)
{
    char    *s;
    ssize   len, i;

    if (str == 0 || set == 0) {
        return 0;
    }
    if (where == 0) {
        where = MPR_TRIM_START | MPR_TRIM_END;
    }
    if (where & MPR_TRIM_START) {
        i = strspn(str, set);
    } else {
        i = 0;
    }
    s = sclone(&str[i]);
    if (where & MPR_TRIM_END) {
        len = slen(s);
        while (len > 0 && strspn(&s[len - 1], set) > 0) {
            s[len - 1] = '\0';
            len--;
        }
    }
    return s;
}


/*
    Map a string to upper case
 */
PUBLIC char *supper(cchar *str)
{
    char    *cp, *s;

    if (str) {
        s = sclone(str);
        for (cp = s; *cp; cp++) {
            if (islower((uchar) *cp)) {
                *cp = (char) toupper((uchar) *cp);
            }
        }
        str = s;
    }
    return (char*) str;
}


/*
    Expand ${token} references in a path or string.
 */
static char *stemplateInner(cchar *str, void *keys, int json)
{
    MprBuf      *buf;
    cchar       *value;
    char        *src, *result, *cp, *tok;

    if (str) {
        if (schr(str, '$') == 0) {
            return sclone(str);
        }
        buf = mprCreateBuf(0, 0);
        for (src = (char*) str; *src; ) {
            if (*src == '$') {
                if (*++src == '{') {
                    for (cp = ++src; *cp && *cp != '}'; cp++) ;
                    tok = snclone(src, cp - src);
                } else {
                    for (cp = src; *cp && (isalnum((uchar) *cp) || *cp == '_'); cp++) ;
                    tok = snclone(src, cp - src);
                }
                if (json) {
                    value = mprGetJson(keys, tok);
                } else {
                    value = mprLookupKey(keys, tok);
                }
                if (value != 0) {
                    mprPutStringToBuf(buf, value);
                    if (src > str && src[-1] == '{') {
                        src = cp + 1;
                    } else {
                        src = cp;
                    }
                } else {
                    mprPutCharToBuf(buf, '$');
                    if (src > str && src[-1] == '{') {
                        mprPutCharToBuf(buf, '{');
                    }
                    mprPutCharToBuf(buf, *src++);
                }
            } else {
                mprPutCharToBuf(buf, *src++);
            }
        }
        mprAddNullToBuf(buf);
        result = sclone(mprGetBufStart(buf));
    } else {
        result = MPR->emptyString;
    }
    return result;
}


PUBLIC char *stemplate(cchar *str, MprHash *keys)
{
    return stemplateInner(str, keys, 0);
}

PUBLIC char *stemplateJson(cchar *str, MprJson *obj)
{
    return stemplateInner(str, obj, 1);
}


/*
    String to list. This parses the string into space separated arguments. Single and double quotes are supported.
    This returns a stable list.
 */
PUBLIC MprList *stolist(cchar *src)
{
    MprList     *list;
    cchar       *start;
    int         quote;

    list = mprCreateList(0, MPR_LIST_STABLE);
    while (src && *src != '\0') {
        while (isspace((uchar) *src)) {
            src++;
        }
        if (*src == '\0')  {
            break;
        }
        for (quote = 0, start = src; *src; src++) {
            if (*src == '\\') {
                src++;
            } else if (*src == '"' || *src == '\'') {
                if (*src == quote) {
                    quote = 0;
                    src++;
                    break;
                } else if (quote == 0) {
                    quote = *src;
                }
            } else if (isspace((uchar) *src) && !quote) {
                break;
            }
        }
        mprAddItem(list, snclone(start, src - start));
    }
    return list;
}


PUBLIC cchar *sjoinArgs(int argc, cchar **argv, cchar *sep)
{
    MprBuf  *buf;
    int     i;

    if (sep == 0) {
        sep = "";
    }
    buf = mprCreateBuf(0, 0);
    for (i = 0; i < argc; i++) {
        mprPutToBuf(buf, "%s%s", argv[i], sep);
    }
    if (argc > 0) {
        mprAdjustBufEnd(buf, -1);
    }
    return mprBufToString(buf);
}


PUBLIC void serase(char *str)
{
    char    *cp;

    for (cp = str; cp && *cp; ) {
        *cp++ = '\0';
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/thread.c ************/


/**
    thread.c - Primitive multi-threading support for Windows

    This module provides threading, mutex and condition variable APIs.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes **********************************/



/*************************** Forward Declarations ****************************/

static void changeState(MprWorker *worker, int state);
static MprWorker *createWorker(MprWorkerService *ws, ssize stackSize);
static int getNextThreadNum(MprWorkerService *ws);
static void manageThreadService(MprThreadService *ts, int flags);
static void manageThread(MprThread *tp, int flags);
static void manageWorker(MprWorker *worker, int flags);
static void manageWorkerService(MprWorkerService *ws, int flags);
static void pruneWorkers(MprWorkerService *ws, MprEvent *timer);
static void threadProc(MprThread *tp);
static void workerMain(MprWorker *worker, MprThread *tp);

/************************************ Code ***********************************/

PUBLIC MprThreadService *mprCreateThreadService()
{
    MprThreadService    *ts;

    if ((ts = mprAllocObj(MprThreadService, manageThreadService)) == 0) {
        return 0;
    }
    if ((ts->pauseThreads = mprCreateCond()) == 0) {
        return 0;
    }
    if ((ts->threads = mprCreateList(-1, 0)) == 0) {
        return 0;
    }
    MPR->mainOsThread = mprGetCurrentOsThread();
    MPR->threadService = ts;
    ts->stackSize = ME_STACK_SIZE;
    /*
        Don't actually create the thread. Just create a thread object for this main thread.
     */
    if ((ts->mainThread = mprCreateThread("main", NULL, NULL, 0)) == 0) {
        return 0;
    }
    ts->mainThread->isMain = 1;
    ts->mainThread->osThread = mprGetCurrentOsThread();
    return ts;
}


PUBLIC void mprStopThreadService()
{
#if ME_WIN_LIKE
    MprThreadService    *ts;
    MprThread           *tp;
    int                 i;

    ts = MPR->threadService;
    for (i = 0; i < ts->threads->length; i++) {
        tp = (MprThread*) mprGetItem(ts->threads, i);
        if (tp->hwnd) {
            mprDestroyWindow(tp->hwnd);
            tp->hwnd = 0;
        }
    }
#endif
}


static void manageThreadService(MprThreadService *ts, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ts->threads);
        mprMark(ts->mainThread);
        mprMark(ts->eventsThread);
        mprMark(ts->pauseThreads);
    }
}


PUBLIC void mprSetThreadStackSize(ssize size)
{
    MPR->threadService->stackSize = size;
}


PUBLIC MprThread *mprGetCurrentThread()
{
    MprThreadService    *ts;
    MprThread           *tp;
    MprOsThread         id;
    int                 i;

    ts = MPR->threadService;
    if (ts && ts->threads) {
        id = mprGetCurrentOsThread();
        for (i = 0; i < ts->threads->length; i++) {
            tp = mprGetItem(ts->threads, i);
            if (tp->osThread == id) {
                return tp;
            }
        }
    }
    return 0;
}


PUBLIC cchar *mprGetCurrentThreadName()
{
    MprThread       *tp;

    if ((tp = mprGetCurrentThread()) == 0) {
        return 0;
    }
    return tp->name;
}


/*
    Return the current thread object
 */
PUBLIC void mprSetCurrentThreadPriority(int pri)
{
    MprThread       *tp;

    if ((tp = mprGetCurrentThread()) == 0) {
        return;
    }
    mprSetThreadPriority(tp, pri);
}


/*
    Create a main thread
 */
PUBLIC MprThread *mprCreateThread(cchar *name, void *entry, void *data, ssize stackSize)
{
    MprThreadService    *ts;
    MprThread           *tp;

    ts = MPR->threadService;
    tp = mprAllocObj(MprThread, manageThread);
    if (tp == 0) {
        return 0;
    }
    tp->data = data;
    tp->entry = entry;
    tp->name = sclone(name);
    tp->mutex = mprCreateLock();
    tp->cond = mprCreateCond();
    tp->pid = getpid();
    tp->priority = MPR_NORMAL_PRIORITY;

    if (stackSize == 0) {
        tp->stackSize = ts->stackSize;
    } else {
        tp->stackSize = stackSize;
    }
#if ME_WIN_LIKE
    tp->threadHandle = 0;
#endif
    assert(ts);
    assert(ts->threads);
    if (mprAddItem(ts->threads, tp) < 0) {
        return 0;
    }
    return tp;
}


static void manageThread(MprThread *tp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(tp->mutex);
        mprMark(tp->cond);
        mprMark(tp->data);
        mprMark(tp->name);

    } else if (flags & MPR_MANAGE_FREE) {
#if ME_WIN_LIKE
        if (tp->threadHandle) {
            CloseHandle(tp->threadHandle);
        }
        if (tp->hwnd) {
            mprDestroyWindow(tp->hwnd);
        }
#endif
    }
}


/*
    Entry thread function
 */ 
#if ME_WIN_LIKE
static uint __stdcall threadProcWrapper(void *data) 
{
    threadProc((MprThread*) data);
    return 0;
}
#elif VXWORKS

static int threadProcWrapper(void *data) 
{
    threadProc((MprThread*) data);
    return 0;
}

#else
PUBLIC void *threadProcWrapper(void *data) 
{
    threadProc((MprThread*) data);
    return 0;
}

#endif


/*
    Thread entry
 */
static void threadProc(MprThread *tp)
{
    assert(tp);

    tp->osThread = mprGetCurrentOsThread();

#if VXWORKS
    tp->pid = tp->osThread;
#else
    tp->pid = getpid();
#endif
    (tp->entry)(tp->data, tp);
    mprRemoveItem(MPR->threadService->threads, tp);
    tp->pid = 0;
}


/*
    Start a thread
 */
PUBLIC int mprStartThread(MprThread *tp)
{
    lock(tp);

#if ME_WIN_LIKE
{
    HANDLE          h;
    uint            threadId;

    h = (HANDLE) _beginthreadex(NULL, 0, threadProcWrapper, (void*) tp, 0, &threadId);
    if (h == NULL) {
        unlock(tp);
        return MPR_ERR_CANT_INITIALIZE;
    }
    tp->osThread = (int) threadId;
    tp->threadHandle = (HANDLE) h;
}
#elif VXWORKS
{
    int     taskHandle, pri;

    taskPriorityGet(taskIdSelf(), &pri);
    taskHandle = taskSpawn(tp->name, pri, VX_FP_TASK, tp->stackSize, (FUNCPTR) threadProcWrapper, (int) tp, 
        0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (taskHandle < 0) {
        mprLog("error mpr thread", 0, "Cannot create thread %s", tp->name);
        unlock(tp);
        return MPR_ERR_CANT_INITIALIZE;
    }
}
#else /* UNIX */
{
    pthread_attr_t  attr;
    pthread_t       h;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, tp->stackSize);

    if (pthread_create(&h, &attr, threadProcWrapper, (void*) tp) != 0) { 
        assert(0);
        pthread_attr_destroy(&attr);
        unlock(tp);
        return MPR_ERR_CANT_CREATE;
    }
    pthread_attr_destroy(&attr);
}
#endif
    unlock(tp);
    return 0;
}


PUBLIC MprOsThread mprGetCurrentOsThread()
{
#if ME_UNIX_LIKE
    return (MprOsThread) pthread_self();
#elif ME_WIN_LIKE
    return (MprOsThread) GetCurrentThreadId();
#elif VXWORKS
    return (MprOsThread) taskIdSelf();
#endif
}


PUBLIC void mprSetThreadPriority(MprThread *tp, int newPriority)
{
#if ME_WIN_LIKE || VXWORKS
    int     osPri = mprMapMprPriorityToOs(newPriority);
#endif
    lock(tp);
#if ME_WIN_LIKE
    SetThreadPriority(tp->threadHandle, osPri);
#elif VXWORKS
    taskPrioritySet(tp->osThread, osPri);
#elif ME_UNIX_LIKE && DISABLED
    /*
        Not worth setting thread priorities on linux
     */
    MprOsThread ost;
    pthread_attr_t attr;
    int policy = 0;
    int max_prio_for_policy = 0;

    ost = pthread_self();
    pthread_attr_init(&attr);
    pthread_attr_getschedpolicy(&attr, &policy);
    max_prio_for_policy = sched_get_priority_max(policy);

    pthread_setschedprio(ost, max_prio_for_policy);
    pthread_attr_destroy(&thAttr);
#elif DEPRECATED
    /*
        Don't set process priority
     */
    setpriority(PRIO_PROCESS, (int) tp->pid, osPri);
#else
    /* Nothing can be done */
#endif
    tp->priority = newPriority;
    unlock(tp);
}


static void manageThreadLocal(MprThreadLocal *tls, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
#if !ME_UNIX_LIKE && !ME_WIN_LIKE
        mprMark(tls->store);
#endif
    } else if (flags & MPR_MANAGE_FREE) {
#if ME_UNIX_LIKE
        if (tls->key) {
            pthread_key_delete(tls->key);
        }
#elif ME_WIN_LIKE
        if (tls->key >= 0) {
            TlsFree(tls->key);
        }
#endif
    }
}


PUBLIC MprThreadLocal *mprCreateThreadLocal()
{
    MprThreadLocal      *tls;

    if ((tls = mprAllocObj(MprThreadLocal, manageThreadLocal)) == 0) {
        return 0;
    }
#if ME_UNIX_LIKE
    if (pthread_key_create(&tls->key, NULL) != 0) {
        tls->key = 0;
        return 0;
    }
#elif ME_WIN_LIKE
    if ((tls->key = TlsAlloc()) < 0) {
        return 0;
    }
#else
    if ((tls->store = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
        return 0;
    }
#endif
    return tls;
}


PUBLIC int mprSetThreadData(MprThreadLocal *tls, void *value)
{
    bool    err;

#if ME_UNIX_LIKE
    err = pthread_setspecific(tls->key, value) != 0;
#elif ME_WIN_LIKE
    err = TlsSetValue(tls->key, value) != 0;
#else
    {
        char    key[32];
        itosbuf(key, sizeof(key), (int64) mprGetCurrentOsThread(), 10);
        err = mprAddKey(tls->store, key, value) == 0;
    }
#endif
    return (err) ? MPR_ERR_CANT_WRITE: 0;
}


PUBLIC void *mprGetThreadData(MprThreadLocal *tls)
{
#if ME_UNIX_LIKE
    return pthread_getspecific(tls->key);
#elif ME_WIN_LIKE
    return TlsGetValue(tls->key);
#else
    {
        char    key[32];
        itosbuf(key, sizeof(key), (int64) mprGetCurrentOsThread(), 10);
        return mprLookupKey(tls->store, key);
    }
#endif
}


#if ME_WIN_LIKE
/*
    Map Mpr priority to Windows native priority. Windows priorities range from -15 to +15 (zero is normal). 
    Warning: +15 will not yield the CPU, -15 may get starved. We should be very wary going above +11.
 */

PUBLIC int mprMapMprPriorityToOs(int mprPriority)
{
    assert(mprPriority >= 0 && mprPriority <= 100);
 
    if (mprPriority <= MPR_BACKGROUND_PRIORITY) {
        return THREAD_PRIORITY_LOWEST;
    } else if (mprPriority <= MPR_LOW_PRIORITY) {
        return THREAD_PRIORITY_BELOW_NORMAL;
    } else if (mprPriority <= MPR_NORMAL_PRIORITY) {
        return THREAD_PRIORITY_NORMAL;
    } else if (mprPriority <= MPR_HIGH_PRIORITY) {
        return THREAD_PRIORITY_ABOVE_NORMAL;
    } else {
        return THREAD_PRIORITY_HIGHEST;
    }
}


/*
    Map Windows priority to Mpr priority
 */ 
PUBLIC int mprMapOsPriorityToMpr(int nativePriority)
{
    int     priority;

    priority = (45 * nativePriority) + 50;
    if (priority < 0) {
        priority = 0;
    }
    if (priority >= 100) {
        priority = 99;
    }
    return priority;
}


#elif VXWORKS
/*
    Map MPR priority to VxWorks native priority.
 */

PUBLIC int mprMapMprPriorityToOs(int mprPriority)
{
    int     nativePriority;

    assert(mprPriority >= 0 && mprPriority < 100);

    nativePriority = (100 - mprPriority) * 5 / 2;

    if (nativePriority < 10) {
        nativePriority = 10;
    } else if (nativePriority > 255) {
        nativePriority = 255;
    }
    return nativePriority;
}


/*
    Map O/S priority to Mpr priority.
 */ 
PUBLIC int mprMapOsPriorityToMpr(int nativePriority)
{
    int     priority;

    priority = (255 - nativePriority) * 2 / 5;
    if (priority < 0) {
        priority = 0;
    }
    if (priority >= 100) {
        priority = 99;
    }
    return priority;
}


#else /* UNIX */
/*
    Map MR priority to linux native priority. Unix priorities range from -19 to +19. Linux does -20 to +19. 
 */
PUBLIC int mprMapMprPriorityToOs(int mprPriority)
{
    assert(mprPriority >= 0 && mprPriority < 100);

    if (mprPriority <= MPR_BACKGROUND_PRIORITY) {
        return 19;
    } else if (mprPriority <= MPR_LOW_PRIORITY) {
        return 10;
    } else if (mprPriority <= MPR_NORMAL_PRIORITY) {
        return 0;
    } else if (mprPriority <= MPR_HIGH_PRIORITY) {
        return -8;
    } else {
        return -19;
    }
    assert(0);
    return 0;
}


/*
    Map O/S priority to Mpr priority.
 */ 
PUBLIC int mprMapOsPriorityToMpr(int nativePriority)
{
    int     priority;

    priority = (nativePriority + 19) * (100 / 40); 
    if (priority < 0) {
        priority = 0;
    }
    if (priority >= 100) {
        priority = 99;
    }
    return priority;
}

#endif /* UNIX */


PUBLIC MprWorkerService *mprCreateWorkerService()
{
    MprWorkerService      *ws;

    if ((ws = mprAllocObj(MprWorkerService, manageWorkerService)) == 0) {
        return 0;
    }
    ws->mutex = mprCreateLock();
    ws->minThreads = MPR_DEFAULT_MIN_THREADS;
    ws->maxThreads = MPR_DEFAULT_MAX_THREADS;

    /*
        Presize the lists so they cannot get memory allocation failures later on.
     */
    ws->idleThreads = mprCreateList(0, 0);
    mprSetListLimits(ws->idleThreads, ws->maxThreads, -1);
    ws->busyThreads = mprCreateList(0, 0);
    mprSetListLimits(ws->busyThreads, ws->maxThreads, -1);
    return ws;
}


static void manageWorkerService(MprWorkerService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->busyThreads);
        mprMark(ws->idleThreads);
        mprMark(ws->mutex);
        mprMark(ws->pruneTimer);
    }
}


PUBLIC int mprStartWorkerService()
{
    MprWorkerService    *ws;

    ws = MPR->workerService;
    mprSetMinWorkers(ws->minThreads);
    return 0;
}


PUBLIC void mprStopWorkers()
{
    MprWorkerService    *ws;
    MprWorker           *worker;
    int                 next;

    ws = MPR->workerService;
    lock(ws);
    if (ws->pruneTimer) {
        mprRemoveEvent(ws->pruneTimer);
        ws->pruneTimer = 0;
    }
    /*
        Wake up all idle workers. Busy workers take care of themselves. An idle thread will wakeup, exit and be 
        removed from the busy list and then delete the thread. We progressively remove the last thread in the idle
        list. ChangeState will move the workers to the busy queue.
     */
    for (next = -1; (worker = (MprWorker*) mprGetPrevItem(ws->idleThreads, &next)) != 0; ) {
        changeState(worker, MPR_WORKER_BUSY);
    }
    unlock(ws);
}


/*
    Define the new minimum number of workers. Pre-allocate the minimum.
 */
PUBLIC void mprSetMinWorkers(int n)
{ 
    MprWorker           *worker;
    MprWorkerService    *ws;

    ws = MPR->workerService;
    lock(ws);
    ws->minThreads = n; 
    if (n > 0) {
        mprLog("info mpr thread", 1, "Pre-start %d workers", ws->minThreads);
    }
    while (ws->numThreads < ws->minThreads) {
        worker = createWorker(ws, ws->stackSize);
        ws->numThreads++;
        ws->maxUsedThreads = max(ws->numThreads, ws->maxUsedThreads);
        changeState(worker, MPR_WORKER_BUSY);
        mprStartThread(worker->thread);
    }
    unlock(ws);
}


/*
    Define a new maximum number of theads. Prune if currently over the max.
 */
PUBLIC void mprSetMaxWorkers(int n)
{
    MprWorkerService  *ws;

    ws = MPR->workerService;

    lock(ws);
    ws->maxThreads = n; 
    if (ws->numThreads > ws->maxThreads) {
        pruneWorkers(ws, NULL);
    }
    if (ws->minThreads > ws->maxThreads) {
        ws->minThreads = ws->maxThreads;
    }
    unlock(ws);
}


PUBLIC int mprGetMaxWorkers()
{
    return MPR->workerService->maxThreads;
}


/*
    Return the current worker thread object
 */
PUBLIC MprWorker *mprGetCurrentWorker()
{
    MprWorkerService    *ws;
    MprWorker           *worker;
    MprThread           *thread;
    int                 next;

    ws = MPR->workerService;
    lock(ws);
    thread = mprGetCurrentThread();
    for (next = -1; (worker = (MprWorker*) mprGetPrevItem(ws->busyThreads, &next)) != 0; ) {
        if (worker->thread == thread) {
            unlock(ws);
            return worker;
        }
    }
    unlock(ws);
    return 0;
}


PUBLIC void mprActivateWorker(MprWorker *worker, MprWorkerProc proc, void *data)
{
    MprWorkerService    *ws;

    ws = worker->workerService;

    lock(ws);
    worker->proc = proc;
    worker->data = data;
    changeState(worker, MPR_WORKER_BUSY);
    unlock(ws);
}


PUBLIC void mprSetWorkerStartCallback(MprWorkerProc start)
{
    MPR->workerService->startWorker = start;
}


PUBLIC void mprGetWorkerStats(MprWorkerStats *stats)
{
    MprWorkerService    *ws;
    MprWorker           *wp;
    int                 next;

    ws = MPR->workerService;

    lock(ws);
    stats->max = ws->maxThreads;
    stats->min = ws->minThreads;
    stats->maxUsed = ws->maxUsedThreads;

    stats->idle = (int) ws->idleThreads->length;
    stats->busy = (int) ws->busyThreads->length;

    stats->yielded = 0;
    for (ITERATE_ITEMS(ws->busyThreads, wp, next)) {
        /*
            Only count as yielded, those workers who call yield from their user code
            This excludes the yield in worker main
         */
        stats->yielded += (wp->thread->yielded && wp->running);
    }
    unlock(ws);
}


PUBLIC int mprAvailableWorkers()
{
    MprWorkerStats  wstats;
    int             activeWorkers, spareThreads, spareCores, result;

    mprGetWorkerStats(&wstats);
    /*
        SpareThreads    == Threads that can be created up to max threads
        ActiveWorkers   == Worker threads actively servicing requests
        SpareCores      == Cores available on the system
        Result          == Idle workers + lesser of SpareCores|SpareThreads
     */
    spareThreads = wstats.max - wstats.busy - wstats.idle;
    activeWorkers = wstats.busy - wstats.yielded;
    spareCores = MPR->heap->stats.cpuCores - activeWorkers;
    if (spareCores <= 0) {
        return 0;
    }
    result = wstats.idle + min(spareThreads, spareCores);
#if KEEP
    printf("Avail %d, busy %d, yielded %d, idle %d, spare-threads %d, spare-cores %d, mustYield %d\n", result, wstats.busy,
        wstats.yielded, wstats.idle, spareThreads, spareCores, MPR->heap->mustYield);
#endif
    return result;
}


PUBLIC int mprStartWorker(MprWorkerProc proc, void *data)
{
    MprWorkerService    *ws;
    MprWorker           *worker;

    ws = MPR->workerService;
    lock(ws);
    if (mprIsStopped()) {
        unlock(ws);
        return MPR_ERR_BAD_STATE;
    }
    /*
        Try to find an idle thread and wake it up. It will wakeup in workerMain(). If not any available, then add 
        another thread to the worker. Must account for workers we've already created but have not yet gone to work 
        and inserted themselves in the idle/busy queues. Get most recently used idle worker so we tend to reuse 
        active threads. This lets the pruner trim idle workers.
     */
    worker = mprGetLastItem(ws->idleThreads);
    if (worker) {
        worker->data = data;
        worker->proc = proc;
        changeState(worker, MPR_WORKER_BUSY);

    } else if (ws->numThreads < ws->maxThreads) {
        if (mprAvailableWorkers() == 0) {
            unlock(ws);
            return MPR_ERR_BUSY;
        }
        worker = createWorker(ws, ws->stackSize);
        ws->numThreads++;
        ws->maxUsedThreads = max(ws->numThreads, ws->maxUsedThreads);
        worker->data = data;
        worker->proc = proc;
        changeState(worker, MPR_WORKER_BUSY);
        mprStartThread(worker->thread);

    } else {
        unlock(ws);
        return MPR_ERR_BUSY;
    }
    if (!ws->pruneTimer && (ws->numThreads < ws->minThreads)) {
        ws->pruneTimer = mprCreateTimerEvent(NULL, "pruneWorkers", MPR_TIMEOUT_PRUNER, pruneWorkers, ws, MPR_EVENT_QUICK);
    }
    unlock(ws);
    return 0;
}


/*
    Trim idle workers
 */
static void pruneWorkers(MprWorkerService *ws, MprEvent *timer)
{
    MprWorker     *worker;
    int           index, pruned;

    if (mprGetDebugMode()) {
        return;
    }
    lock(ws);
    pruned = 0;
    for (index = 0; index < ws->idleThreads->length; index++) {
        if ((ws->numThreads - pruned) <= ws->minThreads) {
            break;
        }
        worker = mprGetItem(ws->idleThreads, index);
        if ((worker->lastActivity + MPR_TIMEOUT_WORKER) < MPR->eventService->now) {
            changeState(worker, MPR_WORKER_PRUNED);
            pruned++;
            index--;
        }
    }
    if (pruned) {
        mprLog("info mpr thread", 4, "Pruned %d workers, pool has %d workers. Limits %d-%d.", 
            pruned, ws->numThreads - pruned, ws->minThreads, ws->maxThreads);
    }
    if (timer && (ws->numThreads < ws->minThreads)) {
        mprRemoveEvent(ws->pruneTimer);
        ws->pruneTimer = 0;
    }
    unlock(ws);
}


static int getNextThreadNum(MprWorkerService *ws)
{
    int     rc;

    //  TODO Atomic
    lock(ws);
    rc = ws->nextThreadNum++;
    unlock(ws);
    return rc;
}


/*
    Define a new stack size for new workers. Existing workers unaffected.
 */
PUBLIC void mprSetWorkerStackSize(int n)
{
    MPR->workerService->stackSize = n; 
}


/*
    Create a new thread for the task
 */
static MprWorker *createWorker(MprWorkerService *ws, ssize stackSize)
{
    MprWorker   *worker;

    char    name[16];

    if ((worker = mprAllocObj(MprWorker, manageWorker)) == 0) {
        return 0;
    }
    worker->workerService = ws;
    worker->idleCond = mprCreateCond();

    fmt(name, sizeof(name), "worker.%u", getNextThreadNum(ws));
    mprLog("info mpr thread", 4, "Create %s, pool has %d workers. Limits %d-%d.", name, ws->numThreads + 1, ws->minThreads, ws->maxThreads);
    worker->thread = mprCreateThread(name, (MprThreadProc) workerMain, worker, stackSize);
    return worker;
}


static void manageWorker(MprWorker *worker, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(worker->data);
        mprMark(worker->thread);
        mprMark(worker->workerService);
        mprMark(worker->idleCond);
    }
}


static void workerMain(MprWorker *worker, MprThread *tp)
{
    MprWorkerService    *ws;

    ws = MPR->workerService;
    assert(worker->state == MPR_WORKER_BUSY);
    assert(!worker->idleCond->triggered);

    if (ws->startWorker) {
        (*ws->startWorker)(worker->data, worker);
    }
    /*
        Very important for performance to elimminate to locking the WorkerService
     */
    while (!(worker->state & MPR_WORKER_PRUNED)) {
        if (worker->proc) {
            worker->running = 1;
            (*worker->proc)(worker->data, worker);
            worker->running = 0;
        }
        worker->lastActivity = MPR->eventService->now;
        if (mprIsStopping()) {
            break;
        }
        assert(worker->cleanup == 0);
        if (worker->cleanup) {
            (*worker->cleanup)(worker->data, worker);
            worker->cleanup = NULL;
        }
        worker->proc = 0;
        worker->data = 0;
        changeState(worker, MPR_WORKER_IDLE);

        /*
            Sleep till there is more work to do. Yield for GC first.
         */
        mprYield(MPR_YIELD_STICKY);
        mprWaitForCond(worker->idleCond, -1);
        mprResetYield();
    }
    lock(ws);
    changeState(worker, 0);
    worker->thread = 0;
    ws->numThreads--;
    unlock(ws);
    mprLog("info mpr thread", 5, "Worker exiting. There are %d workers remaining in the pool.", ws->numThreads);
}


static void changeState(MprWorker *worker, int state)
{
    MprWorkerService    *ws;
    MprList             *lp;
    int                 wakeIdle, wakeDispatchers;

    if (state == worker->state) {
        return;
    }
    wakeIdle = wakeDispatchers = 0;
    lp = 0;
    ws = worker->workerService;
    lock(ws);

    switch (worker->state) {
    case MPR_WORKER_BUSY:
        lp = ws->busyThreads;
        break;

    case MPR_WORKER_IDLE:
        lp = ws->idleThreads;
        wakeIdle = 1;
        break;

    case MPR_WORKER_PRUNED:
        break;
    }

    /*
        Reassign the worker to the appropriate queue
     */
    if (lp) {
        mprRemoveItem(lp, worker);
    }
    lp = 0;
    switch (state) {
    case MPR_WORKER_BUSY:
        lp = ws->busyThreads;
        break;

    case MPR_WORKER_IDLE:
        lp = ws->idleThreads;
        wakeDispatchers = 1;
        break;

    case MPR_WORKER_PRUNED:
        /* Don't put on a queue and the thread will exit */
        wakeDispatchers = 1;
        break;
    }
    worker->state = state;

    if (lp) {
        if (mprAddItem(lp, worker) < 0) {
            unlock(ws);
            assert(!MPR_ERR_MEMORY);
            return;
        }
    }
    unlock(ws);
    if (wakeDispatchers) {
        mprWakePendingDispatchers();
    }
    if (wakeIdle) {
        mprSignalCond(worker->idleCond); 
    }
}


PUBLIC ssize mprGetBusyWorkerCount()
{
    MprWorkerService    *ws;
    ssize               count;

    ws = MPR->workerService;
    lock(ws);
    count = mprGetListLength(MPR->workerService->busyThreads);
    unlock(ws);
    return count;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/time.c ************/


/**
    time.c - Date and Time handling
 
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Defines ***********************************/

#define MS_PER_SEC  (MPR_TICKS_PER_SEC)
#define MS_PER_HOUR (60 * 60 * MPR_TICKS_PER_SEC)
#define MS_PER_MIN  (60 * MPR_TICKS_PER_SEC)
#define MS_PER_DAY  (86400 * MPR_TICKS_PER_SEC)
#define MS_PER_YEAR (INT64(31556952000))

/*
    On some platforms, time_t is only 32 bits (linux-32) and on some 64 bit systems, time calculations
    outside the range of 32 bits are unreliable. This means there is a minimum and maximum year that 
    can be analysed using the O/S localtime routines. However, we really want to use the O/S 
    calculations for daylight savings time, so when a date is outside a 32 bit time_t range, we use
    some trickery to remap the year to a temporary (current) year so localtime can be used.
    FYI: 32 bit time_t expires at: 03:14:07 UTC on Tuesday, 19 January 2038
 */
#define MIN_YEAR    1901
#define MAX_YEAR    2037

/*
    MacOSX cannot handle MIN_TIME == -0x7FFFFFFF
 */
#define MAX_TIME    0x7FFFFFFF
#define MIN_TIME    -0xFFFFFFF

/*
    Token types or'd into the TimeToken value
 */
#define TOKEN_DAY       0x01000000
#define TOKEN_MONTH     0x02000000
#define TOKEN_ZONE      0x04000000
#define TOKEN_OFFSET    0x08000000
#define TOKEN_MASK      0xFF000000

typedef struct TimeToken {
    char    *name;
    int     value;
} TimeToken;

static TimeToken days[] = {
    { "sun",  0 | TOKEN_DAY },
    { "mon",  1 | TOKEN_DAY },
    { "tue",  2 | TOKEN_DAY },
    { "wed",  3 | TOKEN_DAY },
    { "thu",  4 | TOKEN_DAY },
    { "fri",  5 | TOKEN_DAY },
    { "sat",  6 | TOKEN_DAY },
    { 0, 0 },
};

static TimeToken fullDays[] = {
    { "sunday",     0 | TOKEN_DAY },
    { "monday",     1 | TOKEN_DAY },
    { "tuesday",    2 | TOKEN_DAY },
    { "wednesday",  3 | TOKEN_DAY },
    { "thursday",   4 | TOKEN_DAY },
    { "friday",     5 | TOKEN_DAY },
    { "saturday",   6 | TOKEN_DAY },
    { 0, 0 },
};

/*
    Make origin 1 to correspond to user date entries 10/28/2014
 */
static TimeToken months[] = {
    { "jan",  1 | TOKEN_MONTH },
    { "feb",  2 | TOKEN_MONTH },
    { "mar",  3 | TOKEN_MONTH },
    { "apr",  4 | TOKEN_MONTH },
    { "may",  5 | TOKEN_MONTH },
    { "jun",  6 | TOKEN_MONTH },
    { "jul",  7 | TOKEN_MONTH },
    { "aug",  8 | TOKEN_MONTH },
    { "sep",  9 | TOKEN_MONTH },
    { "oct", 10 | TOKEN_MONTH },
    { "nov", 11 | TOKEN_MONTH },
    { "dec", 12 | TOKEN_MONTH },
    { 0, 0 },
};

static TimeToken fullMonths[] = {
    { "january",    1 | TOKEN_MONTH },
    { "february",   2 | TOKEN_MONTH },
    { "march",      3 | TOKEN_MONTH },
    { "april",      4 | TOKEN_MONTH },
    { "may",        5 | TOKEN_MONTH },
    { "june",       6 | TOKEN_MONTH },
    { "july",       7 | TOKEN_MONTH },
    { "august",     8 | TOKEN_MONTH },
    { "september",  9 | TOKEN_MONTH },
    { "october",   10 | TOKEN_MONTH },
    { "november",  11 | TOKEN_MONTH },
    { "december",  12 | TOKEN_MONTH },
    { 0, 0 }
};

static TimeToken ampm[] = {
    { "am", 0 | TOKEN_OFFSET },
    { "pm", (12 * 3600) | TOKEN_OFFSET },
    { 0, 0 },
};


static TimeToken zones[] = {
    { "ut",      0 | TOKEN_ZONE},
    { "utc",     0 | TOKEN_ZONE},
    { "gmt",     0 | TOKEN_ZONE},
    { "edt",  -240 | TOKEN_ZONE},
    { "est",  -300 | TOKEN_ZONE},
    { "cdt",  -300 | TOKEN_ZONE},
    { "cst",  -360 | TOKEN_ZONE},
    { "mdt",  -360 | TOKEN_ZONE},
    { "mst",  -420 | TOKEN_ZONE},
    { "pdt",  -420 | TOKEN_ZONE},
    { "pst",  -480 | TOKEN_ZONE},
    { 0, 0 },
};


static TimeToken offsets[] = {
    { "tomorrow",    86400 | TOKEN_OFFSET},
    { "yesterday",  -86400 | TOKEN_OFFSET},
    { "next week",   (86400 * 7) | TOKEN_OFFSET},
    { "last week",  -(86400 * 7) | TOKEN_OFFSET},
    { 0, 0 },
};

static int timeSep = ':';

/*
    Formats for mprFormatTime
 */
#if WINDOWS
    #define VALID_FMT "AaBbCcDdEeFHhIjklMmnOPpRrSsTtUuvWwXxYyZz+%"
#elif MACOSX
    #define VALID_FMT "AaBbCcDdEeFGgHhIjklMmnOPpRrSsTtUuVvWwXxYyZz+%"
#else
    #define VALID_FMT "AaBbCcDdEeFGgHhIjklMmnOPpRrSsTtUuVvWwXxYyZz+%"
#endif

#define HAS_STRFTIME 1

#if !HAS_STRFTIME
static char *abbrevDay[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static char *day[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static char *abbrevMonth[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static char *month[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
#endif /* !HAS_STRFTIME */


static int normalMonthStart[] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 0,
};
static int leapMonthStart[] = {
    0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 0
};

static MprTime daysSinceEpoch(int year);
static void decodeTime(struct tm *tp, MprTime when, bool local);
static int getTimeZoneOffsetFromTm(struct tm *tp);
static int leapYear(int year);
static int localTime(struct tm *timep, MprTime time);
static MprTime makeTime(struct tm *tp);
static void validateTime(struct tm *tm, struct tm *defaults);

/************************************ Code ************************************/
/*
    Initialize the time service
 */
PUBLIC int mprCreateTimeService()
{
    Mpr                 *mpr;
    TimeToken           *tt;

    mpr = MPR;
    mpr->timeTokens = mprCreateHash(59, MPR_HASH_STATIC_KEYS | MPR_HASH_STATIC_VALUES | MPR_HASH_STABLE);
    for (tt = days; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = fullDays; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = months; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = fullMonths; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = ampm; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = zones; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    for (tt = offsets; tt->name; tt++) {
        mprAddKey(mpr->timeTokens, tt->name, (void*) tt);
    }
    return 0;
}


PUBLIC int mprCompareTime(MprTime t1, MprTime t2)
{
    if (t1 < t2) {
        return -1;
    } else if (t1 == t2) {
        return 0;
    }
    return 1;
}


PUBLIC void mprDecodeLocalTime(struct tm *tp, MprTime when)
{
    decodeTime(tp, when, 1);
}


PUBLIC void mprDecodeUniversalTime(struct tm *tp, MprTime when)
{
    decodeTime(tp, when, 0);
}


PUBLIC char *mprGetDate(char *format)
{
    struct tm   tm;

    mprDecodeLocalTime(&tm, mprGetTime());
    if (format == 0 || *format == '\0') {
        format = MPR_DEFAULT_DATE;
    }
    return mprFormatTm(format, &tm);
}


PUBLIC char *mprFormatLocalTime(cchar *format, MprTime time)
{
    struct tm   tm;
    if (format == 0) {
        format = MPR_DEFAULT_DATE;
    }
    mprDecodeLocalTime(&tm, time);
    return mprFormatTm(format, &tm);
}


PUBLIC char *mprFormatUniversalTime(cchar *format, MprTime time)
{
    struct tm   tm;
    if (format == 0) {
        format = MPR_DEFAULT_DATE;
    }
    mprDecodeUniversalTime(&tm, time);
    return mprFormatTm(format, &tm);
}


/*
    Returns time in milliseconds since the epoch: 0:0:0 UTC Jan 1 1970.
 */
PUBLIC MprTime mprGetTime()
{
#if VXWORKS
    struct timespec  tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    return (MprTime) (((MprTime) tv.tv_sec) * 1000) + (tv.tv_nsec / (1000 * 1000));
#else
    struct timeval  tv;
    gettimeofday(&tv, NULL);
    return (MprTime) (((MprTime) tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
#endif
}


/*
    High resolution timer
 */
#if MPR_HIGH_RES_TIMER
    #if (LINUX || MACOSX) && (ME_CPU_ARCH == ME_CPU_X86 || ME_CPU_ARCH == ME_CPU_X64)
        uint64 mprGetHiResTicks() {
            uint64  now;
            __asm__ __volatile__ ("rdtsc" : "=A" (now));
            return now;
        }
    #elif WINDOWS
        uint64 mprGetHiResTicks() {
            LARGE_INTEGER  now;
            QueryPerformanceCounter(&now);
            return (((uint64) now.HighPart) << 32) + now.LowPart;
        }
    #else
        uint64 mprGetHiResTicks() {
            return (uint64) mprGetTicks();
        }
    #endif
#else 
    uint64 mprGetHiResTicks() {
        return (uint64) mprGetTicks();
    }
#endif

/*
    Ugh! Apparently monotonic clocks are broken on VxWorks prior to 6.7
 */
#if CLOCK_MONOTONIC_RAW
    #if ME_UNIX_LIKE
        #define HAS_MONOTONIC_RAW 1
    #elif VXWORKS
        #if (_WRS_VXWORKS_MAJOR > 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR >= 7))
            #define HAS_MONOTONIC_RAW 1
        #endif
    #endif
#endif
#if CLOCK_MONOTONIC
    #if ME_UNIX_LIKE
        #define HAS_MONOTONIC 1
    #elif VXWORKS
        #if (_WRS_VXWORKS_MAJOR > 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR >= 7))
            #define HAS_MONOTONIC 1
        #endif
    #endif
#endif

/*
    Return time in milliseconds that never goes backwards. This is used for timers and not for time of day.
    The actual value returned is system dependant and does not represent time since Jan 1 1970.
    It may drift from real-time over time.
 */
PUBLIC MprTicks mprGetTicks()
{
#if ME_WIN_LIKE && ME_64 && _WIN32_WINNT >= 0x0600
    /* Windows Vista and later. Test for 64-bit so that building on deprecated Windows XP will work */
    return GetTickCount64();
#elif MACOSX
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return mach_absolute_time() * info.numer / info.denom / (1000 * 1000);
#elif HAS_MONOTONIC_RAW
    /*
        Monotonic raw is the local oscillator. This may over time diverge from real time ticks.
        We prefer this to monotonic because the ticks will always increase by the same regular amount without adjustments.
     */
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC_RAW, &tv);
    return (MprTicks) (((MprTicks) tv.tv_sec) * 1000) + (tv.tv_nsec / (1000 * 1000));
#elif HAS_MONOTONIC
    /*
        Monotonic is the local oscillator with NTP gradual adjustments as NTP learns the differences between the local
        oscillator and NTP measured real clock time. i.e. it will adjust ticks over time to not lose ticks.
     */
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return (MprTicks) (((MprTicks) tv.tv_sec) * 1000) + (tv.tv_nsec / (1000 * 1000));
#else
    /*
        Last chance. Need to resort to mprGetTime which is subject to user and seasonal adjustments.
        This code will prevent it going backwards, but may suffer large jumps forward.
     */
    static MprTime lastTicks = 0;
    static MprTime adjustTicks = 0;
    static MprSpin ticksSpin;
    MprTime     result, diff;

    if (lastTicks == 0) {
        /* This will happen at init time when single threaded */
#if ME_WIN_LIKE
        lastTicks = GetTickCount();
#else
        lastTicks = mprGetTime();
#endif
        mprInitSpinLock(&ticksSpin);
    }
    mprSpinLock(&ticksSpin);
#if ME_WIN_LIKE
    /*
        GetTickCount will wrap in 49.7 days
     */
    result = GetTickCount() + adjustTicks;
#else
    result = mprGetTime() + adjustTicks;
#endif
    if ((diff = (result - lastTicks)) < 0) {
        /*
            Handle time reversals. Don't handle jumps forward. Sorry.
            Note: this is not time day, so it should not matter.
         */
        adjustTicks -= diff;
        result -= diff;
    }
    lastTicks = result;
    mprSpinUnlock(&ticksSpin);
    return result;
#endif
}


/*
    Return the number of milliseconds until the given timeout has expired.
 */
PUBLIC MprTicks mprGetRemainingTicks(MprTicks mark, MprTicks timeout)
{
    MprTicks    now, diff;

    now = mprGetTicks();
    diff = (now - mark);
    if (diff < 0) {
        /*
            Detect time going backwards. Should never happen now.
         */
        assert(diff >= 0);
        diff = 0;
    }
    return (timeout - diff);
}


PUBLIC MprTicks mprGetElapsedTicks(MprTicks mark)
{
    return mprGetTicks() - mark;
}


PUBLIC MprTime mprGetElapsedTime(MprTime mark)
{
    return mprGetTime() - mark;
}


/*
    Get the timezone offset including DST
    Return the timezone offset (including DST) in msec. local == (UTC + offset)
 */
PUBLIC int mprGetTimeZoneOffset(MprTime when)
{
    MprTime     alternate, secs;
    struct tm   t;
    int         offset;

    alternate = when;
    secs = when / MS_PER_SEC;
    if (secs < MIN_TIME || secs > MAX_TIME) {
        /* secs overflows time_t on this platform. Need to map to an alternate valid year */
        decodeTime(&t, when, 0);
        t.tm_year = 111;
        alternate = makeTime(&t);
    }
    t.tm_isdst = -1;
    if (localTime(&t, alternate) < 0) {
        localTime(&t, time(0) * MS_PER_SEC);
    }
    offset = getTimeZoneOffsetFromTm(&t);
    return offset;
}


/*
    Make a time value interpreting "tm" as a local time
 */
PUBLIC MprTime mprMakeTime(struct tm *tp)
{
    MprTime     when, alternate;
    struct tm   t;
    int         offset, year;

    when = makeTime(tp);
    year = tp->tm_year;
    if (MIN_YEAR <= year && year <= MAX_YEAR) {
        localTime(&t, when);
        offset = getTimeZoneOffsetFromTm(&t);
    } else {
        t = *tp;
        t.tm_year = 111;
        alternate = makeTime(&t);
        localTime(&t, alternate);
        offset = getTimeZoneOffsetFromTm(&t);
    }
    return when - offset;
}


PUBLIC MprTime mprMakeUniversalTime(struct tm *tp)
{
    return makeTime(tp);
}


/*************************************** O/S Layer ***********************************/

static int localTime(struct tm *timep, MprTime time)
{
#if ME_UNIX_LIKE
    time_t when = (time_t) (time / MS_PER_SEC);
    if (localtime_r(&when, timep) == 0) {
        return MPR_ERR;
    }
#else
    struct tm   *tp;
    time_t when = (time_t) (time / MS_PER_SEC);
    if ((tp = localtime(&when)) == 0) {
        return MPR_ERR;
    }
    *timep = *tp;
#endif
    return 0;
}


struct tm *universalTime(struct tm *timep, MprTime time)
{
#if ME_UNIX_LIKE
    time_t when = (time_t) (time / MS_PER_SEC);
    return gmtime_r(&when, timep);
#else
    struct tm   *tp;
    time_t      when;
    when = (time_t) (time / MS_PER_SEC);
    if ((tp = gmtime(&when)) == 0) {
        return 0;
    }
    *timep = *tp;
    return timep;
#endif
}


/*
    Return the timezone offset (including DST) in msec. local == (UTC + offset)
    Assumes a valid (local) "tm" with isdst correctly set.
 */
static int getTimeZoneOffsetFromTm(struct tm *tp)
{
#if ME_WIN_LIKE
    int                     offset;
    TIME_ZONE_INFORMATION   tinfo;
    GetTimeZoneInformation(&tinfo);
    offset = tinfo.Bias;
    if (tp->tm_isdst) {
        offset += tinfo.DaylightBias;
    } else {
        offset += tinfo.StandardBias;
    }
    return -offset * 60 * MS_PER_SEC;
#elif VXWORKS
    char  *tze, *p;
    int   offset = 0;
    if ((tze = getenv("TIMEZONE")) != 0) {
        if ((p = strchr(tze, ':')) != 0) {
            if ((p = strchr(tze, ':')) != 0) {
                offset = - stoi(++p) * MS_PER_MIN;
            }
        }
        if (tp->tm_isdst) {
            offset += MS_PER_HOUR;
        }
    }
    return offset;
#elif ME_UNIX_LIKE && !CYGWIN
    return (int) tp->tm_gmtoff * MS_PER_SEC;
#else
    struct timezone     tz;
    struct timeval      tv;
    int                 offset;
    gettimeofday(&tv, &tz);
    offset = -tz.tz_minuteswest * MS_PER_MIN;
    if (tp->tm_isdst) {
        offset += MS_PER_HOUR;
    }
    return offset;
#endif
}

/********************************* Calculations *********************************/
/*
    Convert "struct tm" to MprTime. This ignores GMT offset and DST.
 */
static MprTime makeTime(struct tm *tp)
{
    MprTime     days;
    int         year, month;

    year = tp->tm_year + 1900 + tp->tm_mon / 12; 
    month = tp->tm_mon % 12;
    if (month < 0) {
        month += 12;
        --year;
    }
    days = daysSinceEpoch(year);
    days += leapYear(year) ? leapMonthStart[month] : normalMonthStart[month];
    days += tp->tm_mday - 1;
    return (days * MS_PER_DAY) + ((((((tp->tm_hour * 60)) + tp->tm_min) * 60) + tp->tm_sec) * MS_PER_SEC);
}


static MprTime daysSinceEpoch(int year)
{
    MprTime     days;

    days = ((MprTime) 365) * (year - 1970);
    days += ((year-1) / 4) - (1970 / 4);
    days -= ((year-1) / 100) - (1970 / 100);
    days += ((year-1) / 400) - (1970 / 400);
    return days;
}


static int leapYear(int year)
{
    if (year % 4) {
        return 0;
    } else if (year % 400 == 0) {
        return 1;
    } else if (year % 100 == 0) {
        return 0;
    }
    return 1;
}


static int getMonth(int year, int day)
{
    int     *days, i;

    days = leapYear(year) ? leapMonthStart : normalMonthStart;
    for (i = 1; days[i]; i++) {
        if (day < days[i]) {
            return i - 1;
        }
    }
    return 11;
}


static int getYear(MprTime when)
{
    MprTime     ms;
    int         year;

    year = 1970 + (int) (when / MS_PER_YEAR);
    ms = daysSinceEpoch(year) * MS_PER_DAY;
    if (ms > when) {
        return year - 1;
    } else if (ms + (((MprTime) MS_PER_DAY) * (365 + leapYear(year))) <= when) {
        return year + 1;
    }
    return year;
}


PUBLIC MprTime floorDiv(MprTime x, MprTime divisor)
{
    if (x < 0) {
        return (x - divisor + 1) / divisor;
    } else {
        return x / divisor;
    }
}


/*
    Decode an MprTime into components in a "struct tm" 
 */
static void decodeTime(struct tm *tp, MprTime when, bool local)
{
    MprTime     timeForZoneCalc, secs;
    struct tm   t;
    char        *zoneName;
    int         year, offset, dst;

    zoneName = 0;
    offset = dst = 0;

    if (local) {
        //  OPT -- cache the results somehow
        timeForZoneCalc = when;
        secs = when / MS_PER_SEC;
        if (secs < MIN_TIME || secs > MAX_TIME) {
            /*
                On some systems, localTime won't work for very small (negative) or very large times. 
                Cannot be certain localTime will work for all O/Ss with this year.  Map to an a date with a valid year.
             */
            decodeTime(&t, when, 0);
            t.tm_year = 111;
            timeForZoneCalc = makeTime(&t);
        }
        t.tm_isdst = -1;
        if (localTime(&t, timeForZoneCalc) == 0) {
            offset = getTimeZoneOffsetFromTm(&t);
            dst = t.tm_isdst;
        }
#if ME_UNIX_LIKE && !CYGWIN
        zoneName = (char*) t.tm_zone;
#endif
        when += offset;
    }
    year = getYear(when);

    tp->tm_year     = year - 1900;
    tp->tm_hour     = (int) (floorDiv(when, MS_PER_HOUR) % 24);
    tp->tm_min      = (int) (floorDiv(when, MS_PER_MIN) % 60);
    tp->tm_sec      = (int) (floorDiv(when, MS_PER_SEC) % 60);
    tp->tm_wday     = (int) ((floorDiv(when, MS_PER_DAY) + 4) % 7);
    tp->tm_yday     = (int) (floorDiv(when, MS_PER_DAY) - daysSinceEpoch(year));
    tp->tm_mon      = getMonth(year, tp->tm_yday);
    tp->tm_isdst    = dst != 0;
#if ME_UNIX_LIKE && !CYGWIN
    tp->tm_gmtoff   = offset / MS_PER_SEC;
    tp->tm_zone     = zoneName;
#endif
    if (tp->tm_hour < 0) {
        tp->tm_hour += 24;
    }
    if (tp->tm_min < 0) {
        tp->tm_min += 60;
    }
    if (tp->tm_sec < 0) {
        tp->tm_sec += 60;
    }
    if (tp->tm_wday < 0) {
        tp->tm_wday += 7;
    }
    if (tp->tm_yday < 0) {
        tp->tm_yday += 365;
    }
    if (leapYear(year)) {
        tp->tm_mday = tp->tm_yday - leapMonthStart[tp->tm_mon] + 1;
    } else {
        tp->tm_mday = tp->tm_yday - normalMonthStart[tp->tm_mon] + 1;
    }
    assert(tp->tm_hour >= 0);
    assert(tp->tm_min >= 0);
    assert(tp->tm_sec >= 0);
    assert(tp->tm_wday >= 0);
    assert(tp->tm_mon >= 0);
    /* This asserts with some calculating some intermediate dates <= year 100 */
    assert(tp->tm_yday >= 0);
    assert(tp->tm_yday < 365 || (tp->tm_yday < 366 && leapYear(year)));
    assert(tp->tm_mday >= 1);
}


/********************************* Formatting **********************************/
/*
    Format a time string. This uses strftime if available and so the supported formats vary from platform to platform.
    Strftime should supports some of these these formats:

     %A      full weekday name (Monday)
     %a      abbreviated weekday name (Mon)
     %B      full month name (January)
     %b      abbreviated month name (Jan)
     %C      century. Year / 100. (0-N)
     %c      standard date and time representation
     %D      date (%m/%d/%y)
     %d      day-of-month (01-31)
     %e      day-of-month with a leading space if only one digit ( 1-31)
     %F      same as %Y-%m-%d
     %H      hour (24 hour clock) (00-23)
     %h      same as %b
     %I      hour (12 hour clock) (01-12)
     %j      day-of-year (001-366)
     %k      hour (24 hour clock) (0-23)
     %l      the hour (12-hour clock) as a decimal number (1-12); single digits are preceded by a blank.
     %M      minute (00-59)
     %m      month (01-12)
     %n      a newline
     %P      lower case am / pm
     %p      AM / PM
     %R      same as %H:%M
     %r      same as %H:%M:%S %p
     %S      second (00-59)
     %s      seconds since epoch
     %T      time (%H:%M:%S)
     %t      a tab.
     %U      week-of-year, first day sunday (00-53)
     %u      the weekday (Monday as the first day of the week) as a decimal number (1-7).
     %v      is equivalent to ``%e-%b-%Y''.
     %W      week-of-year, first day monday (00-53)
     %w      weekday (0-6, sunday is 0)
     %X      standard time representation
     %x      standard date representation
     %Y      year with century
     %y      year without century (00-99)
     %Z      timezone name
     %z      offset from UTC (-hhmm or +hhmm)
     %+      national representation of the date and time (the format is similar to that produced by date(1)).
     %%      percent sign

     Some platforms may also support the following format extensions:
     %E*     POSIX locale extensions. Where "*" is one of the characters: c, C, x, X, y, Y.
             representations. 
     %G      a year as a decimal number with century. This year is the one that contains the greater part of
             the week (Monday as the first day of the week).
     %g      the same year as in ``%G'', but as a decimal number without century (00-99).
     %O*     POSIX locale extensions. Where "*" is one of the characters: d, e, H, I, m, M, S, u, U, V, w, W, y.
             Additionly %OB implemented to represent alternative months names (used standalone, without day mentioned). 
     %V      the week number of the year (Monday as the first day of the week) as a decimal number (01-53). If the week 
             containing January 1 has four or more days in the new year, then it is week 1; otherwise it is the last 
             week of the previous year, and the next week is week 1.

    Useful formats:
        RFC822:  "%a, %d %b %Y %H:%M:%S %Z           "Fri, 07 Jan 2003 12:12:21 PDT"
                 "%T %F                              "12:12:21 2007-01-03"
                 "%v                                 "07-Jul-2003"
        RFC3399: "%FT%TZ"                            "1985-04-12T23:20:50.52Z"
 */

#if HAS_STRFTIME
/*
    Preferred implementation as strftime() will be localized
 */
PUBLIC char *mprFormatTm(cchar *format, struct tm *tp)
{
    struct tm       tm;
    cchar           *cp;
    char            localFmt[256], buf[256], *dp, *endp, *sign;
    ssize           size;
    int             value;

    dp = localFmt;
    if (format == 0) {
        format = MPR_DEFAULT_DATE;
    }
    if (tp == 0) {
        mprDecodeLocalTime(&tm, mprGetTime());
        tp = &tm;
    }
    endp = &localFmt[sizeof(localFmt) - 1];
    size = sizeof(localFmt) - 1;
    for (cp = format; *cp && dp < &localFmt[sizeof(localFmt) - 32]; size = (int) (endp - dp - 1)) {
        if (*cp == '%') {
            *dp++ = *cp++;
        again:
            switch (*cp) {
            case '+':
                if (tp->tm_mday < 10) {
                    /* Some platforms don't support 'e' so avoid it here. Put a double space before %d */
                    fmt(dp, size, "%s  %d %s", "a %b", tp->tm_mday, "%H:%M:%S %Z %Y");
                } else {
                    strcpy(dp, "a %b %d %H:%M:%S %Z %Y");
                }
                dp += slen(dp);
                cp++;
                break;

            case 'C':
                dp--;
                itosbuf(dp, size, (1900 + tp->tm_year) / 100, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'D':
                strcpy(dp, "m/%d/%y");
                dp += 7;
                cp++;
                break;

            case 'e':                       /* day of month (1-31). Single digits preceeded by blanks */
                dp--;
                if (tp->tm_mday < 10) {
                    *dp++ = ' ';
                }
                itosbuf(dp, size - 1, (int64) tp->tm_mday, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'E':
                /* Skip the 'E' */
                cp++;
                goto again;

            case 'F':
                strcpy(dp, "Y-%m-%d");
                dp += 7;
                cp++;
                break;

            case 'h':
                *dp++ = 'b';
                cp++;
                break;

            case 'k':
                dp--;
                if (tp->tm_hour < 10) {
                    *dp++ = ' ';
                }
                itosbuf(dp, size - 1, (int64) tp->tm_hour, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'l':
                dp--;
                value = tp->tm_hour;
                if (value < 10) {
                    *dp++ = ' ';
                }
                if (value > 12) {
                    value -= 12;
                }
                itosbuf(dp, size - 1, (int64) value, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'n':
                dp[-1] = '\n';
                cp++;
                break;

            case 'O':
                /* Skip the 'O' */
                cp++;
                goto again;

            case 'P':
                dp--;
                strcpy(dp, (tp->tm_hour > 11) ? "pm" : "am");
                dp += 2;
                cp++;
                break;

            case 'R':
                strcpy(dp, "H:%M");
                dp += 4;
                cp++;
                break;

            case 'r':
                strcpy(dp, "I:%M:%S %p");
                dp += 10;
                cp++;
                break;

            case 's':
                dp--;
                itosbuf(dp, size, (int64) mprMakeTime(tp) / MS_PER_SEC, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'T':
                strcpy(dp, "H:%M:%S");
                dp += 7;
                cp++;
                break;

            case 't':
                dp[-1] = '\t';
                cp++;
                break;

            case 'u':
                dp--;
                value = tp->tm_wday;
                if (value == 0) {
                    value = 7;
                }
                itosbuf(dp, size, (int64) value, 10);
                dp += slen(dp);
                cp++;
                break;

            case 'v':
                /* Inline '%e' */
                dp--;
                if (tp->tm_mday < 10) {
                    *dp++ = ' ';
                }
                itosbuf(dp, size - 1, (int64) tp->tm_mday, 10);
                dp += slen(dp);
                cp++;
                strcpy(dp, "-%b-%Y");
                dp += 6;
                break;

            case 'z':
                dp--;
                value = mprGetTimeZoneOffset(makeTime(tp)) / (MS_PER_SEC * 60);
                sign = (value < 0) ? "-" : "";
                if (value < 0) {
                    value = -value;
                }
                fmt(dp, size, "%s%02d%02d", sign, value / 60, value % 60);
                dp += slen(dp);
                cp++;
                break;

            default: 
                if (strchr(VALID_FMT, (int) *cp) != 0) {
                    *dp++ = *cp++;
                } else {
                    dp--;
                    cp++;
                }
                break;
            }
        } else {
            *dp++ = *cp++;
        }
    }
    *dp = '\0';
    format = localFmt;
    if (*format == '\0') {
        format = "%a %b %d %H:%M:%S %Z %Y";
    }
#if WINDOWS
    _putenv("TZ=");
    _tzset();
#endif
    if (strftime(buf, sizeof(buf) - 1, format, tp) > 0) {
        buf[sizeof(buf) - 1] = '\0';
        return sclone(buf);
    }
    return 0;
}


#else /* !HAS_STRFTIME */
/*
    This implementation is used only on platforms that don't support strftime. This version is not localized.
 */
static void digits(MprBuf *buf, int count, int fill, int value)
{
    char    tmp[32]; 
    int     i, j; 

    if (value < 0) {
        mprPutCharToBuf(buf, '-');
        value = -value;
    }
    for (i = 0; value && i < count; i++) { 
        tmp[i] = '0' + value % 10; 
        value /= 10; 
    } 
    if (fill) {
        for (j = i; j < count; j++) {
            mprPutCharToBuf(buf, fill);
        }
    }
    while (i-- > 0) {
        mprPutCharToBuf(buf, tmp[i]); 
    } 
}


static char *getTimeZoneName(struct tm *tp)
{
#if ME_WIN_LIKE
    TIME_ZONE_INFORMATION   tz;
    WCHAR                   *wzone;
    GetTimeZoneInformation(&tz);
    wzone = tp->tm_isdst ? tz.DaylightName : tz.StandardName;
    return mprToMulti(wzone);
#else
    tzset();
    return sclone(tp->tm_zone);
#endif
}


PUBLIC char *mprFormatTm(cchar *format, struct tm *tp)
{
    struct tm       tm;
    MprBuf          *buf;
    char            *zone;
    int             w, value;

    if (format == 0) {
        format = MPR_DEFAULT_DATE;
    }
    if (tp == 0) {
        mprDecodeLocalTime(&tm, mprGetTime());
        tp = &tm;
    }
    if ((buf = mprCreateBuf(64, -1)) == 0) {
        return 0;
    }
    while ((*format != '\0')) {
        if (*format++ != '%') {
            mprPutCharToBuf(buf, format[-1]);
            continue;
        }
    again:
        switch (*format++) {
        case '%' :                                      /* percent */
            mprPutCharToBuf(buf, '%');
            break;

        case '+' :                                      /* date (Mon May 18 23:29:50 PDT 2014) */
            mprPutStringToBuf(buf, abbrevDay[tp->tm_wday]);
            mprPutCharToBuf(buf, ' ');
            mprPutStringToBuf(buf, abbrevMonth[tp->tm_mon]);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 2, ' ', tp->tm_mday);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 2, '0', tp->tm_hour);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_sec);
            mprPutCharToBuf(buf, ' ');
            zone = getTimeZoneName(tp);
            mprPutStringToBuf(buf, zone);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 4, 0, tp->tm_year + 1900);
            break;

        case 'A' :                                      /* full weekday (Sunday) */
            mprPutStringToBuf(buf, day[tp->tm_wday]);
            break;

        case 'a' :                                      /* abbreviated weekday (Sun) */
            mprPutStringToBuf(buf, abbrevDay[tp->tm_wday]);
            break;

        case 'B' :                                      /* full month (January) */
            mprPutStringToBuf(buf, month[tp->tm_mon]);
            break;

        case 'b' :                                      /* abbreviated month (Jan) */
            mprPutStringToBuf(buf, abbrevMonth[tp->tm_mon]);
            break;

        case 'C' :                                      /* century number (19, 20) */
            digits(buf, 2, '0', (1900 + tp->tm_year) / 100);
            break;

        case 'c' :                                      /* preferred date+time in current locale */
            mprPutStringToBuf(buf, abbrevDay[tp->tm_wday]);
            mprPutCharToBuf(buf, ' ');
            mprPutStringToBuf(buf, abbrevMonth[tp->tm_mon]);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 2, ' ', tp->tm_mday);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 2, '0', tp->tm_hour);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_sec);
            mprPutCharToBuf(buf, ' ');
            digits(buf, 4, 0, tp->tm_year + 1900);
            break;

        case 'D' :                                      /* mm/dd/yy */
            digits(buf, 2, '0', tp->tm_mon + 1);
            mprPutCharToBuf(buf, '/');
            digits(buf, 2, '0', tp->tm_mday);
            mprPutCharToBuf(buf, '/');
            digits(buf, 2, '0', tp->tm_year - 100);
            break;

        case 'd' :                                      /* day of month (01-31) */
            digits(buf, 2, '0', tp->tm_mday);
            break;

        case 'E':
            /* Skip the 'E' */
            goto again;

        case 'e':                                       /* day of month (1-31). Single digits preceeded by a blank */
            digits(buf, 2, ' ', tp->tm_mday);
            break;

        case 'F':                                       /* %m/%d/%y */
            digits(buf, 4, 0, tp->tm_year + 1900);
            mprPutCharToBuf(buf, '-');
            digits(buf, 2, '0', tp->tm_mon + 1);
            mprPutCharToBuf(buf, '-');
            digits(buf, 2, '0', tp->tm_mday);
            break;

        case 'H':                                       /* hour using 24 hour clock (00-23) */
            digits(buf, 2, '0', tp->tm_hour);
            break;

        case 'h':                                       /* Same as %b */
            mprPutStringToBuf(buf, abbrevMonth[tp->tm_mon]);
            break;

        case 'I':                                       /* hour using 12 hour clock (00-01) */
            digits(buf, 2, '0', (tp->tm_hour % 12) ? tp->tm_hour % 12 : 12);
            break;

        case 'j':                                       /* julian day (001-366) */
            digits(buf, 3, '0', tp->tm_yday+1);
            break;

        case 'k':                                       /* hour (0-23). Single digits preceeded by a blank */
            digits(buf, 2, ' ', tp->tm_hour);
            break;

        case 'l':                                       /* hour (1-12). Single digits preceeded by a blank */
            digits(buf, 2, ' ', tp->tm_hour < 12 ? tp->tm_hour : (tp->tm_hour - 12));
            break;

        case 'M':                                       /* minute as a number (00-59) */
            digits(buf, 2, '0', tp->tm_min);
            break;

        case 'm':                                       /* month as a number (01-12) */
            digits(buf, 2, '0', tp->tm_mon + 1);
            break;

        case 'n':                                       /* newline */
            mprPutCharToBuf(buf, '\n');
            break;

        case 'O':
            /* Skip the 'O' */
            goto again;

        case 'p':                                       /* AM/PM */
            mprPutStringToBuf(buf, (tp->tm_hour > 11) ? "PM" : "AM");
            break;

        case 'P':                                       /* am/pm */
            mprPutStringToBuf(buf, (tp->tm_hour > 11) ? "pm" : "am");
            break;

        case 'R':
            digits(buf, 2, '0', tp->tm_hour);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            break;

        case 'r':
            digits(buf, 2, '0', (tp->tm_hour % 12) ? tp->tm_hour % 12 : 12);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_sec);
            mprPutCharToBuf(buf, ' ');
            mprPutStringToBuf(buf, (tp->tm_hour > 11) ? "PM" : "AM");
            break;

        case 'S':                                       /* seconds as a number (00-60) */
            digits(buf, 2, '0', tp->tm_sec);
            break;

        case 's':                                       /* seconds since epoch */
            mprPutToBuf(buf, "%d", mprMakeTime(tp) / MS_PER_SEC);
            break;

        case 'T':
            digits(buf, 2, '0', tp->tm_hour);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_sec);
            break;

        case 't':                                       /* Tab */
            mprPutCharToBuf(buf, '\t');
            break;

        case 'U':                                       /* week number (00-53. Staring with first Sunday */
            w = tp->tm_yday / 7;
            if (tp->tm_yday % 7 > tp->tm_wday) {
                w++;
            }
            digits(buf, 2, '0', w);
            break;

        case 'u':                                       /* Week day (1-7) */
            value = tp->tm_wday;
            if (value == 0) {
                value = 7;
            }
            digits(buf, 1, 0, tp->tm_wday == 0 ? 7 : tp->tm_wday);
            break;

        case 'v':                                       /* %e-%b-%Y */
            digits(buf, 2, ' ', tp->tm_mday);
            mprPutCharToBuf(buf, '-');
            mprPutStringToBuf(buf, abbrevMonth[tp->tm_mon]);
            mprPutCharToBuf(buf, '-');
            digits(buf, 4, '0', tp->tm_year + 1900);
            break;

        case 'W':                                       /* week number (00-53). Staring with first Monday */
            w = (tp->tm_yday + 7 - (tp->tm_wday ?  (tp->tm_wday - 1) : (7 - 1))) / 7;
            digits(buf, 2, '0', w);
            break;

        case 'w':                                       /* day of week (0-6) */
            digits(buf, 1, '0', tp->tm_wday);
            break;

        case 'X':                                       /* preferred time without date */
            digits(buf, 2, '0', tp->tm_hour);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_min);
            mprPutCharToBuf(buf, ':');
            digits(buf, 2, '0', tp->tm_sec);
            break;

        case 'x':                                      /* preferred date without time */
            digits(buf, 2, '0', tp->tm_mon + 1);
            mprPutCharToBuf(buf, '/');
            digits(buf, 2, '0', tp->tm_mday);
            mprPutCharToBuf(buf, '/');
            digits(buf, 2, '0', tp->tm_year + 1900);
            break;

        case 'Y':                                       /* year as a decimal including century (1900) */
            digits(buf, 4, '0', tp->tm_year + 1900);
            break;

        case 'y':                                       /* year without century (00-99) */
            digits(buf, 2, '0', tp->tm_year % 100);
            break;

        case 'Z':                                       /* Timezone */
            zone = getTimeZoneName(tp);
            mprPutStringToBuf(buf, zone);
            break;

        case 'z':
            value = mprGetTimeZoneOffset(makeTime(tp)) / (MS_PER_SEC * 60);
            if (value < 0) {
                mprPutCharToBuf(buf, '-');
                value = -value;
            }
            digits(buf, 2, '0', value / 60);
            digits(buf, 2, '0', value % 60);
            break;

        case 'g':
        case 'G':
        case 'V':
            break;

        default:
            mprPutCharToBuf(buf, '%');
            mprPutCharToBuf(buf, format[-1]);
            break;
        }
    }
    mprAddNullToBuf(buf);
    return sclone(mprGetBufStart(buf));
}
#endif /* HAS_STRFTIME */


/*************************************** Parsing ************************************/

static int lookupSym(cchar *token, int kind)
{
    TimeToken   *tt;

    if ((tt = (TimeToken*) mprLookupKey(MPR->timeTokens, token)) == 0) {
        return -1;
    }
    if (kind != (tt->value & TOKEN_MASK)) {
        return -1;
    }
    return tt->value & ~TOKEN_MASK;
}


static int getNum(char **token, int sep)
{
    int     num;

    if (*token == 0) {
        return 0;
    }

    num = atoi(*token);
    *token = strchr(*token, sep);
    if (*token) {
        *token += 1;
    }
    return num;
}


static int getNumOrSym(char **token, int sep, int kind, int *isAlpah)
{
    char    *cp;
    int     num;

    assert(token && *token);

    if (*token == 0) {
        return 0;
    }
    if (isalpha((uchar) **token)) {
        *isAlpah = 1;
        cp = strchr(*token, sep);
        if (cp) {
            *cp++ = '\0';
        }
        num = lookupSym(*token, kind);
        *token = cp;
        return num;
    }
    num = atoi(*token);
    *token = strchr(*token, sep);
    if (*token) {
        *token += 1;
    }
    *isAlpah = 0;
    return num;
}


static void swapDayMonth(struct tm *tp)
{
    int     tmp;

    tmp = tp->tm_mday;
    tp->tm_mday = tp->tm_mon;
    tp->tm_mon = tmp;
}


/*
    Parse the a date/time string according to the given zoneFlags and return the result in *time. Missing date items 
    may be provided via the defaults argument.
 */ 
PUBLIC int mprParseTime(MprTime *time, cchar *dateString, int zoneFlags, struct tm *defaults)
{
    TimeToken       *tt;
    struct tm       tm;
    char            *str, *next, *token, *cp, *sep;
    int64           value;
    int             kind, hour, min, negate, value1, value2, value3, alpha, alpha2, alpha3;
    int             dateSep, offset, zoneOffset, explicitZone, fullYear;

    if (!dateString) {
        dateString = "";
    }
    offset = 0;
    zoneOffset = 0;
    explicitZone = 0;
    sep = ", \t";
    cp = 0;
    next = 0;
    fullYear = 0;

    /*
        Set these mandatory values to -1 so we can tell if they are set to valid values
        WARNING: all the calculations use tm_year with origin 0, not 1900. It is fixed up below.
     */
    tm.tm_year = -MAXINT;
    tm.tm_mon = tm.tm_mday = tm.tm_hour = tm.tm_sec = tm.tm_min = tm.tm_wday = -1;
    tm.tm_min = tm.tm_sec = tm.tm_yday = -1;
#if ME_UNIX_LIKE && !CYGWIN
    tm.tm_gmtoff = 0;
    tm.tm_zone = 0;
#endif

    /*
        Set to -1 to try to determine if DST is in effect
     */
    tm.tm_isdst = -1;
    str = slower(dateString);

    /*
        Handle ISO dates: "2009-05-21t16:06:05.000z
     */
    if (strchr(str, ' ') == 0 && strchr(str, '-') && str[slen(str) - 1] == 'z') {
        for (cp = str; *cp; cp++) {
            if (*cp == '-') {
                *cp = '/';
            } else if (*cp == 't' && cp > str && isdigit((uchar) cp[-1]) && isdigit((uchar) cp[1]) ) {
                *cp = ' ';
            }
        }
    }
    token = stok(str, sep, &next);

    while (token && *token) {
        if (snumber(token)) {
            /*
                Parse either day of month or year. Priority to day of month. Format: <29> Jan <15> <2014>
             */ 
            value = stoi(token);
            if (value > 3000) {
                *time = value;
                return 0;
            } else if (value > 32 || (tm.tm_mday >= 0 && tm.tm_year == -MAXINT)) {
                if (value >= 1000) {
                    fullYear = 1;
                }
                tm.tm_year = (int) value - 1900;
            } else if (tm.tm_mday < 0) {
                tm.tm_mday = (int) value;
            }

        } else if ((*token == '+') || (*token == '-') ||
                ((strncmp(token, "gmt", 3) == 0 || strncmp(token, "utc", 3) == 0) &&
                ((cp = strchr(&token[3], '+')) != 0 || (cp = strchr(&token[3], '-')) != 0))) {
            /*
                Timezone. Format: [GMT|UTC][+-]NN[:]NN
             */
            if (!isalpha((uchar) *token)) {
                cp = token;
            }
            negate = *cp == '-' ? -1 : 1;
            cp++;
            hour = getNum(&cp, timeSep);
            if (hour >= 100) {
                hour /= 100;
            }
            min = getNum(&cp, timeSep);
            zoneOffset = negate * (hour * 60 + min);
            explicitZone = 1;

        } else if (isalpha((uchar) *token)) {
            if ((tt = (TimeToken*) mprLookupKey(MPR->timeTokens, token)) != 0) {
                kind = tt->value & TOKEN_MASK;
                value = tt->value & ~TOKEN_MASK; 
                switch (kind) {

                case TOKEN_DAY:
                    tm.tm_wday = (int) value;
                    break;

                case TOKEN_MONTH:
                    tm.tm_mon = (int) value;
                    break;

                case TOKEN_OFFSET:
                    /* Named timezones or symbolic names like: tomorrow, yesterday, next week ... */ 
                    /* Units are seconds */
                    offset += (int) value;
                    break;

                case TOKEN_ZONE:
                    zoneOffset = (int) value;
                    explicitZone = 1;
                    break;

                default:
                    /* Just ignore unknown values */
                    break;
                }
            }

        } else if ((cp = strchr(token, timeSep)) != 0 && isdigit((uchar) token[0])) {
            /*
                Time:  10:52[:23]
                Must not parse GMT-07:30
             */
            tm.tm_hour = getNum(&token, timeSep);
            tm.tm_min = getNum(&token, timeSep);
            tm.tm_sec = getNum(&token, timeSep);

        } else {
            dateSep = '/';
            if (strchr(token, dateSep) == 0) {
                dateSep = '-';
                if (strchr(token, dateSep) == 0) {
                    dateSep = '.';
                    if (strchr(token, dateSep) == 0) {
                        dateSep = 0;
                    }
                }
            }
            if (dateSep) {
                /*
                    Date:  07/28/2014, 07/28/08, Jan/28/2014, Jaunuary-28-2014, 28-jan-2014
                    Support order: dd/mm/yy, mm/dd/yy and yyyy/mm/dd
                    Support separators "/", ".", "-"
                 */
                value1 = getNumOrSym(&token, dateSep, TOKEN_MONTH, &alpha);
                value2 = getNumOrSym(&token, dateSep, TOKEN_MONTH, &alpha2);
                value3 = getNumOrSym(&token, dateSep, TOKEN_MONTH, &alpha3);

                if (value1 > 31) {
                    /* yy/mm/dd */
                    tm.tm_year = value1;
                    tm.tm_mon = value2;
                    tm.tm_mday = value3;

                } else if (value1 > 12 || alpha2) {
                    /* 
                        dd/mm/yy 
                        Cannot detect 01/02/03  This will be evaluated as Jan 2 2003 below.
                     */
                    tm.tm_mday = value1;
                    tm.tm_mon = value2;
                    tm.tm_year = value3;

                } else {
                    /*
                        The default to parse is mm/dd/yy unless the mm value is out of range
                     */
                    tm.tm_mon = value1;
                    tm.tm_mday = value2;
                    tm.tm_year = value3;
                }
            }
        }
        token = stok(NULL, sep, &next);
    }

    /*
        Y2K fix and rebias
     */
    if (0 <= tm.tm_year && tm.tm_year < 100 && !fullYear) {
        if (tm.tm_year < 50) {
            tm.tm_year += 2000;
        } else {
            tm.tm_year += 1900;
        }
    }
    if (tm.tm_year >= 1900) {
        tm.tm_year -= 1900;
    }

    /*
        Convert back to origin 0 for months
     */
    if (tm.tm_mon > 0) {
        tm.tm_mon--;
    }

    /*
        Validate and fill in missing items with defaults
     */
    validateTime(&tm, defaults);

    if (zoneFlags == MPR_LOCAL_TIMEZONE && !explicitZone) {
        *time = mprMakeTime(&tm);
    } else {
        *time = mprMakeUniversalTime(&tm);
        *time += -(zoneOffset * MS_PER_MIN);
    }
    *time += (offset * MS_PER_SEC);
    return 0;
}


static void validateTime(struct tm *tp, struct tm *defaults)
{
    struct tm   empty;

    /*
        Fix apparent day-mon-year ordering issues. Cannot fix everything!
     */
    if ((12 <= tp->tm_mon && tp->tm_mon <= 31) && 0 <= tp->tm_mday && tp->tm_mday <= 11) {
        /*
            Looks like day month are swapped
         */
        swapDayMonth(tp);
    }

    if (tp->tm_year != -MAXINT && tp->tm_mon >= 0 && tp->tm_mday >= 0 && tp->tm_hour >= 0) {
        /*  Everything defined */
        return;
    }

    /*
        Use empty time if missing
     */
    if (defaults == NULL) {
        memset(&empty, 0, sizeof(empty));
        defaults = &empty;
        empty.tm_mday = 1;
        empty.tm_year = 70;
    }
    if (tp->tm_hour < 0 && tp->tm_min < 0 && tp->tm_sec < 0) {
        tp->tm_hour = defaults->tm_hour;
        tp->tm_min = defaults->tm_min;
        tp->tm_sec = defaults->tm_sec;
    }

    /*
        Get weekday, if before today then make next week
     */
    if (tp->tm_wday >= 0 && tp->tm_year == -MAXINT && tp->tm_mon < 0 && tp->tm_mday < 0) {
        tp->tm_mday = defaults->tm_mday + (tp->tm_wday - defaults->tm_wday + 7) % 7;
        tp->tm_mon = defaults->tm_mon;
        tp->tm_year = defaults->tm_year;
    }

    /*
        Get month, if before this month then make next year
     */
    if (tp->tm_mon >= 0 && tp->tm_mon <= 11 && tp->tm_mday < 0) {
        if (tp->tm_year == -MAXINT) {
            tp->tm_year = defaults->tm_year + (((tp->tm_mon - defaults->tm_mon) < 0) ? 1 : 0);
        }
        tp->tm_mday = defaults->tm_mday;
    }

    /*
        Get date, if before current time then make tomorrow
     */
    if (tp->tm_hour >= 0 && tp->tm_year == -MAXINT && tp->tm_mon < 0 && tp->tm_mday < 0) {
        tp->tm_mday = defaults->tm_mday + ((tp->tm_hour - defaults->tm_hour) < 0 ? 1 : 0);
        tp->tm_mon = defaults->tm_mon;
        tp->tm_year = defaults->tm_year;
    }
    if (tp->tm_year == -MAXINT) {
        tp->tm_year = defaults->tm_year;
    }
    if (tp->tm_mon < 0) {
        tp->tm_mon = defaults->tm_mon;
    }
    if (tp->tm_mday < 0) {
        tp->tm_mday = defaults->tm_mday;
    }
    if (tp->tm_yday < 0) {
        tp->tm_yday = (leapYear(tp->tm_year + 1900) ? 
            leapMonthStart[tp->tm_mon] : normalMonthStart[tp->tm_mon]) + tp->tm_mday - 1;
    }
    if (tp->tm_hour < 0) {
        tp->tm_hour = defaults->tm_hour;
    }
    if (tp->tm_min < 0) {
        tp->tm_min = defaults->tm_min;
    }
    if (tp->tm_sec < 0) {
        tp->tm_sec = defaults->tm_sec;
    }
}


/********************************* Compatibility **********************************/
/*
    Compatibility for windows and VxWorks
 */
#if ME_WIN_LIKE || (VXWORKS && (_WRS_VXWORKS_MAJOR < 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR < 9)))

PUBLIC int gettimeofday(struct timeval *tv, struct timezone *tz)

{
    #if ME_WIN_LIKE
        FILETIME        fileTime;
        MprTime         now;
        static int      tzOnce;

        if (NULL != tv) {
            /* Convert from 100-nanosec units to microsectonds */
            GetSystemTimeAsFileTime(&fileTime);
            now = ((((MprTime) fileTime.dwHighDateTime) << BITS(uint)) + ((MprTime) fileTime.dwLowDateTime));
            now /= 10;

            now -= TIME_GENESIS;
            tv->tv_sec = (long) (now / 1000000);
            tv->tv_usec = (long) (now % 1000000);
        }
        if (NULL != tz) {
            TIME_ZONE_INFORMATION   zone;
            int                     rc, bias;
            rc = GetTimeZoneInformation(&zone);
            bias = (int) zone.Bias;
            if (rc == TIME_ZONE_ID_DAYLIGHT) {
                tz->tz_dsttime = 1;
            } else {
                tz->tz_dsttime = 0;
            }
            tz->tz_minuteswest = bias;
        }
        return 0;

    #elif VXWORKS
        struct tm       tm;
        struct timespec now;
        time_t          t;
        char            *tze, *p;
        int             rc;

        if ((rc = clock_gettime(CLOCK_REALTIME, &now)) == 0) {
            tv->tv_sec  = now.tv_sec;
            tv->tv_usec = (now.tv_nsec + 500) / MS_PER_SEC;
            if ((tze = getenv("TIMEZONE")) != 0) {
                if ((p = strchr(tze, ':')) != 0) {
                    if ((p = strchr(tze, ':')) != 0) {
                        tz->tz_minuteswest = stoi(++p);
                    }
                }
                t = tickGet();
                tz->tz_dsttime = (localtime_r(&t, &tm) == 0) ? tm.tm_isdst : 0;
            }
        }
        return rc;
    #endif
}
#endif /* ME_WIN_LIKE || VXWORKS */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/vxworks.c ************/


/**
    vxworks.c - Vxworks specific adaptions

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if VXWORKS
/*********************************** Code *************************************/

PUBLIC int mprCreateOsService()
{
    return 0;
}


PUBLIC int mprStartOsService()
{
    return 0;
}


PUBLIC void mprStopOsService()
{
}


#if _WRS_VXWORKS_MAJOR < 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR < 9)

PUBLIC int access(const char *path, int mode)
{
    struct stat sbuf;
    return stat((char*) path, &sbuf);
}
#endif


PUBLIC int mprGetRandomBytes(char *buf, int length, bool block)
{
    int     i;

    for (i = 0; i < length; i++) {
        buf[i] = (char) (mprGetTime() >> i);
    }
    return 0;
}


#if _WRS_VXWORKS_MAJOR < 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR < 9)
int mprFindVxSym(SYMTAB_ID sid, char *name, char **pvalue)
{
    SYM_TYPE    type;

    return symFindByName(sid, name, pvalue, &type);
}
#else

int mprFindVxSym(SYMTAB_ID sid, char *name, char **pvalue)
{
    SYMBOL_DESC     symDesc;

    memset(&symDesc, 0, sizeof(SYMBOL_DESC));
    symDesc.mask = SYM_FIND_BY_NAME;
    symDesc.name = name;

    if (symFind(sid, &symDesc) == ERROR) {
        return ERROR;
    }
    if (pvalue != NULL) {
        *pvalue = (char*) symDesc.value;
    }
    return OK;
}
#endif


PUBLIC int mprLoadNativeModule(MprModule *mp)
{
    MprModuleEntry  fn;
    MprPath         info;
    char            *at, *entry;
    void            *handle;
    int             fd;

    assert(mp);
    fn = 0;
    handle = 0;

    entry = mp->entry;
#if ME_CPU_ARCH == MPR_CPU_IX86 || ME_CPU_ARCH == MPR_CPU_IX64 || ME_CPU_ARCH == MPR_CPU_SH
    entry = sjoin("_", entry, NULL);
#endif
    if (!mp->entry || mprFindFxSym(sysSymTbl, entry, (char**) (void*) &fn) == -1) {
        if ((at = mprSearchForModule(mp->path)) == 0) {
            mprLog("error mpr", 0, "Cannot find module \"%s\", cwd: \"%s\", search path \"%s\"", mp->path, mprGetCurrentPath(),
                mprGetModuleSearchPath());
            return MPR_ERR_CANT_ACCESS;
        }
        mp->path = at;
        mprGetPathInfo(mp->path, &info);
        mp->modified = info.mtime;

        mprLog("info mpr", 4, "Loading native module %s", mp->path);
        if ((fd = open(mp->path, O_RDONLY, 0664)) < 0) {
            mprLog("error mpr", 0, "Cannot open module \"%s\"", mp->path);
            return MPR_ERR_CANT_OPEN;
        }
        handle = loadModule(fd, LOAD_GLOBAL_SYMBOLS);
        if (handle == 0) {
            close(fd);
            if (handle) {
                unldByModuleId(handle, 0);
            }
            mprLog("error mpr", 0, "Cannot load module %s", mp->path);
            return MPR_ERR_CANT_READ;
        }
        close(fd);
        mp->handle = handle;

    } else if (mp->entry) {
        mprLog("info mpr", 2, "Activating module %s", mp->name);
    }
    if (mp->entry) {
        if (mprFindVxSym(sysSymTbl, entry, (char**) (void*) &fn) == -1) {
            mprLog("error mpr", 0, "Cannot find symbol %s when loading %s", entry, mp->path);
            return MPR_ERR_CANT_READ;
        }
        if ((fn)(mp->moduleData, mp) < 0) {
            mprLog("error mpr", 0, "Initialization for %s failed.", mp->path);
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    return 0;
}


PUBLIC int mprUnloadNativeModule(MprModule *mp)
{
    unldByModuleId((MODULE_ID) mp->handle, 0);
    return 0;
}


PUBLIC void mprNap(MprTicks milliseconds)
{
    struct timespec timeout;
    int             rc;

    assert(milliseconds >= 0);
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_nsec = (milliseconds % 1000) * 1000000;
    do {
        rc = nanosleep(&timeout, &timeout);
    } while (rc < 0 && errno == EINTR);
}


PUBLIC void mprSetFilesLimit(int limit)
{
}


PUBLIC void mprSleep(MprTicks timeout)
{
    mprYield(MPR_YIELD_STICKY);
    mprNap(timeout);
    mprResetYield();
}


PUBLIC void mprWriteToOsLog(cchar *message, int level)
{
}


PUBLIC pid_t mprGetPid(void) 
{
    return (pid_t) taskIdSelf();
}


#if _WRS_VXWORKS_MAJOR < 6 || (_WRS_VXWORKS_MAJOR == 6 && _WRS_VXWORKS_MINOR < 9)

PUBLIC int fsync(int fd) { 
    return 0; 
}
#endif


PUBLIC int ftruncate(int fd, off_t offset) { 
    return 0; 
}


PUBLIC int usleep(uint msec)
{
    struct timespec     timeout;
    int                 rc;

    timeout.tv_sec = msec / (1000 * 1000);
    timeout.tv_nsec = msec % (1000 * 1000) * 1000;
    do {
        rc = nanosleep(&timeout, &timeout);
    } while (rc < 0 && errno == EINTR);
    return 0;
}


/*
    Create a routine to pull in the GCC support routines for double and int64 manipulations for some platforms. Do this
    incase modules reference these routines. Without this, the modules have to reference them. Which leads to multiple 
    defines if two modules include them. (Code to pull in moddi3, udivdi3, umoddi3)
 */
double  __mpr_floating_point_resolution(double a, double b, int64 c, int64 d, uint64 e, uint64 f) {
    a = a / b; a = a * b; c = c / d; c = c % d; e = e / f; e = e % f;
    c = (int64) a; d = (uint64) a; a = (double) c; a = (double) e;
    return (a == b) ? a : b;
}

#else
void vxworksDummy() {}
#endif /* VXWORKS */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/wait.c ************/


/*
    wait.c - Wait for I/O service.

    This module provides wait management for sockets and other file descriptors and allows users to create wait
    handlers which will be called when I/O events are detected. Multiple backends (one at a time) are supported.

    This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/***************************** Forward Declarations ***************************/

static void ioEvent(void *data, MprEvent *event);
static void manageWaitService(MprWaitService *ws, int flags);
static void manageWaitHandler(MprWaitHandler *wp, int flags);

/************************************ Code ************************************/
/*
    Initialize the service
 */
PUBLIC MprWaitService *mprCreateWaitService()
{
    MprWaitService  *ws;

    ws = mprAllocObj(MprWaitService, manageWaitService);
    if (ws == 0) {
        return 0;
    }
    MPR->waitService = ws;
    ws->handlers = mprCreateList(-1, 0);
    ws->mutex = mprCreateLock();
    ws->spin = mprCreateSpinLock();
    mprCreateNotifierService(ws);
    return ws;
}


static void manageWaitService(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->handlers);
        mprMark(ws->handlerMap);
        mprMark(ws->mutex);
        mprMark(ws->spin);
    }
#if ME_EVENT_NOTIFIER == MPR_EVENT_ASYNC
    mprManageAsync(ws, flags);
#endif
#if ME_EVENT_NOTIFIER == MPR_EVENT_EPOLL
    mprManageEpoll(ws, flags);
#endif
#if ME_EVENT_NOTIFIER == MPR_EVENT_KQUEUE
    mprManageKqueue(ws, flags);
#endif
#if ME_EVENT_NOTIFIER == MPR_EVENT_SELECT
    mprManageSelect(ws, flags);
#endif
}


PUBLIC void mprStopWaitService()
{
#if ME_WIN_LIKE
    MprWaitService  *ws;

    ws = MPR->waitService;
    if (ws) {
        mprDestroyWindowClass(ws->wclass);
        ws->wclass = 0;
    }
#endif
    MPR->waitService = 0;
}


static MprWaitHandler *initWaitHandler(MprWaitHandler *wp, int fd, int mask, MprDispatcher *dispatcher, void *proc, 
    void *data, int flags)
{
    MprWaitService  *ws;

    assert(fd >= 0);
    ws = MPR->waitService;

#if ME_DEBUG
    {
        MprWaitHandler  *op;
        int             index;

        for (ITERATE_ITEMS(ws->handlers, op, index)) {
            assert(op->fd >= 0);
            if (op->fd == fd) {
                mprLog("error mpr event", 0, "Duplicate fd in wait handlers");
            } else if (op->fd < 0) {
                mprLog("error mpr event", 0, "Invalid fd in wait handlers, probably forgot to call mprRemoveWaitHandler");
            }
        }
    }
#endif
    wp->fd              = fd;
    wp->notifierIndex   = -1;
    wp->dispatcher      = dispatcher;
    wp->proc            = proc;
    wp->flags           = 0;
    wp->handlerData     = data;
    wp->service         = ws;
    wp->flags           = flags;

    if (mprGetListLength(ws->handlers) >= FD_SETSIZE) {
        mprLog("error mpr event", 0, "Too many io handlers: %d", FD_SETSIZE);
        return 0;
    }
#if ME_UNIX_LIKE || VXWORKS
#if ME_EVENT_NOTIFIER == MPR_EVENT_SELECT
    if (fd >= FD_SETSIZE) {
        mprLog("error mpr event", 0, "File descriptor %d exceeds max io of %d", fd, FD_SETSIZE);
    }
#endif
#endif
    if (mask) {
        if (mprAddItem(ws->handlers, wp) < 0) {
            return 0;
        }
        mprNotifyOn(wp, mask);
    }
    return wp;
}


PUBLIC MprWaitHandler *mprCreateWaitHandler(int fd, int mask, MprDispatcher *dispatcher, void *proc, void *data, int flags)
{
    MprWaitHandler  *wp;

    assert(fd >= 0);

    if ((wp = mprAllocObj(MprWaitHandler, manageWaitHandler)) == 0) {
        return 0;
    }
    return initWaitHandler(wp, fd, mask, dispatcher, proc, data, flags);
}


static void manageWaitHandler(MprWaitHandler *wp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(wp->handlerData);
        mprMark(wp->event);
        mprMark(wp->dispatcher);
        mprMark(wp->requiredWorker);
        mprMark(wp->thread);
        mprMark(wp->callbackComplete);
    }
}


/*
    This is a special case API, it is called by finalizers such as closeSocket.
    It needs special handling for the shutdown case.
 */
PUBLIC void mprRemoveWaitHandler(MprWaitHandler *wp)
{
    if (wp) {
        if (!mprIsStopped()) {
            /* Avoid locking APIs during shutdown - the locks may have been freed */
            mprRemoveItem(wp->service->handlers, wp);
            if (wp->fd >= 0 && wp->desiredMask) {
                mprNotifyOn(wp, 0);
            }
        }
        wp->fd = INVALID_SOCKET;
    }
}


PUBLIC void mprDestroyWaitHandler(MprWaitHandler *wp)
{
    MprWaitService      *ws;

    if (wp == 0) {
        return;
    }
    ws = wp->service;
    lock(ws);
    if (wp->fd >= 0) {
        mprRemoveWaitHandler(wp);
        wp->fd = INVALID_SOCKET;
        if (wp->event) {
            mprRemoveEvent(wp->event);
            wp->event = 0;
        }
    }
    wp->dispatcher = 0;
    unlock(ws);
}


PUBLIC void mprQueueIOEvent(MprWaitHandler *wp)
{
    MprDispatcher   *dispatcher;
    MprEvent        *event;

    if (wp->flags & MPR_WAIT_NEW_DISPATCHER) {
        dispatcher = mprCreateDispatcher("IO", MPR_DISPATCHER_AUTO);
    } else if (wp->dispatcher) {
        dispatcher = wp->dispatcher;
    } else {
        dispatcher = mprGetDispatcher();
    }
    event = mprCreateEvent(dispatcher, "IOEvent", 0, ioEvent, wp->handlerData, MPR_EVENT_DONT_QUEUE);
    event->mask = wp->presentMask;
    event->handler = wp;
    wp->event = event;
    mprQueueEvent(dispatcher, event);
}


static void ioEvent(void *data, MprEvent *event)
{
    assert(event);
    assert(event->handler);

    event->handler->event = 0;
    event->handler->proc(data, event);
}


PUBLIC void mprWaitOn(MprWaitHandler *wp, int mask)
{
    lock(wp->service);
    if (mask != wp->desiredMask) {
        if (wp->flags & MPR_WAIT_RECALL_HANDLER) {
            wp->service->needRecall = 1;
        }
        mprNotifyOn(wp, mask);
    }
    unlock(wp->service);
}


/*
    Set a handler to be recalled without further I/O
 */
PUBLIC void mprRecallWaitHandlerByFd(Socket fd)
{
    MprWaitService  *ws;
    MprWaitHandler  *wp;
    int             index;

    ws = MPR->waitService;
    lock(ws);
    for (index = 0; (wp = (MprWaitHandler*) mprGetNextItem(ws->handlers, &index)) != 0; ) {
        if (wp->fd == fd) {
            wp->flags |= MPR_WAIT_RECALL_HANDLER;
            ws->needRecall = 1;
            mprWakeEventService();
            break;
        }
    }
    unlock(ws);
}


PUBLIC void mprRecallWaitHandler(MprWaitHandler *wp)
{
    MprWaitService  *ws;

    if (wp) {
        ws = MPR->waitService;
        lock(ws);
        wp->flags |= MPR_WAIT_RECALL_HANDLER;
        ws->needRecall = 1;
        mprWakeEventService();
        unlock(ws);
    }
}


/*
    Recall a handler which may have buffered data. Only called by notifiers.
 */
PUBLIC void mprDoWaitRecall(MprWaitService *ws)
{
    MprWaitHandler      *wp;
    int                 index;

    lock(ws);
    ws->needRecall = 0;
    for (index = 0; (wp = (MprWaitHandler*) mprGetNextItem(ws->handlers, &index)) != 0; ) {
        if ((wp->flags & MPR_WAIT_RECALL_HANDLER) && (wp->desiredMask & MPR_READABLE)) {
            wp->presentMask |= MPR_READABLE;
            wp->flags &= ~MPR_WAIT_RECALL_HANDLER;
            mprNotifyOn(wp, 0);
            mprQueueIOEvent(wp);
        }
    }
    unlock(ws);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/wide.c ************/


/**
    unicode.c - Unicode support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if ME_CHAR_LEN > 1
#if KEEP
/************************************ Code ************************************/
/*
    Format a number as a string. Support radix 10 and 16.
    Count is the length of buf in characters.
 */
PUBLIC wchar *itow(wchar *buf, ssize count, int64 value, int radix)
{
    wchar   numBuf[32];
    wchar   *cp, *dp, *endp;
    char    digits[] = "0123456789ABCDEF";
    int     negative;

    if (radix != 10 && radix != 16) {
        return 0;
    }
    cp = &numBuf[sizeof(numBuf)];
    *--cp = '\0';

    if (value < 0) {
        negative = 1;
        value = -value;
        count--;
    } else {
        negative = 0;
    }
    do {
        *--cp = digits[value % radix];
        value /= radix;
    } while (value > 0);

    if (negative) {
        *--cp = '-';
    }
    dp = buf;
    endp = &buf[count];
    while (dp < endp && *cp) {
        *dp++ = *cp++;
    }
    *dp++ = '\0';
    return buf;
}


PUBLIC wchar *wchr(wchar *str, int c)
{
    wchar   *s;

    if (str == NULL) {
        return 0;
    }
    for (s = str; *s; ) {
        if (*s == c) {
            return s;
        }
    }
    return 0;
}


PUBLIC int wcasecmp(wchar *s1, wchar *s2)
{
    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    return wncasecmp(s1, s2, max(slen(s1), slen(s2)));
}


PUBLIC wchar *wclone(wchar *str)
{
    wchar   *result, nullBuf[1];
    ssize   len, size;

    if (str == NULL) {
        nullBuf[0] = 0;
        str = nullBuf;
    }
    len = wlen(str);
    size = (len + 1) * sizeof(wchar);
    if ((result = mprAlloc(size)) != NULL) {
        memcpy(result, str, len * sizeof(wchar));
    }
    result[len] = '\0';
    return result;
}


PUBLIC int wcmp(wchar *s1, wchar *s2)
{
    if (s1 == s2) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    return wncmp(s1, s2, max(slen(s1), slen(s2)));
}


/*
    Count is the maximum number of characters to compare
 */
PUBLIC wchar *wncontains(wchar *str, wchar *pattern, ssize count)
{
    wchar   *cp, *s1, *s2;
    ssize   lim;

    assert(0 <= count && count < MAXINT);

    if (count < 0) {
        count = MAXINT;
    }
    if (str == 0) {
        return 0;
    }
    if (pattern == 0 || *pattern == '\0') {
        return str;
    }
    for (cp = str; *cp && count > 0; cp++, count--) {
        s1 = cp;
        s2 = pattern;
        for (lim = count; *s1 && *s2 && (*s1 == *s2) && lim > 0; lim--) {
            s1++;
            s2++;
        }
        if (*s2 == '\0') {
            return cp;
        }
    }
    return 0;
}


PUBLIC wchar *wcontains(wchar *str, wchar *pattern)
{
    return wncontains(str, pattern, -1);
}


/*
    count is the size of dest in characters
 */
PUBLIC ssize wcopy(wchar *dest, ssize count, wchar *src)
{
    ssize      len;

    assert(src);
    assert(dest);
    assert(0 < count && count < MAXINT);

    len = wlen(src);
    if (count <= len) {
        assert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    memcpy(dest, src, (len + 1) * sizeof(wchar));
    return len;
}


PUBLIC int wends(wchar *str, wchar *suffix)
{
    if (str == NULL || suffix == NULL) {
        return 0;
    }
    if (wncmp(&str[wlen(str) - wlen(suffix)], suffix, -1) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC wchar *wfmt(wchar *fmt, ...)
{
    va_list     ap;
    char        *mfmt, *mresult;

    assert(fmt);

    va_start(ap, fmt);
    mfmt = awtom(fmt, NULL);
    mresult = sfmtv(mfmt, ap);
    va_end(ap);
    return amtow(mresult, NULL);
}


PUBLIC wchar *wfmtv(wchar *fmt, va_list arg)
{
    char        *mfmt, *mresult;

    assert(fmt);
    mfmt = awtom(fmt, NULL);
    mresult = sfmtv(mfmt, arg);
    return amtow(mresult, NULL);
}


/*
    Compute a hash for a Unicode string 
    (Based on work by Paul Hsieh, see http://www.azillionmonkeys.com/qed/hash.html)
    Count is the length of name in characters
 */
PUBLIC uint whash(wchar *name, ssize count)
{
    uint    tmp, rem, hash;

    assert(name);
    assert(0 <= count && count < MAXINT);

    if (count < 0) {
        count = wlen(name);
    }
    hash = count;
    rem = count & 3;

    for (count >>= 2; count > 0; count--, name += 4) {
        hash  += name[0] | (name[1] << 8);
        tmp   =  ((name[2] | (name[3] << 8)) << 11) ^ hash;
        hash  =  (hash << 16) ^ tmp;
        hash  += hash >> 11;
    }
    switch (rem) {
    case 3: 
        hash += name[0] + (name[1] << 8);
        hash ^= hash << 16;
        hash ^= name[2] << 18;
        hash += hash >> 11;
        break;
    case 2: 
        hash += name[0] + (name[1] << 8);
        hash ^= hash << 11;
        hash += hash >> 17;
        break;
    case 1: 
        hash += name[0];
        hash ^= hash << 10;
        hash += hash >> 1;
    }
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;
    return hash;
}


/*
    Count is the length of name in characters
 */
PUBLIC uint whashlower(wchar *name, ssize count)
{
    uint    tmp, rem, hash;

    assert(name);
    assert(0 <= count && count < MAXINT);

    if (count < 0) {
        count = wlen(name);
    }
    hash = count;
    rem = count & 3;

    for (count >>= 2; count > 0; count--, name += 4) {
        hash  += tolower((uchar) name[0]) | (tolower((uchar) name[1]) << 8);
        tmp   =  ((tolower((uchar) name[2]) | (tolower((uchar) name[3]) << 8)) << 11) ^ hash;
        hash  =  (hash << 16) ^ tmp;
        hash  += hash >> 11;
    }
    switch (rem) {
    case 3: 
        hash += tolower((uchar) name[0]) + (tolower((uchar) name[1]) << 8);
        hash ^= hash << 16;
        hash ^= tolower((uchar) name[2]) << 18;
        hash += hash >> 11;
        break;
    case 2: 
        hash += tolower((uchar) name[0]) + (tolower((uchar) name[1]) << 8);
        hash ^= hash << 11;
        hash += hash >> 17;
        break;
    case 1: 
        hash += tolower((uchar) name[0]);
        hash ^= hash << 10;
        hash += hash >> 1;
    }
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;
    return hash;
}


PUBLIC wchar *wjoin(wchar *str, ...)
{
    wchar       *result;
    va_list     ap;

    va_start(ap, str);
    result = wrejoinv(NULL, str, ap);
    va_end(ap);
    return result;
}


PUBLIC wchar *wjoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp, nullBuf[1];
    int         required, len, blen;

    va_copy(ap, args);
    required = 1;
    blen = wlen(buf);
    if (buf) {
        required += blen;
    }
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprAlloc(required * sizeof(wchar))) == 0) {
        return 0;
    }
    dp = dest;
    if (buf) {
        wcopy(dp, required, buf);
        dp += blen;
        required -= blen;
    }
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        len = wlen(str);
        dp += len;
        required -= len;
        str = va_arg(ap, wchar*);
    }
    *dp = '\0';
    return dest;
}


/*
    Return the length of "s" in characters
 */
PUBLIC ssize wlen(wchar *s)
{
    ssize  i;

    i = 0;
    if (s) {
        while (*s) s++;
    }
    return i;
}


/*
    Map a string to lower case 
 */
PUBLIC wchar *wlower(wchar *str)
{
    wchar   *cp, *s;

    assert(str);

    if (str) {
        s = wclone(str);
        for (cp = s; *cp; cp++) {
            if (isupper((uchar) *cp)) {
                *cp = (wchar) tolower((uchar) *cp);
            }
        }
        str = s;
    }
    return str;
}


/*
    Count is the maximum number of characters to compare
 */
PUBLIC int wncasecmp(wchar *s1, wchar *s2, ssize count)
{
    int     rc;

    assert(0 <= count && count < MAXINT);

    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; count > 0 && *s1 && rc == 0; s1++, s2++, count--) {
        rc = tolower((uchar) *s1) - tolower((uchar) *s2);
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


/*
    Count is the maximum number of characters to compare
 */
PUBLIC int wncmp(wchar *s1, wchar *s2, ssize count)
{
    int     rc;

    assert(0 <= count && count < MAXINT);

    if (s1 == 0 && s2 == 0) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; count > 0 && *s1 && rc == 0; s1++, s2++, count--) {
        rc = *s1 - *s2;
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


/*
    This routine copies at most "count" characters from a string. It ensures the result is always null terminated and 
    the buffer does not overflow. DestCount is the maximum size of dest in characters.
    Returns MPR_ERR_WONT_FIT if the buffer is too small.
 */
PUBLIC ssize wncopy(wchar *dest, ssize destCount, wchar *src, ssize count)
{
    ssize      len;

    assert(dest);
    assert(src);
    assert(dest != src);
    assert(0 <= count && count < MAXINT);
    assert(0 < destCount && destCount < MAXINT);

    len = wlen(src);
    len = min(len, count);
    if (destCount <= len) {
        assert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    if (len > 0) {
        memcpy(dest, src, len * sizeof(wchar));
        dest[len] = '\0';
    } else {
        *dest = '\0';
        len = 0;
    } 
    return len;
}


PUBLIC wchar *wpbrk(wchar *str, wchar *set)
{
    wchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return str;
            }
        }
    }
    return 0;
}


PUBLIC wchar *wrchr(wchar *str, int c)
{
    wchar   *s;

    if (str == NULL) {
        return 0;
    }
    for (s = &str[wlen(str)]; *s; ) {
        if (*s == c) {
            return s;
        }
    }
    return 0;
}


PUBLIC wchar *wrejoin(wchar *buf, ...)
{
    wchar       *result;
    va_list     ap;

    va_start(ap, buf);
    result = wrejoinv(buf, buf, ap);
    va_end(ap);
    return result;
}


PUBLIC wchar *wrejoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp, nullBuf[1];
    int         required, len, n;

    va_copy(ap, args);
    len = wlen(buf);
    required = len + 1;
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprRealloc(buf, required * sizeof(wchar))) == 0) {
        return 0;
    }
    dp = &dest[len];
    required -= len;
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        n = wlen(str);
        dp += n;
        required -= n;
        str = va_arg(ap, wchar*);
    }
    assert(required >= 0);
    *dp = '\0';
    return dest;
}


PUBLIC ssize wspn(wchar *str, wchar *set)
{
    wchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return break;
            }
        }
        if (*str != *sp) {
            return break;
        }
    }
    return count;
}
 

PUBLIC int wstarts(wchar *str, wchar *prefix)
{
    if (str == NULL || prefix == NULL) {
        return 0;
    }
    if (wncmp(str, prefix, wlen(prefix)) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC int64 wtoi(wchar *str)
{
    return wtoiradix(str, 10, NULL);
}


PUBLIC int64 wtoiradix(wchar *str, int radix, int *err)
{
    char    *bp, buf[32];

    for (bp = buf; bp < &buf[sizeof(buf)]; ) {
        *bp++ = *str++;
    }
    buf[sizeof(buf) - 1] = 0;
    return stoiradix(buf, radix, err);
}


PUBLIC wchar *wtok(wchar *str, wchar *delim, wchar **last)
{
    wchar   *start, *end;
    ssize   i;

    start = str ? str : *last;

    if (start == 0) {
        *last = 0;
        return 0;
    }
    i = wspn(start, delim);
    start += i;
    if (*start == '\0') {
        *last = 0;
        return 0;
    }
    end = wpbrk(start, delim);
    if (end) {
        *end++ = '\0';
        i = wspn(end, delim);
        end += i;
    }
    *last = end;
    return start;
}


/*
    Count is the length in characters to extract
 */
PUBLIC wchar *wsub(wchar *str, ssize offset, ssize count)
{
    wchar   *result;
    ssize   size;

    assert(str);
    assert(offset >= 0);
    assert(0 <= count && count < MAXINT);

    if (str == 0) {
        return 0;
    }
    size = (count + 1) * sizeof(wchar);
    if ((result = mprAlloc(size)) == NULL) {
        return NULL;
    }
    wncopy(result, count + 1, &str[offset], count);
    return result;
}


PUBLIC wchar *wtrim(wchar *str, wchar *set, int where)
{
    wchar   s;
    ssize   len, i;

    if (str == NULL || set == NULL) {
        return str;
    }
    s = wclone(str);
    if (where & MPR_TRIM_START) {
        i = wspn(s, set);
    } else {
        i = 0;
    }
    s += i;
    if (where & MPR_TRIM_END) {
        len = wlen(s);
        while (len > 0 && wspn(&s[len - 1], set) > 0) {
            s[len - 1] = '\0';
            len--;
        }
    }
    return s;
}


/*
    Map a string to upper case
 */
PUBLIC char *wupper(wchar *str)
{
    wchar   *cp, *s;

    assert(str);
    if (str) {
        s = wclone(str);
        for (cp = s; *cp; cp++) {
            if (islower((uchar) *cp)) {
                *cp = (wchar) toupper((uchar) *cp);
            }
        }
        str = s;
    }
    return str;
}
#endif /* KEEP */

/*********************************** Conversions *******************************/
/*
    Convert a wide unicode string into a multibyte string buffer. If count is supplied, it is used as the source length 
    in characters. Otherwise set to -1. DestCount is the max size of the dest buffer in bytes. At most destCount - 1 
    characters will be stored. The dest buffer will always have a trailing null appended.  If dest is NULL, don't copy 
    the string, just return the length of characters. Return a count of bytes copied to the destination or -1 if an 
    invalid unicode sequence was provided in src.
    NOTE: does not allocate.
 */
PUBLIC ssize wtom(char *dest, ssize destCount, wchar *src, ssize count)
{
    ssize   len;

    if (destCount < 0) {
        destCount = MAXSSIZE;
    }
    if (count > 0) {
#if ME_CHAR_LEN == 1
        if (dest) {
            len = scopy(dest, destCount, src);
        } else {
            len = min(slen(src), destCount - 1);
        }
#elif ME_WIN_LIKE
        len = WideCharToMultiByte(CP_ACP, 0, src, count, dest, (DWORD) destCount - 1, NULL, NULL);
#else
        len = wcstombs(dest, src, destCount - 1);
#endif
        if (dest) {
            if (len >= 0) {
                dest[len] = 0;
            }
        } else if (len >= destCount) {
            assert(!MPR_ERR_WONT_FIT);
            return MPR_ERR_WONT_FIT;
        }
    }
    return len;
}


/*
    Convert a multibyte string to a unicode string. If count is supplied, it is used as the source length in bytes.
    Otherwise set to -1. DestCount is the max size of the dest buffer in characters. At most destCount - 1 
    characters will be stored. The dest buffer will always have a trailing null characters appended.  If dest is NULL, 
    don't copy the string, just return the length of characters. Return a count of characters copied to the destination 
    or -1 if an invalid multibyte sequence was provided in src.
    NOTE: does not allocate.
 */
PUBLIC ssize mtow(wchar *dest, ssize destCount, cchar *src, ssize count) 
{
    ssize      len;

    if (destCount < 0) {
        destCount = MAXSSIZE;
    }
    if (destCount > 0) {
#if ME_CHAR_LEN == 1
        if (dest) {
            len = scopy(dest, destCount, src);
        } else {
            len = min(slen(src), destCount - 1);
        }
#elif ME_WIN_LIKE
        len = MultiByteToWideChar(CP_ACP, 0, src, count, dest, (DWORD) destCount - 1);
#else
        len = mbstowcs(dest, src, destCount - 1);
#endif
        if (dest) {
            if (len >= 0) {
                dest[len] = 0;
            }
        } else if (len >= destCount) {
            assert(!MPR_ERR_WONT_FIT);
            return MPR_ERR_WONT_FIT;
        }
    }
    return len;
}


PUBLIC wchar *amtow(cchar *src, ssize *lenp)
{
    wchar   *dest;
    ssize   len;

    len = mtow(NULL, MAXSSIZE, src, -1);
    if (len < 0) {
        return NULL;
    }
    if ((dest = mprAlloc((len + 1) * sizeof(wchar))) != NULL) {
        mtow(dest, len + 1, src, -1);
    }
    if (lenp) {
        *lenp = len;
    }
    return dest;
}


//  KEEP UNICODE - need a version that can supply a length

PUBLIC char *awtom(wchar *src, ssize *lenp)
{
    char    *dest;
    ssize   len;

    len = wtom(NULL, MAXSSIZE, src, -1);
    if (len < 0) {
        return NULL;
    }
    if ((dest = mprAlloc(len + 1)) != 0) {
        wtom(dest, len + 1, src, -1);
    }
    if (lenp) {
        *lenp = len;
    }
    return dest;
}


#if KEEP

#define BOM_MSB_FIRST       0xFEFF
#define BOM_LSB_FIRST       0xFFFE

/*
    Surrogate area  (0xD800 <= x && x <= 0xDFFF) => mapped into 0x10000 ... 0x10FFFF
 */

static int utf8Length(int c)
{
    if (c & 0x80) {
        return 1;
    }
    if ((c & 0xc0) != 0xc0) {
        return 0;
    }
    if ((c & 0xe0) != 0xe0) {
        return 2;
    }
    if ((c & 0xf0) != 0xf0) {
        return 3;
    }
    if ((c & 0xf8) != 0xf8) {
        return 4;
    }
    return 0;
}


static int isValidUtf8(cuchar *src, int len)
{
    if (len == 4 && (src[4] < 0x80 || src[3] > 0xBF)) {
        return 0;
    }
    if (len >= 3 && (src[3] < 0x80 || src[2] > 0xBF)) {
        return 0;
    }
    if (len >= 2 && src[1] > 0xBF) {
        return 0;
    }
    if (src[0]) {
        if (src[0] == 0xE0) {
            if (src[1] < 0xA0) {
                return 0;
            }
        } else if (src[0] == 0xED) {
            if (src[1] < 0xA0) {
                return 0;
            }
        } else if (src[0] == 0xF0) {
            if (src[1] < 0xA0) {
                return 0;
            }
        } else if (src[0] == 0xF4) {
            if (src[1] < 0xA0) {
                return 0;
            }
        } else if (src[1] < 0x80) {
            return 0;
        }
    }
    if (len >= 1) {
        if (src[0] >= 0x80 && src[0] < 0xC2) {
            return 0;
        }
    }
    if (src[0] >= 0xF4) {
        return 0;
    }
    return 1;
}


static int offsets[6] = { 0x00000000UL, 0x00003080UL, 0x000E2080UL, 0x03C82080UL, 0xFA082080UL, 0x82082080UL };

PUBLIC ssize xmtow(wchar *dest, ssize destMax, cchar *src, ssize len) 
{
    wchar   *dp, *dend;
    cchar   *sp, *send;
    int     i, c, count;

    assert(0 <= len && len < MAXINT);

    if (len < 0) {
        len = slen(src);
    }
    if (dest) {
        dend = &dest[destMax];
    }
    count = 0;
    for (sp = src, send = &src[len]; sp < send; ) {
        len = utf8Length(*sp) - 1;
        if (&sp[len] >= send) {
            return MPR_ERR_BAD_FORMAT;
        }
        if (!isValidUtf8((uchar*) sp, len + 1)) {
            return MPR_ERR_BAD_FORMAT;
        }
        for (c = 0, i = len; i >= 0; i--) {
            c = *sp++;
            c <<= 6;
        }
        c -= offsets[len];
        count++;
        if (dp >= dend) {
            assert(!MPR_ERR_WONT_FIT);
            return MPR_ERR_WONT_FIT;
        }
        if (c <= 0xFFFF) {
            if (dest) {
                if (c >= 0xD800 && c <= 0xDFFF) {
                    *dp++ = 0xFFFD;
                } else {
                    *dp++ = c;
                }
            }
        } else if (c > 0x10FFFF) {
            *dp++ = 0xFFFD;
        } else {
            c -= 0x0010000UL;
            *dp++ = (c >> 10) + 0xD800;
            if (dp >= dend) {
                assert(!MPR_ERR_WONT_FIT);
                return MPR_ERR_WONT_FIT;
            }
            *dp++ = (c & 0x3FF) + 0xDC00;
            count++;
        }
    }
    return count;
}

static cuchar marks[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

/*
   if (c < 0x80) 
      b1 = c >> 0  & 0x7F | 0x00
      b2 = null
      b3 = null
      b4 = null
   else if (c < 0x0800)
      b1 = c >> 6  & 0x1F | 0xC0
      b2 = c >> 0  & 0x3F | 0x80
      b3 = null
      b4 = null
   else if (c < 0x010000)
      b1 = c >> 12 & 0x0F | 0xE0
      b2 = c >> 6  & 0x3F | 0x80
      b3 = c >> 0  & 0x3F | 0x80
      b4 = null
   else if (c < 0x110000)
      b1 = c >> 18 & 0x07 | 0xF0
      b2 = c >> 12 & 0x3F | 0x80
      b3 = c >> 6  & 0x3F | 0x80
      b4 = c >> 0  & 0x3F | 0x80
   end if
*/

PUBLIC ssize xwtom(char *dest, ssize destMax, wchar *src, ssize len)
{
    wchar   *sp, *send;
    char    *dp, *dend;
    int     i, c, c2, count, bytes, mark, mask;

    assert(0 <= len && len < MAXINT);

    if (len < 0) {
        len = wlen(src);
    }
    if (dest) {
        dend = &dest[destMax];
    }
    count = 0;
    mark = 0x80;
    mask = 0xBF;
    for (sp = src, send = &src[len]; sp < send; ) {
        c = *sp++;
        if (c >= 0xD800 && c <= 0xD8FF) {
            if (sp < send) {
                c2 = *sp++;
                if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                    c = ((c - 0xD800) << 10) + (c2 - 0xDC00) + 0x10000;
                }
            } else {
                assert(!MPR_ERR_WONT_FIT);
                return MPR_ERR_WONT_FIT;
            }
        }
        if (c < 0x80) {
            bytes = 1;
        } else if (c < 0x10000) {
            bytes = 2;
        } else if (c < 0x110000) {
            bytes = 4;
        } else {
            bytes = 3;
            c = 0xFFFD;
        }
        if (dest) {
            dp += bytes;
            if (dp >= dend) {
                assert(!MPR_ERR_WONT_FIT);
                return MPR_ERR_WONT_FIT;
            }
            for (i = 1; i < bytes; i++) {
                *--dp = (c | mark) & mask;
                c >>= 6;
            }
            *--dp = (c | marks[bytes]);
            dp += bytes;
        }
        count += bytes;
    }
    return count;
}


#endif /* KEEP */

#else /* ME_CHAR_LEN == 1 */

PUBLIC wchar *amtow(cchar *src, ssize *len)
{
    if (len) {
        *len = slen(src);
    }
    return (wchar*) sclone(src);
}


PUBLIC char *awtom(wchar *src, ssize *len)
{
    if (len) {
        *len = slen((char*) src);
    }
    return sclone((char*) src);
}


#endif /* ME_CHAR_LEN > 1 */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/win.c ************/


/**
    win.c - Windows specific adaptions

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if CYGWIN
 #include "w32api/windows.h"
#endif

#if ME_WIN_LIKE
/*********************************** Code *************************************/
/*
    Initialize the O/S platform layer
 */ 

PUBLIC int mprCreateOsService()
{
    WSADATA     wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }
    return 0;
}


PUBLIC int mprStartOsService()
{
    return 0;
}


PUBLIC void mprStopOsService()
{
    WSACleanup();
}


PUBLIC long mprGetInst()
{
    return (long) MPR->appInstance;
}


PUBLIC HWND mprGetHwnd()
{
    return MPR->waitService->hwnd;
}


PUBLIC int mprGetRandomBytes(char *buf, ssize length, bool block)
{
    HCRYPTPROV      prov;
    int             rc;

    rc = 0;
    if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | 0x40)) {
        return mprGetError();
    }
    if (!CryptGenRandom(prov, (wsize) length, buf)) {
        rc = mprGetError();
    }
    CryptReleaseContext(prov, 0);
    return rc;
}


PUBLIC int mprLoadNativeModule(MprModule *mp)
{
    MprModuleEntry  fn;
    void            *handle;

    assert(mp);

    if ((handle = (HANDLE) MPR->appInstance) == 0) {
        handle = GetModuleHandle(NULL);
    }
    if (!handle || !mp->entry || !GetProcAddress(handle, mp->entry)) {
#if ME_STATIC
        mprLog("error mpr", 0, "Cannot load module %s, product built static", mp->name);
        return MPR_ERR_BAD_STATE;
#else
        MprPath info;
        char    *at, *baseName;
        if ((at = mprSearchForModule(mp->path)) == 0) {
            mprLog("error mpr", 0, "Cannot find module \"%s\", cwd=\"%s\", search=\"%s\"", mp->path, mprGetCurrentPath(),
                mprGetModuleSearchPath());
            return MPR_ERR_CANT_ACCESS;
        }
        mp->path = at;
        mprGetPathInfo(mp->path, &info);
        mp->modified = info.mtime;
        baseName = mprGetPathBase(mp->path);
        mprLog("info mpr", 4, "Loading native module %s", baseName);
        if ((handle = LoadLibrary(wide(mp->path))) == 0) {
            mprLog("error mpr", 0, "Cannot load module %s, errno=\"%d\"", mp->path, mprGetOsError());
            return MPR_ERR_CANT_READ;
        } 
        mp->handle = handle;
#endif /* !ME_STATIC */

    } else if (mp->entry) {
        mprLog("info mpr", 4, "Activating native module %s", mp->name);
    }
    if (mp->entry) {
        if ((fn = (MprModuleEntry) GetProcAddress((HINSTANCE) handle, mp->entry)) == 0) {
            mprLog("error mpr", 0, "Cannot load module %s, cannot find function \"%s\"", mp->name, mp->entry);
            FreeLibrary((HINSTANCE) handle);
            return MPR_ERR_CANT_ACCESS;
        }
        if ((fn)(mp->moduleData, mp) < 0) {
            mprLog("error mpr", 0, "Initialization for module %s failed", mp->name);
            FreeLibrary((HINSTANCE) handle);
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    return 0;
}


PUBLIC int mprUnloadNativeModule(MprModule *mp)
{
    assert(mp->handle);

    if (FreeLibrary((HINSTANCE) mp->handle) == 0) {
        return MPR_ERR_ABORTED;
    }
    return 0;
}


PUBLIC void mprSetFilesLimit(int limit)
{
}


PUBLIC void mprSetInst(HINSTANCE inst)
{
    MPR->appInstance = inst;
}


PUBLIC void mprSetHwnd(HWND h)
{
    MPR->waitService->hwnd = h;
}


PUBLIC void mprSetSocketMessage(int socketMessage)
{
    MPR->waitService->socketMessage = socketMessage;
}


PUBLIC void mprNap(MprTicks timeout)
{
    Sleep((int) timeout);
}


PUBLIC void mprSleep(MprTicks timeout)
{
    mprYield(MPR_YIELD_STICKY);
    mprNap(timeout);
    mprResetYield();
}


PUBLIC void mprWriteToOsLog(cchar *message, int level)
{
    HKEY        hkey;
    void        *event;
    long        errorType;
    ulong       exists;
    char        buf[ME_MAX_BUFFER], logName[ME_MAX_BUFFER], *cp, *value;
    wchar       *lines[9];
    int         type;
    static int  once = 0;

    scopy(buf, sizeof(buf), message);
    cp = &buf[slen(buf) - 1];
    while (*cp == '\n' && cp > buf) {
        *cp-- = '\0';
    }
    type = EVENTLOG_ERROR_TYPE;
    lines[0] = wide(buf);
    lines[1] = 0;
    lines[2] = lines[3] = lines[4] = lines[5] = 0;
    lines[6] = lines[7] = lines[8] = 0;

    if (once == 0) {
        /*  Initialize the registry */
        once = 1;
        fmt(logName, sizeof(logName), "SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application\\%s", mprGetAppName());
        hkey = 0;

        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, wide(logName), 0, NULL, 0, KEY_ALL_ACCESS, NULL, 
                &hkey, &exists) == ERROR_SUCCESS) {
            value = "%SystemRoot%\\System32\\netmsg.dll";
            if (RegSetValueEx(hkey, UT("EventMessageFile"), 0, REG_EXPAND_SZ, (uchar*) value, 
                    (int) slen(value) + 1) != ERROR_SUCCESS) {
                RegCloseKey(hkey);
                return;
            }
            errorType = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE | EVENTLOG_INFORMATION_TYPE;
            if (RegSetValueEx(hkey, UT("TypesSupported"), 0, REG_DWORD, (uchar*) &errorType, 
                    sizeof(DWORD)) != ERROR_SUCCESS) {
                RegCloseKey(hkey);
                return;
            }
            RegCloseKey(hkey);
        }
    }

    event = RegisterEventSource(0, wide(mprGetAppName()));
    if (event) {
        /*
            3299 is the event number for the generic message in netmsg.dll.
            "%1 %2 %3 %4 %5 %6 %7 %8 %9" -- thanks Apache for the tip
         */
        ReportEvent(event, EVENTLOG_ERROR_TYPE, 0, 3299, NULL, sizeof(lines) / sizeof(wchar*), 0, lines, 0);
        DeregisterEventSource(event);
    }
}


#endif /* ME_WIN_LIKE */


#if ME_WIN_LIKE || CYGWIN
/*
    Determine the registry hive by the first portion of the path. Return 
    a pointer to the rest of key path after the hive portion.
 */ 
static cchar *getHive(cchar *keyPath, HKEY *hive)
{
    char    key[ME_MAX_PATH], *cp;
    ssize   len;

    assert(keyPath && *keyPath);

    *hive = 0;

    scopy(key, sizeof(key), keyPath);
    key[sizeof(key) - 1] = '\0';

    if ((cp = schr(key, '\\')) != 0) {
        *cp++ = '\0';
    }
    if (cp == 0 || *cp == '\0') {
        return 0;
    }
    if (!scaselesscmp(key, "HKEY_LOCAL_MACHINE") || !scaselesscmp(key, "HKLM")) {
        *hive = HKEY_LOCAL_MACHINE;
    } else if (!scaselesscmp(key, "HKEY_CURRENT_USER") || !scaselesscmp(key, "HKCU")) {
        *hive = HKEY_CURRENT_USER;
    } else if (!scaselesscmp(key, "HKEY_USERS")) {
        *hive = HKEY_USERS;
    } else if (!scaselesscmp(key, "HKEY_CLASSES_ROOT")) {
        *hive = HKEY_CLASSES_ROOT;
    } else {
        return 0;
    }
    if (*hive == 0) {
        return 0;
    }
    len = slen(key) + 1;
    return keyPath + len;
}


PUBLIC MprList *mprListRegistry(cchar *key)
{
    HKEY        top, h;
    wchar       name[ME_MAX_PATH];
    MprList     *list;
    int         index, size;

    assert(key && *key);

    /*
        Get the registry hive
     */
    if ((key = getHive(key, &top)) == 0) {
        return 0;
    }
    if (RegOpenKeyEx(top, wide(key), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return 0;
    }
    list = mprCreateList(0, 0);
    index = 0; 
    while (1) {
        size = sizeof(name) / sizeof(wchar);
        if (RegEnumValue(h, index, name, &size, 0, NULL, NULL, NULL) != ERROR_SUCCESS) {
            break;
        }
        mprAddItem(list, sclone(multi(name)));
        index++;
    }
    RegCloseKey(h);
    return list;
}


PUBLIC char *mprReadRegistry(cchar *key, cchar *name)
{
    HKEY        top, h;
    char        *value;
    ulong       type, size;

    assert(key && *key);

    /*
        Get the registry hive
     */
    if ((key = getHive(key, &top)) == 0) {
        return 0;
    }
    if (RegOpenKeyEx(top, wide(key), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return 0;
    }

    /*
        Get the type
     */
    if (RegQueryValueEx(h, wide(name), 0, &type, 0, &size) != ERROR_SUCCESS) {
        RegCloseKey(h);
        return 0;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        RegCloseKey(h);
        return 0;
    }
    if ((value = mprAlloc(size + 1)) == 0) {
        return 0;
    }
    if (RegQueryValueEx(h, wide(name), 0, &type, (uchar*) value, &size) != ERROR_SUCCESS) {
        RegCloseKey(h);
        return 0;
    }
    RegCloseKey(h);
    value[size] = '\0';
    return value;
}


PUBLIC int mprWriteRegistry(cchar *key, cchar *name, cchar *value)
{
    HKEY    top, h, subHandle;
    ulong   disposition;

    assert(key && *key);
    assert(value && *value);

    /*
        Get the registry hive
     */
    if ((key = getHive(key, &top)) == 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    if (name && *name) {
        /*
            Write a registry string value
         */
        if (RegOpenKeyEx(top, wide(key), 0, KEY_ALL_ACCESS, &h) != ERROR_SUCCESS) {
            return MPR_ERR_CANT_ACCESS;
        }
        if (RegSetValueEx(h, wide(name), 0, REG_SZ, (uchar*) value, (int) slen(value) + 1) != ERROR_SUCCESS) {
            RegCloseKey(h);
            return MPR_ERR_CANT_READ;
        }

    } else {
        /*
            Create a new sub key
         */
        if (RegOpenKeyEx(top, wide(key), 0, KEY_CREATE_SUB_KEY, &h) != ERROR_SUCCESS){
            return MPR_ERR_CANT_ACCESS;
        }
        if (RegCreateKeyEx(h, wide(value), 0, NULL, REG_OPTION_NON_VOLATILE,
                KEY_ALL_ACCESS, NULL, &subHandle, &disposition) != ERROR_SUCCESS) {
            return MPR_ERR_CANT_ACCESS;
        }
        RegCloseKey(subHandle);
    }
    RegCloseKey(h);
    return 0;
}


#else
void winDummy() {}
#endif /* ME_WIN_LIKE || CYGWIN */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */



/********* Start of file src/xml.c ************/


/**
    xml.c - A simple SAX style XML parser

    This is a recursive descent parser for XML text files. It is a one-pass simple parser that invokes a user 
    supplied callback for key tokens in the XML file. The user supplies a read function so that XML files can 
    be parsed from disk or in-memory. 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Forwards **********************************/

static MprXmlToken getXmlToken(MprXml *xp, int state);
static int  getNextChar(MprXml *xp);
static void manageXml(MprXml *xml, int flags);
static int  scanFor(MprXml *xp, char *str);
static int  parseNext(MprXml *xp, int state);
static int  putLastChar(MprXml *xp, int c);
static void xmlError(MprXml *xp, char *fmt, ...);
static void trimToken(MprXml *xp);

/************************************ Code ************************************/

PUBLIC MprXml *mprXmlOpen(ssize initialSize, ssize maxSize)
{
    MprXml  *xp;

    xp = mprAllocObj(MprXml, manageXml);

    xp->inBuf = mprCreateBuf(ME_MAX_BUFFER, ME_MAX_BUFFER);
    xp->tokBuf = mprCreateBuf(initialSize, maxSize);
    return xp;
}


static void manageXml(MprXml *xml, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(xml->inBuf);
        mprMark(xml->tokBuf);
        mprMark(xml->parseArg);
        mprMark(xml->inputArg);
        mprMark(xml->errMsg);
    }
}


PUBLIC void mprXmlSetParserHandler(MprXml *xp, MprXmlHandler h)
{
    assert(xp);
    xp->handler = h;
}


PUBLIC void mprXmlSetInputStream(MprXml *xp, MprXmlInputStream s, void *arg)
{
    assert(xp);

    xp->readFn = s;
    xp->inputArg = arg;
}


/*
    Set the parse arg
 */ 
PUBLIC void mprXmlSetParseArg(MprXml *xp, void *parseArg)
{
    assert(xp);

    xp->parseArg = parseArg;
}


/*
    Set the parse arg
 */ 
PUBLIC void *mprXmlGetParseArg(MprXml *xp)
{
    assert(xp);

    return xp->parseArg;
}


/*
    Parse an XML file. Return 0 for success, -1 for error.
 */ 
PUBLIC int mprXmlParse(MprXml *xp)
{
    assert(xp);

    return parseNext(xp, MPR_XML_BEGIN);
}


/*
    XML recursive descent parser. Return -1 for errors, 0 for EOF and 1 if there is still more data to parse.
 */
static int parseNext(MprXml *xp, int state)
{
    MprXmlHandler   handler;
    MprXmlToken     token;
    MprBuf          *tokBuf;
    char            *tname, *aname;
    int             rc;

    assert(state >= 0);

    tokBuf = xp->tokBuf;
    handler = xp->handler;
    tname = aname = 0;
    rc = 0;

    /*
        In this parse loop, the state is never assigned EOF or ERR. In such cases we always return EOF or ERR.
     */
    while (1) {

        token = getXmlToken(xp, state);

        if (token == MPR_XMLTOK_TOO_BIG) {
            xmlError(xp, "XML token is too big");
            return MPR_ERR_WONT_FIT;
        }

        switch (state) {
        case MPR_XML_BEGIN:     /* ------------------------------------------ */
            /*
                Expect to get an element, comment or processing instruction 
             */
            switch (token) {
            case MPR_XMLTOK_EOF:
                return 0;

            case MPR_XMLTOK_LS:
                /*
                    Recurse to handle the new element, comment etc.
                 */
                rc = parseNext(xp, MPR_XML_AFTER_LS);
                if (rc < 0) {
                    return rc;
                }
                break;

            default:
                xmlError(xp, "Syntax error");
                return MPR_ERR_BAD_SYNTAX;
            }
            break;

        case MPR_XML_AFTER_LS: /* ------------------------------------------ */
            switch (token) {
            case MPR_XMLTOK_COMMENT:
                state = MPR_XML_COMMENT;
                rc = (*handler)(xp, state, "!--", 0, mprGetBufStart(tokBuf));
                if (rc < 0) {
                    return rc;
                }
                return 1;

            case MPR_XMLTOK_CDATA:
                state = MPR_XML_CDATA;
                rc = (*handler)(xp, state, "!--", 0, mprGetBufStart(tokBuf));
                if (rc < 0) {
                    return rc;
                }
                return 1;

            case MPR_XMLTOK_INSTRUCTIONS:
                /* Just ignore processing instructions */
                return 1;

            case MPR_XMLTOK_TEXT:
                state = MPR_XML_NEW_ELT;
                tname = sclone(mprGetBufStart(tokBuf));
                if (tname == 0) {
                    assert(!MPR_ERR_MEMORY);
                    return MPR_ERR_MEMORY;
                }
                rc = (*handler)(xp, state, tname, 0, 0);
                if (rc < 0) {
                    return rc;
                }
                break;

            default:
                xmlError(xp, "Syntax error");
                return MPR_ERR_BAD_SYNTAX;
            }
            break;

        case MPR_XML_NEW_ELT:   /* ------------------------------------------ */
            /*
                We have seen the opening "<element" for a new element and have not yet seen the terminating 
                ">" of the opening element.
             */
            switch (token) {
            case MPR_XMLTOK_TEXT:
                /*
                    Must be an attribute name
                 */
                aname = sclone(mprGetBufStart(tokBuf));
                token = getXmlToken(xp, state);
                if (token != MPR_XMLTOK_EQ) {
                    xmlError(xp, "Missing assignment for attribute \"%s\"", aname);
                    return MPR_ERR_BAD_SYNTAX;
                }

                token = getXmlToken(xp, state);
                if (token != MPR_XMLTOK_TEXT) {
                    xmlError(xp, "Missing value for attribute \"%s\"", aname);
                    return MPR_ERR_BAD_SYNTAX;
                }
                state = MPR_XML_NEW_ATT;
                rc = (*handler)(xp, state, tname, aname, mprGetBufStart(tokBuf));
                if (rc < 0) {
                    return rc;
                }
                state = MPR_XML_NEW_ELT;
                break;

            case MPR_XMLTOK_GR:
                /*
                    This is ">" the termination of the opening element
                 */
                if (*tname == '\0') {
                    xmlError(xp, "Missing element name");
                    return MPR_ERR_BAD_SYNTAX;
                }

                /*
                    Tell the user that the opening element is now complete
                 */
                state = MPR_XML_ELT_DEFINED;
                rc = (*handler)(xp, state, tname, 0, 0);
                if (rc < 0) {
                    return rc;
                }
                state = MPR_XML_ELT_DATA;
                break;

            case MPR_XMLTOK_SLASH_GR:
                /*
                    If we see a "/>" then this is a solo element
                 */
                if (*tname == '\0') {
                    xmlError(xp, "Missing element name");
                    return MPR_ERR_BAD_SYNTAX;
                }
                state = MPR_XML_SOLO_ELT_DEFINED;
                rc = (*handler)(xp, state, tname, 0, 0);
                if (rc < 0) {
                    return rc;
                }
                return 1;
 
            default:
                xmlError(xp, "Syntax error");
                return MPR_ERR_BAD_SYNTAX;
            }
            break;

        case MPR_XML_ELT_DATA:      /* -------------------------------------- */
            /*
                We have seen the full opening element "<name ...>" and now await data or another element.
             */
            if (token == MPR_XMLTOK_LS) {
                /*
                    Recurse to handle the new element, comment etc.
                 */
                rc = parseNext(xp, MPR_XML_AFTER_LS);
                if (rc < 0) {
                    return rc;
                }
                break;

            } else if (token == MPR_XMLTOK_LS_SLASH) {
                state = MPR_XML_END_ELT;
                break;

            } else if (token != MPR_XMLTOK_TEXT) {
                return rc;
            }
            if (mprGetBufLength(tokBuf) > 0) {
                /*
                    Pass the data between the element to the user
                 */
                rc = (*handler)(xp, state, tname, 0, mprGetBufStart(tokBuf));
                if (rc < 0) {
                    return rc;
                }
            }
            break;

        case MPR_XML_END_ELT:           /* -------------------------------------- */
            if (token != MPR_XMLTOK_TEXT) {
                xmlError(xp, "Missing closing element name for \"%s\"", tname);
                return MPR_ERR_BAD_SYNTAX;
            }
            /*
                The closing element name must match the opening element name 
             */
            if (strcmp(tname, mprGetBufStart(tokBuf)) != 0) {
                xmlError(xp, "Closing element name \"%s\" does not match on line %d. Opening name \"%s\"",
                    mprGetBufStart(tokBuf), xp->lineNumber, tname);
                return MPR_ERR_BAD_SYNTAX;
            }
            rc = (*handler)(xp, state, tname, 0, 0);
            if (rc < 0) {
                return rc;
            }
            if (getXmlToken(xp, state) != MPR_XMLTOK_GR) {
                xmlError(xp, "Syntax error");
                return MPR_ERR_BAD_SYNTAX;
            }
            return 1;

        case MPR_XML_EOF:       /* ---------------------------------------------- */
            return 0;

        case MPR_XML_ERR:   /* ---------------------------------------------- */
        default:
            return MPR_ERR;
        }
    }
    assert(0);
}


/*
    Lexical analyser for XML. Return the next token reading input as required. It uses a one token look ahead and 
    push back mechanism (LAR1 parser). Text token identifiers are left in the tokBuf parser buffer on exit. This Lex 
    has special cases for the states MPR_XML_ELT_DATA where we have an optimized read of element data, and 
    MPR_XML_AFTER_LS where we distinguish between element names, processing instructions and comments. 
 */
static MprXmlToken getXmlToken(MprXml *xp, int state)
{
    MprBuf      *tokBuf;
    char        *cp;
    int         c, rc;

    assert(state >= 0);
    tokBuf = xp->tokBuf;

    if ((c = getNextChar(xp)) < 0) {
        return MPR_XMLTOK_EOF;
    }
    mprFlushBuf(tokBuf);

    /*
        Special case parsing for names and for element data. We do this for performance so we can return to the caller 
        the largest token possible.
     */
    if (state == MPR_XML_ELT_DATA) {
        /*
            Read all the data up to the start of the closing element "<" or the start of a sub-element.
         */
        if (c == '<') {
            if ((c = getNextChar(xp)) < 0) {
                return MPR_XMLTOK_EOF;
            }
            if (c == '/') {
                return MPR_XMLTOK_LS_SLASH;
            }
            putLastChar(xp, c);
            return MPR_XMLTOK_LS;
        }
        do {
            if (mprPutCharToBuf(tokBuf, c) < 0) {
                return MPR_XMLTOK_TOO_BIG;
            }
            if ((c = getNextChar(xp)) < 0) {
                return MPR_XMLTOK_EOF;
            }
        } while (c != '<');

        /*
            Put back the last look-ahead character
         */
        putLastChar(xp, c);

        /*
            If all white space, then zero the token buffer
         */
        for (cp = tokBuf->start; *cp; cp++) {
            if (!isspace((uchar) *cp & 0x7f)) {
                return MPR_XMLTOK_TEXT;
            }
        }
        mprFlushBuf(tokBuf);
        return MPR_XMLTOK_TEXT;
    }

    while (1) {
        switch (c) {
        case ' ':
        case '\n':
        case '\t':
        case '\r':
            break;

        case '<':
            if ((c = getNextChar(xp)) < 0) {
                return MPR_XMLTOK_EOF;
            }
            if (c == '/') {
                return MPR_XMLTOK_LS_SLASH;
            }
            putLastChar(xp, c);
            return MPR_XMLTOK_LS;

        case '=':
            return MPR_XMLTOK_EQ;

        case '>':
            return MPR_XMLTOK_GR;

        case '/':
            if ((c = getNextChar(xp)) < 0) {
                return MPR_XMLTOK_EOF;
            }
            if (c == '>') {
                return MPR_XMLTOK_SLASH_GR;
            }
            return MPR_XMLTOK_ERR;

        case '\"':
        case '\'':
            xp->quoteChar = c;
            /* Fall through */

        default:
            /*
                We handle element names, attribute names and attribute values 
                here. We do NOT handle data between elements here. Read the 
                token.  Stop on white space or a closing element ">"
             */
            if (xp->quoteChar) {
                if ((c = getNextChar(xp)) < 0) {
                    return MPR_XMLTOK_EOF;
                }
                while (c != xp->quoteChar) {
                    if (mprPutCharToBuf(tokBuf, c) < 0) {
                        return MPR_XMLTOK_TOO_BIG;
                    }
                    if ((c = getNextChar(xp)) < 0) {
                        return MPR_XMLTOK_EOF;
                    }
                }
                xp->quoteChar = 0;

            } else {
                while (!isspace((uchar) c) && c != '>' && c != '/' && c != '=') {
                    if (mprPutCharToBuf(tokBuf, c) < 0) {
                        return MPR_XMLTOK_TOO_BIG;
                    }
                    if ((c = getNextChar(xp)) < 0) {
                        return MPR_XMLTOK_EOF;
                    }
                }
                putLastChar(xp, c);
            }
            if (mprGetBufLength(tokBuf) < 0) {
                return MPR_XMLTOK_ERR;
            }
            mprAddNullToBuf(tokBuf);

            if (state == MPR_XML_AFTER_LS) {
                /*
                    If we are just inside an element "<", then analyze what we have to see if we have an element name, 
                    instruction or comment. Tokbuf will hold "?" for instructions or "!--" for comments.
                 */
                if (mprLookAtNextCharInBuf(tokBuf) == '?') {
                    /*  Just ignore processing instructions */
                    rc = scanFor(xp, "?>");
                    if (rc < 0) {
                        return MPR_XMLTOK_TOO_BIG;
                    } else if (rc == 0) {
                        return MPR_XMLTOK_ERR;
                    }
                    return MPR_XMLTOK_INSTRUCTIONS;

                } else if (mprLookAtNextCharInBuf(tokBuf) == '!') {
                    if (strncmp((char*) tokBuf->start, "![CDATA[", 8) == 0) {
                        mprAdjustBufStart(tokBuf, 8);
                        rc = scanFor(xp, "]]>");
                        if (rc < 0) {
                            return MPR_XMLTOK_TOO_BIG;
                        } else if (rc == 0) {
                            return MPR_XMLTOK_ERR;
                        }
                        return MPR_XMLTOK_CDATA;

                    } else {
                        mprFlushBuf(tokBuf);
                        rc = scanFor(xp, "-->");
                        if (rc < 0) {
                            return MPR_XMLTOK_TOO_BIG;
                        } else if (rc == 0) {
                            return MPR_XMLTOK_ERR;
                        }
                        return MPR_XMLTOK_COMMENT;
                    }
                }
            }
            trimToken(xp);
            return MPR_XMLTOK_TEXT;
        }
        if ((c = getNextChar(xp)) < 0) {
            return MPR_XMLTOK_EOF;
        }
    }

    /* Should never get here */
    assert(0);
    return MPR_XMLTOK_ERR;
}


/*
    Scan for a pattern. Trim the pattern from the token. Return 1 if the pattern was found, return 0 if not found. 
    Return < 0 on errors.
 */
static int scanFor(MprXml *xp, char *pattern)
{
    MprBuf  *tokBuf;
    char    *start, *p, *cp;
    int     c;

    assert(pattern);

    tokBuf = xp->tokBuf;
    assert(tokBuf);

    start = mprGetBufStart(tokBuf);
    while (1) {
        cp = start;
        for (p = pattern; *p; p++) {
            if (cp >= (char*) tokBuf->end) {
                if ((c = getNextChar(xp)) < 0) {
                    return 0;
                }
                if (mprPutCharToBuf(tokBuf, c) < 0) {
                    return -1;
                }
            }
            if (*cp++ != *p) {
                break;
            }
        }
        if (*p == '\0') {
            /*
                Remove the pattern from the tokBuf
             */
            mprAdjustBufEnd(tokBuf, - (int) slen(pattern));
            trimToken(xp);
            return 1;
        }
        start++;
    }
}


/*
    Get another character. We read and buffer blocks of data if we need more data to parse.
 */
static int getNextChar(MprXml *xp)
{
    MprBuf  *inBuf;
    ssize   l;
    int     c;

    inBuf = xp->inBuf;
    if (mprGetBufLength(inBuf) <= 0) {
        /*
            Flush to reset the servp/endp pointers to the start of the buffer so we can do a maximal read 
         */
        mprFlushBuf(inBuf);
        l = (xp->readFn)(xp, xp->inputArg, mprGetBufStart(inBuf), mprGetBufSpace(inBuf));
        if (l <= 0) {
            return -1;
        }
        mprAdjustBufEnd(inBuf, l);
    }
    c = mprGetCharFromBuf(inBuf);
    if (c == '\n') {
        xp->lineNumber++;
    }
    return c;
}


/*
    Put back a character in the input buffer
 */
static int putLastChar(MprXml *xp, int c)
{
    if (mprInsertCharToBuf(xp->inBuf, (char) c) < 0) {
        assert(0);
        return MPR_ERR_BAD_STATE;
    }
    if (c == '\n') {
        xp->lineNumber--;
    }
    return 0;
}


/*
    Output a parse message
 */ 
static void xmlError(MprXml *xp, char *fmt, ...)
{
    va_list     args;
    char        *buf;

    assert(fmt);

    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);
    xp->errMsg = sfmt("XML error: %s\nAt line %d\n", buf, xp->lineNumber);
}


/*
    Remove trailing whitespace in a token and ensure it is terminated with a NULL for easy parsing
 */
static void trimToken(MprXml *xp)
{
    while (isspace((uchar) mprLookAtLastCharInBuf(xp->tokBuf))) {
        mprAdjustBufEnd(xp->tokBuf, -1);
    }
    mprAddNullToBuf(xp->tokBuf);
}


PUBLIC cchar *mprXmlGetErrorMsg(MprXml *xp)
{
    if (xp->errMsg == 0) {
        return "";
    }
    return xp->errMsg;
}


PUBLIC int mprXmlGetLineNumber(MprXml *xp)
{
    return xp->lineNumber;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
