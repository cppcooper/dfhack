#pragma once
#include <PluginManager.h>
#include <modules/World.h>
#include <modules/Maps.h>
#include <modules/Job.h>
#include <df/map_block.h>
#include "channel-groups.h"
#include "plugin.h"

using namespace DFHack;

// Uses GroupData to detect an unsafe work environment
class ChannelManager {
private:
    ChannelManager()= default;
protected:
    ChannelGroups groups = ChannelGroups(jobs);
    ChannelJobs jobs;
public:

    static ChannelManager& Get(){
        static ChannelManager instance;
        return instance;
    }

    void build_groups(bool full_scan = false) { groups.scan(full_scan); debug(); }
    void destroy_groups() { groups.clear(); debug(); }
    void manage_groups();
    void manage_group(const df::coord &map_pos, bool use_mm_arg_value = false, bool marker_mode = false);
    void manage_group(const Group &group, bool use_mm_arg_value = false, bool marker_mode = false);
    bool manage_one(const df::coord &map_pos, bool use_mm_arg_value = false, bool marker_mode = false);
    void erase(const df::coord &map_pos);
    bool contains(const df::coord &map_pos) const { return groups.contains(map_pos); }
    void debug() {
        DEBUG(groups).print(" DEBUGGING GROUPS:\n");
        groups.debug_map();
        groups.debug_groups();
    }
};
