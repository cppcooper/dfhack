#include <channel-groups.h>
#include <tile-cache.h>
#include <inlines.h>
#include <modules/Maps.h>
#include <df/block_square_event_designation_priorityst.h>

#include <random>

// iterates the DF job list and adds channel jobs to the `jobs` container
void ChannelJobs::load_channel_jobs() {
    job_ptrs.clear();
    df::job_list_link* node = df::global::world->jobs.list.next;
    while (node) {
        df::job* job = node->item;
        node = node->next;
        if (is_channel_job(job)) {
            job_ptrs[job->pos] = job;
        }
    }
}

// adds map_pos to a group if an adjacent one exists, or creates one if none exist... if multiple exist they're merged into the first found
void ChannelGroups::add(const df::coord &map_pos) {
    // if we've already added this, we don't need to do it again
    if (pos_to_groups_idx_map.count(map_pos)) {
        return;
    }
    /* We need to add map_pos to an existing group if possible...
     * So what we do is we look at neighbours to see if they belong to one or more existing groups
     * If there is more than one group, we'll be merging them
     */
    df::coord neighbors[8];
    get_neighbours(map_pos, neighbors);
    Group* group = nullptr;
    int group_index = -1;

    DEBUG(groups).print("    add(" COORD ")\n", COORDARGS(map_pos));
    // and so we begin iterating the neighbours
    for (auto &neighbour: neighbors) {
        if unlikely(!Maps::isValidTilePos(neighbour)) continue;
        // go to the next neighbour if this one doesn't have a group
        if likely(!pos_to_groups_idx_map.contains(neighbour)) continue;
        if (!group){
            // get the group, since at least one exists...
            group_index = pos_to_groups_idx_map.find(neighbour)->second;
            group = &groups_array.at(group_index);
            continue;
        }
        // we don't do anything if the found group is the same as the existing group
        auto index2 = pos_to_groups_idx_map.find(neighbour)->second;
        if likely(group_index == index2) continue;

        // different. merge
        Group* group2 = &groups_array.at(index2);
        // which N is smaller, transfer it's elements
        if (groups_array[group_index].size() > groups_array[index2].size()) {
            for (auto pos: *group2) {
                group->emplace(pos);
                pos_to_groups_idx_map[pos] = group_index;
            }
            group2->clear();
            free_spots.emplace(index2);
            // group2 merged into group
        } else {
            for (auto pos: *group) {
                group2->emplace(pos);
                pos_to_groups_idx_map[pos] = index2;
            }
            group->clear();
            group = group2;
            free_spots.emplace(group_index);
            group_index = index2;
            // group merged into group2. group updated
        }
    }
    // if we haven't found at least one group by now we need to create/get one
    if (!group) {
        // first we check if we can re-use a group that's been freed
        if (!free_spots.empty()) {
            // first element in a set is always the lowest value, so we re-use from the front of the vector
            group_index = *free_spots.begin();
            group = &groups_array[group_index];
            free_spots.erase(free_spots.begin());
        } else {
            // we create a brand-new group to use
            group_index = groups_array.size();
            groups_array.emplace_back();
            group = &groups_array[group_index];
        }
    }
    // puts the "add" in "ChannelGroups::add"
    pos_to_groups_idx_map[map_pos] = group_index;
    group->emplace(map_pos);
    DEBUG(groups).print(" = group[%d] of (" COORD ") is size: %zu\n", group_index, COORDARGS(map_pos), group->size());

    // we may have performed a merge, so we update all the `coord -> group index` mappings
    // for (auto &wpos: *group) {
    //     pos_to_groups_idx_map[wpos] = group_index;
    // }
    DEBUG(groups).print(" <- add() exits, there are %zu mappings\n", pos_to_groups_idx_map.size());
}

// scans a single tile for channel designations
void ChannelGroups::scan_one(const df::coord &map_pos) {
    df::map_block* block = Maps::getTileBlock(map_pos);
    int16_t lx = map_pos.x % 16;
    int16_t ly = map_pos.y % 16;
    if (is_dig_designation(block->designation[lx][ly]) || block->occupancy[lx][ly].bits.dig_marked ) {
        // We have a dig designated, or marked. Some of these will not need intervention.
        for (df::block_square_event* event: block->block_events) {
            if (auto evT = virtual_cast<df::block_square_event_designation_priorityst>(event)) {
                // we want to let the user keep some designations free of being managed
                TRACE(groups).print("   tile designation priority: %d\n", evT->priority[lx][ly]);
                if (evT->priority[lx][ly] < 1001 * config.ignore_threshold) {
                    add(map_pos);
                }
            }
        }
    } else if (TileCache::Get().hasChanged(map_pos, block->tiletype[lx][ly])) {
        TileCache::Get().uncache(map_pos);
        remove(map_pos);
        if (jobs.contains(map_pos)) {
            jobs.erase(map_pos);
        }
        block->designation[lx][ly].bits.dig = df::tile_dig_designation::No;
    }
}

// builds groupings of adjacent channel designations
void ChannelGroups::scan(bool full_scan) {
    static std::default_random_engine RNG(0);
    static std::bernoulli_distribution sometimes_scanFULLY(0.15);
    if (!full_scan) {
        full_scan = sometimes_scanFULLY(RNG);
    }

    scan_jobs();
    DEBUG(groups).print("  scan()\n");
    // foreach block
    for (int32_t z = mapz - 1; z >= 0; --z) {
        for (int32_t by = 0; by < mapy; ++by) {
            for (int32_t bx = 0; bx < mapx; ++bx) {
                // the block
                if (df::map_block* block = Maps::getBlock(bx, by, z)) {
                    // skip this block?
                    if (!full_scan && !block->flags.bits.designated) {
                        continue;
                    }
                    // foreach tile
                    for (int16_t lx = 0; lx < 16; ++lx) {
                        for (int16_t ly = 0; ly < 16; ++ly) {
                            // the tile, check if it has a channel designation
                            df::coord map_pos((bx * 16) + lx, (by * 16) + ly, z);
                            scan_one(map_pos);
                        }
                    }
                }
            }
        }
    }
    INFO(groups).print("scan() exits\n");
}

// updates groupings of adjacent channel designations based on changes to the job list
void ChannelGroups::scan_jobs() {
    // save current jobs, then clear and load the current jobs
    std::unordered_set<df::coord> last_job_locations(
        std::ranges::begin(jobs.keys()),
        std::ranges::end(jobs.keys())
    );
    jobs.load_channel_jobs();
    std::unordered_set<df::coord> current_job_locations(
        std::ranges::begin(jobs.keys()),
        std::ranges::end(jobs.keys())
    );
    // transpose channel jobs to
    std::set<df::coord> new_jobs;
    set_difference(current_job_locations, last_job_locations, new_jobs);

    for (auto &pos : new_jobs) {
        add(pos);
    }
}

// clears out the containers for unloading maps or disabling the plugin
void ChannelGroups::clear() {
    debug_map();
    WARN(groups).print(" <- clearing groups\n");
    jobs.clear();
    free_spots.clear();
    pos_to_groups_idx_map.clear();
    for(size_t i = 0; i < groups_array.size(); ++i) {
        groups_array[i].clear();
        free_spots.emplace(i);
    }
}

// erases map_pos from its group, and deletes mappings IFF the group is empty
void ChannelGroups::remove(const df::coord &map_pos) {
    if (!pos_to_groups_idx_map.contains(map_pos)) {
        return;
    }
    // get the group, and map_pos' block*
    int group_index = pos_to_groups_idx_map.find(map_pos)->second;
    Group &group = groups_array[group_index];
    // erase map_pos from the group
    group.erase(map_pos);
    pos_to_groups_idx_map.erase(map_pos);
    // clean up if the group is empty
    if (group.empty()) {
        // likely redundant: erase `coord -> group group_index` mappings
        for (auto iter = pos_to_groups_idx_map.begin(); iter != pos_to_groups_idx_map.end();) {
            if unlikely(group_index == iter->second) {
                iter = pos_to_groups_idx_map.erase(iter);
                continue;
            }
            ++iter;
        }
        // flag the `groups` group_index as available
        free_spots.insert(group_index);
    }
}

// finds a group corresponding to a map position if one exists
std::set<df::coord>* ChannelGroups::find(const df::coord &map_pos) {
    const auto iter = pos_to_groups_idx_map.find(map_pos);
    if (iter != pos_to_groups_idx_map.end()) {
        return &groups_array[iter->second];
    }
    return nullptr;
}

// returns a count of 0 or 1 depending on whether map_pos is mapped to a group
bool ChannelGroups::contains(const df::coord &map_pos) const {
    return pos_to_groups_idx_map.contains(map_pos) || jobs.contains(map_pos);
}

// prints debug info about the groups stored, and their members
void ChannelGroups::debug_groups() {
    if (DFHack::debug_groups.isEnabled(DebugCategory::LDEBUG)) {
        int idx = 0;
        DEBUG(groups).print(" debugging group data\n");
        for (auto &group: groups_array) {
            DEBUG(groups).print("  group %d (size: %zu)\n", idx, group.size());
            for (auto &pos: group) {
                DEBUG(groups).print("   (%d,%d,%d)\n", pos.x, pos.y, pos.z);
            }
            idx++;
        }
    }
}

// prints debug info group mappings
void ChannelGroups::debug_map() {
    if (DFHack::debug_groups.isEnabled(DebugCategory::LTRACE)) {
        INFO(groups).print("Group Mappings: %zu\n", pos_to_groups_idx_map.size());
        for (auto &pair: pos_to_groups_idx_map) {
            TRACE(groups).print(" map[" COORD "] = %d\n", COORDARGS(pair.first), pair.second);
        }
    }
}
