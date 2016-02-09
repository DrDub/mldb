/** thread_pool_impl.h                                             -*- C++ -*-
    Jeremy Barnes, 18 December 2015
    Copyright (c) 2015 Datacratic Inc.  All rights reserved.

    This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

    Implementation of the thread pool abstraction for when work needs to be
    spread over multiple threads.
*/

#pragma once

#include "thread_pool.h"
#include "mldb/base/exc_assert.h"
#include <atomic>
#include <vector>

namespace Datacratic {

template<typename Item>
struct ThreadQueue {
    static constexpr unsigned int NUMBER_OF_ITEMS = 4096u;
    static constexpr unsigned int MASK = NUMBER_OF_ITEMS - 1u;

    ThreadQueue()
        : bottom(0), top(0), num_queued(0)
    {
        for (auto & i: items)
            i = nullptr;
    }
    
    std::atomic<long long> bottom, top;
    std::atomic<Item *> items[NUMBER_OF_ITEMS];
    std::atomic<unsigned> num_queued;

    Item * push(Item * item)
    {
        ExcAssert(item);
        
        if (num_queued == NUMBER_OF_ITEMS)
            return item;

        long long b = bottom.load(std::memory_order_relaxed);

        // Switch the new item in
        items[b & MASK] = item;
        
        // ensure the item is written before b+1 is published to other threads.
        // on x86/64, a compiler barrier is enough.
        bottom.store(b+1, std::memory_order_release);

        ++num_queued;

        return nullptr;
    }

    Item * steal()
    {
        long long t = top.load(std::memory_order_acquire);
 
        // ensure that top is always read before bottom.
        long long b = bottom.load(std::memory_order_acquire);
        if (t >= b)
            return nullptr;  // no work to be taken

        // non-empty queue
        Item * item = items[t & MASK];
 
        // the interlocked function serves as a compiler barrier, and guarantees that the read happens before the CAS.
        if (!top.compare_exchange_strong(t, t + 1)) {
            // a concurrent steal or pop operation removed an element from the deque in the meantime.
            return nullptr;
        }
        
        ExcAssert(item);
        --num_queued;

        return item;
    }

    Item * pop()
    {
        while (num_queued.load(std::memory_order_relaxed)) {
            long long b1 = bottom;
            long long b = b1 - 1;
            bottom.exchange(b, std::memory_order_acq_rel);
 
            long long t = top.load(std::memory_order_acquire);

            if (t <= b) {
                // non-empty queue
                Item* ptr = items[b & MASK];
                if (t != b) {
                    // there's still more than one item left in the queue
                    ExcAssert(ptr);
                    --num_queued;
                    return ptr;
                }
 
                // this is the last item in the queue
                if (!top.compare_exchange_strong(t, t+1)) {
                    // failed race against steal operation
                    bottom = b1;
                    continue;
                }
                
                // Won race against steal operation
                bottom.store(t+1, std::memory_order_relaxed);
                ExcAssert(ptr);
                --num_queued;
                return ptr;
            }
            else {
                // deque was already empty
                bottom = b1;
                continue;
            }
        }
        return nullptr;
    }
};

} // namespace Datacratic

