/******************************************************************************
 * page_alloc.c
 * 
 * Simple buddy heap allocator for Xen.
 * 
 * Copyright (c) 2002-2004 K A Fraser
 * Copyright (c) 2006 IBM Ryan Harper <ryanh@us.ibm.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/spinlock.h>
#include <xen/mm.h>
#include <xen/irq.h>
#include <xen/softirq.h>
#include <xen/domain_page.h>
#include <xen/keyhandler.h>
#include <xen/perfc.h>
#include <xen/numa.h>
#include <xen/nodemask.h>
#include <public/sysctl.h>
#include <asm/page.h>
#include <asm/numa.h>
#include <asm/flushtlb.h>

/*
 * Comma-separated list of hexadecimal page numbers containing bad bytes.
 * e.g. 'badpage=0x3f45,0x8a321'.
 */
static char opt_badpage[100] = "";
string_param("badpage", opt_badpage);

/*
 * no-bootscrub -> Free pages are not zeroed during boot.
 */
static int opt_bootscrub __initdata = 1;
boolean_param("bootscrub", opt_bootscrub);

/*
 * Bit width of the DMA heap -- used to override NUMA-node-first.
 * allocation strategy, which can otherwise exhaust low memory.
 */
static unsigned int dma_bitsize;
integer_param("dma_bits", dma_bitsize);

#define round_pgdown(_p)  ((_p)&PAGE_MASK)
#define round_pgup(_p)    (((_p)+(PAGE_SIZE-1))&PAGE_MASK)

#ifndef NDEBUG
/* Avoid callers relying on allocations returning zeroed pages. */
#define scrub_page(p) memset((p), 0xc2, PAGE_SIZE)
#else
/* For a production build, clear_page() is the fastest way to scrub. */
#define scrub_page(p) clear_page(p)
#endif

static DEFINE_SPINLOCK(page_scrub_lock);
PAGE_LIST_HEAD(page_scrub_list);
static unsigned long scrub_pages;

/* Offlined page list, protected by heap_lock. */
PAGE_LIST_HEAD(page_offlined_list);
/* Broken page list, protected by heap_lock. */
PAGE_LIST_HEAD(page_broken_list);

/*********************
 * ALLOCATION BITMAP
 *  One bit per page of memory. Bit set => page is allocated.
 */

unsigned long *alloc_bitmap;
#define PAGES_PER_MAPWORD (sizeof(unsigned long) * 8)

#define allocated_in_map(_pn)                       \
({  unsigned long ___pn = (_pn);                    \
    !!(alloc_bitmap[___pn/PAGES_PER_MAPWORD] &      \
       (1UL<<(___pn&(PAGES_PER_MAPWORD-1)))); })

/*
 * Hint regarding bitwise arithmetic in map_{alloc,free}:
 *  -(1<<n)  sets all bits >= n. 
 *  (1<<n)-1 sets all bits <  n.
 * Variable names in map_{alloc,free}:
 *  *_idx == Index into `alloc_bitmap' array.
 *  *_off == Bit offset within an element of the `alloc_bitmap' array.
 */

static void map_alloc(unsigned long first_page, unsigned long nr_pages)
{
    unsigned long start_off, end_off, curr_idx, end_idx;

#ifndef NDEBUG
    unsigned long i;
    /* Check that the block isn't already allocated. */
    for ( i = 0; i < nr_pages; i++ )
        ASSERT(!allocated_in_map(first_page + i));
#endif

    curr_idx  = first_page / PAGES_PER_MAPWORD;
    start_off = first_page & (PAGES_PER_MAPWORD-1);
    end_idx   = (first_page + nr_pages) / PAGES_PER_MAPWORD;
    end_off   = (first_page + nr_pages) & (PAGES_PER_MAPWORD-1);

    if ( curr_idx == end_idx )
    {
        alloc_bitmap[curr_idx] |= ((1UL<<end_off)-1) & -(1UL<<start_off);
    }
    else 
    {
        alloc_bitmap[curr_idx] |= -(1UL<<start_off);
        while ( ++curr_idx < end_idx ) alloc_bitmap[curr_idx] = ~0UL;
        alloc_bitmap[curr_idx] |= (1UL<<end_off)-1;
    }
}

static void map_free(unsigned long first_page, unsigned long nr_pages)
{
    unsigned long start_off, end_off, curr_idx, end_idx;

#ifndef NDEBUG
    unsigned long i;
    /* Check that the block isn't already freed. */
    for ( i = 0; i < nr_pages; i++ )
        ASSERT(allocated_in_map(first_page + i));
#endif

    curr_idx  = first_page / PAGES_PER_MAPWORD;
    start_off = first_page & (PAGES_PER_MAPWORD-1);
    end_idx   = (first_page + nr_pages) / PAGES_PER_MAPWORD;
    end_off   = (first_page + nr_pages) & (PAGES_PER_MAPWORD-1);

    if ( curr_idx == end_idx )
    {
        alloc_bitmap[curr_idx] &= -(1UL<<end_off) | ((1UL<<start_off)-1);
    }
    else 
    {
        alloc_bitmap[curr_idx] &= (1UL<<start_off)-1;
        while ( ++curr_idx != end_idx ) alloc_bitmap[curr_idx] = 0;
        alloc_bitmap[curr_idx] &= -(1UL<<end_off);
    }
}



/*************************
 * BOOT-TIME ALLOCATOR
 */

static unsigned long first_valid_mfn = ~0UL;

/* Initialise allocator to handle up to @max_page pages. */
paddr_t __init init_boot_allocator(paddr_t bitmap_start)
{
    unsigned long bitmap_size;

    bitmap_start = round_pgup(bitmap_start);

    /*
     * Allocate space for the allocation bitmap. Include an extra longword
     * of padding for possible overrun in map_alloc and map_free.
     */
    bitmap_size  = max_page / 8;
    bitmap_size += sizeof(unsigned long);
    bitmap_size  = round_pgup(bitmap_size);
    alloc_bitmap = (unsigned long *)maddr_to_virt(bitmap_start);

    /* All allocated by default. */
    memset(alloc_bitmap, ~0, bitmap_size);

    return bitmap_start + bitmap_size;
}

void __init init_boot_pages(paddr_t ps, paddr_t pe)
{
    unsigned long bad_spfn, bad_epfn, i;
    const char *p;

    ps = round_pgup(ps);
    pe = round_pgdown(pe);
    if ( pe <= ps )
        return;

    first_valid_mfn = min_t(unsigned long, ps >> PAGE_SHIFT, first_valid_mfn);

    map_free(ps >> PAGE_SHIFT, (pe - ps) >> PAGE_SHIFT);

    /* Check new pages against the bad-page list. */
    p = opt_badpage;
    while ( *p != '\0' )
    {
        bad_spfn = simple_strtoul(p, &p, 0);
        bad_epfn = bad_spfn;

        if ( *p == '-' )
        {
            p++;
            bad_epfn = simple_strtoul(p, &p, 0);
            if ( bad_epfn < bad_spfn )
                bad_epfn = bad_spfn;
        }

        if ( *p == ',' )
            p++;
        else if ( *p != '\0' )
            break;

        if ( bad_epfn == bad_spfn )
            printk("Marking page %lx as bad\n", bad_spfn);
        else
            printk("Marking pages %lx through %lx as bad\n",
                   bad_spfn, bad_epfn);

        for ( i = bad_spfn; i <= bad_epfn; i++ )
            if ( (i < max_page) && !allocated_in_map(i) )
                map_alloc(i, 1);
    }
}

unsigned long __init alloc_boot_pages(
    unsigned long nr_pfns, unsigned long pfn_align)
{
    unsigned long pg, i;

    /* Search backwards to obtain highest available range. */
    for ( pg = (max_page - nr_pfns) & ~(pfn_align - 1);
          pg >= first_valid_mfn;
          pg = (pg + i - nr_pfns) & ~(pfn_align - 1) )
    {
        for ( i = 0; i < nr_pfns; i++ )
            if ( allocated_in_map(pg+i) )
                break;
        if ( i == nr_pfns )
        {
            map_alloc(pg, nr_pfns);
            return pg;
        }
    }

    return 0;
}



/*************************
 * BINARY BUDDY ALLOCATOR
 */

#define MEMZONE_XEN 0
#define NR_ZONES    (PADDR_BITS - PAGE_SHIFT)

#define bits_to_zone(b) (((b) < (PAGE_SHIFT + 1)) ? 0 : ((b) - PAGE_SHIFT - 1))
#define page_to_zone(pg) (is_xen_heap_page(pg) ? MEMZONE_XEN :  \
                          (fls(page_to_mfn(pg)) - 1))

typedef struct page_list_head heap_by_zone_and_order_t[NR_ZONES][MAX_ORDER+1];
static heap_by_zone_and_order_t *_heap[MAX_NUMNODES];
#define heap(node, zone, order) ((*_heap[node])[zone][order])

static unsigned long *avail[MAX_NUMNODES];

static DEFINE_SPINLOCK(heap_lock);

static unsigned long init_node_heap(int node, unsigned long mfn,
                                    unsigned long nr)
{
    /* First node to be discovered has its heap metadata statically alloced. */
    static heap_by_zone_and_order_t _heap_static;
    static unsigned long avail_static[NR_ZONES];
    static int first_node_initialised;
    unsigned long needed = (sizeof(**_heap) +
                            sizeof(**avail) * NR_ZONES +
                            PAGE_SIZE - 1) >> PAGE_SHIFT;
    int i, j;

    if ( !first_node_initialised )
    {
        _heap[node] = &_heap_static;
        avail[node] = avail_static;
        first_node_initialised = 1;
        needed = 0;
    }
#ifdef DIRECTMAP_VIRT_END
    else if ( nr >= needed &&
              (mfn + needed) <= (virt_to_mfn(DIRECTMAP_VIRT_END - 1) + 1) )
    {
        _heap[node] = mfn_to_virt(mfn);
        avail[node] = mfn_to_virt(mfn + needed - 1) +
                      PAGE_SIZE - sizeof(**avail) * NR_ZONES;
    }
#endif
    else if ( get_order_from_bytes(sizeof(**_heap)) ==
              get_order_from_pages(needed) )
    {
        _heap[node] = alloc_xenheap_pages(get_order_from_pages(needed), 0);
        BUG_ON(!_heap[node]);
        avail[node] = (void *)_heap[node] + (needed << PAGE_SHIFT) -
                      sizeof(**avail) * NR_ZONES;
        needed = 0;
    }
    else
    {
        _heap[node] = xmalloc(heap_by_zone_and_order_t);
        avail[node] = xmalloc_array(unsigned long, NR_ZONES);
        BUG_ON(!_heap[node] || !avail[node]);
        needed = 0;
    }

    memset(avail[node], 0, NR_ZONES * sizeof(long));

    for ( i = 0; i < NR_ZONES; i++ )
        for ( j = 0; j <= MAX_ORDER; j++ )
            INIT_PAGE_LIST_HEAD(&(*_heap[node])[i][j]);

    return needed;
}

/* Allocate 2^@order contiguous pages. */
static struct page_info *alloc_heap_pages(
    unsigned int zone_lo, unsigned int zone_hi,
    unsigned int node, unsigned int order)
{
    unsigned int i, j, zone;
    unsigned int num_nodes = num_online_nodes();
    unsigned long request = 1UL << order;
    cpumask_t extra_cpus_mask, mask;
    struct page_info *pg;

    if ( node == NUMA_NO_NODE )
        node = cpu_to_node(smp_processor_id());

    ASSERT(node >= 0);
    ASSERT(zone_lo <= zone_hi);
    ASSERT(zone_hi < NR_ZONES);

    if ( unlikely(order > MAX_ORDER) )
        return NULL;

    spin_lock(&heap_lock);

    /*
     * Start with requested node, but exhaust all node memory in requested 
     * zone before failing, only calc new node value if we fail to find memory 
     * in target node, this avoids needless computation on fast-path.
     */
    for ( i = 0; i < num_nodes; i++ )
    {
        zone = zone_hi;
        do {
            /* Check if target node can support the allocation. */
            if ( !avail[node] || (avail[node][zone] < request) )
                continue;

            /* Find smallest order which can satisfy the request. */
            for ( j = order; j <= MAX_ORDER; j++ )
                if ( (pg = page_list_remove_head(&heap(node, zone, j))) )
                    goto found;
        } while ( zone-- > zone_lo ); /* careful: unsigned zone may wrap */

        /* Pick next node, wrapping around if needed. */
        node = next_node(node, node_online_map);
        if (node == MAX_NUMNODES)
            node = first_node(node_online_map);
    }

    /* No suitable memory blocks. Fail the request. */
    spin_unlock(&heap_lock);
    return NULL;

 found: 
    /* We may have to halve the chunk a number of times. */
    while ( j != order )
    {
        PFN_ORDER(pg) = --j;
        page_list_add_tail(pg, &heap(node, zone, j));
        pg += 1 << j;
    }
    
    map_alloc(page_to_mfn(pg), request);
    ASSERT(avail[node][zone] >= request);
    avail[node][zone] -= request;

    spin_unlock(&heap_lock);

    cpus_clear(mask);

    for ( i = 0; i < (1 << order); i++ )
    {
        /* Reference count must continuously be zero for free pages. */
        BUG_ON(pg[i].count_info != 0);

        if ( pg[i].u.free.need_tlbflush )
        {
            /* Add in extra CPUs that need flushing because of this page. */
            cpus_andnot(extra_cpus_mask, cpu_online_map, mask);
            tlbflush_filter(extra_cpus_mask, pg[i].tlbflush_timestamp);
            cpus_or(mask, mask, extra_cpus_mask);
        }

        /* Initialise fields which have other uses for free pages. */
        pg[i].u.inuse.type_info = 0;
        page_set_owner(&pg[i], NULL);
    }

    if ( unlikely(!cpus_empty(mask)) )
    {
        perfc_incr(need_flush_tlb_flush);
        flush_tlb_mask(mask);
    }

    return pg;
}

/* Remove any offlined page in the buddy pointed to by head. */
static int reserve_offlined_page(struct page_info *head)
{
    unsigned int node = phys_to_nid(page_to_maddr(head));
    int zone = page_to_zone(head), i, head_order = PFN_ORDER(head), count = 0;
    struct page_info *cur_head;
    int cur_order;

    ASSERT(spin_is_locked(&heap_lock));

    cur_head = head;

    page_list_del(head, &heap(node, zone, head_order));

    while ( cur_head < (head + (1 << head_order)) )
    {
        struct page_info *pg;
        int next_order;

        if ( test_bit(_PGC_offlined, &cur_head->count_info) )
        {
            cur_head++;
            continue;
        }

        next_order = cur_order = 0;

        while ( cur_order < head_order )
        {
            next_order = cur_order + 1;

            if ( (cur_head + (1 << next_order)) >= (head + ( 1 << head_order)) )
                goto merge;

            for ( i = (1 << cur_order), pg = cur_head + (1 << cur_order );
                  i < (1 << next_order);
                  i++, pg++ )
                if ( test_bit(_PGC_offlined, &pg->count_info) )
                    break;
            if ( i == ( 1 << next_order) )
            {
                cur_order = next_order;
                continue;
            }
            else
            {
            merge:
                /* We don't consider merging outside the head_order. */
                page_list_add_tail(cur_head, &heap(node, zone, cur_order));
                PFN_ORDER(cur_head) = cur_order;
                cur_head += (1 << cur_order);
                break;
            }
        }
    }

    for ( cur_head = head; cur_head < head + ( 1UL << head_order); cur_head++ )
    {
        if ( !test_bit(_PGC_offlined, &cur_head->count_info) )
            continue;

        avail[node][zone]--;

        map_alloc(page_to_mfn(cur_head), 1);

        page_list_add_tail(cur_head,
                           test_bit(_PGC_broken, &cur_head->count_info) ?
                           &page_broken_list : &page_offlined_list);

        count++;
    }

    return count;
}

/* Free 2^@order set of pages. */
static void free_heap_pages(
    struct page_info *pg, unsigned int order)
{
    unsigned long mask;
    unsigned int i, node = phys_to_nid(page_to_maddr(pg)), tainted = 0;
    unsigned int zone = page_to_zone(pg);

    ASSERT(order <= MAX_ORDER);
    ASSERT(node >= 0);

    for ( i = 0; i < (1 << order); i++ )
    {
        /*
         * Cannot assume that count_info == 0, as there are some corner cases
         * where it isn't the case and yet it isn't a bug:
         *  1. page_get_owner() is NULL
         *  2. page_get_owner() is a domain that was never accessible by
         *     its domid (e.g., failed to fully construct the domain).
         *  3. page was never addressable by the guest (e.g., it's an
         *     auto-translate-physmap guest and the page was never included
         *     in its pseudophysical address space).
         * In all the above cases there can be no guest mappings of this page.
         */
        ASSERT(!(pg[i].count_info & PGC_offlined));
        pg[i].count_info &= PGC_offlining | PGC_broken;
        if ( pg[i].count_info & PGC_offlining )
        {
            pg[i].count_info &= ~PGC_offlining;
            pg[i].count_info |= PGC_offlined;
            tainted = 1;
        }

        /* If a page has no owner it will need no safety TLB flush. */
        pg[i].u.free.need_tlbflush = (page_get_owner(&pg[i]) != NULL);
        if ( pg[i].u.free.need_tlbflush )
            pg[i].tlbflush_timestamp = tlbflush_current_time();
    }

    spin_lock(&heap_lock);

    map_free(page_to_mfn(pg), 1 << order);
    avail[node][zone] += 1 << order;

    /* Merge chunks as far as possible. */
    while ( order < MAX_ORDER )
    {
        mask = 1UL << order;

        if ( (page_to_mfn(pg) & mask) )
        {
            /* Merge with predecessor block? */
            if ( allocated_in_map(page_to_mfn(pg)-mask) ||
                 (PFN_ORDER(pg-mask) != order) )
                break;
            pg -= mask;
            page_list_del(pg, &heap(node, zone, order));
        }
        else
        {
            /* Merge with successor block? */
            if ( allocated_in_map(page_to_mfn(pg)+mask) ||
                 (PFN_ORDER(pg+mask) != order) )
                break;
            page_list_del(pg + mask, &heap(node, zone, order));
        }

        order++;

        /* After merging, pg should remain in the same node. */
        ASSERT(phys_to_nid(page_to_maddr(pg)) == node);
    }

    PFN_ORDER(pg) = order;
    page_list_add_tail(pg, &heap(node, zone, order));

    if ( tainted )
        reserve_offlined_page(pg);

    spin_unlock(&heap_lock);
}


/*
 * Following possible status for a page:
 * free and Online; free and offlined; free and offlined and broken;
 * assigned and online; assigned and offlining; assigned and offling and broken
 *
 * Following rules applied for page offline:
 * Once a page is broken, it can't be assigned anymore
 * A page will be offlined only if it is free
 * return original count_info
 *
 */
static unsigned long mark_page_offline(struct page_info *pg, int broken)
{
    unsigned long nx, x, y = pg->count_info;

    ASSERT(page_is_ram_type(page_to_mfn(pg), RAM_TYPE_CONVENTIONAL));
    ASSERT(spin_is_locked(&heap_lock));

    do {
        nx = x = y;

        if ( ((x & PGC_offlined_broken) == PGC_offlined_broken) )
            return y;

        if ( x & PGC_offlined )
        {
            /* PGC_offlined means it is a free page. */
            if ( broken && !(nx & PGC_broken) )
                nx |= PGC_broken;
            else
                return y;
        }
        else
        {
            /* It is not offlined, not reserved page */
            nx |= (allocated_in_map(page_to_mfn(pg)) ?
                   PGC_offlining : PGC_offlined);
        }

        if ( broken )
            nx |= PGC_broken;
    } while ( (y = cmpxchg(&pg->count_info, x, nx)) != x );

    return y;
}

static int reserve_heap_page(struct page_info *pg)
{
    struct page_info *head = NULL;
    unsigned int i, node = phys_to_nid(page_to_maddr(pg));
    unsigned int zone = page_to_zone(pg);

    for ( i = 0; i <= MAX_ORDER; i++ )
    {
        struct page_info *tmp;

        if ( page_list_empty(&heap(node, zone, i)) )
            continue;

        page_list_for_each_safe ( head, tmp, &heap(node, zone, i) )
        {
            if ( (head <= pg) &&
                 (head + (1UL << i) > pg) )
                return reserve_offlined_page(head);
        }
    }

    return -EINVAL;

}

int offline_page(unsigned long mfn, int broken, uint32_t *status)
{
    unsigned long old_info = 0;
    struct domain *owner;
    int ret = 0;
    struct page_info *pg;

    if ( mfn > max_page )
    {
        dprintk(XENLOG_WARNING,
                "try to offline page out of range %lx\n", mfn);
        return -EINVAL;
    }

    *status = 0;
    pg = mfn_to_page(mfn);

#if defined(__x86_64__)
     /* Xen's txt mfn in x86_64 is reserved in e820 */
    if ( is_xen_fixed_mfn(mfn) )
#elif defined(__i386__)
    if ( is_xen_heap_mfn(mfn) )
#endif
    {
        *status = PG_OFFLINE_XENPAGE | PG_OFFLINE_FAILED |
          (DOMID_XEN << PG_OFFLINE_OWNER_SHIFT);
        return -EPERM;
    }

    /*
     * N.B. xen's txt in x86_64 is marked reserved and handled already
     *  Also kexec range is reserved
     */
     if ( !page_is_ram_type(mfn, RAM_TYPE_CONVENTIONAL) )
     {
        *status = PG_OFFLINE_FAILED | PG_OFFLINE_NOT_CONV_RAM;
        return -EINVAL;
     }

    spin_lock(&heap_lock);

    old_info = mark_page_offline(pg, broken);

    if ( !allocated_in_map(mfn) )
    {
        /* Free pages are reserve directly */
        reserve_heap_page(pg);
        *status = PG_OFFLINE_OFFLINED;
    }
    else if ( test_bit(_PGC_offlined, &pg->count_info) )
    {
        *status = PG_OFFLINE_OFFLINED;
    }
    else if ( (owner = page_get_owner_and_reference(pg)) )
    {
            *status = PG_OFFLINE_OWNED | PG_OFFLINE_PENDING |
              (owner->domain_id << PG_OFFLINE_OWNER_SHIFT);
            /* Release the reference since it will not be allocated anymore */
            put_page(pg);
    }
    else if ( old_info & PGC_xen_heap)
    {
        *status = PG_OFFLINE_XENPAGE | PG_OFFLINE_PENDING |
          (DOMID_XEN << PG_OFFLINE_OWNER_SHIFT);
    }
    else
    {
        /*
         * assign_pages does not hold heap_lock, so small window that the owner
         * may be set later, but please notice owner will only change from
         * NULL to be set, not verse, since page is offlining now.
         * No windows If called from #MC handler, since all CPU are in softirq
         * If called from user space like CE handling, tools can wait some time
         * before call again.
         */
        *status = PG_OFFLINE_ANONYMOUS | PG_OFFLINE_FAILED |
                  (DOMID_INVALID << PG_OFFLINE_OWNER_SHIFT );
    }

    if ( broken )
        *status |= PG_OFFLINE_BROKEN;

    spin_unlock(&heap_lock);

    return ret;
}

/*
 * Online the memory.
 *   The caller should make sure end_pfn <= max_page,
 *   if not, expand_pages() should be called prior to online_page().
 */
unsigned int online_page(unsigned long mfn, uint32_t *status)
{
    struct page_info *pg;
    int ret = 0, free = 0;

    if ( mfn > max_page )
    {
        dprintk(XENLOG_WARNING, "call expand_pages() first\n");
        return -EINVAL;
    }

    pg = mfn_to_page(mfn);

    *status = 0;

    spin_lock(&heap_lock);

    if ( unlikely(is_page_broken(pg)) )
    {
        ret = -EINVAL;
        *status = PG_ONLINE_FAILED |PG_ONLINE_BROKEN;
    }
    else if ( pg->count_info & PGC_offlined )
    {
        clear_bit(_PGC_offlined, &pg->count_info);
        page_list_del(pg, &page_offlined_list);
        *status = PG_ONLINE_ONLINED;
        free = 1;
    }
    else if ( pg->count_info & PGC_offlining )
    {
        clear_bit(_PGC_offlining, &pg->count_info);
        *status = PG_ONLINE_ONLINED;
    }
    spin_unlock(&heap_lock);

    if ( free )
        free_heap_pages(pg, 0);

    return ret;
}

int query_page_offline(unsigned long mfn, uint32_t *status)
{
    struct page_info *pg;

    if ( (mfn > max_page) || !page_is_ram_type(mfn, RAM_TYPE_CONVENTIONAL) )
    {
        dprintk(XENLOG_WARNING, "call expand_pages() first\n");
        return -EINVAL;
    }

    *status = 0;
    spin_lock(&heap_lock);

    pg = mfn_to_page(mfn);

    if (pg->count_info & PGC_offlining)
        *status |= PG_OFFLINE_STATUS_OFFLINE_PENDING;
    if (pg->count_info & PGC_broken)
        *status |= PG_OFFLINE_STATUS_BROKEN;
    if (pg->count_info & PGC_offlined)
        *status |= PG_OFFLINE_STATUS_OFFLINED;

    spin_unlock(&heap_lock);

    return 0;
}

/*
 * Hand the specified arbitrary page range to the specified heap zone
 * checking the node_id of the previous page.  If they differ and the
 * latter is not on a MAX_ORDER boundary, then we reserve the page by
 * not freeing it to the buddy allocator.
 */
static void init_heap_pages(
    struct page_info *pg, unsigned long nr_pages)
{
    unsigned int nid_curr, nid_prev;
    unsigned long i;

    nid_prev = phys_to_nid(page_to_maddr(pg-1));

    for ( i = 0; i < nr_pages; nid_prev = nid_curr, i++ )
    {
        nid_curr = phys_to_nid(page_to_maddr(pg+i));

        if ( unlikely(!avail[nid_curr]) )
        {
            unsigned long n;

            n = init_node_heap(nid_curr, page_to_mfn(pg+i), nr_pages - i);
            if ( n )
            {
                BUG_ON(i + n > nr_pages);
                i += n - 1;
                continue;
            }
        }

        /*
         * Free pages of the same node, or if they differ, but are on a
         * MAX_ORDER alignment boundary (which already get reserved).
         */
        if ( (nid_curr == nid_prev) ||
             !(page_to_mfn(pg+i) & ((1UL << MAX_ORDER) - 1)) )
            free_heap_pages(pg+i, 0);
        else
            printk("Reserving non-aligned node boundary @ mfn %#lx\n",
                   page_to_mfn(pg+i));
    }
}

static unsigned long avail_heap_pages(
    unsigned int zone_lo, unsigned int zone_hi, unsigned int node)
{
    unsigned int i, zone;
    unsigned long free_pages = 0;

    if ( zone_hi >= NR_ZONES )
        zone_hi = NR_ZONES - 1;

    for_each_online_node(i)
    {
        if ( !avail[i] )
            continue;
        for ( zone = zone_lo; zone <= zone_hi; zone++ )
            if ( (node == -1) || (node == i) )
                free_pages += avail[i][zone];
    }

    return free_pages;
}

#define avail_for_domheap(mfn) !(allocated_in_map(mfn) || is_xen_heap_mfn(mfn))
void __init end_boot_allocator(void)
{
    unsigned long i, nr = 0;
    int curr_free, next_free;

    /* Pages that are free now go to the domain sub-allocator. */
    if ( (curr_free = next_free = avail_for_domheap(first_valid_mfn)) )
        map_alloc(first_valid_mfn, 1);
    for ( i = first_valid_mfn; i < max_page; i++ )
    {
        curr_free = next_free;
        next_free = avail_for_domheap(i+1);
        if ( next_free )
            map_alloc(i+1, 1); /* prevent merging in free_heap_pages() */
        if ( curr_free )
            ++nr;
        else if ( nr )
        {
            init_heap_pages(mfn_to_page(i - nr), nr);
            nr = 0;
        }
    }
    if ( nr )
        init_heap_pages(mfn_to_page(i - nr), nr);

    if ( !dma_bitsize && (num_online_nodes() > 1) )
    {
#ifdef CONFIG_X86
        dma_bitsize = min_t(unsigned int,
                            fls(NODE_DATA(0)->node_spanned_pages) - 1
                            + PAGE_SHIFT - 2,
                            32);
#else
        dma_bitsize = 32;
#endif
    }

    printk("Domain heap initialised");
    if ( dma_bitsize )
        printk(" DMA width %u bits", dma_bitsize);
    printk("\n");
}
#undef avail_for_domheap

/*
 * Scrub all unallocated pages in all heap zones. This function is more
 * convoluted than appears necessary because we do not want to continuously
 * hold the lock while scrubbing very large memory areas.
 */
void __init scrub_heap_pages(void)
{
    void *p;
    unsigned long mfn;

    if ( !opt_bootscrub )
        return;

    printk("Scrubbing Free RAM: ");

    for ( mfn = first_valid_mfn; mfn < max_page; mfn++ )
    {
        process_pending_softirqs();

        /* Quick lock-free check. */
        if ( allocated_in_map(mfn) )
            continue;

        /* Every 100MB, print a progress dot. */
        if ( (mfn % ((100*1024*1024)/PAGE_SIZE)) == 0 )
            printk(".");

        spin_lock(&heap_lock);

        /* Re-check page status with lock held. */
        if ( !allocated_in_map(mfn) )
        {
            if ( is_xen_heap_mfn(mfn) )
            {
                p = page_to_virt(mfn_to_page(mfn));
                memguard_unguard_range(p, PAGE_SIZE);
                scrub_page(p);
                memguard_guard_range(p, PAGE_SIZE);
            }
            else
            {
                p = map_domain_page(mfn);
                scrub_page(p);
                unmap_domain_page(p);
            }
        }

        spin_unlock(&heap_lock);
    }

    printk("done.\n");
}



/*************************
 * XEN-HEAP SUB-ALLOCATOR
 */

#if !defined(__x86_64__) && !defined(__ia64__)

void init_xenheap_pages(paddr_t ps, paddr_t pe)
{
    ps = round_pgup(ps);
    pe = round_pgdown(pe);
    if ( pe <= ps )
        return;

    memguard_guard_range(maddr_to_virt(ps), pe - ps);

    /*
     * Yuk! Ensure there is a one-page buffer between Xen and Dom zones, to
     * prevent merging of power-of-two blocks across the zone boundary.
     */
    if ( ps && !is_xen_heap_mfn(paddr_to_pfn(ps)-1) )
        ps += PAGE_SIZE;
    if ( !is_xen_heap_mfn(paddr_to_pfn(pe)) )
        pe -= PAGE_SIZE;

    init_heap_pages(maddr_to_page(ps), (pe - ps) >> PAGE_SHIFT);
}


void *alloc_xenheap_pages(unsigned int order, unsigned int memflags)
{
    struct page_info *pg;

    ASSERT(!in_irq());

    pg = alloc_heap_pages(
        MEMZONE_XEN, MEMZONE_XEN, cpu_to_node(smp_processor_id()), order);
    if ( unlikely(pg == NULL) )
        return NULL;

    memguard_unguard_range(page_to_virt(pg), 1 << (order + PAGE_SHIFT));

    return page_to_virt(pg);
}


void free_xenheap_pages(void *v, unsigned int order)
{
    ASSERT(!in_irq());

    if ( v == NULL )
        return;

    memguard_guard_range(v, 1 << (order + PAGE_SHIFT));

    free_heap_pages(virt_to_page(v), order);
}

#else

void init_xenheap_pages(paddr_t ps, paddr_t pe)
{
    init_domheap_pages(ps, pe);
}

void *alloc_xenheap_pages(unsigned int order, unsigned int memflags)
{
    struct page_info *pg;
    unsigned int i;

    ASSERT(!in_irq());

    pg = alloc_domheap_pages(NULL, order, memflags);
    if ( unlikely(pg == NULL) )
        return NULL;

    for ( i = 0; i < (1u << order); i++ )
        pg[i].count_info |= PGC_xen_heap;

    return page_to_virt(pg);
}

void free_xenheap_pages(void *v, unsigned int order)
{
    struct page_info *pg;
    unsigned int i;

    ASSERT(!in_irq());

    if ( v == NULL )
        return;

    pg = virt_to_page(v);

    for ( i = 0; i < (1u << order); i++ )
        pg[i].count_info &= ~PGC_xen_heap;

    free_heap_pages(pg, order);
}

#endif



/*************************
 * DOMAIN-HEAP SUB-ALLOCATOR
 */

void init_domheap_pages(paddr_t ps, paddr_t pe)
{
    unsigned long smfn, emfn;

    ASSERT(!in_irq());

    smfn = round_pgup(ps) >> PAGE_SHIFT;
    emfn = round_pgdown(pe) >> PAGE_SHIFT;

    init_heap_pages(mfn_to_page(smfn), emfn - smfn);
}


int assign_pages(
    struct domain *d,
    struct page_info *pg,
    unsigned int order,
    unsigned int memflags)
{
    unsigned long i;

    spin_lock(&d->page_alloc_lock);

    if ( unlikely(d->is_dying) )
    {
        gdprintk(XENLOG_INFO, "Cannot assign page to domain%d -- dying.\n",
                d->domain_id);
        goto fail;
    }

    if ( !(memflags & MEMF_no_refcount) )
    {
        if ( unlikely((d->tot_pages + (1 << order)) > d->max_pages) )
        {
            gdprintk(XENLOG_INFO, "Over-allocation for domain %u: %u > %u\n",
                    d->domain_id, d->tot_pages + (1 << order), d->max_pages);
            goto fail;
        }

        if ( unlikely(d->tot_pages == 0) )
            get_knownalive_domain(d);

        d->tot_pages += 1 << order;
    }

    for ( i = 0; i < (1 << order); i++ )
    {
        ASSERT(page_get_owner(&pg[i]) == NULL);
        ASSERT((pg[i].count_info & ~(PGC_allocated | 1)) == 0);
        page_set_owner(&pg[i], d);
        wmb(); /* Domain pointer must be visible before updating refcnt. */
        pg[i].count_info = PGC_allocated | 1;
        page_list_add_tail(&pg[i], &d->page_list);
    }

    spin_unlock(&d->page_alloc_lock);
    return 0;

 fail:
    spin_unlock(&d->page_alloc_lock);
    return -1;
}


struct page_info *alloc_domheap_pages(
    struct domain *d, unsigned int order, unsigned int memflags)
{
    struct page_info *pg = NULL;
    unsigned int bits = memflags >> _MEMF_bits, zone_hi = NR_ZONES - 1;
    unsigned int node = (uint8_t)((memflags >> _MEMF_node) - 1), dma_zone;

    ASSERT(!in_irq());

    if ( (node == NUMA_NO_NODE) && (d != NULL) )
        node = domain_to_node(d);

    bits = domain_clamp_alloc_bitsize(d, bits ? : (BITS_PER_LONG+PAGE_SHIFT));
    if ( (zone_hi = min_t(unsigned int, bits_to_zone(bits), zone_hi)) == 0 )
        return NULL;

    if ( dma_bitsize && ((dma_zone = bits_to_zone(dma_bitsize)) < zone_hi) )
        pg = alloc_heap_pages(dma_zone + 1, zone_hi, node, order);

    if ( (pg == NULL) &&
         ((pg = alloc_heap_pages(MEMZONE_XEN + 1, zone_hi,
                                 node, order)) == NULL) )
         return NULL;

    if ( (d != NULL) && assign_pages(d, pg, order, memflags) )
    {
        free_heap_pages(pg, order);
        return NULL;
    }
    
    return pg;
}

void free_domheap_pages(struct page_info *pg, unsigned int order)
{
    int            i, drop_dom_ref;
    struct domain *d = page_get_owner(pg);

    ASSERT(!in_irq());

    if ( unlikely(is_xen_heap_page(pg)) )
    {
        /* NB. May recursively lock from relinquish_memory(). */
        spin_lock_recursive(&d->page_alloc_lock);

        for ( i = 0; i < (1 << order); i++ )
            page_list_del2(&pg[i], &d->xenpage_list, &d->arch.relmem_list);

        d->xenheap_pages -= 1 << order;
        drop_dom_ref = (d->xenheap_pages == 0);

        spin_unlock_recursive(&d->page_alloc_lock);
    }
    else if ( likely(d != NULL) )
    {
        /* NB. May recursively lock from relinquish_memory(). */
        spin_lock_recursive(&d->page_alloc_lock);

        for ( i = 0; i < (1 << order); i++ )
        {
            BUG_ON((pg[i].u.inuse.type_info & PGT_count_mask) != 0);
            page_list_del2(&pg[i], &d->page_list, &d->arch.relmem_list);
        }

        d->tot_pages -= 1 << order;
        drop_dom_ref = (d->tot_pages == 0);

        spin_unlock_recursive(&d->page_alloc_lock);

        if ( likely(!d->is_dying) )
        {
            free_heap_pages(pg, order);
        }
        else
        {
            /*
             * Normally we expect a domain to clear pages before freeing them,
             * if it cares about the secrecy of their contents. However, after
             * a domain has died we assume responsibility for erasure.
             */
            for ( i = 0; i < (1 << order); i++ )
            {
                page_set_owner(&pg[i], NULL);
                spin_lock(&page_scrub_lock);
                page_list_add(&pg[i], &page_scrub_list);
                scrub_pages++;
                spin_unlock(&page_scrub_lock);
            }
        }
    }
    else
    {
        /* Freeing anonymous domain-heap pages. */
        free_heap_pages(pg, order);
        drop_dom_ref = 0;
    }

    if ( drop_dom_ref )
        put_domain(d);
}

unsigned long avail_domheap_pages_region(
    unsigned int node, unsigned int min_width, unsigned int max_width)
{
    int zone_lo, zone_hi;

    zone_lo = min_width ? bits_to_zone(min_width) : (MEMZONE_XEN + 1);
    zone_lo = max_t(int, MEMZONE_XEN + 1, min_t(int, NR_ZONES - 1, zone_lo));

    zone_hi = max_width ? bits_to_zone(max_width) : (NR_ZONES - 1);
    zone_hi = max_t(int, MEMZONE_XEN + 1, min_t(int, NR_ZONES - 1, zone_hi));

    return avail_heap_pages(zone_lo, zone_hi, node);
}

unsigned long avail_domheap_pages(void)
{
    return avail_heap_pages(MEMZONE_XEN + 1,
                            NR_ZONES - 1,
                            -1);
}

static void pagealloc_keyhandler(unsigned char key)
{
    unsigned int zone = MEMZONE_XEN;
    unsigned long n, total = 0;

    printk("Physical memory information:\n");
    printk("    Xen heap: %lukB free\n",
           avail_heap_pages(zone, zone, -1) << (PAGE_SHIFT-10));

    while ( ++zone < NR_ZONES )
    {
        if ( (zone + PAGE_SHIFT) == dma_bitsize )
        {
            printk("    DMA heap: %lukB free\n", total << (PAGE_SHIFT-10));
            total = 0;
        }

        if ( (n = avail_heap_pages(zone, zone, -1)) != 0 )
        {
            total += n;
            printk("    heap[%02u]: %lukB free\n", zone, n << (PAGE_SHIFT-10));
        }
    }

    printk("    Dom heap: %lukB free\n", total << (PAGE_SHIFT-10));
}


static __init int pagealloc_keyhandler_init(void)
{
    register_keyhandler('m', pagealloc_keyhandler, "memory info");
    return 0;
}
__initcall(pagealloc_keyhandler_init);



/*************************
 * PAGE SCRUBBING
 */

static DEFINE_PER_CPU(struct timer, page_scrub_timer);

static void page_scrub_softirq(void)
{
    PAGE_LIST_HEAD(list);
    struct page_info  *pg;
    void             *p;
    int               i;
    s_time_t          start = NOW();
    static spinlock_t serialise_lock = SPIN_LOCK_UNLOCKED;

    /* free_heap_pages() does not parallelise well. Serialise this function. */
    if ( !spin_trylock(&serialise_lock) )
    {
        set_timer(&this_cpu(page_scrub_timer), NOW() + MILLISECS(1));
        return;
    }

    /* Aim to do 1ms of work every 10ms. */
    do {
        spin_lock(&page_scrub_lock);

        /* Peel up to 16 pages from the list. */
        for ( i = 0; i < 16; i++ )
        {
            if ( !(pg = page_list_remove_head(&page_scrub_list)) )
                break;
            page_list_add_tail(pg, &list);
        }
        
        if ( unlikely(i == 0) )
        {
            spin_unlock(&page_scrub_lock);
            goto out;
        }

        scrub_pages -= i;

        spin_unlock(&page_scrub_lock);

        /* Scrub each page in turn. */
        while ( (pg = page_list_remove_head(&list)) ) {
            p = map_domain_page(page_to_mfn(pg));
            scrub_page(p);
            unmap_domain_page(p);
            free_heap_pages(pg, 0);
        }
    } while ( (NOW() - start) < MILLISECS(1) );

    set_timer(&this_cpu(page_scrub_timer), NOW() + MILLISECS(10));

 out:
    spin_unlock(&serialise_lock);
}

static void page_scrub_timer_fn(void *unused)
{
    page_scrub_schedule_work();
}

unsigned long avail_scrub_pages(void)
{
    return scrub_pages;
}

static void dump_heap(unsigned char key)
{
    s_time_t      now = NOW();
    int           i, j;

    printk("'%c' pressed -> dumping heap info (now-0x%X:%08X)\n", key,
           (u32)(now>>32), (u32)now);

    for ( i = 0; i < MAX_NUMNODES; i++ )
    {
        if ( !avail[i] )
            continue;
        for ( j = 0; j < NR_ZONES; j++ )
            printk("heap[node=%d][zone=%d] -> %lu pages\n",
                   i, j, avail[i][j]);
    }
}

static __init int register_heap_trigger(void)
{
    register_keyhandler('H', dump_heap, "dump heap info");
    return 0;
}
__initcall(register_heap_trigger);


static __init int page_scrub_init(void)
{
    int cpu;
    for_each_cpu ( cpu )
        init_timer(&per_cpu(page_scrub_timer, cpu),
                   page_scrub_timer_fn, NULL, cpu);
    open_softirq(PAGE_SCRUB_SOFTIRQ, page_scrub_softirq);
    return 0;
}
__initcall(page_scrub_init);

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
