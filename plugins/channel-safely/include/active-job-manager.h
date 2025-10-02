#pragma once
#include "channel-manager.h"

#include <cinttypes>
#include <unordered_set>

struct ActiveJob {
    int32_t id{}; // job id
    df::job* job{};
    df::unit* worker{};
    df::coord pos;
};

struct ActiveWorker {
    int32_t id{}; // worker id
    df::unit* worker{};
    df::coord last_safe_pos;
};

namespace std {
    template <>
    struct hash<ActiveJob> {
        std::size_t operator()(const ActiveJob& job) const {
            size_t r = 43;
            const size_t m = 65537;
            r = m*(r+job.id);
            return r;
        }
    };

    template <>
    struct hash<ActiveWorker> {
        std::size_t operator()(const ActiveWorker& worker) const {
            size_t r = 43;
            const size_t m = 65537;
            r = m*(r+worker.id);
            return r;
        }
    };

}

class ActiveJobManager {
    std::unordered_map<int32_t, df::coord> safe_locations; // <id, pos>
    std::unordered_map<int32_t, int32_t> endangered_units; //<id, last tick>
    std::unordered_map<int32_t, ActiveWorker> active_workers;
    std::unordered_map<int32_t, ActiveJob> active_jobs;
    std::unordered_set<df::coord> active_dig_sites;
    std::unordered_set<df::coord> cancel_queue;
    int32_t last_tick = 0;
    int32_t last_monitor_tick = 0;
    int32_t last_resurrect_tick = 0;
protected:
    bool has_cavein_conditions(const df::coord &map_pos) const;
    bool possible_cavein(const df::coord &map_pos) const;
    void cleanup();
public:
    void on_update(color_ostream &out);
    void on_job_start(df::job* job);
    void on_job_completed(color_ostream &out, df::job* job);
    void on_report_event(df::report* report);
    void cancel(df::coord site);
    void handle_cancellation();
    void handle_resurrect(color_ostream &out);
    void clear();
    bool needs_resurrect_queued();
};
