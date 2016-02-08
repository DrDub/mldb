/* worker_task.h                                                   -*- C++ -*-
   Jeremy Barnes, 30 October 2005
   Copyright (c) 2005 Jeremy Barnes  All rights reserved.

   This file is part of MLDB. Copyright 2015 Datacratic. All rights reserved.

   Task list of work to do.
*/

#pragma once

#include <functional>

namespace ML {

#if 0
/** This task represents a job that is run on a thread somewhere.  It is
    simply a function with no arguments; the arguments need to be encoded
    elsewhere.
*/

typedef std::function<void ()> Job;

extern const Job NO_JOB;

/** Return the number of threads that we run with.  If the NUM_THREADS
    environment variable is set, we use that.  Otherwise, we use the
    number of CPU cores that we have available.
*/
int num_threads();


/*****************************************************************************/
/* WORKER_TASK                                                               */
/*****************************************************************************/

/* This class is a generic class that works through a list of jobs.
   The jobs can be arranged in groups, with a job that gets run once the
   group is finished, and the groups can be arranged in a hierarchy.

   The jobs are scheduled such that with any group, jobs belonging to an
   earlier subgroup are all scheduled before any belonging to a later
   subgroup, regardless of the order in which they are scheduled.  (This
   corresponds to a depth first search through the group tree).  The effect
   is to guarantee that the average number of groups outstanding will be
   as small as possible.

   It works multithreaded, and deals with all locking and unlocking.
*/

class Worker_Task {
public:
    typedef long long Id;  // 64 bits so no wraparound

    /** Return the instance.  Creates it with the number of threads given,
        or num_threads() if thr == -1.  If thr == 0, then only local work
        is done (no work is transferred to other threads). */
    static Worker_Task & instance(int thr = -1);

    Worker_Task(int threads);

    virtual ~Worker_Task();

    /** Allocate a new job group.  The given job will be called once the group
        is finished.  Note that if nothing is ever added to the group, it won't
        be finished automatically unless check_finished() is called.

        It will be inserted into the jobs list just after the last job with
        the same parent group, so that the children of parent groups will be
        completed in preference to their sisters.

        If lock is set to true, then it will not ever be automatically removed
        until it is unlocked.  This stops a newly-created group from being
        instantly removed.
    */
    Id get_group(const Job & group_finish,
                 const std::string & info,
                 Id parent_group = -1,
                 bool lock = true);

    /** Unlock the group so that it can be removed. */
    void unlock_group(int group);

    /** Add a job that belongs to the given group.  Jobs which are scheduled into
        the same group will be scheduled together. */
    Id add(const Job & job, const std::string & info, Id group = -1);

    /** Lend the calling thread to the worker task until the given group
        has finished.

        An exception in a group job is handled by throwing an exception from
        this function.
    */
    void run_until_finished(int group, bool unlock = false);

private:
    struct Itl;
    std::unique_ptr<Itl> itl;
};
#endif

} // namespace ML
