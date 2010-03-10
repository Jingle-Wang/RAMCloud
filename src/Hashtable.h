/* Copyright (c) 2009-2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// RAMCloud pragma [CPPLINT=0]

/**
 * \file
 * Header file for Hashtable.
 */

#ifndef RAMCLOUD_HASHTABLE_H
#define RAMCLOUD_HASHTABLE_H

#include <Common.h>

namespace RAMCloud {

/**
 * A map from object IDs to a pointer to the Log in memory where the latest
 * version of the object resides.
 *
 * This is used in resolving most object-level %RAMCloud requests. For example,
 * to read and write a %RAMCloud object, this lets you find the location of the
 * current version of the object.
 *
 * Currently, the Hashtable class assumes it is scoped to a specific %RAMCloud
 * table, so it does not concern itself with table IDs.
 *
 * \section impl Implementation Details
 *
 * The Hashtable is an array of buckets, indexed by the hash of the object ID.
 * Each bucket consists of one or more chained cache lines, the first of which
 * lives inline in the array of buckets. Each cache line consists of several
 * hash table entries (Entry) in no particular order, which contain additional
 * bits from the hash function to disambiguate most bucket collisions and a
 * pointer to the latest version of the object in the Log.
 *
 * If there are too many hash table entries to fit the bucket's first cache
 * line, additional cache lines are allocated (outside of the array of
 * buckets). In this case, the last hash table entry in each of the
 * non-terminal cache lines has a pointer to the next cache line instead of a
 * Log pointer.
 */
class Hashtable {
public:

    /**
     * Keeps track of statistics for a frequency distribution.
     * See #Hashtable::PerfCounters::lookupKeyPtrDist for an example.
     */
    struct PerfDistribution {

        /**
         * The number of bins in which to categorize samples.
         * See #bins
         */
        static const uint64_t NBINS = 5000;

        /**
         * The width of each bin.
         * See #bins.
         */
        static const uint64_t BIN_WIDTH = 10;

        /**
         * The frequencies of samples that fall into each bin.
         * The first bin will have the number of samples with a value between 0
         * (inclusive) and BIN_WIDTH (exclusive), the second between BIN_WIDTH
         * and BIN_WIDTH * 2, etc.
         */
        uint64_t bins[NBINS];

        /**
         * The frequency of samples that exceeded the highest bin.
         * This is equivalent to the sum of the values in all bins beyond the
         * end of the \a bins array.
         */
        uint64_t binOverflows;

        /**
         * The minimum sample encountered.
         * This will be <tt>~OUL</tt> if no samples were stored.
         */
        uint64_t min;

        /**
         * The maximum sample.
         * This will be <tt>OUL</tt> if no samples were stored.
         */
        uint64_t max;

        PerfDistribution();
        void storeSample(uint64_t value);
    };

    /**
     * Performance counters for the Hashtable.
     */
    struct PerfCounters {

        /**
         * The sum of the number of CPU cycles spent across all #Insert()
         * operations.
         */
        uint64_t insertCycles;

        /**
         * The sum of the number of CPU cycles spent across all #LookupKeyPtr()
         * operations.
         */
        uint64_t lookupKeyPtrCycles;

        /**
         * The sum of the number of times a chain pointer was followed across all
         * #Insert() operations.
         */
        uint64_t insertChainsFollowed;

        /**
         * The sum of the number of times a chain pointer was followed across all
         * #LookupKeyPtr() operations.
         */
        uint64_t lookupKeyPtrChainsFollowed;

        /**
         * The sum of the number of times there was an Entry collision across
         * all #LookupKeyPtr() operations. This is when the buckets collide
         * for a key, and the extra disambiguation bits inside the Entry
         * collide, but following the Log pointer reveals that the entry does
         * not correspond to the given key.
         */
        uint64_t lookupKeyPtrHashCollisions;

        /**
         * The number of CPU cycles spent for #LookupKeyPtr() operations.
         */
        PerfDistribution lookupKeyPtrDist;

        PerfCounters();
    };

    explicit Hashtable(uint64_t nlines);
    void *Lookup(uint64_t key);
    void Insert(uint64_t key, void *ptr);
    bool Delete(uint64_t key);
    bool Replace(uint64_t key, void *ptr);

    /**
     * \return
     *      A read-only view of the Hashtable's performance counters.
     */
    const PerfCounters& getPerfCounters() {
        return this->perfCounters;
    }

private:

    class Entry;
    Entry *LookupKeyPtr(uint64_t key);
    void StoreSample(uint64_t ticks);
    void *MallocAligned(uint64_t len);

    /**
     * The number of hash table entries (Entry) in a cache line.
     */
    static const uint32_t ENTRIES_PER_CACHE_LINE = 8;

    struct cacheline;

    /**
     * A hash table entry.
     *
     * Hash table entries live on cache lines.
     *
     * A normal hash table entry (see #setLogPointer(), #getLogPointer(), and
     * #hashMatches()) consists of additional bits from the hash function on
     * the object ID to disambiguate most bucket collisions and a pointer into
     * the Log where the latest version of the object lives. In this case, its
     * chain bit will not be set and its pointer will not be \c NULL.
     *
     * A chaining hash table entry (see #setChainPointer(), #getChainPointer(),
     * and #isChainLink()) instead consists of a pointer to another cache line
     * where additional entries can be found. In this case, its chain bit will
     * be set.
     *
     * A hash table entry can also be unused (see #clear() and #isAvailable()).
     * In this case, its pointer will be set to \c NULL.
     */
    class Entry {

      public:
        void clear();
        void setLogPointer(uint64_t hash, void *ptr);
        void setChainPointer(cacheline *ptr);
        bool isAvailable();
        void* getLogPointer();
        cacheline* getChainPointer();
        bool hashMatches(uint64_t hash);
        bool isChainLink();

      private:
        /**
         * The packed value stored in the entry.
         *
         * The exact bits are, from MSB to LSB:
         * \li 16 bits of a hash
         * \li 1 bit for whether the pointer is a chain
         * \li 47 bits for the pointer
         *
         * The main reason why it's not a struct with bit fields is that we'll
         * probably want to use atomic operations to set it eventually.
         *
         * Because the exact format is subject to change, you should always set
         * this using #pack() and access its contained fields using #unpack().
         */
        uint64_t value;

        void pack(uint64_t hash, bool chain, void *ptr);

        /**
         * This is the return type of #unpack().
         * See the parameters of #pack() for an explanation.
         */
        struct UnpackedEntry {
            uint64_t hash;
            bool chain;
            void *ptr;
        };

        UnpackedEntry unpack();

        friend class HashtableEntryTest;
    };

    /**
     * A cache line, part of a hash table bucket and composed of hash table
     * entries.
     */
    struct cacheline {
        /**
         * An array of hash table entries.
         * The final hash table entry may be a chain pointer to another cache
         * line. See Hashtable for more info.
         */
        Entry entries[ENTRIES_PER_CACHE_LINE];
    };

    /**
     * The array of buckets.
     */
    cacheline *table;

    /**
     * The number of buckets allocated to the table.
     */
    uint64_t table_lines;

    /**
     * Whether to allocate memory using #xmalloc_aligned_hugetlb() instead of
     * #xmalloc_aligned_xmalloc().
     */
    bool use_huge_tlb;

    /**
     * The performance counters for the Hash table.
     * See #getPerfCounters().
     */
    PerfCounters perfCounters;

    friend class HashtableEntryTest;
    friend class HashtableTest;
    DISALLOW_COPY_AND_ASSIGN(Hashtable);
};


} // namespace RAMCloud

#endif
