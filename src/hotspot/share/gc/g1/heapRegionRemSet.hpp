/*
 * Copyright (c) 2001, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_GC_G1_HEAPREGIONREMSET_HPP
#define SHARE_GC_G1_HEAPREGIONREMSET_HPP

#include "gc/g1/g1CodeCacheRemSet.hpp"
#include "gc/g1/g1FromCardCache.hpp"
#include "gc/g1/sparsePRT.hpp"
#include "runtime/atomic.hpp"
#include "utilities/bitMap.hpp"

// Remembered set for a heap region.  Represent a set of "cards" that
// contain pointers into the owner heap region.  Cards are defined somewhat
// abstractly, in terms of what the "BlockOffsetTable" in use can parse.

class G1CollectedHeap;
class G1BlockOffsetTable;
class G1CardLiveData;
class HeapRegion;
class PerRegionTable;
class SparsePRT;
class nmethod;

// The "_coarse_map" is a bitmap with one bit for each region, where set
// bits indicate that the corresponding region may contain some pointer
// into the owning region.

// The "_fine_grain_entries" array is an open hash table of PerRegionTables
// (PRTs), indicating regions for which we're keeping the RS as a set of
// cards.  The strategy is to cap the size of the fine-grain table,
// deleting an entry and setting the corresponding coarse-grained bit when
// we would overflow this cap.

// We use a mixture of locking and lock-free techniques here.  We allow
// threads to locate PRTs without locking, but threads attempting to alter
// a bucket list obtain a lock.  This means that any failing attempt to
// find a PRT must be retried with the lock.  It might seem dangerous that
// a read can find a PRT that is concurrently deleted.  This is all right,
// because:
//
//   1) We only actually free PRT's at safe points (though we reuse them at
//      other times).
//   2) We find PRT's in an attempt to add entries.  If a PRT is deleted,
//      it's _coarse_map bit is set, so the that we were attempting to add
//      is represented.  If a deleted PRT is re-used, a thread adding a bit,
//      thinking the PRT is for a different region, does no harm.

class OtherRegionsTable {
  G1CollectedHeap* _g1h;
  Mutex*           _m;

  // These are protected by "_m".
  CHeapBitMap _coarse_map;
  size_t      _n_coarse_entries;
  static jint _n_coarsenings;

  PerRegionTable** _fine_grain_regions;
  size_t           _n_fine_entries;

  // The fine grain remembered sets are doubly linked together using
  // their 'next' and 'prev' fields.
  // This allows fast bulk freeing of all the fine grain remembered
  // set entries, and fast finding of all of them without iterating
  // over the _fine_grain_regions table.
  PerRegionTable * _first_all_fine_prts;
  PerRegionTable * _last_all_fine_prts;

  // Used to sample a subset of the fine grain PRTs to determine which
  // PRT to evict and coarsen.
  size_t        _fine_eviction_start;
  static size_t _fine_eviction_stride;
  static size_t _fine_eviction_sample_size;

  SparsePRT   _sparse_table;

  // These are static after init.
  static size_t _max_fine_entries;
  static size_t _mod_max_fine_entries_mask;

  // Requires "prt" to be the first element of the bucket list appropriate
  // for "hr".  If this list contains an entry for "hr", return it,
  // otherwise return "NULL".
  PerRegionTable* find_region_table(size_t ind, HeapRegion* hr) const;

  // Find, delete, and return a candidate PerRegionTable, if any exists,
  // adding the deleted region to the coarse bitmap.  Requires the caller
  // to hold _m, and the fine-grain table to be full.
  PerRegionTable* delete_region_table();

  // link/add the given fine grain remembered set into the "all" list
  void link_to_all(PerRegionTable * prt);
  // unlink/remove the given fine grain remembered set into the "all" list
  void unlink_from_all(PerRegionTable * prt);

  bool contains_reference_locked(OopOrNarrowOopStar from) const;

  size_t occ_fine() const;
  size_t occ_coarse() const;
  size_t occ_sparse() const;

public:
  // Create a new remembered set. The given mutex is used to ensure consistency.
  OtherRegionsTable(Mutex* m);

  template <class Closure>
  void iterate(Closure& v);

  // Returns the card index of the given within_region pointer relative to the bottom
  // of the given heap region.
  static CardIdx_t card_within_region(OopOrNarrowOopStar within_region, HeapRegion* hr);
  // Adds the reference from "from to this remembered set.
  void add_reference(OopOrNarrowOopStar from, uint tid);

  // Returns whether the remembered set contains the given reference.
  bool contains_reference(OopOrNarrowOopStar from) const;

  // Returns whether this remembered set (and all sub-sets) have an occupancy
  // that is less or equal than the given occupancy.
  bool occupancy_less_or_equal_than(size_t limit) const;

  // Returns whether this remembered set (and all sub-sets) does not contain any entry.
  bool is_empty() const;

  // Returns the number of cards contained in this remembered set.
  size_t occupied() const;

  static jint n_coarsenings() { return _n_coarsenings; }

  // Returns size of the actual remembered set containers in bytes.
  size_t mem_size() const;
  // Returns the size of static data in bytes.
  static size_t static_mem_size();
  // Returns the size of the free list content in bytes.
  static size_t fl_mem_size();

  // Clear the entire contents of this remembered set.
  void clear();
};

class PerRegionTable: public CHeapObj<mtGC> {
  friend class OtherRegionsTable;

  HeapRegion*     _hr;
  CHeapBitMap     _bm;
  jint            _occupied;

  // next pointer for free/allocated 'all' list
  PerRegionTable* _next;

  // prev pointer for the allocated 'all' list
  PerRegionTable* _prev;

  // next pointer in collision list
  PerRegionTable * _collision_list_next;

  // Global free list of PRTs
  static PerRegionTable* volatile _free_list;

protected:
  PerRegionTable(HeapRegion* hr) :
    _hr(hr),
    _bm(HeapRegion::CardsPerRegion, mtGC),
    _occupied(0),
    _next(NULL), _prev(NULL),
    _collision_list_next(NULL)
  {}

public:
  // We need access in order to union things into the base table.
  BitMap* bm() { return &_bm; }

  HeapRegion* hr() const { return Atomic::load_acquire(&_hr); }

  jint occupied() const {
    // Overkill, but if we ever need it...
    // guarantee(_occupied == _bm.count_one_bits(), "Check");
    return _occupied;
  }

  void init(HeapRegion* hr, bool clear_links_to_all_list);

  inline void add_reference(OopOrNarrowOopStar from);

  inline void add_card(CardIdx_t from_card_index);

  // (Destructively) union the bitmap of the current table into the given
  // bitmap (which is assumed to be of the same size.)
  void union_bitmap_into(BitMap* bm) {
    bm->set_union(_bm);
  }

  // Mem size in bytes.
  size_t mem_size() const {
    return sizeof(PerRegionTable) + _bm.size_in_words() * HeapWordSize;
  }

  // Requires "from" to be in "hr()".
  bool contains_reference(OopOrNarrowOopStar from) const {
    assert(hr()->is_in_reserved(from), "Precondition.");
    size_t card_ind = pointer_delta(from, hr()->bottom(),
                                    G1CardTable::card_size);
    return _bm.at(card_ind);
  }

  // Bulk-free the PRTs from prt to last, assumes that they are
  // linked together using their _next field.
  static void bulk_free(PerRegionTable* prt, PerRegionTable* last) {
    while (true) {
      PerRegionTable* fl = _free_list;
      last->set_next(fl);
      PerRegionTable* res = Atomic::cmpxchg(&_free_list, fl, prt);
      if (res == fl) {
        return;
      }
    }
    ShouldNotReachHere();
  }

  static void free(PerRegionTable* prt) {
    bulk_free(prt, prt);
  }

  // Returns an initialized PerRegionTable instance.
  static PerRegionTable* alloc(HeapRegion* hr);

  PerRegionTable* next() const { return _next; }
  void set_next(PerRegionTable* next) { _next = next; }
  PerRegionTable* prev() const { return _prev; }
  void set_prev(PerRegionTable* prev) { _prev = prev; }

  // Accessor and Modification routines for the pointer for the
  // singly linked collision list that links the PRTs within the
  // OtherRegionsTable::_fine_grain_regions hash table.
  //
  // It might be useful to also make the collision list doubly linked
  // to avoid iteration over the collisions list during scrubbing/deletion.
  // OTOH there might not be many collisions.

  PerRegionTable* collision_list_next() const {
    return _collision_list_next;
  }

  void set_collision_list_next(PerRegionTable* next) {
    _collision_list_next = next;
  }

  PerRegionTable** collision_list_next_addr() {
    return &_collision_list_next;
  }

  static size_t fl_mem_size() {
    PerRegionTable* cur = _free_list;
    size_t res = 0;
    while (cur != NULL) {
      res += cur->mem_size();
      cur = cur->next();
    }
    return res;
  }

  static void test_fl_mem_size();
};

class HeapRegionRemSet : public CHeapObj<mtGC> {
  friend class VMStructs;

private:
  G1BlockOffsetTable* _bot;

  // A set of code blobs (nmethods) whose code contains pointers into
  // the region that owns this RSet.
  G1CodeRootSet _code_roots;

  Mutex _m;

  OtherRegionsTable _other_regions;

  HeapRegion* _hr;

  void clear_fcc();

public:
  HeapRegionRemSet(G1BlockOffsetTable* bot, HeapRegion* hr);

  // Setup sparse and fine-grain tables sizes.
  static void setup_remset_size();

  bool is_empty() const {
    return (strong_code_roots_list_length() == 0) && _other_regions.is_empty();
  }

  bool occupancy_less_or_equal_than(size_t occ) const {
    return (strong_code_roots_list_length() == 0) && _other_regions.occupancy_less_or_equal_than(occ);
  }

  // For each PRT in the card (remembered) set call one of the following methods
  // of the given closure:
  //
  // set_full_region_dirty(uint region_idx) - pass the region index for coarse PRTs
  // set_bitmap_dirty(uint region_idx, BitMap* bitmap) - pass the region index and bitmap for fine PRTs
  // set_cards_dirty(uint region_idx, elem_t* cards, uint num_cards) - pass region index and cards for sparse PRTs
  template <class Closure>
  inline void iterate_prts(Closure& cl);

  size_t occupied() {
    MutexLocker x(&_m, Mutex::_no_safepoint_check_flag);
    return occupied_locked();
  }
  size_t occupied_locked() {
    return _other_regions.occupied();
  }

  static jint n_coarsenings() { return OtherRegionsTable::n_coarsenings(); }

private:
  enum RemSetState {
    Untracked,
    Updating,
    Complete
  };

  RemSetState _state;

  static const char* _state_strings[];
  static const char* _short_state_strings[];
public:

  const char* get_state_str() const { return _state_strings[_state]; }
  const char* get_short_state_str() const { return _short_state_strings[_state]; }

  bool is_tracked() { return _state != Untracked; }
  bool is_updating() { return _state == Updating; }
  bool is_complete() { return _state == Complete; }

  void set_state_empty() {
    guarantee(SafepointSynchronize::is_at_safepoint() || !is_tracked(), "Should only set to Untracked during safepoint but is %s.", get_state_str());
    if (_state == Untracked) {
      return;
    }
    clear_fcc();
    _state = Untracked;
  }

  void set_state_updating() {
    guarantee(SafepointSynchronize::is_at_safepoint() && !is_tracked(), "Should only set to Updating from Untracked during safepoint but is %s", get_state_str());
    clear_fcc();
    _state = Updating;
  }

  void set_state_complete() {
    clear_fcc();
    _state = Complete;
  }

  void add_reference(OopOrNarrowOopStar from, uint tid) {
    RemSetState state = _state;
    if (state == Untracked) {
      return;
    }

    uint cur_idx = _hr->hrm_index();
    uintptr_t from_card = uintptr_t(from) >> CardTable::card_shift;

    if (G1FromCardCache::contains_or_replace(tid, cur_idx, from_card)) {
      assert(contains_reference(from), "We just found " PTR_FORMAT " in the FromCardCache", p2i(from));
      return;
    }

    _other_regions.add_reference(from, tid);
  }

  // The region is being reclaimed; clear its remset, and any mention of
  // entries for this region in other remsets.
  void clear(bool only_cardset = false);
  void clear_locked(bool only_cardset = false);

  // The actual # of bytes this hr_remset takes up.
  // Note also includes the strong code root set.
  size_t mem_size() {
    MutexLocker x(&_m, Mutex::_no_safepoint_check_flag);
    return _other_regions.mem_size()
      // This correction is necessary because the above includes the second
      // part.
      + (sizeof(HeapRegionRemSet) - sizeof(OtherRegionsTable))
      + strong_code_roots_mem_size();
  }

  // Returns the memory occupancy of all static data structures associated
  // with remembered sets.
  static size_t static_mem_size() {
    return OtherRegionsTable::static_mem_size() + G1CodeRootSet::static_mem_size();
  }

  // Returns the memory occupancy of all free_list data structures associated
  // with remembered sets.
  static size_t fl_mem_size() {
    return OtherRegionsTable::fl_mem_size();
  }

  bool contains_reference(OopOrNarrowOopStar from) const {
    return _other_regions.contains_reference(from);
  }

  // Routines for managing the list of code roots that point into
  // the heap region that owns this RSet.
  void add_strong_code_root(nmethod* nm);
  void add_strong_code_root_locked(nmethod* nm);
  void remove_strong_code_root(nmethod* nm);

  // Applies blk->do_code_blob() to each of the entries in
  // the strong code roots list
  void strong_code_roots_do(CodeBlobClosure* blk) const;

  void clean_strong_code_roots(HeapRegion* hr);

  // Returns the number of elements in the strong code roots list
  size_t strong_code_roots_list_length() const {
    return _code_roots.length();
  }

  // Returns true if the strong code roots contains the given
  // nmethod.
  bool strong_code_roots_list_contains(nmethod* nm) {
    return _code_roots.contains(nm);
  }

  // Returns the amount of memory, in bytes, currently
  // consumed by the strong code roots.
  size_t strong_code_roots_mem_size();

  static void invalidate_from_card_cache(uint start_idx, size_t num_regions) {
    G1FromCardCache::invalidate(start_idx, num_regions);
  }

#ifndef PRODUCT
  static void print_from_card_cache() {
    G1FromCardCache::print();
  }

  static void test();
#endif
};

#endif // SHARE_GC_G1_HEAPREGIONREMSET_HPP
