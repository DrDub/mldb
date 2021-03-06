// This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

/* parallel_merge_sort.h
   Jeremy Barnes, 12 August 2015
   Copyright (c) 2015 Datacratic Inc.  All rights reserved.


*/

#pragma once

#include <thread>
#include <algorithm>
#include <vector>
#include <memory>

namespace Datacratic {
namespace MLDB {

template<class VecOfVecs, typename SortFn, typename MergeFn, typename SizeFn>
void parallelMergeSortRecursive(VecOfVecs & range, unsigned first, unsigned last,
                                const SortFn & sort,
                                const MergeFn & merge,
                                const SizeFn & size,
                                size_t threadThreshold)
{
    if (first == last)
        return;

    if (first == last - 1) {
        sort(range[first]);
    }
    else {
        int mid = (first + last) / 2;

        auto doSide = [&] (int first, int last)
            {
                size_t totalToDo = 0;
                for (int i = first;  i < last;  ++i)
                    totalToDo += size(range[i]);

                std::thread t;

                auto run = [=,&range,&sort,&merge,&size] ()
                {
                    parallelMergeSortRecursive(range, first, last, sort, merge, size,
                                               threadThreshold);
                };

                if (totalToDo > threadThreshold) {
                    t = std::move(std::thread(run));
                }
                else run();

                return std::move(t);
            };

        std::thread t1(doSide(first, mid));
        std::thread t2(doSide(mid, last));

        if (t1.joinable())
            t1.join();
        if (t2.joinable())
            t2.join();
                    
        merge(range[first], range[mid]);
    }
}

template<class T, class Compare = std::less<T> >
std::vector<T>
parallelMergeSort(std::vector<std::vector<T> > & range,
                  const Compare & cmp = std::less<T>(),
                  size_t threadThreshold = 10000)
{
    if (range.empty())
        return {};

    auto sort = [&] (const std::vector<T> & v)
        {
            std::sort(v.begin(), v.end(), cmp);
        };

    auto size = [] (const std::vector<T> & v)
        {
            return v.size();
        };

    auto merge = [&] (std::vector<T> & v1, std::vector<T> & v2)
        {
            size_t split = v1.size();

            v1.insert(v1.end(), 
                      std::make_move_iterator(v2.begin()),
                      std::make_move_iterator(v2.end()));
            v2.clear();

            std::inplace_merge(v1.begin(),
                               v1.begin() + split,
                               v1.end(),
                               cmp);
        };

    parallelMergeSortRecursive(range, 0, range.size(), sort, merge, size, threadThreshold);

    return std::move(range[0]);
}

template<class T, class Compare = std::less<T> >
std::vector<T>
parallelMergeSort(std::vector<std::shared_ptr<std::vector<T> > > & range,
                  const Compare & cmp = std::less<T>(),
                  size_t threadThreshold = 10000)
{
    if (range.empty())
        return {};

    auto sort = [&] (const std::shared_ptr<std::vector<T> > & v)
        {
            std::sort((*v).begin(), v->end(), cmp);
        };
    
    auto size = [] (const std::shared_ptr<std::vector<T> > & v) -> size_t
        {
            return v->size();
        };

    auto merge = [&] (std::shared_ptr<std::vector<T> > & v1,
                      std::shared_ptr<std::vector<T> > & v2)
        {
            size_t split = v1->size();

            v1->insert(v1->end(), 
                       std::make_move_iterator(v2->begin()),
                       std::make_move_iterator(v2->end()));
            v2->clear();

            std::inplace_merge(v1->begin(),
                               v1->begin() + split,
                               v1->end(),
                               cmp);
        };

    parallelMergeSortRecursive(range, 0, range.size(), sort, merge, size, threadThreshold);

    return std::move(*range[0]);
}

} // namespace MLDB
} // namespace Datacratic
