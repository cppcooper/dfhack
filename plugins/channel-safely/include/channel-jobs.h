#pragma once
#include <PluginManager.h>
#include <modules/Job.h>
#include <modules/EventManager.h> //hash functions (they should probably get moved at this point, the ones that aren't specifically for EM anyway)
#include <df/world.h>
#include <df/job.h>

#include <unordered_set>
#include <unordered_map>
#include <ranges>

using namespace DFHack;

/* Used to read/store/iterate channel digging jobs
 * jobs: list of coordinates with channel jobs associated to them
 * load_channel_jobs: iterates world->jobs.list to find channel jobs and adds them into the `jobs` map
 * clear: empties the container
 * erase: finds a job corresponding to a coord, removes the mapping in jobs, and calls Job::removeJob, then returns an iterator following the element removed
 * find: returns an iterator to a job if one exists for a map coordinate
 * begin: returns jobs.begin()
 * end: returns jobs.end()
 */
class ChannelJobs {
private:
    std::unordered_map<df::coord, df::job*> job_ptrs;
public:
    void load_channel_jobs();
    void clear() {
        job_ptrs.clear();
    }
    auto keys() const { return job_ptrs | std::views::keys; }
    void erase(const df::coord &pos) {
        job_ptrs.erase(pos);
    }
    bool contains(const df::coord &pos) const { return job_ptrs.contains(pos); }
    df::job* find_job(const df::coord &pos) const { return job_ptrs.contains(pos) ? job_ptrs.find(pos)->second : nullptr; }
};
