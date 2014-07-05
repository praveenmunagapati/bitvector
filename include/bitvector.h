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

#include <iostream>
#include <iomanip>

namespace bitvector
{
    using std::floor;
    using std::ceil;
    using std::log2;
    using std::sqrt;
    using std::pow;
    
    template<size_t W>
    class bitvector
    {
    public:
        bitvector(size_t capacity)
        {
            _capacity = capacity;
            
            _counter_width = ceil(log2(_capacity)) + 1;
            
            _degree = W / _counter_width;
            
            _leaves_buffer = ceil(sqrt(W) - 1);
            
            _nodes_buffer  = ceil(sqrt(_degree) - 1);
            
            // Total number of leaves to allocate space for
            size_t leaves_count = ceil(_capacity * (_leaves_buffer + 1) /
                                      (_leaves_buffer * (W - _leaves_buffer)));
            
            // Maximum height of the tree
            size_t max_height = ceil(log(leaves_count) / log(_degree + 1));
            
            // Minimum number of fields for each node
            size_t minimum_degree = (_nodes_buffer * (_degree - _nodes_buffer)) /
                                    (_nodes_buffer - 1);
            
            // Total number of internal nodes
            size_t nodes_count = ceil(leaves_count /
                                      pow(minimum_degree + 1, max_height));
            
            _sizes.resize(_counter_width, nodes_count);
            _ranks.resize(_counter_width, nodes_count);
            _pointers.resize(_counter_width, nodes_count);
            
            _leaves.reserve(leaves_count);
            _leaves.resize(leaves_count);
        }
        
        void info() const {
            std::cout << "Word width = " << W << " bits\n"
                      << "Capacity = " << _capacity << " bits\n"
                      << "Counter width = " << _counter_width << " bits\n"
                      << "Degree = " << _degree << "\n"
                      << "b = " << _leaves_buffer << "\n"
                      << "b' = " << _nodes_buffer << "\n"
                      << "Number of nodes = " << _sizes.size() << "\n"
                      << "Number of leaves = " << _leaves.size() << "\n";
        }
        
        size_t capacity() const { return _capacity; }
        size_t size() const { return _size; }
        
        bool empty() const { return _size == 0; }
        bool full() const { return _size == _capacity; }
        
    private:
        // This "allocation" is only to take the first free node and return it
        size_t alloc_node() {
            assert(_free_node < _sizes.size());
            return _free_node++;
        }
        
    private:
        // Maximum number of bits stored in the vector
        // Refered as N in the paper
        size_t _capacity;
        
        // Current number of bits stored in the bitvector
        size_t _size = 0;
        
        // Index of the first unused node in the nodes arrays
        size_t _free_node = 0;
        
        // Height of the root node (i.e. of the tree)
        size_t _height;
        
        // Packed arrays of data representing the nodes
        packed_array<W> _sizes;
        packed_array<W> _ranks;
        packed_array<W> _pointers;
        std::vector<word_t<W>> _leaves;
        
        // Bit width of the nodes' counters inside nodes' words
        size_t _counter_width;
        
        // Number of counters per node, refered as d in the paper
        size_t _degree;
        
        // Number of leaves used for redistribution for ammortized
        // constant insertion time. Refered as b in the paper
        size_t _leaves_buffer;
        
        // Number of leaves used for redistribution for ammortized
        // constant insertion time. Refered as b' in the paper
        size_t _nodes_buffer;
    };
}

#endif // BITVECTOR_H
