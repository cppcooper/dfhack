/* Prevent channeling down into known open space.
Author:  Josh Cooper
Created: Aug. 4 2020
Updated: Dec. 8 2022
*/
/*
This skeletal logic has not been kept up-to-date since ~v0.5

 Enable plugin:
 -> build groups
 -> manage designations

 Unpause event:
 -> build groups
 -> manage designations

 Manage Designation(s):
 -> for each group in groups:
    -> does any tile in group have a group above
        -> Yes: set entire group to marker mode
        -> No: activate entire group (still checks is_safe_to_dig_down before activating each designation)

 Job started event:
 -> validate job type (channel)
 -> check pathing:
    -> Can: add job/worker to tracking
    -> Can: set tile to restricted
    -> Cannot: remove worker
    -> Cannot: insta-dig & delete job
    -> Cannot: set designation to Marker Mode (no insta-digging)

 OnUpdate:
 -> check worker location:
    -> CanFall: check if a fall would be safe:
        -> Safe: do nothing
        -> Unsafe: remove worker
        -> Unsafe: insta-dig & delete job (presumes the job is only accessible from directly on the tile)
        -> Unsafe: set designation to Marker Mode (no insta-digging)
 -> check tile occupancy:
    -> HasUnit: check if a fall would be safe:
        -> Safe: do nothing, let them fall
        -> Unsafe: remove worker for 1 tick (test if this "pauses" or cancels the job)
        -> Unsafe: Add feature to teleport unit?

 Job completed event:
 -> validate job type (channel)
 -> verify completion:
    -> IsOpenSpace: mark done
    -> IsOpenSpace: manage tile below
    -> NotOpenSpace: check for designation
        -> HasDesignation: do nothing
        -> NoDesignation: mark done (erases from group)
        -> NoDesignation: manage tile below
*/

#include "plugin.h"
#include "inlines.h"
#include "channel-manager.h"
#include "active-job-manager.h"
#include "tile-cache.h"

#include <Debug.h>
#include <PluginManager.h>

#include <modules/EventManager.h>
#include <modules/Units.h>

#include <df/block_square_event_designation_priorityst.h>
#include <df/report.h>
#include <df/tile_traffic.h>
#include <df/world.h>
#include <df/unit.h>

#include <ranges>
#include <cinttypes>
#include <on-tick.h>
#include <unordered_map>
#include <unordered_set>

// Debugging
namespace DFHack {
    DBG_DECLARE(channelsafely, plugin, DebugCategory::LINFO);
    DBG_DECLARE(channelsafely, monitor, DebugCategory::LERROR);
    DBG_DECLARE(channelsafely, manager, DebugCategory::LERROR);
    DBG_DECLARE(channelsafely, groups, DebugCategory::LERROR);
    DBG_DECLARE(channelsafely, jobs, DebugCategory::LERROR);
}

DFHACK_PLUGIN("channel-safely");
DFHACK_PLUGIN_IS_ENABLED(enabled);
REQUIRE_GLOBAL(world);

namespace EM = EventManager;
using namespace DFHack;
using namespace EM::EventType;

int32_t mapx, mapy, mapz;
Configuration config;
PersistentDataItem psetting;
PersistentDataItem pfeature;
const std::string FCONFIG_KEY = std::string(plugin_name) + "/feature";
const std::string SCONFIG_KEY = std::string(plugin_name) + "/setting";

enum FeatureConfigData {
    VISION,
    MONITOR,
    RESURRECT,
    INSTADIG,
    RISKAVERSE
};

enum SettingConfigData {
    REFRESH_RATE,
    MONITOR_RATE,
    IGNORE_THRESH,
    FALL_THRESH,
    WATCH_DURATION
};

// dig-now.cpp
extern void refresh(DFHack::color_ostream&, void*);

namespace CSP {
    OnTick tick_it_master;
    ActiveJobManager active_job_manager;
    std::unordered_set<df::coord> dignow_queue;

    void ClearData() {
        ChannelManager::Get().destroy_groups();
        dignow_queue.clear();
        active_job_manager.clear();
    }

    void SaveSettings() {
        if (pfeature.isValid() && psetting.isValid()) {
            try {
                pfeature.ival(MONITOR) = config.monitoring;
                pfeature.ival(VISION) = config.require_vision;
                pfeature.ival(INSTADIG) = config.insta_dig;
                pfeature.ival(RESURRECT) = config.resurrect;
                pfeature.ival(RISKAVERSE) = config.riskaverse;

                psetting.ival(REFRESH_RATE) = config.refresh_freq;
                //psetting.ival(MONITOR_RATE) = config.monitor_freq;
                psetting.ival(IGNORE_THRESH) = config.ignore_threshold;
                psetting.ival(FALL_THRESH) = config.fall_threshold;
                psetting.ival(WATCH_DURATION) = config.res_watch_duration;
            } catch (std::exception &e) {
                ERR(plugin).print("%s\n", e.what());
            }
        }
    }

    void LoadSettings() {
        pfeature = World::GetPersistentSiteData(FCONFIG_KEY);
        psetting = World::GetPersistentSiteData(SCONFIG_KEY);

        if (!pfeature.isValid() || !psetting.isValid()) {
            pfeature = World::AddPersistentSiteData(FCONFIG_KEY);
            psetting = World::AddPersistentSiteData(SCONFIG_KEY);
            SaveSettings();
        } else {
            try {
                config.monitoring = pfeature.ival(MONITOR);
                config.require_vision = pfeature.ival(VISION);
                config.insta_dig = pfeature.ival(INSTADIG);
                config.resurrect = pfeature.ival(RESURRECT);
                config.riskaverse = pfeature.ival(RISKAVERSE);

                config.ignore_threshold = psetting.ival(IGNORE_THRESH);
                config.fall_threshold = psetting.ival(FALL_THRESH);
                config.refresh_freq = psetting.ival(REFRESH_RATE);
                //config.monitor_freq = psetting.ival(MONITOR_RATE);
                config.res_watch_duration = psetting.ival(WATCH_DURATION);
            } catch (std::exception &e) {
                ERR(plugin).print("%s\n", e.what());
            }
        }
    }

    void UnpauseEvent(bool full_scan = false){
        SaveSettings();
        DEBUG(plugin).print("UnpauseEvent()\n");
        ChannelManager::Get().build_groups(full_scan);
        ChannelManager::Get().manage_groups();
        DEBUG(plugin).print("UnpauseEvent() exits\n");
    }

    void JobStartedEvent(color_ostream &out, void* j) {
        if (!enabled || World::isFortressMode() || Maps::IsValid()) {
            return;
        }
        auto job = static_cast<df::job*>(j);
        if likely(is_channel_job(job)) {
            active_job_manager.on_job_start(job);
        }
    }

    void JobCompletedEvent(color_ostream &out, void* j) {
        if (!enabled || World::isFortressMode() || Maps::IsValid()) {
            return;
        }
        auto job = static_cast<df::job*>(j);
        // we only care if the job is a channeling one
        if likely(is_channel_job(job)) {
            active_job_manager.on_job_completed(out, job);
        }
    }

    void NewReportEvent(color_ostream &out, void* r) {
        auto report_id = (int32_t)(intptr_t(r));
        df::report* report = df::report::find(report_id);
        if (!report) {
            WARN(plugin).print("Error: NewReportEvent() received an invalid report_id - a report* cannot be found\n");
            return;
        }
        active_job_manager.on_report_event(report);
    }
}

command_result channel_safely(color_ostream &out, std::vector<std::string> &parameters);

DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    commands.push_back(PluginCommand("channel-safely",
                                     "Automatically manage channel designations.",
                                     channel_safely,
                                     false));;
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    EM::unregisterAll(plugin_self);
    return CR_OK;
}

DFhackCExport command_result plugin_load_site_data (color_ostream &out) {

    CSP::LoadSettings();
    if (enabled) {
        std::vector<std::string> params;
        channel_safely(out, params);
    }
    return DFHack::CR_OK;
}

DFhackCExport command_result plugin_enable(color_ostream &out, bool enable) {
    if (!Core::getInstance().isMapLoaded() || !World::IsSiteLoaded()) {
        out.printerr("Cannot enable %s without a loaded fort.\n", plugin_name);
        return CR_FAILURE;
    }

    if (enable && !enabled) {
        // register events to check jobs / update tracking
        //EM::EventHandler updateHandler(plugin_self,CSP::OnUpdate, 0);
        EM::EventHandler jobStartHandler(plugin_self,CSP::JobStartedEvent, 0);
        EM::EventHandler jobCompletionHandler(plugin_self,CSP::JobCompletedEvent, 0);
        EM::EventHandler reportHandler(plugin_self,CSP::NewReportEvent, 0);
        //EM::registerTick(updateHandler,1);
        //EM::registerListener(EventType::TICK, updateHandler);
        EM::registerListener(EventType::REPORT, reportHandler);
        EM::registerListener(EventType::JOB_STARTED, jobStartHandler);
        EM::registerListener(EventType::JOB_COMPLETED, jobCompletionHandler);
        // manage designations to start off (first time building groups [very important])
        out.print("channel-safely: enabled!\n");
        refresh(out, nullptr); // scans and queues the next tick event
    } else if (!enable) {
        // don't need the groups if the plugin isn't going to be enabled
        CSP::ClearData();
        EM::unregisterAll(plugin_self);
        out.print("channel-safely: disabled!\n");
    }
    enabled = enable;
    return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event) {
    switch (event) {
        case SC_UNPAUSED:
            if (enabled && World::isFortressMode() && Maps::IsValid()) {
                // manage all designations on unpause
                CSP::UnpauseEvent(true);
            }
            break;
        case SC_MAP_LOADED:
            // cache the map size
            Maps::getSize(mapx, mapy, mapz);
            CSP::ClearData();
            ChannelManager::Get().build_groups(true);
            break;
        case SC_WORLD_UNLOADED:
        case SC_MAP_UNLOADED:
            CSP::ClearData();
            EM::unregisterAll(plugin_self);
            enabled = false;
            break;
        default:
            return DFHack::CR_OK;
    }
    return DFHack::CR_OK;
}

DFhackCExport command_result plugin_onupdate(color_ostream &out, state_change_event event) {
    return DFHack::CR_OK;
}

command_result channel_safely(color_ostream &out, std::vector<std::string> &parameters) {
    if (!Core::getInstance().isMapLoaded() || !World::IsSiteLoaded()) {
        out.printerr("Cannot run %s without a loaded fort.\n", plugin_name);
        return CR_FAILURE;
    }

    if (!parameters.empty()) {
        if (parameters[0] == "runonce") {
            CSP::UnpauseEvent(true);
            return DFHack::CR_OK;
        } else if (parameters[0] == "rebuild") {
            ChannelManager::Get().destroy_groups();
            ChannelManager::Get().build_groups(true);
        }
        if (parameters.size() >= 2 && parameters.size() <= 3) {
            bool state = false;
            bool set = false;
            if (parameters[0] == "enable") {
                state = true;
            } else if (parameters[0] == "disable") {
                state = false;
            } else if (parameters[0] == "set") {
                set = true;
            } else {
                return DFHack::CR_WRONG_USAGE;
            }
            try {
                if(parameters[1] == "monitoring"){
                    if (state != config.monitoring) {
                        config.monitoring = state;
                        // if this is a fresh start
                        if (state && !config.resurrect) {
                            // we need a fresh start
                            CSP::ClearData();
                        }
                    }
                } else if (parameters[1] == "risk-averse") {
                    config.riskaverse = state;
                } else if (parameters[1] == "require-vision") {
                    config.require_vision = state;
                } else if (parameters[1] == "insta-dig") {
                    config.insta_dig = state;
                    //config.insta_dig = false;
                } else if (parameters[1] == "resurrect") {
                    if (state != config.resurrect) {
                        config.resurrect = state;
                        // if this is a fresh start
                        if (state && !config.monitoring) {
                            // we need a fresh start
                            CSP::ClearData();
                        }
                    }
                } else if (parameters[1] == "refresh-freq" && set && parameters.size() == 3) {
                    config.refresh_freq = std::abs(std::stol(parameters[2]));
                /*} else if (parameters[1] == "monitor-freq" && set && parameters.size() == 3) {
                    config.monitor_freq = std::abs(std::stol(parameters[2]));*/
                } else if (parameters[1] == "watch-duration" && set && parameters.size() == 3) {
                    config.res_watch_duration = std::abs(std::stol(parameters[2]));
                } else if (parameters[1] == "ignore-threshold" && set && parameters.size() == 3) {
                    config.ignore_threshold = std::abs(std::stol(parameters[2]));
                } else if (parameters[1] == "fall-threshold" && set && parameters.size() == 3) {
                    uint8_t t = std::abs(std::stol(parameters[2]));
                    if (t > 0) {
                        config.fall_threshold = t;
                    } else {
                        out.printerr("fall-threshold must have a value greater than 0 or the plugin does a lot of nothing.\n");
                        return DFHack::CR_FAILURE;
                    }
                } else {
                    return DFHack::CR_WRONG_USAGE;
                }
            } catch (const std::exception &e) {
                out.printerr("%s\n", e.what());
                return DFHack::CR_FAILURE;
            }
        }
    } else {
        out.print("Channel-Safely is %s\n", enabled ? "ENABLED." : "DISABLED.");
        out.print(" FEATURES:\n");
        out.print("  %-20s\t%s\n", "risk-averse: ", config.riskaverse ? "on." : "off.");
        out.print("  %-20s\t%s\n", "monitoring: ", config.monitoring ? "on." : "off.");
        out.print("  %-20s\t%s\n", "require-vision: ", config.require_vision ? "on." : "off.");
        out.print("  %-20s\t%s\n", "insta-dig: ", config.insta_dig ? "on." : "off.");
        out.print("  %-20s\t%s\n", "resurrect: ", config.resurrect ? "on." : "off.");
        out.print(" SETTINGS:\n");
        out.print("  %-20s\t%" PRIi32 "\n", "refresh-freq: ", config.refresh_freq);
        //out.print("  %-20s\t%" PRIi32 "\n", "monitor-freq: ", config.monitor_freq);
        out.print("  %-20s\t%" PRIi32 "\n", "watch-duration: ", config.res_watch_duration);
        out.print("  %-20s\t%" PRIu8 "\n", "ignore-threshold: ", config.ignore_threshold);
        out.print("  %-20s\t%" PRIu8 "\n", "fall-threshold: ", config.fall_threshold);
    }
    CSP::SaveSettings();
    return DFHack::CR_OK;
}
