/*
 * Copyright 2014 Nicola Gigante
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef BITVECTOR_H
#define BITVECTOR_H

#include "bits.h"
#include "packed_view.h"

#include <cstddef>
#include <cmath>
#include <memory>
#include <type_traits>
#include <bitset>
#include <stdexcept>
#include <vector>

#include <ostream>
#include <iomanip>

// FIXME: remove this include after having finished the debugging
#include <iostream>

namespace bitvector
{
    using std::floor;
    using std::ceil;
    using std::log2;
    using std::sqrt;
    using std::pow;
    using std::min;
    using std::max;
    
    template<size_t W>
    class bitvector
    {
        template<bool Const>
        class subtree_ref_t;
        
        template<bool Const>
        friend class subtree_ref_t;
        
        using subtree_ref       = subtree_ref_t<false>;
        using subtree_const_ref = subtree_ref_t<true>;
        
    public:
        bitvector(size_t capacity)
        {
            _capacity = capacity;
            
            _counter_width = ceil(log2(_capacity)) + 1;
            
            _degree = W / counter_width();
            
            _leaves_buffer = min(max(size_t(ceil(sqrt(W)) - 1), size_t(1)),
                                 _degree);
            
            _nodes_buffer  = max(size_t(ceil(sqrt(_degree)) - 1), size_t(1));
            
            // Total number of leaves to allocate space for
            size_t leaves_count = ceil(_capacity * (_leaves_buffer + 1) /
                                      (_leaves_buffer * (W - _leaves_buffer)));
            
            // Minimum number of fields for each node
            size_t minimum_degree = (_nodes_buffer * (_degree - _nodes_buffer)) /
                                    (_nodes_buffer + 1);
            
            // Total number of internal nodes
            size_t nodes_count = 0;
            size_t level_count = leaves_count;
            do
            {
                level_count = ceil(float(level_count) / (minimum_degree + 1));
                nodes_count += level_count;
            } while(level_count > 1);
            
            // Width of pointers
            _pointer_width = ceil(log2(std::max(nodes_count, leaves_count + 1)));
            
            assert(_pointer_width <= _counter_width);
            assert(_pointer_width * (_degree + 1) <= W);
            
            // Allocate space for nodes and leaves
            _sizes.resize(counter_width(), nodes_count * _degree);
            _ranks.resize(counter_width(), nodes_count * _degree);
            _pointers.resize(pointer_width(), nodes_count * (_degree + 1));
            
            _leaves.reserve(leaves_count + 1);
            
            // Unused sentinel for null pointers to leaves
            // (not needed for internal nodes)
            alloc_leaf();
            
            // Space for the root node
            alloc_node();
            
            // Setup the first leaf of the empty root
            root().insert_child(0);
        }
        
        // Debugging info
        friend std::ostream &operator<<(std::ostream &s, bitvector const&v) {
            s << "Word width = " << W << " bits\n"
              << "Capacity = " << v.capacity() << " bits\n"
              << "Size counter width = " << v.counter_width() << " bits\n"
              << "Pointers width = " << v.pointer_width() << " bits\n"
              << "Degree = " << v._degree << "\n"
              << "b = " << v._leaves_buffer << "\n"
              << "b' = " << v._nodes_buffer << "\n"
              << "Number of nodes = " << v._sizes.size() / v.degree() << "\n"
              << "Number of leaves = " << v._leaves.capacity() << "\n"
              << "Index mask:\n"
              << to_binary(v.index_mask(), v.counter_width()) << "\n"
              << "Size flag mask:\n"
              << to_binary(v.size_flag_mask(), v.counter_width()) << "\n";
        }
        
        size_t capacity() const { return _capacity; }
        size_t size() const { return _size; }
        size_t rank() const { return _rank; }
        size_t degree() const { return _degree; }
        
        size_t counter_width() const { return _counter_width; }
        size_t pointer_width() const { return _pointer_width; }
        
        bool empty() const { return _size == 0; }
        bool full() const { return _size == _capacity; }
        
        bool access(size_t index) const { return access(root(), index); }
        
        void insert(size_t index, bool bit) { insert(root(), index, bit); }
        
        subtree_ref       root()       { return { *this, 0, _height, _size, _rank }; }
        subtree_const_ref root() const { return { *this, 0, _height, _size, _rank }; }
        
    private:
        // This "allocation" is only to take the first free node and return it
        size_t alloc_node() {
            assert(_free_node < _sizes.size());
            return _free_node++;
        }
        
        size_t alloc_leaf() {
            _leaves.push_back(0);
            return _leaves.size() - 1;
        }
        
        word_t<W> index_mask() const { return _sizes.index_mask(); }
        
        word_t<W> size_flag_mask() const {
            return _sizes.flagbit_mask();
        }
        
        bool access(subtree_const_ref t, size_t index) const
        {
            if(index >= t.size())
                throw std::out_of_range("Index out of bounds");
            
            if(t.is_leaf()) // We have a leaf
                return get_bit(t.leaf(), index);
            else { // We're in a node
                size_t child, new_index;
                std::tie(child, new_index) = t.find(index);
                
                return access(t.child(child), new_index);
            }
        }
        
        void insert(subtree_ref t, size_t index, bool bit)
        {
            // If we see a full node in this point it must be the root,
            // otherwise we've violated our invariants.
            // So, since it's a root and it's full, we prepare the split
            // by allocating a new node and swapping it with the old root,
            // thus ensuring the root is always at index zero.
            // Then we go ahead pretending we started the insertion from the
            // new root, so we don't duplicate the node splitting code
            // below
            if(t.nkeys() == degree())
            {
                assert(t.is_root());
                
                // Copy the old root into another node
                subtree_ref old_root = t.copy();
                
                // Empty the root and make it point to the old one
                t.sizes() = index_mask() * t.size();
                t.ranks() = index_mask() * t.rank();
                t.pointers() = 0;
                t.pointers(0) = old_root.index();
                
                // The only point in the algorithm were the height increases
                ++_height;
                
                assert(root().nkeys() == 1);
                assert(root().child(0).nkeys() == degree());
                
                // Pretend we were inserting from the new root
                return insert(root(), index, bit);
            }
            
            // Find where we have to insert this bit
            size_t child, new_index;
            std::tie(child, new_index) = t.find_insert_point(index);
            
            // Then go ahead
            if(t.height() == 1) // We'll reach a leaf
            {
                // 1. Check if we need a split and/or a redistribution of bits
                if(t.child(child).size() == W)
                {
                    // The leaf is full, we need a redistribution
                    size_t begin, end, count;
                    std::tie(begin, end, count) = find_adjacent_children(t, child);
                    
                    // Check if we need to split or only to redistribute
                    if(count >= _leaves_buffer * (W - _leaves_buffer)) {
                        // We need to split. The node should not be full
                        assert(t.nkeys() < degree());
                        t.insert_child(end++);
                    }
                    
                    // redistribute
                    redistribute_bits(t, begin, end, count);
                    
                    // Search again where to insert the bit
                    std::tie(child, new_index) = t.find_insert_point(index);
                }
                
                // 2. Update counters
                t.sizes(child, degree()) += index_mask();
                t.ranks(child, degree()) += index_mask() * bit;
                
                // 3. Insert the bit
                word_t<W> &leaf = t.child(child).leaf();
                leaf = insert_bit(leaf, new_index, bit);
                _size += 1;
                _rank += bit;
            }
            else // We'll have another node
            {
                // 1. Check if we need a split and/or a redistribution of keys
                if(t.child(child).nkeys() == degree())
                {
                    // The node is full, we need a redistribution
                    size_t begin, end, count;
                    std::tie(begin, end, count) = find_adjacent_children(t, child);
                    
                    // FIXME: How to decide if we need to split?
                    //        Let's just always split for now
                    //if(count >= _nodes_buffer * (degree() + 1 - _nodes_buffer)) {
                    if(true) {
                        // We need to split. The node should not be full
                        assert(t.nkeys() < degree());
                        t.insert_child(end++);
                    }
                    
                    // redistribute
                    redistribute_keys(t, begin, end, count);
                    
                    // Search again where to insert the bit
                    std::tie(child, new_index) = t.find_insert_point(index);
                }
                
                // 2. Update counters
                t.sizes(child, degree()) += index_mask();
                t.ranks(child, degree()) += index_mask() * bit;
                
                // 3. Continue the traversal
                insert(t.child(child), new_index, bit);
            }
        }
        
        // Utility functions for insert()
        
        // Find the group of children adjacent to 'child',
        // with the maximum number of free slots (bits or keys, it depends).
        // It returns a tuple with:
        //
        // - The begin and the end of the interval selected interval of children
        // - The count of slots contained in total in the found leaves
        //   I repeat: the number of slots, not the number of free slots
        std::tuple<size_t, size_t, size_t>
        find_adjacent_children(subtree_ref t, size_t child)
        {
            const bool is_leaf = t.child(child).is_leaf();
            const size_t buffer = is_leaf ? _leaves_buffer : _nodes_buffer;
            const size_t max_count = is_leaf ? W : degree();
            const auto count = [&](size_t i) {
                return t.pointers(i) == 0 ? max_count :
                       is_leaf            ? W - t.child(i).size() :
                                            degree() - t.child(i).nkeys();
            };
            
            size_t begin = child > buffer ? child - buffer - 1 : 0;
            size_t end = std::min(begin + buffer, degree() + 1);
            
            size_t freeslots = 0;
            size_t maxfreeslots = 0;
            std::pair<size_t, size_t> window = { begin, end };
            
            // Sum for the initial window
            for(size_t i = begin; i < end; ++i)
                freeslots += count(i);
            maxfreeslots = freeslots;
            
            // Slide the window
            while(begin < child && end < degree() + 1)
            {
                freeslots = freeslots - count(begin) + count(end - 1);
                
                begin += 1;
                end += 1;
                
                if(freeslots > maxfreeslots) {
                    window = { begin, end };
                    maxfreeslots = freeslots;
                }
            }
            
            // Reverse the count of free slots to get the total number of bits
            size_t total = max_count * buffer - maxfreeslots;
            
            assert(begin <= child && child < end);
            return { window.first, window.second, total };
        }
        
        // FIXME: rewrite using a temporary space of two words instead
        //        of the whole buffer, thus avoiding the dynamic allocation
        void redistribute_bits(subtree_ref t, size_t begin, size_t end, size_t count)
        {
            size_t b = end - begin;
            size_t bits_per_leaf = count / b;
            size_t rem           = count % b;
            
            assert(b == _leaves_buffer || b == _leaves_buffer + 1);
            
            std::vector<word_t<W>> leaves_bits(_leaves_buffer);
            packed_view<W> view(1, count, leaves_bits);
            
            for(size_t i = begin, p = 0; i < end; ++i) {
                if(t.pointers(i) != 0) {
                    view(p, p + t.child(i).size()) = t.child(i).leaf();
                    p += t.child(i).size();
                }
            }
            
            size_t p = 0;
            size_t popcnt = 0;
            
            if(begin > 0 && t.pointers(begin - 1)) {
                t.sizes(begin, end) = index_mask() * t.child(begin - 1).size();
                t.ranks(begin, end) = index_mask() * t.child(begin - 1).rank();
            }
            
            t.clear_keys(begin, end);
            for(size_t i = begin; i < end; ++i)
            {
                size_t n = bits_per_leaf;
                if(rem) {
                    n += 1;
                    rem -= 1;
                }
                
                if(t.pointers(i) == 0)
                    t.insert_child(i);
                
                word_t<W> leaf = view(p, p + n);

                t.child(i).leaf() = leaf;
                if(i < degree()) {
                    t.sizes(i, degree()) += (p += n);
                    t.ranks(i, degree()) += (popcnt += popcount(leaf));
                }
                
                count -= n;
            }
            
            assert(count == 0);
        }
        
        // FIXME: rewrite using a temporary space of two words instead
        //        of the whole buffer, thus avoiding the dynamic allocation
        void redistribute_keys(subtree_ref t, size_t begin, size_t end, size_t count)
        {
            size_t b = end - begin;
            size_t keys_per_node = count / b;
            size_t rem           = count % b;
            
            assert(b == _nodes_buffer || b == _nodes_buffer + 1);
            
            struct pointer {
                size_t size;
                size_t rank;
                size_t ptr;
            };
            
            std::vector<pointer> pointers(_nodes_buffer * (degree() + 1));
        
            for(size_t i = begin; i != end; ++i)
                for(size_t c = 0; c < degree() + 1; ++c)
                    pointers.push_back({ t.child(i).child(c).size(),
                                         t.child(i).child(c).rank(),
                                         t.child(i).pointers(c) });
            
            size_t p = 0;
            for(size_t i = begin; i != end; ++i)
            {
                size_t n = keys_per_node;
                if(rem) {
                    n += 1;
                    rem -= 1;
                }
                
                // Clear the node
                t.child(i).sizes() = 0;
                t.child(i).ranks() = 0;
                
                for(size_t j = 0; j < n; ++j)
                {
                    t.child(i).pointers(j) = pointers[p + j].ptr;
                    t.child(i).sizes(j, degree()) +=
                                     index_mask() * pointers[p + j].size;
                    t.child(i).ranks(j, degree()) +=
                                     index_mask() * pointers[p + j].rank;
                }
                
                count -= n;
                p += n;
            }
            
            assert(count == 0);
        }
        
    private:
        // Maximum number of bits stored in the vector
        // Refered as N in the paper
        size_t _capacity;
        
        // Current number of bits stored in the bitvector
        size_t _size = 0;
        
        // Total rank of the bitvector
        size_t _rank = 0;
        
        // Height of the tree (distance of the root node from the leaves)
        size_t _height = 1;
        
        // Bit width of the nodes' counters inside nodes' words
        size_t _counter_width;
        
        // Bit width of nodes' pointers
        size_t _pointer_width;
        
        // Number of counters per node, refered as d in the paper
        size_t _degree;
        
        // Number of leaves used for redistribution for ammortized
        // constant insertion time. Refered as b in the paper
        size_t _leaves_buffer;
        
        // Number of leaves used for redistribution for ammortized
        // constant insertion time. Refered as b' in the paper
        size_t _nodes_buffer;
        
        // Index of the first unused node in the nodes arrays
        size_t _free_node = 0;
        
        // Packed arrays of data representing the nodes
        packed_array<W, flag_bit> _sizes;
        packed_array<W> _ranks;
        packed_array<W> _pointers;
        
        std::vector<word_t<W>> _leaves;
    };
    
    template<size_t W>
    template<bool Const>
    class bitvector<W>::subtree_ref_t
    {
        using bitvector_t = typename
                      std::conditional<Const, bitvector const, bitvector>::type;
        
        using leaf_reference = typename
                          std::conditional<Const, word_t<W>, word_t<W>&>::type;
        
        using reference = typename
        std::conditional<Const, typename packed_array<W>::const_reference,
                                typename packed_array<W>::reference>::type;
        
        using size_reference = typename
        std::conditional<Const, typename packed_array<W, flag_bit>::const_reference,
                                typename packed_array<W, flag_bit>::reference>::type;
        
        // Reference to the parent bitvector structure
        bitvector_t &_vector;
        
        // Index of the root node of the subtree
        size_t _index = 0;
        
        // Height of the subtree (distance of the root from leaves)
        size_t _height = 0;
        
        // Total size (number of bits) of the subtree
        size_t _size = 0;
        
        // Total rank (number of set bit) of the subtree
        size_t _rank = 0;
        
    public:
        subtree_ref_t(bitvector_t &vector, size_t index, size_t height,
                      size_t size, size_t rank)
            : _vector(vector), _index(index), _height(height),
              _size(size), _rank(rank) { }
        
        subtree_ref_t(subtree_ref_t const&) = default;
        subtree_ref_t &operator=(subtree_ref_t const&) = default;
        
        template<bool C, REQUIRES(Const && not C)>
        subtree_ref_t(subtree_ref_t<C> const&r)
            : subtree_ref_t(r._vector, r._index, r._height, r._size) { }
        
        // Height of the subtree
        size_t height() const { return _height; }
        
        // Index of the node/leaf in the bitvector internal data array
        size_t index() const { return _index; }
        
        // Size of the subtree
        size_t size() const { return _size; }
        
        // Rank of the subtree
        size_t rank() const { return _rank; }
        
        // Convenience shorthand for the degree of the subvector
        size_t degree() const { return _vector.degree(); }
        
        // Pair of methods to know if the subtree is a leaf or not
        bool is_leaf() const { return _height == 0; }
        bool is_node() const { return _height > 0; }
        
        // The root node is at index zero and top height
        bool is_root() const {
            assert(_index != 0 || _height == _vector._height);
            assert(_height != _vector._height || _index == 0);
            
            return _index == 0;
        }
        
        template<bool C = Const, REQUIRES(not C)>
        void clear_keys(size_t begin, size_t end) const
        {
            sizes(begin, end) = 0;
            ranks(begin, end) = 0;
        }
        
        template<bool C = Const, REQUIRES(not C)>
        void clear_keys() const
        {
            clear(0, degree());
        }
        
        template<bool C = Const, REQUIRES(not C)>
        subtree_ref copy() const
        {
            subtree_ref r = *this;
            if(is_node()) {
                r._index = _vector.alloc_node();
            
                r.sizes()    = sizes();
                r.ranks()    = ranks();
                r.pointers() = pointers();
            } else {
                r._index = _vector.alloc_leaf();
                r.leaf() = leaf();
            }
            
            return r;
        }
        
        // This method creates a subtree_ref for the child at index k,
        // computing its size and its height. It's valid only if the height
        // is greater than 1 (so our children are internal nodes, not leaves).
        // For accessing the leaves of level 1 nodes, use the leaf() method
        subtree_ref_t child(size_t k) const
        {
            assert(k <= degree());
            assert(pointers(k) != 0);
            
            size_t p = pointers(k);
            size_t h = height() - 1;
            
            // Size of the subtree
            size_t s = k == 0        ? sizes(k) :
                       k == degree() ? size()   - sizes(k - 1) :
                                       sizes(k) - sizes(k - 1);
            
            // Rank of the subtree
            size_t r = k == 0        ? ranks(k) :
                       k == degree() ? rank()   - ranks(k - 1) :
                                       ranks(k) - ranks(k - 1);
            
            return { _vector, p, h, s, r };
        }
        
        // This method insert a new empty child into the node in position k,
        // shifting right the subsequent keys
        template<bool C = Const, REQUIRES(not C)>
        void insert_child(size_t k) const
        {
            assert(is_node());
            assert(k <= degree());
            
            if(k < degree()) {
                // FIXME:
                // There should be this assert, but since we use this method
                // inside redistribute_bits, it's not always true
                // (the node is in inconsistent state at that point)
                // assert(nkeys() < degree());
                
                size_t s = sizes(k);
                size_t r = ranks(k);
            
                sizes(k, degree()) <<= _vector.counter_width();
                ranks(k, degree()) <<= _vector.counter_width();
                pointers(k + 1, degree() + 1) <<= _vector.pointer_width();
            
                sizes(k) = s;
                ranks(k) = r;
            }
            
            pointers(k) = _height == 1 ? _vector.alloc_leaf()
                                       : _vector.alloc_node();
        }

        // Access to the value of the leaf, if this subtree_ref refers to a leaf
        leaf_reference leaf() const
        {
            assert(is_leaf());
            return _vector._leaves[_index];
        }
        
        // Finds the subtree where the bit at the given index can be inserted.
        // The position found by this function is suitable for insertion,
        // if you need lookup, use find().
        //
        // The returned pair contains:
        //  - The index of the subtree
        //  - The new index, relative to the subtree, where to insert the bit
        std::pair<size_t, size_t>
        find_insert_point(size_t index) const
        {
            assert(is_node());
            
            size_t child = degree() -
            popcount(_vector.size_flag_mask() &
                     (sizes() - _vector.index_mask() * word_t<W>(index)));
            
            size_t new_index = index;
            if(child > 0)
                new_index -= sizes(child - 1);
            
            //assert(new_index < this->child(child).size());
            
            return { child, new_index };
        }
        
        // Finds the subtree where the bit at the given index is located.
        //
        // The returned pair contains:
        //  - The index of the subtree
        //  - The new index, relative to the subtree, where to find the bit
        std::pair<size_t, size_t>
        find(size_t index) const
        {
            size_t child, new_index;
            std::tie(child, new_index) = find_insert_point(index);
            
            if(new_index == this->child(child).size()) {
                child += 1;
                new_index = 0;
            }
            
            assert(child < degree() + 1);
            
            return { child, new_index };
        }
        
        // Number of used keys inside the node
        size_t nkeys() const {
            return std::min(std::get<0>(find_insert_point(size() - 1)) + 1,
                            degree());
        }
        
        // Word composed by the size fields in the interval [begin, end)
        size_reference sizes(size_t begin, size_t end) const
        {
            assert(is_node());
            assert(begin < degree());
            assert(end <= degree());
            return _vector._sizes(_index * degree() + begin, _index * degree() + end);
        }
        
        // Word of the size fields.
        size_reference sizes() const { return sizes(0, degree()); }
        
        // Value of the size field at index k (with the flag bit stripped)
        //
        // NOTE: This is NOT the size of the subtree rooted at index k.
        //       To get that, use n.child(k).size()
        //
        size_reference sizes(size_t k) const { return sizes(k, k + 1); }
        
        // Word composed by the rank fields in the interval [begin, end)
        reference ranks(size_t begin, size_t end) const
        {
            assert(is_node());
            assert(begin < degree());
            assert(end <= degree());
            return _vector._ranks(_index * degree() + begin, _index * degree() + end);
        }
        
        // Word of the rank fields
        reference ranks() const { return ranks(0, degree()); }
        
        // Value of the rank field at index k
        reference ranks(size_t k) const { return ranks(k, k + 1); }
        
        // Word composed by the pointer fields in the interval [begin, end)
        reference pointers(size_t begin, size_t end) const
        {
            assert(is_node());
            assert(begin == end || begin < degree() + 1);
            assert(end <= degree() + 1);
            return _vector._pointers(_index * (degree() + 1) + begin,
                                 _index * (degree() + 1) + end);
        }
        
        // Word of the pointer fields
        reference pointers() const { return pointers(0, degree() + 1); }
        
        // Value of the pointer field at index k
        reference pointers(size_t k) const { return pointers(k, k + 1); }
        
        // Input / Output of nodes for debugging
        friend std::ostream &operator<<(std::ostream &o, subtree_ref_t t)
        {
            if(t.is_leaf())
                o << to_binary(t.leaf());
            else {
                const int counter_width = int(t._vector.counter_width());
                const int pointer_width = int(t._vector.pointer_width());
                
                o << "Node at index: " << t.index() << "\n"
                  << "Total size:    " << t.size() << "\n"
                  << "Total rank:    " << t.rank() << "\n";
                
                o << "Sizes: |";
                o << std::setw(W % counter_width) << "" << "|";
                for(size_t i = t.degree() - 1; i > 0; --i)
                    o << std::setw(counter_width) << t.sizes(i) << "|";
                o << std::setw(counter_width) << t.sizes(0) << "|\n";
                o << "       |" << to_binary(word_t<W>(t.sizes()), counter_width, '|') << "|\n";
                
                o << "Ranks: |";
                o << std::setw(W % counter_width) << "" << "|";
                for(size_t i = t.degree() - 1; i > 0; --i)
                    o << std::setw(counter_width) << t.ranks(i) << "|";
                o << std::setw(counter_width) << t.ranks(0) << "|\n";
                o << "       |" << to_binary(word_t<W>(t.ranks()), counter_width, '|') << "|\n";
                
                o << "\nPtrs:  |";
                o << std::setw(int(W - pointer_width * (t.degree() + 1) + 1)) << "" << "|";
                for(size_t i = t.degree(); i > 0; --i)
                    o << std::setw(pointer_width) << t.pointers(i) << "|";
                o << std::setw(pointer_width) << t.pointers(0) << "|\n";
                o << "       |" << to_binary(word_t<W>(t.pointers()), pointer_width, '|') << "|\n";
            }
            
            return o;
        }
    };
}

#endif // BITVECTOR_H
