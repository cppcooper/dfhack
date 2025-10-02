#include <active-job-manager.h>
#include <channel-manager.h>
#include <tile-cache.h>
#include <inlines.h>

#include <modules/EventManager.h> //hash function for df::coord
#include <df/block_square_event_designation_priorityst.h>
#include <df/unit.h>

#define NUMARGS(...) std::tuple_size<decltype(std::make_tuple(__VA_ARGS__))>::value
#define d_assert(condition, ...) \
            static_assert(NUMARGS(__VA_ARGS__) >= 1, "d_assert(condition, format, ...) requires at least up to format as arguments"); \
            if (!condition) {                                                   \
                DFHack::Core::getInstance().getConsole().printerr(__VA_ARGS__); \
                assert(0);                                                      \
            }

namespace CSP {
    extern ActiveJobManager active_job_manager;
}

df::unit* find_nearest_dwarf(const df::coord &map_pos) {
    df::unit* nearest = nullptr;
    uint32_t distance;
    for (auto unit : df::global::world->units.active) {
        if (!nearest) {
            nearest = unit;
            distance = calc_distance(unit->pos, map_pos);
        } else if (unit->status.labors[df::unit_labor::MINE]) {
            uint32_t d = calc_distance(unit->pos, map_pos);
            if (d < distance) {
                nearest = unit;
                distance = d;
            } else if (Maps::canWalkBetween(unit->pos, map_pos)) {
                return unit;
            }
        }
    }
    return nearest;
}

// sets mark flags as necessary, for all designations
void ChannelManager::manage_groups() {
    // make sure we've got a fort map to analyze
    if (World::isFortressMode() && Maps::IsValid()) {
        // iterate the groups we built/updated
        for (const auto &group: groups.keys()) {
            manage_group(group, true, has_any_groups_above(groups, group));
        }
    }
}

void ChannelManager::manage_group(const std::set<df::coord> &group, bool use_mm_arg_value, bool marker_mode) {
    if (!use_mm_arg_value) {
        marker_mode = has_any_groups_above(groups, group);
    }
    // cavein prevention
    bool cavein_possible = false;
    uint8_t least_access = 100;

    std::unordered_map<df::coord, uint8_t> cavein_candidates;
    if (!marker_mode) {
        /* To prevent cave-ins we're looking at accessibility of tiles with open space below them
         * If it has space below, it has somewhere to fall
         * Accessibility tells us how close to a cave-in a tile is, low values are at risk of cave-ins
         * To count access, we find a random miner dwarf and count how many tile neighbours they can path to
         * */
        // find a dwarf to path from
        df::coord miner_pos = find_nearest_dwarf(*group.begin())->pos;

        // Analyze designations
        for (const auto &pos: group) {
            df::coord below(pos);
            below.z--;
            const auto visible = Maps::isTileVisible(below);
            if (config.require_vision && !visible) {
                // skipping because not visible and requires vision
                continue;
            }
            const auto below_ttype = *Maps::getTileType(below);
            if ((visible || !config.require_vision) && !DFHack::isOpenTerrain(below_ttype) && !DFHack::isFloorTerrain(below_ttype)) {
                // skipping because not floor or open space below while visible or not requiring vision
                continue;
            }
            // open space below /or floor
            DEBUG(manager).print("analysis: cave-in condition found\n");
            auto access = count_accessibility(miner_pos, pos);
            // if any
            cavein_possible = config.riskaverse;
            cavein_candidates.emplace(pos, access);
            least_access = std::min(access, least_access);
        }
        DEBUG(manager).print("cavein possible(%d)\n"
                             "%zu candidates\n"
                             "least access %d\n", cavein_possible, cavein_candidates.size(), least_access);
    }
    for (auto &pos: group) {
        // if no cave-in is possible [or we don't check for], we'll just execute normally and move on
        if likely(marker_mode || !config.riskaverse || !cavein_possible) {
            TRACE(manager).print("cave-in evaluated false\n");
            d_assert(manage_one(pos, true, marker_mode), "manage_one() failed. L122");
            continue;
        }
        // marker_mode = false
        // cavein is only possible if marker_mode is false
        // we want to dig the cavein candidates first, the least accessible ones specifically
        //const static uint8_t OFFSET = 2; //value has been tweaked to avoid cave-ins whilst activating as many designations as possible
        if (!cavein_candidates.contains(pos)) {
            // not a cavein candidate
            d_assert(manage_one(pos, true, marker_mode), "manage_one() failed. L131");
            continue;
        }
        // if (cavein_candidates[pos] > least_access+OFFSET) {
        //     // not a cavein candidate
        //     d_assert(manage_one(pos, true, marker_mode), "manage_one() failed. L136");
        //     continue;
        // }
        // df::coord local(pos);
        // local.x %= 16;
        // local.y %= 16;
        // auto block = Maps::ensureTileBlock(pos);
        // // if we don't find the priority in block_events, it probably means bad things
        // for (df::block_square_event* event: block->block_events) {
        //     if (auto evT = virtual_cast<df::block_square_event_designation_priorityst>(event)) {
        //         // we want to let the user keep some designations free of being managed
        //         auto b = std::max(static_cast<uint8_t>(1), cavein_candidates[pos]);
        //         auto v = (b * 1001);
        //         DEBUG(manager).print("(" COORD ") 1001(%d) -> %d {least-access: %d}\n",COORDARGS(pos), b, v, least_access);
        //         evT->priority[Coord(local)] = v;
        //     }
        // }
        d_assert(manage_one(pos, true, marker_mode), "manage_one() failed. L154");
    }
}

void ChannelManager::manage_group(const df::coord &map_pos, bool use_mm_arg_value, bool marker_mode) {
    if (!groups.contains(map_pos)) {
        groups.scan_one(map_pos);
    }
    auto group_ptr = groups.find(map_pos);
    if (!group_ptr) return;
    manage_group(*group_ptr, use_mm_arg_value, marker_mode);
}

bool ChannelManager::manage_one(const df::coord &map_pos, bool use_mm_arg_value, bool marker_mode) {
    if (!Maps::isValidTilePos(map_pos)) {
        return false;
    }
    df::map_block* block = Maps::getTileBlock(map_pos);
    // process only for layers below the top 3
    if (map_pos.z < mapz - 3) {
        // do we already know whether to set marker mode?
        if (!use_mm_arg_value) {
            // marker_mode is set to true if it is unsafe to dig
            marker_mode = !is_safe_to_dig_down(map_pos) || has_group_above(groups, map_pos); // wonder if the |= can shortcut
            auto gptr = groups.find(map_pos);
            if (!marker_mode && gptr) {
                marker_mode = has_any_groups_above(groups, *gptr);
            }
        }
        if (marker_mode) {
            CSP::active_job_manager.cancel(map_pos);
        } else if (!block->flags.bits.designated) {
            // marker mode is disabled, so this is a live designation
            block->flags.bits.designated = true;
        }
        // we calculate the position inside the block*
        df::coord local(map_pos);
        local.x = local.x % 16;
        local.y = local.y % 16;
        block->designation[Coord(local)].bits.dig = tile_dig_designation::Channel;
        block->occupancy[Coord(local)].bits.dig_marked = marker_mode;
        //block->occupancy[Coord(local)].bits.
        DEBUG(manager).print("manage_one((" COORD "), %d, %d): marker mode: %s\n",
            COORDARGS(map_pos), use_mm_arg_value, marker_mode, marker_mode ? "ENABLED" : "DISABLED");
    }
    return true;
}

void ChannelManager::erase(const df::coord &map_pos) {
    jobs.erase(map_pos);
    groups.remove(map_pos);
}
