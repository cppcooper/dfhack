#include <active-job-manager.h>
#include <inlines.h>
#include <on-tick.h>
#include <ranges>

#include <tile-cache.h>
#include <Debug.h>
#include <PluginManager.h>

#include <modules/Units.h>
//#include <df/job.h>
#include <df/block_square_event_designation_priorityst.h>
#include <df/report.h>

namespace CSP {
    extern OnTick tick_it_master;;
    extern std::unordered_set<df::coord> dignow_queue;
}

df::coord simulate_fall(const df::coord &pos) {
    if unlikely(!Maps::isValidTilePos(pos)) {
        ERR(plugin).print("Error: simulate_fall(" COORD ") - invalid coordinate\n", COORDARGS(pos));
        return {};
    }
    df::coord resting_pos(pos);

    while (Maps::ensureTileBlock(resting_pos)) {
        df::tiletype tt = *Maps::getTileType(resting_pos);
        if (isWalkable(tt))
            break;
        --resting_pos.z;
    }

    return resting_pos;
}

df::coord simulate_area_fall(const df::coord &pos) {
    df::coord neighbours[8]{};
    get_neighbours(pos, neighbours);
    df::coord lowest = simulate_fall(pos);
    for (auto p : neighbours) {
        if unlikely(!Maps::isValidTilePos(p)) continue;
        auto nlow = simulate_fall(p);
        if (nlow.z < lowest.z) {
            lowest = nlow;
        }
    }
    return lowest;
}

int count_visibility_grid_neighbourhood(const df::coord &pos) {
    // check shared neighbour for cave-in conditions
    df::coord neighbours[5];
    neighbours[0]=pos;
    get_grid_neighbours(neighbours[0], &neighbours[1]);
    int visible_tiles = 0;
    for (auto n: neighbours) {
        if (bool visible = Maps::isTileVisible(n); visible) {
            visible_tiles++;
        }
    }
    return visible_tiles;
}

int count_empty_grid_neighbourhood(const df::coord &pos) {
    // check shared neighbour for cave-in conditions
    df::coord neighbours[5];
    neighbours[0]=pos;
    get_grid_neighbours(neighbours[0], &neighbours[1]);
    int empty_tiles = 0;
    for (auto n: neighbours) {
        bool visible = Maps::isTileVisible(n);
        if (config.riskaverse && !visible) {
            empty_tiles++;
        } else if (bool open = DFHack::isOpenTerrain(*Maps::getTileType(n)); open) {
            if (!config.require_vision) {
                empty_tiles++;
            } else if (visible) {
                empty_tiles++;
            }
        }
    }
    return empty_tiles;
}

bool ActiveJobManager::has_cavein_conditions(const df::coord &map_pos) const {
    if (!config.riskaverse) {
        return false;
    }
    df::coord below{map_pos};
    below.z--;
    auto v2 = count_visibility_grid_neighbourhood(below);
    auto e2 = count_empty_grid_neighbourhood(below);
    bool below_risk = false;
    if (config.require_vision) {
        below_risk = v2 >= 4 && e2 >= 4;
    } else {
        below_risk = e2 >= 4;
    }
    if (!below_risk) {
        return false;
    }
    auto v = count_visibility_grid_neighbourhood(map_pos);
    auto e = count_empty_grid_neighbourhood(map_pos);
    if (config.require_vision) {
        return v >= 3 && e >= 3;
    } else {
        return e >= 3;
    }
}

bool ActiveJobManager::possible_cavein(const df::coord &map_pos) const {
    for (auto dig_pos : active_dig_sites) {
        if (dig_pos == map_pos) continue;
        if (calc_distance(map_pos, dig_pos) <= 2) {
            // find neighbours
            df::coord n1[8];
            df::coord n2[8];
            get_neighbours(map_pos, n1);
            get_neighbours(dig_pos, n2);
            // find shared neighbours
            for (int i = 0; i < 7; ++i) {
                for (int j = i + 1; j < 8; ++j) {
                    if (n1[i] == n2[j]) {
                        if (has_cavein_conditions(n1[i])) {
                            WARN(jobs).print("Channel-Safely::jobs: Cave-in conditions detected at (" COORD ")\n", COORDARGS(n1[i]));
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void ActiveJobManager::cleanup() {
    if (active_jobs.empty()) {
        return;
    }
    // make two sets. 1 for valid active job ids. 1 for valid active job dig sites.
    std::unordered_set<int32_t> valid_job_ids;
    std::unordered_set<df::coord> valid_dig_sites;
    // iterate over valid jobs
    for (df::job_list_link* node = &df::global::world->jobs.list; node != nullptr; node = node->next) {
        if (df::job* job = node->item; active_jobs.contains(job->id)) {
            valid_job_ids.emplace(job->id);
            valid_dig_sites.emplace(job->pos);
        }
    }

    // erase stale data
    std::erase_if(active_jobs,[&](const std::pair<int32_t, ActiveJob> &kv) {
        return !valid_job_ids.contains(kv.first);
    });
    std::erase_if(active_workers, [&](const std::pair<int32_t, ActiveWorker> &kv) {
        return !valid_job_ids.contains(kv.first);
    });
    std::erase_if(active_dig_sites,[&](const df::coord &pos) {
        return !valid_dig_sites.contains(pos);
    });
    std::erase_if(cancel_queue, [&](const df::coord &pos) {
        return !active_dig_sites.contains(pos);
    });
    // clean up any "endangered" workers that have been tracked for longer than the configured duration
    std::erase_if(endangered_units, [tick = df::global::world->frame_counter](const std::pair<int32_t,int32_t> &kv) {
        return tick - kv.second > config.res_watch_duration;
    });
    // erase cancellation data
    std::erase_if(active_jobs,[&](const std::pair<int32_t, ActiveJob> &kv) {
       return cancel_queue.contains(kv.second.pos);
    });
    std::erase_if(active_workers, [&](const std::pair<int32_t, ActiveWorker> &kv) {
       return !active_jobs.contains(kv.first);
    });
    std::erase_if(active_dig_sites, [&](const df::coord &pos) {
        return cancel_queue.contains(pos);
    });
}

void ActiveJobManager::on_update(color_ostream &out) {
    INFO(jobs).print("onUpdate()");
    int32_t tick = df::global::world->frame_counter;
    // clean up stale df::job*
    if ((config.monitoring || config.resurrect) && tick - last_tick >= 1) {
        last_tick = tick;
        cleanup();
    }


    // monitoring activity
    if (config.monitoring && tick - last_monitor_tick >= 666){//config.monitor_freq) {
        last_monitor_tick = tick;
        TRACE(monitor).print("OnUpdate() monitoring now\n");

        // iterate active jobs
        for (auto& [id,ajob]: active_jobs) {
            if unlikely(!ajob.worker) continue;
            if unlikely(!Units::isAlive(ajob.worker)) continue;
            if unlikely(!Maps::isValidTilePos(ajob.pos)) continue;
            if likely(ajob.worker->pos != ajob.pos) continue;
            if likely(is_safe_fall(ajob.pos)) continue;

            // unsafe
            WARN(monitor).print(" -> unsafe job\n");
            Job::removeWorker(ajob.job);

            if (!config.resurrect) {
                // set marker mode
                Maps::getTileOccupancy(ajob.pos)->bits.dig_marked = true;

                using df_bsedp = df::block_square_event_designation_priorityst;
                // prevent algorithm from re-enabling designation
                for (auto &blk_evt: Maps::getBlock(ajob.pos)->block_events) {
                    if (auto bsedp = virtual_cast<df_bsedp>(blk_evt)) {
                        df::coord local(ajob.pos);
                        local.x = local.x % 16;
                        local.y = local.y % 16;
                        bsedp->priority[Coord(local)] = config.ignore_threshold * 1000 + 1;
                        break;
                    }
                }
                DEBUG(monitor).print(" -> set marker mode\n");
            }
        }
        TRACE(monitor).print("OnUpdate() monitoring done\n");
    }
}

extern DFHack::EventManager::EventHandler resurrectHandler;

void ActiveJobManager::on_job_start(df::job* job) {
    if (!ChannelManager::Get().contains(job->pos)) {
        ChannelManager::Get().build_groups(false);
    }
    df::unit* worker = Job::getWorker(job);
    // there is a valid worker (living citizen) on the job? right..
    if unlikely(!worker || !Units::isAlive(worker) || !Units::isCitizen(worker)) {
        DEBUG(jobs).print("on_job_start: invalid worker, function exits early.");
        return;
    }
    auto pos = job->pos;
    ActiveJob ajob {job->id, job, worker, pos};
    ActiveWorker aworker {worker->id, worker, worker->pos};
    // we only track ActiveJob's when monitoring or resurrecting
    if (config.monitoring || config.resurrect) {
        active_jobs.emplace(ajob.id,ajob);
        active_workers.emplace(ajob.id, aworker);
        safe_locations[aworker.id] = aworker.last_safe_pos;
        if (config.resurrect && !CSP::tick_it_master.resurrect_queued) {
            CSP::tick_it_master.resurrect_queued = true;
            // todo: verify this means NEXT tick.. or just use 0
            EventManager::registerTick(resurrectHandler,1);
        }
    }
    // cavein prevention is the rest of the function
    if (!config.riskaverse) {
        return;
    }
    // if a cavein is possible - we'll try to cancel the job
    if (has_cavein_conditions(pos)) {
        /* todo:
            * test if the game crashes or the jobs start polluting the list indefinitely
            * prediction is that the jobs will cause the tiles to flash forever
        */
        if (!Job::removeWorker(job)) { WARN(jobs).print("  Unable to remove worker from job."); }
        cancel_queue.emplace(pos);
        return;
    }
    // manage the group the job belongs to
    ChannelManager::Get().manage_group(pos, true, false);
    // we track active dig sites for cavein detection
    active_dig_sites.emplace(pos);
    // set tile to restricted
    TRACE(jobs).print("   setting job tile to restricted\n");
    Maps::getTileDesignation(job->pos)->bits.traffic = df::tile_traffic::Restricted;
}

void ActiveJobManager::on_job_completed(color_ostream &out, df::job* job) {
    if (!active_jobs.contains(job->id)) {
        return;
    }
    auto ajob = active_jobs[job->id];
    auto aworker = active_workers[ajob.id];
    if (config.resurrect && !Units::isAlive(aworker.worker)) {
        resurrect(out, aworker.id);
        df::coord lowest = simulate_fall(aworker.last_safe_pos);
        Units::teleport(aworker.worker, lowest);
    }

    // verify completion
    auto block = Maps::getTileBlock(ajob.pos);
    df::coord local(ajob.pos);
    local.x = local.x % 16;
    local.y = local.y % 16;
    if (!TileCache::Get().hasChanged(ajob.pos, block->tiletype[Coord(local)])) {
        return;
    }
    // the job can be considered done
    ChannelManager::Get().erase(ajob.pos);
    ChannelManager::Get().manage_group(ajob.pos, true, false);
    block->designation[Coord(local)].bits.traffic = df::tile_traffic::Normal;
    df::coord below(ajob.pos);
    below.z--;
    DEBUG(jobs).print(" -> (" COORD ") is marked done, managing group below.\n", COORDARGS(ajob.pos));
    ChannelManager::Get().manage_group(below);

    // erase tracked data
    active_jobs.erase(ajob.id);
    active_workers.erase(ajob.id);
    active_dig_sites.erase(ajob.pos);
    TileCache::Get().uncache(ajob.pos);
    //CSP::dignow_queue.erase(ajob.pos);
}

void ActiveJobManager::on_report_event(df::report* report) {
    switch (report->type) {
        case announcement_type::CANCEL_JOB:
            if (config.insta_dig) {
                bool valid = false;
                if (ChannelManager::Get().contains(report->pos)) {
                    CSP::dignow_queue.emplace(report->pos);
                    valid = true;
                }
                if (ChannelManager::Get().contains(report->pos2)) {
                    CSP::dignow_queue.emplace(report->pos2);
                    valid = true;
                }
                if (valid) {
                    WARN(jobs).print("Report: canceled a channel job. [insta-dig: on]");
                }
                DEBUG(plugin).print("%d, pos: " COORD ", pos2: " COORD "\n%s\n", report->id, COORDARGS(report->pos),
                                    COORDARGS(report->pos2), report->text.c_str());
            }
            break;
        case announcement_type::CAVE_COLLAPSE:
            if (config.resurrect) {
                DEBUG(plugin).print("CAVE IN\n%d, pos: " COORD ", pos2: " COORD "\n%s\n", report->id, COORDARGS(report->pos),
                                    COORDARGS(report->pos2), report->text.c_str());

                df::coord below = report->pos;
                below.z -= 1;
                below = simulate_area_fall(below);
                df::coord areaMin{report->pos};
                df::coord areaMax{areaMin};
                areaMin.x -= 15;
                areaMin.y -= 15;
                areaMax.x += 15;
                areaMax.y += 15;
                areaMin.z = below.z;
                areaMax.z += 1;
                std::vector<df::unit*> units;
                Units::getUnitsInBox(units, COORDARGS(areaMin), COORDARGS(areaMax));
                int32_t tick = df::global::world->frame_counter;
                for (auto unit: units) {
                    endangered_units[unit->id] = tick;
                    DEBUG(plugin).print(" [id %d] was near a cave in.\n", unit->id);
                    if (safe_locations.emplace(unit->id, unit->pos).second) {
                        DEBUG(plugin).print(" [id %d] doesn't have a safe location saved, we've saved their current position.\n", unit->id);
                    }
                }
                for (auto [job_id,aworker] : active_workers) {
                    if (endangered_units.contains(aworker.id)) {
                        endangered_units[aworker.id] = tick;
                        DEBUG(plugin).print(" [id %d] is/was an endangereed worker, we'll extend tracking them too.\n", aworker.id);
                    }
                }
                if (!CSP::tick_it_master.resurrect_queued) {
                    CSP::tick_it_master.resurrect_queued = true;
                    // todo: verify this means NEXT tick.. or just use 0
                    EventManager::registerTick(resurrectHandler,1);
                }
            }
            break;
        default:
            break;
    }
}

extern DFHack::EventManager::EventHandler cancelHandler;

void ActiveJobManager::cancel(df::coord site) {
    if (!CSP::tick_it_master.cancel_queued) {
        CSP::tick_it_master.cancel_queued = true;
        // todo: verify this means NEXT tick.. or just use 0
        EventManager::registerTick(cancelHandler,1);
    }
    cancel_queue.emplace(site);
}

void ActiveJobManager::handle_cancellation() {
    // cancel jobs in the cancel queue
    ChannelJobs jobs;
    jobs.load_channel_jobs();
    for (auto pos : cancel_queue) {
        INFO(jobs).print("Canceling job: " COORD, COORDARGS(pos));
        if (auto job_ptr = jobs.find_job(pos); job_ptr) {
            if unlikely(job_ptr->id < 0) {
                Job::removePostings(job_ptr, true);
                revert_designation(pos, job_ptr->job_type);
                continue;
            }
            Job::removeWorker(job_ptr);
            Job::removePostings(job_ptr, true);
            Job::removeJob(job_ptr);
            revert_designation(pos, job_ptr->job_type);
        }
    }
    cancel_queue.clear();
}

void ActiveJobManager::handle_resurrect(color_ostream &out) {
    if (!config.resurrect) {
        return;
    }
    for (auto [id, aworker] : active_workers) {
        if (Units::isAlive(aworker.worker)) {
            continue;
        }
        resurrect(out, aworker.id);
        df::coord lowest = simulate_fall(aworker.last_safe_pos);
        Units::teleport(aworker.worker, lowest);
    }
    // resurrect any dead endangered units
    for (auto unit : df::global::world->units.all) {
        if (!endangered_units.contains(unit->id) || !safe_locations.contains(unit->id)) {
            continue;
        }
        if (Units::isAlive(unit)) {
            continue;
        }
        resurrect(out, unit->id);
        df::coord lowest = simulate_fall(safe_locations[unit->id]);
        Units::teleport(unit, lowest);
    }
}

void ActiveJobManager::clear() {
    safe_locations.clear();
    endangered_units.clear();
    active_dig_sites.clear();
    active_workers.clear();
    active_jobs.clear();
    cancel_queue.clear();
}

bool ActiveJobManager::needs_resurrect_queued() {
    return !active_workers.empty() || !endangered_units.empty();
}

