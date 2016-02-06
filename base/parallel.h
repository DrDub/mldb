/** parallel.h                                                     -*- C++ -*-
    Jeremy Barnes, 5 February 2016
    Copyright (c) 2016 Datacratic Inc.  All righs reserved.

   This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

*/

#pragma once

#include "thread_pool.h"
#include <atomic>

namespace Datacratic {

/** Run a set of jobs in multiple threads.  The iterator will be iterated
    through the range and the doWork function will be called with each
    value of the iterator in a different thread.
*/
template<typename It, typename It2, typename Fn>
void parallelReduce(It first, It2 last, Fn doWork)
{
    auto n = last - first;

    std::atomic<uint64_t> jobsDone(0);

    ThreadPool & tp = ThreadPool::instance();

    for (auto it = first;  it != last;  ++it) {
        tp.add([&,it] () { doWork(it); ++jobsDone; });
    }
    
    while (jobsDone < n)
        tp.work();
    
}

} // namespace Datacratic
