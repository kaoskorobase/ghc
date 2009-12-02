/* -----------------------------------------------------------------------------
 *
 * (c) The GHC Team, 1998-2008
 *
 * Storage manager front end
 *
 * Documentation on the architecture of the Storage Manager can be
 * found in the online commentary:
 * 
 *   http://hackage.haskell.org/trac/ghc/wiki/Commentary/Rts/Storage
 *
 * ---------------------------------------------------------------------------*/

#include "PosixSource.h"
#include "Rts.h"

#include "Storage.h"
#include "RtsUtils.h"
#include "Stats.h"
#include "BlockAlloc.h"
#include "Weak.h"
#include "Sanity.h"
#include "Arena.h"
#include "Capability.h"
#include "Schedule.h"
#include "RetainerProfile.h"	// for counting memory blocks (memInventory)
#include "OSMem.h"
#include "Trace.h"
#include "GC.h"
#include "Evac.h"

#include <string.h>

#include "ffi.h"

/* 
 * All these globals require sm_mutex to access in THREADED_RTS mode.
 */
StgClosure    *caf_list         = NULL;
StgClosure    *revertible_caf_list = NULL;
rtsBool       keepCAFs;

nat alloc_blocks_lim;    /* GC if n_large_blocks in any nursery
                          * reaches this. */

bdescr *exec_block;

generation *generations = NULL;	/* all the generations */
generation *g0		= NULL; /* generation 0, for convenience */
generation *oldest_gen  = NULL; /* oldest generation, for convenience */

nat total_steps         = 0;
step *all_steps         = NULL; /* single array of steps */

ullong total_allocated = 0;	/* total memory allocated during run */

step *nurseries         = NULL; /* array of nurseries, size == n_capabilities */

#ifdef THREADED_RTS
/*
 * Storage manager mutex:  protects all the above state from
 * simultaneous access by two STG threads.
 */
Mutex sm_mutex;
#endif

static void allocNurseries ( void );

static void
initStep (step *stp, int g, int s)
{
    stp->no = s;
    stp->abs_no = RtsFlags.GcFlags.steps * g + s;
    stp->blocks = NULL;
    stp->n_blocks = 0;
    stp->n_words = 0;
    stp->live_estimate = 0;
    stp->old_blocks = NULL;
    stp->n_old_blocks = 0;
    stp->gen = &generations[g];
    stp->gen_no = g;
    stp->large_objects = NULL;
    stp->n_large_blocks = 0;
    stp->scavenged_large_objects = NULL;
    stp->n_scavenged_large_blocks = 0;
    stp->mark = 0;
    stp->compact = 0;
    stp->bitmap = NULL;
#ifdef THREADED_RTS
    initSpinLock(&stp->sync_large_objects);
#endif
    stp->threads = END_TSO_QUEUE;
    stp->old_threads = END_TSO_QUEUE;
}

void
initStorage( void )
{
  nat g, s;
  generation *gen;

  if (generations != NULL) {
      // multi-init protection
      return;
  }

  initMBlocks();

  /* Sanity check to make sure the LOOKS_LIKE_ macros appear to be
   * doing something reasonable.
   */
  /* We use the NOT_NULL variant or gcc warns that the test is always true */
  ASSERT(LOOKS_LIKE_INFO_PTR_NOT_NULL((StgWord)&stg_BLACKHOLE_info));
  ASSERT(LOOKS_LIKE_CLOSURE_PTR(&stg_dummy_ret_closure));
  ASSERT(!HEAP_ALLOCED(&stg_dummy_ret_closure));
  
  if (RtsFlags.GcFlags.maxHeapSize != 0 &&
      RtsFlags.GcFlags.heapSizeSuggestion > 
      RtsFlags.GcFlags.maxHeapSize) {
    RtsFlags.GcFlags.maxHeapSize = RtsFlags.GcFlags.heapSizeSuggestion;
  }

  if (RtsFlags.GcFlags.maxHeapSize != 0 &&
      RtsFlags.GcFlags.minAllocAreaSize > 
      RtsFlags.GcFlags.maxHeapSize) {
      errorBelch("maximum heap size (-M) is smaller than minimum alloc area size (-A)");
      RtsFlags.GcFlags.minAllocAreaSize = RtsFlags.GcFlags.maxHeapSize;
  }

  initBlockAllocator();
  
#if defined(THREADED_RTS)
  initMutex(&sm_mutex);
#endif

  ACQUIRE_SM_LOCK;

  /* allocate generation info array */
  generations = (generation *)stgMallocBytes(RtsFlags.GcFlags.generations 
					     * sizeof(struct generation_),
					     "initStorage: gens");

  /* Initialise all generations */
  for(g = 0; g < RtsFlags.GcFlags.generations; g++) {
    gen = &generations[g];
    gen->no = g;
    gen->mut_list = allocBlock();
    gen->collections = 0;
    gen->par_collections = 0;
    gen->failed_promotions = 0;
    gen->max_blocks = 0;
  }

  /* A couple of convenience pointers */
  g0 = &generations[0];
  oldest_gen = &generations[RtsFlags.GcFlags.generations-1];

  /* allocate all the steps into an array.  It is important that we do
     it this way, because we need the invariant that two step pointers
     can be directly compared to see which is the oldest.
     Remember that the last generation has only one step. */
  total_steps = 1 + (RtsFlags.GcFlags.generations - 1) * RtsFlags.GcFlags.steps;
  all_steps   = stgMallocBytes(total_steps * sizeof(struct step_),
                               "initStorage: steps");

  /* Allocate step structures in each generation */
  if (RtsFlags.GcFlags.generations > 1) {
    /* Only for multiple-generations */

    /* Oldest generation: one step */
    oldest_gen->n_steps = 1;
    oldest_gen->steps   = all_steps + (RtsFlags.GcFlags.generations - 1)
	                              * RtsFlags.GcFlags.steps;

    /* set up all except the oldest generation with 2 steps */
    for(g = 0; g < RtsFlags.GcFlags.generations-1; g++) {
      generations[g].n_steps = RtsFlags.GcFlags.steps;
      generations[g].steps   = all_steps + g * RtsFlags.GcFlags.steps;
    }
    
  } else {
    /* single generation, i.e. a two-space collector */
    g0->n_steps = 1;
    g0->steps   = all_steps;
  }

  nurseries = stgMallocBytes (n_capabilities * sizeof(struct step_),
			      "initStorage: nurseries");

  /* Initialise all steps */
  for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
    for (s = 0; s < generations[g].n_steps; s++) {
	initStep(&generations[g].steps[s], g, s);
    }
  }
  
  for (s = 0; s < n_capabilities; s++) {
      initStep(&nurseries[s], 0, s);
  }
  
  /* Set up the destination pointers in each younger gen. step */
  for (g = 0; g < RtsFlags.GcFlags.generations-1; g++) {
    for (s = 0; s < generations[g].n_steps-1; s++) {
      generations[g].steps[s].to = &generations[g].steps[s+1];
    }
    generations[g].steps[s].to = &generations[g+1].steps[0];
  }
  oldest_gen->steps[0].to = &oldest_gen->steps[0];
  
  for (s = 0; s < n_capabilities; s++) {
      nurseries[s].to = generations[0].steps[0].to;
  }
  
  /* The oldest generation has one step. */
  if (RtsFlags.GcFlags.compact || RtsFlags.GcFlags.sweep) {
      if (RtsFlags.GcFlags.generations == 1) {
	  errorBelch("WARNING: compact/sweep is incompatible with -G1; disabled");
      } else {
	  oldest_gen->steps[0].mark = 1;
          if (RtsFlags.GcFlags.compact)
              oldest_gen->steps[0].compact = 1;
      }
  }

  generations[0].max_blocks = 0;

  /* The allocation area.  Policy: keep the allocation area
   * small to begin with, even if we have a large suggested heap
   * size.  Reason: we're going to do a major collection first, and we
   * don't want it to be a big one.  This vague idea is borne out by 
   * rigorous experimental evidence.
   */
  allocNurseries();

  weak_ptr_list = NULL;
  caf_list = NULL;
  revertible_caf_list = NULL;
   
  /* initialise the allocate() interface */
  alloc_blocks_lim = RtsFlags.GcFlags.minAllocAreaSize;

  exec_block = NULL;

#ifdef THREADED_RTS
  initSpinLock(&gc_alloc_block_sync);
  whitehole_spin = 0;
#endif

  N = 0;

  initGcThreads();

  IF_DEBUG(gc, statDescribeGens());

  RELEASE_SM_LOCK;
}

void
exitStorage (void)
{
    stat_exit(calcAllocated());
}

void
freeStorage (void)
{
    stgFree(all_steps); // frees all the steps
    stgFree(generations);
    freeAllMBlocks();
#if defined(THREADED_RTS)
    closeMutex(&sm_mutex);
#endif
    stgFree(nurseries);
    freeGcThreads();
}

/* -----------------------------------------------------------------------------
   CAF management.

   The entry code for every CAF does the following:
     
      - builds a CAF_BLACKHOLE in the heap
      - pushes an update frame pointing to the CAF_BLACKHOLE
      - invokes UPD_CAF(), which:
          - calls newCaf, below
	  - updates the CAF with a static indirection to the CAF_BLACKHOLE
      
   Why do we build a BLACKHOLE in the heap rather than just updating
   the thunk directly?  It's so that we only need one kind of update
   frame - otherwise we'd need a static version of the update frame too.

   newCaf() does the following:
       
      - it puts the CAF on the oldest generation's mut-once list.
        This is so that we can treat the CAF as a root when collecting
	younger generations.

   For GHCI, we have additional requirements when dealing with CAFs:

      - we must *retain* all dynamically-loaded CAFs ever entered,
        just in case we need them again.
      - we must be able to *revert* CAFs that have been evaluated, to
        their pre-evaluated form.

      To do this, we use an additional CAF list.  When newCaf() is
      called on a dynamically-loaded CAF, we add it to the CAF list
      instead of the old-generation mutable list, and save away its
      old info pointer (in caf->saved_info) for later reversion.

      To revert all the CAFs, we traverse the CAF list and reset the
      info pointer to caf->saved_info, then throw away the CAF list.
      (see GC.c:revertCAFs()).

      -- SDM 29/1/01

   -------------------------------------------------------------------------- */

void
newCAF(StgClosure* caf)
{
  ACQUIRE_SM_LOCK;

#ifdef DYNAMIC
  if(keepCAFs)
  {
    // HACK:
    // If we are in GHCi _and_ we are using dynamic libraries,
    // then we can't redirect newCAF calls to newDynCAF (see below),
    // so we make newCAF behave almost like newDynCAF.
    // The dynamic libraries might be used by both the interpreted
    // program and GHCi itself, so they must not be reverted.
    // This also means that in GHCi with dynamic libraries, CAFs are not
    // garbage collected. If this turns out to be a problem, we could
    // do another hack here and do an address range test on caf to figure
    // out whether it is from a dynamic library.
    ((StgIndStatic *)caf)->saved_info  = (StgInfoTable *)caf->header.info;
    ((StgIndStatic *)caf)->static_link = caf_list;
    caf_list = caf;
  }
  else
#endif
  {
    /* Put this CAF on the mutable list for the old generation.
    * This is a HACK - the IND_STATIC closure doesn't really have
    * a mut_link field, but we pretend it has - in fact we re-use
    * the STATIC_LINK field for the time being, because when we
    * come to do a major GC we won't need the mut_link field
    * any more and can use it as a STATIC_LINK.
    */
    ((StgIndStatic *)caf)->saved_info = NULL;
    recordMutableGen(caf, oldest_gen->no);
  }
  
  RELEASE_SM_LOCK;
}

// An alternate version of newCaf which is used for dynamically loaded
// object code in GHCi.  In this case we want to retain *all* CAFs in
// the object code, because they might be demanded at any time from an
// expression evaluated on the command line.
// Also, GHCi might want to revert CAFs, so we add these to the
// revertible_caf_list.
//
// The linker hackily arranges that references to newCaf from dynamic
// code end up pointing to newDynCAF.
void
newDynCAF(StgClosure *caf)
{
    ACQUIRE_SM_LOCK;

    ((StgIndStatic *)caf)->saved_info  = (StgInfoTable *)caf->header.info;
    ((StgIndStatic *)caf)->static_link = revertible_caf_list;
    revertible_caf_list = caf;

    RELEASE_SM_LOCK;
}

/* -----------------------------------------------------------------------------
   Nursery management.
   -------------------------------------------------------------------------- */

static bdescr *
allocNursery (step *stp, bdescr *tail, nat blocks)
{
    bdescr *bd;
    nat i;

    // Allocate a nursery: we allocate fresh blocks one at a time and
    // cons them on to the front of the list, not forgetting to update
    // the back pointer on the tail of the list to point to the new block.
    for (i=0; i < blocks; i++) {
	// @LDV profiling
	/*
	  processNursery() in LdvProfile.c assumes that every block group in
	  the nursery contains only a single block. So, if a block group is
	  given multiple blocks, change processNursery() accordingly.
	*/
	bd = allocBlock();
	bd->link = tail;
	// double-link the nursery: we might need to insert blocks
	if (tail != NULL) {
	    tail->u.back = bd;
	}
        initBdescr(bd, stp);
	bd->flags = 0;
	bd->free = bd->start;
	tail = bd;
    }
    tail->u.back = NULL;
    return tail;
}

static void
assignNurseriesToCapabilities (void)
{
    nat i;

    for (i = 0; i < n_capabilities; i++) {
	capabilities[i].r.rNursery        = &nurseries[i];
	capabilities[i].r.rCurrentNursery = nurseries[i].blocks;
	capabilities[i].r.rCurrentAlloc   = NULL;
    }
}

static void
allocNurseries( void )
{ 
    nat i;

    for (i = 0; i < n_capabilities; i++) {
	nurseries[i].blocks = 
	    allocNursery(&nurseries[i], NULL, 
			 RtsFlags.GcFlags.minAllocAreaSize);
	nurseries[i].n_blocks    = RtsFlags.GcFlags.minAllocAreaSize;
	nurseries[i].old_blocks   = NULL;
	nurseries[i].n_old_blocks = 0;
    }
    assignNurseriesToCapabilities();
}
      
void
resetNurseries( void )
{
    nat i;
    bdescr *bd;
    step *stp;

    for (i = 0; i < n_capabilities; i++) {
	stp = &nurseries[i];
	for (bd = stp->blocks; bd; bd = bd->link) {
	    bd->free = bd->start;
	    ASSERT(bd->gen_no == 0);
	    ASSERT(bd->step == stp);
	    IF_DEBUG(sanity,memset(bd->start, 0xaa, BLOCK_SIZE));
	}
        // these large objects are dead, since we have just GC'd
        freeChain(stp->large_objects);
        stp->large_objects = NULL;
        stp->n_large_blocks = 0;
    }
    assignNurseriesToCapabilities();
}

lnat
countNurseryBlocks (void)
{
    nat i;
    lnat blocks = 0;

    for (i = 0; i < n_capabilities; i++) {
	blocks += nurseries[i].n_blocks;
        blocks += nurseries[i].n_large_blocks;
    }
    return blocks;
}

static void
resizeNursery ( step *stp, nat blocks )
{
  bdescr *bd;
  nat nursery_blocks;

  nursery_blocks = stp->n_blocks;
  if (nursery_blocks == blocks) return;

  if (nursery_blocks < blocks) {
      debugTrace(DEBUG_gc, "increasing size of nursery to %d blocks", 
		 blocks);
    stp->blocks = allocNursery(stp, stp->blocks, blocks-nursery_blocks);
  } 
  else {
    bdescr *next_bd;
    
    debugTrace(DEBUG_gc, "decreasing size of nursery to %d blocks", 
	       blocks);

    bd = stp->blocks;
    while (nursery_blocks > blocks) {
	next_bd = bd->link;
	next_bd->u.back = NULL;
	nursery_blocks -= bd->blocks; // might be a large block
	freeGroup(bd);
	bd = next_bd;
    }
    stp->blocks = bd;
    // might have gone just under, by freeing a large block, so make
    // up the difference.
    if (nursery_blocks < blocks) {
	stp->blocks = allocNursery(stp, stp->blocks, blocks-nursery_blocks);
    }
  }
  
  stp->n_blocks = blocks;
  ASSERT(countBlocks(stp->blocks) == stp->n_blocks);
}

// 
// Resize each of the nurseries to the specified size.
//
void
resizeNurseriesFixed (nat blocks)
{
    nat i;
    for (i = 0; i < n_capabilities; i++) {
	resizeNursery(&nurseries[i], blocks);
    }
}

// 
// Resize the nurseries to the total specified size.
//
void
resizeNurseries (nat blocks)
{
    // If there are multiple nurseries, then we just divide the number
    // of available blocks between them.
    resizeNurseriesFixed(blocks / n_capabilities);
}


/* -----------------------------------------------------------------------------
   move_TSO is called to update the TSO structure after it has been
   moved from one place to another.
   -------------------------------------------------------------------------- */

void
move_TSO (StgTSO *src, StgTSO *dest)
{
    ptrdiff_t diff;

    // relocate the stack pointer... 
    diff = (StgPtr)dest - (StgPtr)src; // In *words* 
    dest->sp = (StgPtr)dest->sp + diff;
}

/* -----------------------------------------------------------------------------
   split N blocks off the front of the given bdescr, returning the
   new block group.  We add the remainder to the large_blocks list
   in the same step as the original block.
   -------------------------------------------------------------------------- */

bdescr *
splitLargeBlock (bdescr *bd, nat blocks)
{
    bdescr *new_bd;

    ACQUIRE_SM_LOCK;

    ASSERT(countBlocks(bd->step->large_objects) == bd->step->n_large_blocks);

    // subtract the original number of blocks from the counter first
    bd->step->n_large_blocks -= bd->blocks;

    new_bd = splitBlockGroup (bd, blocks);
    initBdescr(new_bd, bd->step);
    new_bd->flags   = BF_LARGE | (bd->flags & BF_EVACUATED); 
    // if new_bd is in an old generation, we have to set BF_EVACUATED
    new_bd->free    = bd->free;
    dbl_link_onto(new_bd, &bd->step->large_objects);

    ASSERT(new_bd->free <= new_bd->start + new_bd->blocks * BLOCK_SIZE_W);

    // add the new number of blocks to the counter.  Due to the gaps
    // for block descriptors, new_bd->blocks + bd->blocks might not be
    // equal to the original bd->blocks, which is why we do it this way.
    bd->step->n_large_blocks += bd->blocks + new_bd->blocks;

    ASSERT(countBlocks(bd->step->large_objects) == bd->step->n_large_blocks);

    RELEASE_SM_LOCK;

    return new_bd;
}

/* -----------------------------------------------------------------------------
   allocate()

   This allocates memory in the current thread - it is intended for
   use primarily from STG-land where we have a Capability.  It is
   better than allocate() because it doesn't require taking the
   sm_mutex lock in the common case.

   Memory is allocated directly from the nursery if possible (but not
   from the current nursery block, so as not to interfere with
   Hp/HpLim).
   -------------------------------------------------------------------------- */

StgPtr
allocate (Capability *cap, lnat n)
{
    bdescr *bd;
    StgPtr p;
    step *stp;

    if (n >= LARGE_OBJECT_THRESHOLD/sizeof(W_)) {
	lnat req_blocks =  (lnat)BLOCK_ROUND_UP(n*sizeof(W_)) / BLOCK_SIZE;

        // Attempting to allocate an object larger than maxHeapSize
        // should definitely be disallowed.  (bug #1791)
        if (RtsFlags.GcFlags.maxHeapSize > 0 && 
            req_blocks >= RtsFlags.GcFlags.maxHeapSize) {
            heapOverflow();
            // heapOverflow() doesn't exit (see #2592), but we aren't
            // in a position to do a clean shutdown here: we
            // either have to allocate the memory or exit now.
            // Allocating the memory would be bad, because the user
            // has requested that we not exceed maxHeapSize, so we
            // just exit.
	    stg_exit(EXIT_HEAPOVERFLOW);
        }

        stp = &nurseries[cap->no];

	bd = allocGroup(req_blocks);
	dbl_link_onto(bd, &stp->large_objects);
	stp->n_large_blocks += bd->blocks; // might be larger than req_blocks
        initBdescr(bd, stp);
	bd->flags = BF_LARGE;
	bd->free = bd->start + n;
	return bd->start;
    }

    /* small allocation (<LARGE_OBJECT_THRESHOLD) */

    TICK_ALLOC_HEAP_NOCTR(n);
    CCS_ALLOC(CCCS,n);
    
    bd = cap->r.rCurrentAlloc;
    if (bd == NULL || bd->free + n > bd->start + BLOCK_SIZE_W) {
        
        // The CurrentAlloc block is full, we need to find another
        // one.  First, we try taking the next block from the
        // nursery:
        bd = cap->r.rCurrentNursery->link;
        
        if (bd == NULL || bd->free + n > bd->start + BLOCK_SIZE_W) {
            // The nursery is empty, or the next block is already
            // full: allocate a fresh block (we can't fail here).
            ACQUIRE_SM_LOCK;
            bd = allocBlock();
            cap->r.rNursery->n_blocks++;
            RELEASE_SM_LOCK;
            initBdescr(bd, cap->r.rNursery);
            bd->flags = 0;
            // If we had to allocate a new block, then we'll GC
            // pretty quickly now, because MAYBE_GC() will
            // notice that CurrentNursery->link is NULL.
        } else {
            // we have a block in the nursery: take it and put
            // it at the *front* of the nursery list, and use it
            // to allocate() from.
            cap->r.rCurrentNursery->link = bd->link;
            if (bd->link != NULL) {
                bd->link->u.back = cap->r.rCurrentNursery;
            }
        }
        dbl_link_onto(bd, &cap->r.rNursery->blocks);
        cap->r.rCurrentAlloc = bd;
        IF_DEBUG(sanity, checkNurserySanity(cap->r.rNursery));
    }
    p = bd->free;
    bd->free += n;
    return p;
}

/* ---------------------------------------------------------------------------
   Allocate a fixed/pinned object.

   We allocate small pinned objects into a single block, allocating a
   new block when the current one overflows.  The block is chained
   onto the large_object_list of generation 0 step 0.

   NOTE: The GC can't in general handle pinned objects.  This
   interface is only safe to use for ByteArrays, which have no
   pointers and don't require scavenging.  It works because the
   block's descriptor has the BF_LARGE flag set, so the block is
   treated as a large object and chained onto various lists, rather
   than the individual objects being copied.  However, when it comes
   to scavenge the block, the GC will only scavenge the first object.
   The reason is that the GC can't linearly scan a block of pinned
   objects at the moment (doing so would require using the
   mostly-copying techniques).  But since we're restricting ourselves
   to pinned ByteArrays, not scavenging is ok.

   This function is called by newPinnedByteArray# which immediately
   fills the allocated memory with a MutableByteArray#.
   ------------------------------------------------------------------------- */

StgPtr
allocatePinned (Capability *cap, lnat n)
{
    StgPtr p;
    bdescr *bd;
    step *stp;

    // If the request is for a large object, then allocate()
    // will give us a pinned object anyway.
    if (n >= LARGE_OBJECT_THRESHOLD/sizeof(W_)) {
	p = allocate(cap, n);
        Bdescr(p)->flags |= BF_PINNED;
        return p;
    }

    TICK_ALLOC_HEAP_NOCTR(n);
    CCS_ALLOC(CCCS,n);

    bd = cap->pinned_object_block;
    
    // If we don't have a block of pinned objects yet, or the current
    // one isn't large enough to hold the new object, allocate a new one.
    if (bd == NULL || (bd->free + n) > (bd->start + BLOCK_SIZE_W)) {
        ACQUIRE_SM_LOCK
	cap->pinned_object_block = bd = allocBlock();
        RELEASE_SM_LOCK
        stp = &nurseries[cap->no];
	dbl_link_onto(bd, &stp->large_objects);
	stp->n_large_blocks++;
        initBdescr(bd, stp);
	bd->flags  = BF_PINNED | BF_LARGE;
	bd->free   = bd->start;
    }

    p = bd->free;
    bd->free += n;
    return p;
}

/* -----------------------------------------------------------------------------
   Write Barriers
   -------------------------------------------------------------------------- */

/*
   This is the write barrier for MUT_VARs, a.k.a. IORefs.  A
   MUT_VAR_CLEAN object is not on the mutable list; a MUT_VAR_DIRTY
   is.  When written to, a MUT_VAR_CLEAN turns into a MUT_VAR_DIRTY
   and is put on the mutable list.
*/
void
dirty_MUT_VAR(StgRegTable *reg, StgClosure *p)
{
    Capability *cap = regTableToCapability(reg);
    bdescr *bd;
    if (p->header.info == &stg_MUT_VAR_CLEAN_info) {
	p->header.info = &stg_MUT_VAR_DIRTY_info;
	bd = Bdescr((StgPtr)p);
	if (bd->gen_no > 0) recordMutableCap(p,cap,bd->gen_no);
    }
}

// Setting a TSO's link field with a write barrier.
// It is *not* necessary to call this function when
//    * setting the link field to END_TSO_QUEUE
//    * putting a TSO on the blackhole_queue
//    * setting the link field of the currently running TSO, as it
//      will already be dirty.
void
setTSOLink (Capability *cap, StgTSO *tso, StgTSO *target)
{
    bdescr *bd;
    if (tso->dirty == 0 && (tso->flags & TSO_LINK_DIRTY) == 0) {
        tso->flags |= TSO_LINK_DIRTY;
	bd = Bdescr((StgPtr)tso);
	if (bd->gen_no > 0) recordMutableCap((StgClosure*)tso,cap,bd->gen_no);
    }
    tso->_link = target;
}

void
dirty_TSO (Capability *cap, StgTSO *tso)
{
    bdescr *bd;
    if (tso->dirty == 0 && (tso->flags & TSO_LINK_DIRTY) == 0) {
	bd = Bdescr((StgPtr)tso);
	if (bd->gen_no > 0) recordMutableCap((StgClosure*)tso,cap,bd->gen_no);
    }
    tso->dirty = 1;
}

/*
   This is the write barrier for MVARs.  An MVAR_CLEAN objects is not
   on the mutable list; a MVAR_DIRTY is.  When written to, a
   MVAR_CLEAN turns into a MVAR_DIRTY and is put on the mutable list.
   The check for MVAR_CLEAN is inlined at the call site for speed,
   this really does make a difference on concurrency-heavy benchmarks
   such as Chaneneos and cheap-concurrency.
*/
void
dirty_MVAR(StgRegTable *reg, StgClosure *p)
{
    Capability *cap = regTableToCapability(reg);
    bdescr *bd;
    bd = Bdescr((StgPtr)p);
    if (bd->gen_no > 0) recordMutableCap(p,cap,bd->gen_no);
}

/* -----------------------------------------------------------------------------
 * Stats and stuff
 * -------------------------------------------------------------------------- */

/* -----------------------------------------------------------------------------
 * calcAllocated()
 *
 * Approximate how much we've allocated: number of blocks in the
 * nursery + blocks allocated via allocate() - unused nusery blocks.
 * This leaves a little slop at the end of each block.
 * -------------------------------------------------------------------------- */

lnat
calcAllocated( void )
{
  nat allocated;
  bdescr *bd;
  nat i;

  allocated = countNurseryBlocks() * BLOCK_SIZE_W;
  
  for (i = 0; i < n_capabilities; i++) {
      Capability *cap;
      for ( bd = capabilities[i].r.rCurrentNursery->link; 
	    bd != NULL; bd = bd->link ) {
	  allocated -= BLOCK_SIZE_W;
      }
      cap = &capabilities[i];
      if (cap->r.rCurrentNursery->free < 
	  cap->r.rCurrentNursery->start + BLOCK_SIZE_W) {
	  allocated -= (cap->r.rCurrentNursery->start + BLOCK_SIZE_W)
	      - cap->r.rCurrentNursery->free;
      }
      if (cap->pinned_object_block != NULL) {
          allocated -= (cap->pinned_object_block->start + BLOCK_SIZE_W) - 
              cap->pinned_object_block->free;
      }
  }

  total_allocated += allocated;
  return allocated;
}  

/* Approximate the amount of live data in the heap.  To be called just
 * after garbage collection (see GarbageCollect()).
 */
lnat 
calcLiveBlocks(void)
{
  nat g, s;
  lnat live = 0;
  step *stp;

  for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
    for (s = 0; s < generations[g].n_steps; s++) {
      /* approximate amount of live data (doesn't take into account slop
       * at end of each block).
       */
      if (g == 0 && s == 0 && RtsFlags.GcFlags.generations > 1) { 
	  continue; 
      }
      stp = &generations[g].steps[s];
      live += stp->n_large_blocks + stp->n_blocks;
    }
  }
  return live;
}

lnat
countOccupied(bdescr *bd)
{
    lnat words;

    words = 0;
    for (; bd != NULL; bd = bd->link) {
        ASSERT(bd->free <= bd->start + bd->blocks * BLOCK_SIZE_W);
        words += bd->free - bd->start;
    }
    return words;
}

// Return an accurate count of the live data in the heap, excluding
// generation 0.
lnat
calcLiveWords(void)
{
    nat g, s;
    lnat live;
    step *stp;
    
    live = 0;
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
        for (s = 0; s < generations[g].n_steps; s++) {
            if (g == 0 && s == 0 && RtsFlags.GcFlags.generations > 1) continue; 
            stp = &generations[g].steps[s];
            live += stp->n_words + countOccupied(stp->large_objects);
        } 
    }
    return live;
}

/* Approximate the number of blocks that will be needed at the next
 * garbage collection.
 *
 * Assume: all data currently live will remain live.  Steps that will
 * be collected next time will therefore need twice as many blocks
 * since all the data will be copied.
 */
extern lnat 
calcNeeded(void)
{
    lnat needed = 0;
    nat g, s;
    step *stp;
    
    for (g = 0; g < RtsFlags.GcFlags.generations; g++) {
	for (s = 0; s < generations[g].n_steps; s++) {
	    if (g == 0 && s == 0) { continue; }
	    stp = &generations[g].steps[s];

            // we need at least this much space
            needed += stp->n_blocks + stp->n_large_blocks;

            // any additional space needed to collect this gen next time?
	    if (g == 0 || // always collect gen 0
                (generations[g].steps[0].n_blocks +
                 generations[g].steps[0].n_large_blocks 
                 > generations[g].max_blocks)) {
                // we will collect this gen next time
                if (stp->mark) {
                    //  bitmap:
                    needed += stp->n_blocks / BITS_IN(W_);
                    //  mark stack:
                    needed += stp->n_blocks / 100;
                }
                if (stp->compact) {
                    continue; // no additional space needed for compaction
                } else {
                    needed += stp->n_blocks;
                }
	    }
	}
    }
    return needed;
}

/* ----------------------------------------------------------------------------
   Executable memory

   Executable memory must be managed separately from non-executable
   memory.  Most OSs these days require you to jump through hoops to
   dynamically allocate executable memory, due to various security
   measures.

   Here we provide a small memory allocator for executable memory.
   Memory is managed with a page granularity; we allocate linearly
   in the page, and when the page is emptied (all objects on the page
   are free) we free the page again, not forgetting to make it
   non-executable.

   TODO: The inability to handle objects bigger than BLOCK_SIZE_W means that
         the linker cannot use allocateExec for loading object code files
         on Windows. Once allocateExec can handle larger objects, the linker
         should be modified to use allocateExec instead of VirtualAlloc.
   ------------------------------------------------------------------------- */

#if defined(linux_HOST_OS)

// On Linux we need to use libffi for allocating executable memory,
// because it knows how to work around the restrictions put in place
// by SELinux.

void *allocateExec (nat bytes, void **exec_ret)
{
    void **ret, **exec;
    ACQUIRE_SM_LOCK;
    ret = ffi_closure_alloc (sizeof(void *) + (size_t)bytes, (void**)&exec);
    RELEASE_SM_LOCK;
    if (ret == NULL) return ret;
    *ret = ret; // save the address of the writable mapping, for freeExec().
    *exec_ret = exec + 1;
    return (ret + 1);
}

// freeExec gets passed the executable address, not the writable address. 
void freeExec (void *addr)
{
    void *writable;
    writable = *((void**)addr - 1);
    ACQUIRE_SM_LOCK;
    ffi_closure_free (writable);
    RELEASE_SM_LOCK
}

#else

void *allocateExec (nat bytes, void **exec_ret)
{
    void *ret;
    nat n;

    ACQUIRE_SM_LOCK;

    // round up to words.
    n  = (bytes + sizeof(W_) + 1) / sizeof(W_);

    if (n+1 > BLOCK_SIZE_W) {
	barf("allocateExec: can't handle large objects");
    }

    if (exec_block == NULL || 
	exec_block->free + n + 1 > exec_block->start + BLOCK_SIZE_W) {
	bdescr *bd;
	lnat pagesize = getPageSize();
	bd = allocGroup(stg_max(1, pagesize / BLOCK_SIZE));
	debugTrace(DEBUG_gc, "allocate exec block %p", bd->start);
	bd->gen_no = 0;
	bd->flags = BF_EXEC;
	bd->link = exec_block;
	if (exec_block != NULL) {
	    exec_block->u.back = bd;
	}
	bd->u.back = NULL;
	setExecutable(bd->start, bd->blocks * BLOCK_SIZE, rtsTrue);
	exec_block = bd;
    }
    *(exec_block->free) = n;  // store the size of this chunk
    exec_block->gen_no += n;  // gen_no stores the number of words allocated
    ret = exec_block->free + 1;
    exec_block->free += n + 1;

    RELEASE_SM_LOCK
    *exec_ret = ret;
    return ret;
}

void freeExec (void *addr)
{
    StgPtr p = (StgPtr)addr - 1;
    bdescr *bd = Bdescr((StgPtr)p);

    if ((bd->flags & BF_EXEC) == 0) {
	barf("freeExec: not executable");
    }

    if (*(StgPtr)p == 0) {
	barf("freeExec: already free?");
    }

    ACQUIRE_SM_LOCK;

    bd->gen_no -= *(StgPtr)p;
    *(StgPtr)p = 0;

    if (bd->gen_no == 0) {
        // Free the block if it is empty, but not if it is the block at
        // the head of the queue.
        if (bd != exec_block) {
            debugTrace(DEBUG_gc, "free exec block %p", bd->start);
            dbl_link_remove(bd, &exec_block);
            setExecutable(bd->start, bd->blocks * BLOCK_SIZE, rtsFalse);
            freeGroup(bd);
        } else {
            bd->free = bd->start;
        }
    }

    RELEASE_SM_LOCK
}    

#endif /* mingw32_HOST_OS */

#ifdef DEBUG

// handy function for use in gdb, because Bdescr() is inlined.
extern bdescr *_bdescr( StgPtr p );

bdescr *
_bdescr( StgPtr p )
{
    return Bdescr(p);
}

#endif
