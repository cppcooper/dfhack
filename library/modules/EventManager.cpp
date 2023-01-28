#include "Core.h"
#include "Console.h"
#include "Debug.h"
#include "VTableInterpose.h"
#include "modules/Buildings.h"
#include "modules/Constructions.h"
#include "modules/EventManager.h"
#include "modules/Once.h"
#include "modules/Job.h"
#include "modules/Units.h"
#include "modules/World.h"

#include "df/announcement_type.h"
#include "df/building.h"
#include "df/construction.h"
#include "df/general_ref.h"
#include "df/general_ref_type.h"
#include "df/general_ref_unit_workerst.h"
#include "df/global_objects.h"
#include "df/historical_figure.h"
#include "df/interaction.h"
#include "df/item.h"
#include "df/item_actual.h"
#include "df/item_constructed.h"
#include "df/item_crafted.h"
#include "df/item_weaponst.h"
#include "df/job.h"
#include "df/job_list_link.h"
#include "df/report.h"
#include "df/plotinfost.h"
#include "df/unit.h"
#include "df/unit_flags1.h"
#include "df/unit_inventory_item.h"
#include "df/unit_report_type.h"
#include "df/unit_syndrome.h"
#include "df/unit_wound.h"
#include "df/world.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <array>

namespace DFHack {
    DBG_DECLARE(eventmanager, log, DebugCategory::LINFO);
}

using namespace std;
using namespace DFHack;
using namespace EventManager;
using namespace df::enums;

/*
 * TODO:
 *  error checking
 *  consider a typedef instead of a struct for EventHandler
 **/

static multimap<int32_t, EventHandler> tickQueue;

//TODO: consider unordered_map of pairs, or unordered_map of unordered_set, or whatever
static multimap<Plugin*, EventHandler> handlers[EventType::EVENT_MAX];
static int32_t eventLastTick[EventType::EVENT_MAX];

static const int32_t ticksPerYear = 403200;

void DFHack::EventManager::registerListener(EventType::EventType e, EventHandler handler, Plugin* plugin) {
    DEBUG(log).print("registering handler %p from plugin %s for event %d\n", handler.eventHandler, plugin->getName().c_str(), e);
    handlers[e].insert(pair<Plugin*, EventHandler>(plugin, handler));
}

int32_t DFHack::EventManager::registerTick(EventHandler handler, int32_t when, Plugin* plugin, bool absolute) {
    if ( !absolute ) {
        df::world* world = df::global::world;
        if ( world ) {
            when += world->frame_counter;
        } else {
            if ( Once::doOnce("EventManager registerTick unhonored absolute=false") )
                Core::getInstance().getConsole().print("EventManager::registerTick: warning! absolute flag=false not honored.\n");
        }
    }
    handler.freq = when;
    tickQueue.insert(pair<int32_t, EventHandler>(handler.freq, handler));
    DEBUG(log).print("registering handler %p from plugin %s for event TICK\n", handler.eventHandler, plugin->getName().c_str());
    handlers[EventType::TICK].insert(pair<Plugin*,EventHandler>(plugin,handler));
    return when;
}

static void removeFromTickQueue(EventHandler getRidOf) {
    for ( auto j = tickQueue.find(getRidOf.freq); j != tickQueue.end(); ) {
        if ( (*j).first > getRidOf.freq )
            break;
        if ( (*j).second != getRidOf ) {
            j++;
            continue;
        }
        j = tickQueue.erase(j);
    }
}

void DFHack::EventManager::unregister(EventType::EventType e, EventHandler handler, Plugin* plugin) {
    for ( auto i = handlers[e].find(plugin); i != handlers[e].end(); ) {
        if ( (*i).first != plugin )
            break;
        EventHandler &handle = (*i).second;
        if ( handle != handler ) {
            i++;
            continue;
        }
        DEBUG(log).print("unregistering handler %p from plugin %s for event %d\n", handler.eventHandler, plugin->getName().c_str(), e);
        i = handlers[e].erase(i);
        if ( e == EventType::TICK )
            removeFromTickQueue(handler);
    }
}

void DFHack::EventManager::unregisterAll(Plugin* plugin) {
    DEBUG(log).print("unregistering all handlers for plugin %s\n", plugin->getName().c_str());
    for ( auto i = handlers[EventType::TICK].find(plugin); i != handlers[EventType::TICK].end(); i++ ) {
        if ( (*i).first != plugin )
            break;

        removeFromTickQueue((*i).second);
    }
    for (auto &handler : handlers) {
        handler.erase(plugin);
    }
}

static void manageTickEvent(color_ostream& out);
static void manageJobInitiatedEvent(color_ostream& out);
static void manageJobStartedEvent(color_ostream& out);
static void manageJobCompletedEvent(color_ostream& out);
static void manageNewUnitActiveEvent(color_ostream& out);
static void manageUnitDeathEvent(color_ostream& out);
static void manageItemCreationEvent(color_ostream& out);
static void manageBuildingEvent(color_ostream& out);
static void manageConstructionEvent(color_ostream& out);
static void manageSyndromeEvent(color_ostream& out);
static void manageInvasionEvent(color_ostream& out);
static void manageEquipmentEvent(color_ostream& out);
static void manageReportEvent(color_ostream& out);
static void manageUnitAttackEvent(color_ostream& out);
static void manageUnloadEvent(color_ostream& out){};
static void manageInteractionEvent(color_ostream& out);

typedef void (*eventManager_t)(color_ostream&);

// integrate new events into this function, and no longer worry about syncing the enum list with the `eventManager` array
eventManager_t getManager(EventType::EventType t) {
    switch (t) {
        case EventType::TICK:
            return manageTickEvent;
        case EventType::JOB_INITIATED:
            return manageJobInitiatedEvent;
        case EventType::JOB_STARTED:
            return manageJobStartedEvent;
        case EventType::JOB_COMPLETED:
            return manageJobCompletedEvent;
        case EventType::UNIT_NEW_ACTIVE:
            return manageNewUnitActiveEvent;
        case EventType::UNIT_DEATH:
            return manageUnitDeathEvent;
        case EventType::ITEM_CREATED:
            return manageItemCreationEvent;
        case EventType::BUILDING:
            return manageBuildingEvent;
        case EventType::CONSTRUCTION:
            return manageConstructionEvent;
        case EventType::SYNDROME:
            return manageSyndromeEvent;
        case EventType::INVASION:
            return manageInvasionEvent;
        case EventType::INVENTORY_CHANGE:
            return manageEquipmentEvent;
        case EventType::REPORT:
            return manageReportEvent;
        case EventType::UNIT_ATTACK:
            return manageUnitAttackEvent;
        case EventType::UNLOAD:
            return manageUnloadEvent;
        case EventType::INTERACTION:
            return manageInteractionEvent;
        case EventType::EVENT_MAX:
            return nullptr;
            //default:
            //we don't do this... because then the compiler wouldn't error for missing cases in the enum
    }
    return nullptr;
}

std::array<eventManager_t,EventType::EVENT_MAX> compileManagerArray() {
    std::array<eventManager_t, EventType::EVENT_MAX> managers{};
    auto t = (EventType::EventType) 0;
    while (t < EventType::EVENT_MAX) {
        managers[t] = getManager(t);
        t = (EventType::EventType) int(t + 1);
    }
    return managers;
}

//job initiated
static unordered_set<int32_t> jobs;
static int32_t lastJobId = -1;

//job completed
static unordered_map<int32_t, df::job*> prevJobs;

//active units
static unordered_set<int32_t> activeUnits;

//unit death
static unordered_set<int32_t> livingUnits;
//static unordered_set<int32_t> livingUnits; // we're tracking the living so that when one dies we can simply remove it
static unordered_set<int32_t> deadUnits; // we scan and add new dead to the list

//item creation
static int32_t nextItem;
static std::unordered_set<int32_t> newItems;

//building
static int32_t nextBuilding;
static unordered_set<int32_t> buildings;

//construction
static unordered_set<df::construction> constructions;
static bool gameLoaded;

//syndrome
static int32_t lastSyndromeTime;
static std::unordered_set<SyndromeData> syndromes;

//invasion
static int32_t nextInvasion;
static std::unordered_set<int32_t> invasions;

//equipment change
//static unordered_map<int32_t, vector<df::unit_inventory_item> > equipmentLog;
static unordered_map<int32_t, vector<InventoryItem> > equipmentLog;

//report
static int32_t lastReport;

//unit attack
static int32_t lastReportUnitAttack;
static std::map<int32_t,std::vector<int32_t> > reportToRelevantUnits;
static int32_t reportToRelevantUnitsTime = -1;

//interaction
static int32_t lastReportInteraction;

static InteractionData getAttacker(color_ostream& out, df::report* attackEvent, df::unit* lastAttacker,
                                   df::report* defendEvent, const vector<df::unit*>& relevantUnits);
static std::vector<df::unit*> gatherRelevantUnits(color_ostream& out, df::report* r1, df::report* r2);

static int32_t getTime(){
    if(df::global::world) {
        return (*df::global::cur_year) * ticksPerYear + (*df::global::cur_year_tick);
    }
    return -1;
}

/** Move Map
 * scan: sendInvasionEvents
 *
 * scan_units: sendUnitNewActiveEvents
 * scan_units: sendUnitDeathEvents
 * scan_units: sendSyndromeEvents
 * scan_units: sendInventoryChangeEvents
 * scan_unit_inventory: sendInventoryChangeEvents
 * scan_unit_reports: updateReportToRelevantUnits
 *
 * scan_items: sendItemCreationEvents
 *
 * scan_jobs: sendJobInitiatedEvents
 * scan_jobs: sendJobStartedEvents
 * scan_jobs: sendJobCompletedEvents
 *
 * scan_buildings: sendBuildingEvents
 * scan_buildings: sendConstructionEvents
 *
 * scan_reports: sendReportEvents
 * scan_reports: sendInteractionEvents
 * scan_reports: sendUnitAttackEvents
 * parse_strike_report: sendUnitAttackEvents
 * */

// todo: move into "Cache.cpp" and create a cache module
class Scanner{
private:
    Scanner() = default;
    // current tick's data
    std::unordered_set<int32_t> valid_items;
    std::unordered_set<int32_t> valid_reports;
    std::unordered_set<int32_t> valid_buildings;
    std::unordered_map<int32_t, std::shared_ptr<df::job>> current_jobs;
    std::unordered_map<df::coord, df::construction*> valid_constructions;

    std::unordered_set<int32_t> living_units;
    std::unordered_set<int32_t> new_jobs;
    std::unordered_set<int32_t> started_jobs;
    std::unordered_set<int32_t> completed_jobs;

public:
    inline void scan(color_ostream& out, const int32_t &tick) {
        if (!df::global::world)
            return;
        scan_units(out, tick);
        scan_items(out, tick);
        scan_jobs(out, tick);
        scan_buildings(out, tick);
        scan_reports(out, tick); // depends on scanning units first

        // if there's a new invasion we'll add it
        if (!df::global::plotinfo)
            return;
        invasions.emplace(nextInvasion);
        nextInvasion = df::global::plotinfo->invasions.next_id;
    }
    inline void clear() {
        valid_units.clear();
        active_units.clear();
        valid_reports.clear();
        valid_buildings.clear();
    }
    static Scanner& get() {
        static Scanner instance;
        return instance;
    }

    /** todo:
     *   - check reports
     *   - check unit inv
     *   - check unit reports
     *   - check strike parsing
     *  done:
     *   - check units
     *   - check items
     *   - check jobs
     *   - check buildings
     * */
protected:
    void scan_units(color_ostream &out, const int32_t &tick) {
        int32_t current_time = getTime();

        // loop all units
        for (df::unit* unit: df::global::world->units.all) {
            int32_t id = unit->id;
            bool scan_unit = true;
            if (!Units::isAlive(unit)) {
                // it is dead
                if (living_units.count(id)) {
                    // it is not on the dead list
                    living_units.erase(id);
                    deadUnits.emplace(id);
                } else {
                    // it has been dead, so we won't scan it
                    scan_unit = false;
                }
            } else if (!living_units.count(id)) {
                // it is alive, and not on the living list yet
                living_units.emplace(id);
                deadUnits.erase(id);
            }
            if (scan_unit) {
                // also scan its inventory
                scan_unit_inventory(out, tick, unit);
                // scan the unit for reports
                scan_unit_reports(out, tick, unit);
                // scan for new syndromes
                int32_t idx = 0;
                for (const auto &syndrome: unit->syndromes.active) {
                    int32_t syndrome_start_time = syndrome->year * ticksPerYear + syndrome->year_time;
                    // emplace the syndrome if it started now or in the past
                    if (syndrome_start_time <= current_time) {
                        syndromes.emplace(unit->id, (int32_t) idx);
                    }
                    idx++;
                }
            }
        }
    }

    void scan_items(color_ostream &out, const int32_t &tick){
        std::unordered_set<int32_t> previous_items;
        // we swap the valid items to the previous items list and we clear the valid items list [to prune invalid items]
        previous_items.swap(valid_items);

        // loop all current items
        for (df::item* item : df::global::world->items.all) {
            auto id = item->id;
            valid_items.emplace(id); // we only care that the id is a valid reference

            // todo: break out into other events? (foreign/trader/spider_web/etc.)
            //invaders
            if (item->flags.bits.foreign)
                continue;
            //traders who bring back your items?
            if (item->flags.bits.trader)
                continue;
            //migrants
            if (item->flags.bits.owned)
                continue;
            //spider webs don't count
            if (item->flags.bits.spider_web)
                continue;

            // check if this item existed last tick
            if (!previous_items.count(id)) {
                // it didn't, so it must be new
                newItems.emplace(id);
            }
        }
    }

    void scan_jobs(color_ostream &out, const int32_t &tick) {
        if (!df::global::job_next_id)
            return;

        static std::unordered_map<int32_t, std::shared_ptr<df::job>> previous_jobs;
        current_jobs.clear();

        // loop current valid jobs
        for (df::job_list_link* link = df::global::world->jobs.list.next; link != nullptr; link = link->next) {
            if (df::job* job = link->item) {
                if (!job) continue;
                auto id = job->id;
                auto cp = std::shared_ptr<df::job>(Job::cloneJobStruct(job, true),
                                                   [](df::job* p) { Job::deleteJobStruct(p, true); });
                // cache the current job data
                current_jobs.emplace(id, cp);

                if (!new_jobs.count(id)) {
                    new_jobs.emplace(id);
                }
                if (Job::getWorker(job) && !started_jobs.count(id)) {
                    started_jobs.emplace(id);
                }
            }
        }
        for (const auto &map_pair : previous_jobs) {
            const int32_t &id = map_pair.first;
            const auto &job_from_last_tick = map_pair.second;

            // the timer needs to have been at 0 last tick
            if (job_from_last_tick->completion_timer != 0)
                continue;

            // check whether the job still exists
            if (current_jobs.count(id)) {
                // it exists, but it may be a repeat job
                if (!job_from_last_tick->flags.bits.repeat)
                    continue;
                const auto &job_from_this_tick = current_jobs[id];
                if (job_from_this_tick->completion_timer != -1)
                    continue;
                completed_jobs.emplace(id);
            } else {
                // no longer exists, so it recently finished or was cancelled
                if (job_from_last_tick->flags.bits.repeat)
                    continue; // the idea here is that a repeat job should still exist, so it must have been deleted?
                completed_jobs.emplace(id);
            }
        }
        previous_jobs.clear();
        previous_jobs = std::unordered_map<int32_t, std::shared_ptr<df::job>>(current_jobs.begin(), current_jobs.end());
    }

    void scan_buildings(color_ostream &out, const int32_t &tick) {
        std::unordered_set<int32_t> previous_buildings;
        std::unordered_map<df::coord, df::construction*> previous_constructions;
        previous_buildings.swap(valid_buildings); // = valid_buildings.clear()
        previous_constructions.swap(valid_constructions);

        // loop all current buildings
        for (auto &building : df::global::world->buildings.all) {
            auto id = building->id;
            valid_buildings.emplace(id);
            // check if it existed last tick
            if (!previous_buildings.count(id)) {
                // it's new, add it
                createdBuildings.emplace(tick, id);
            }
        }
        // update destroyed building list
        for (const auto &id : previous_buildings) {
            // check if it exists right now
            if (!valid_buildings.count(id)) {
                // it's gone now, add it
                destroyedBuildings.emplace(tick, id);
            }
        }
        // loop all current constructions
        for (auto &c: df::global::world->constructions) {
            // add construction if unique to position
            valid_constructions.emplace(c->pos, c);
            // check if it existed last tick
            if (!previous_constructions.count(c->pos)) {
                createdConstructions.emplace(tick, *c); // hashes based on c->pos (df::coord)
            }
        }
        // update destroyed constructions list
        for (auto &key_value: previous_constructions) {
            const auto &p = key_value.first;
            const auto &c = key_value.second;
            // check if it exists right now
            if (!valid_constructions.count(p)) {
                // it's gone now, add it
                destroyedConstructions.emplace(tick, *c);
            }
        }

        // clean up stale id's [createdBuildings]
        for (auto creation_iter = createdBuildings.begin(); creation_iter != createdBuildings.end();) {
            auto created_tick = creation_iter->first;
            auto id = creation_iter->second;
            // if the id has been destroyed we'll need to possibly erase it from createdBuildings
            auto destruction_iter = destroyedBuildings.find(id);
            if (destruction_iter != destroyedBuildings.end()) {
                auto destroyed_tick = destruction_iter->first;
                // remove if destroyed happened after created
                if (destroyed_tick > created_tick) {
                    creation_iter = createdBuildings.erase(creation_iter);
                    continue; // next
                }
            }
            ++creation_iter; // next
        }
        // clean up re-used id's [destroyed list](only keep latest)
        for (auto destruction_iter = destroyedBuildings.begin(); destruction_iter != destroyedBuildings.end();) {
            // todo: this loop may not serve a purpose, we should check if a destroyed building's id can be re-used..
            //  or if the building can be restored
            auto destroyed_tick = destruction_iter->first;
            auto id = destruction_iter->second;
            // if the id has been created we need to see if it's been created again
            auto creation_iter = createdBuildings.find(id);
            if (creation_iter != createdBuildings.end()){
                auto created_tick = creation_iter->first;
                // remove if created happened after destroyed
                if (created_tick > destroyed_tick) {
                    destruction_iter = destroyedBuildings.erase(destruction_iter);
                    continue; // next
                }
            }
            ++destruction_iter; // next
        }

        // constructions don't need cleaning up, they never go stale (they are POD) though their originals may be gone
    }

    void scan_reports(color_ostream &out, const int32_t &tick) {
        df::report* lastAttackEvent = nullptr;
        df::unit* lastAttacker = nullptr;
        bool ie_skip_next = false; // interaction event, skip next report
        unordered_map<int32_t, unordered_set<int32_t> > history;
        auto &reports = df::global::world->status.reports;
        size_t idx = -1;
        valid_reports.clear();

        // loop the global reports
        for (auto &report: df::global::world->status.reports) {
            ++idx; // too many `continue` to do at the end
            auto id = report->id;
            valid_reports.emplace(id);
            // emplace the report id for sendReportEvents
            newReports.emplace(tick, id);
            // check if the report is a continuation
            if (report->flags.bits.continuation)
                continue;
            bool is_actor = false;
            // parse event type
            switch (report->type) {
                case df::announcement_type::COMBAT_STRIKE_DETAILS:
                    parse_strike_report(out, tick, report);
                    break;
                case df::announcement_type::INTERACTION_ACTOR:
                    is_actor = true;
                case df::announcement_type::INTERACTION_TARGET: {
                    if (ie_skip_next) {
                        ie_skip_next = false;
                        // this report was processed as part of the last report
                        continue;
                    }
                    InteractionData data;
                    if (is_actor) {
                        // get units referencing the report
                        vector<df::unit*> relevant_units = gatherRelevantUnits(out, report, nullptr);
                        // todo: read getAttacker again, add comment
                        data = getAttacker(out, report, nullptr, nullptr, relevant_units);
                        lastAttackEvent = report;
                        lastAttacker = nullptr;
                        // since report is of an attack, we'll read the next report in the hopes it's the defender
                        if (idx + 1 < reports.size()) {
                            auto &next_report = reports[idx + 1];
                            // the report type needs to be INTERACTION_TARGET
                            if (next_report->type == df::announcement_type::INTERACTION_TARGET) {
                                relevant_units = gatherRelevantUnits(out, report, next_report);
                                InteractionData data2 = getAttacker(out,
                                                                    report,
                                                                    nullptr,
                                                                    next_report,
                                                                    relevant_units);
                                if (data.attacker == data2.attacker &&
                                    (data.defender == -1 || data.defender == data2.defender)) {
                                    data = data2;
                                    ie_skip_next = true;
                                }
                            }
                        }
                    } else {
                        vector<df::unit*> relevant_units = gatherRelevantUnits(out, lastAttackEvent, report);
                        data = getAttacker(out, lastAttackEvent, lastAttacker, report, relevant_units);
                    }
                    if (data.attacker < 0) {
                        continue;
                    }
                    if (history[data.attacker].count(data.defender))
                        continue;
                    history[data.attacker].emplace(data.defender);
                    lastAttacker = df::unit::find(data.attacker);
                    interactionEvents.emplace(tick, data);
                    break;
                }
                default:
                    continue;
            }
        }
        // delete bad id references
        for (auto iter = reportToRelevantUnits.begin(); iter != reportToRelevantUnits.end();) {
            // check for a valid reference
            if (!valid_reports.count(iter->first)) {
                // none found, so remove it
                iter = reportToRelevantUnits.erase(iter);
            } else {
                ++iter;
            }
        }
        // delete bad id references
        for (auto iter = newReports.begin(); iter != newReports.end();) {
            // check for a valid reference
            if (!valid_reports.count(iter->first)) {
                // none found, so remove it
                iter = newReports.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    void scan_unit_inventory(color_ostream &out, const int32_t &tick, df::unit* unit) {
        auto unitId = unit->id;
        static std::shared_ptr<InventoryItem> null_item(nullptr);
        static unordered_map<int32_t, unordered_map<int32_t, std::shared_ptr<InventoryItem>>> previous_inventories;
        unordered_map<int32_t, std::shared_ptr<InventoryItem>> &previous_inventory = previous_inventories[unitId]; // from last tick
        unordered_map<int32_t, std::shared_ptr<InventoryItem>> current_inventory; // from this tick

        // iterate current tick's inventory for unit (std::vector)
        for (auto unit_item: unit->inventory) {
            auto itemId = unit_item->item->id;
            std::shared_ptr<InventoryItem> inv_item(new InventoryItem(itemId, *unit_item));
            current_inventory.emplace(itemId, inv_item);

            // check if the item existed last tick
            if (!previous_inventory.count(itemId)) {
                // it didn't, so it must be new
                equipmentChanges.emplace(tick, InventoryChangeData(unitId, null_item, inv_item)); // allocate direct to shared_ptr
            } else {
                // it did, so we can compare for changes
                std::shared_ptr<InventoryItem> &inv_item_last_tick = previous_inventory.find(itemId)->second;
                df::unit_inventory_item &prevTick = inv_item_last_tick->item;
                df::unit_inventory_item &thisTick = *unit_item;
                // check whether something changed
                if (prevTick.mode != thisTick.mode ||
                    prevTick.body_part_id != thisTick.body_part_id ||
                    prevTick.wound_id != thisTick.wound_id) {
                    // something changed
                    equipmentChanges.emplace(tick, InventoryChangeData(unitId, inv_item_last_tick, inv_item));
                }
            }
        }
        //check for dropped items
        for (auto &key_value: previous_inventory) {
            std::shared_ptr<InventoryItem> &inv_item = key_value.second;
            // check if the unit still has the item
            if (current_inventory.count(inv_item->itemId)) {
                // it does not
                equipmentChanges.emplace(tick, InventoryChangeData(unitId, inv_item, null_item));
            }
        }

        // todo: switch to hashed search? ie. `std::unordered_set<item ids> valid_items`
        for (auto iter = equipmentChanges.begin(); iter != equipmentChanges.end();) {
            if (!valid_units.count(iter->second.unitId)
                || df::item::binsearch_index(df::global::world->items.all, iter->second.item_new->itemId, true) < 0
                || df::item::binsearch_index(df::global::world->items.all, iter->second.item_old->itemId, true) < 0) {
                // we have stale data
                iter = equipmentChanges.erase(iter);
            } else {
                ++iter;
            }
        }
        previous_inventory.swap(current_inventory);
    }

    void scan_unit_reports(color_ostream &out, const int32_t &tick, df::unit* unit) {
        int idx = 0;
        // loop unit's reports
        for (auto &log : unit->reports.log) {
            // we don't want unit_report_type::Sparring
            switch((unit_report_type::unit_report_type)idx){
                case unit_report_type::Sparring:
                    continue;
                case unit_report_type::Combat:
                case unit_report_type::Hunting:
                    for (auto &report_id: log) {
                        // add the unit onto a set of units belonging to the report `report_id`
                        reportToRelevantUnits[report_id].emplace(unit->id);
                    }
                    break;
            }
        }
        // clean up stale unit id's
        for (auto &key_value : reportToRelevantUnits){
            // we want to iterate the units related to various reports
            auto &units = key_value.second;
            for(auto iter = units.begin(); iter != units.end();) {
                // we check for the id in `all_units` to determine whether it's stale
                if (!valid_units.count(*iter)){
                    iter = units.erase(iter);
                } else {
                    ++iter;
                }
            }
        }
        // stale report id's are cleaned up on the scan_reports stack
    }

    void parse_strike_report(color_ostream &out, const int32_t &tick, df::report* report) {
        std::unordered_map<int32_t, std::unordered_map<int32_t, bool> > alreadyDone;
        // process the strike reports
        if (!report) {
            return; //TODO: error
        }
        // prepare our report text
        std::string reportStr = report->text;
        for (int32_t id2 = report->id + 1;; id2++) {
            df::report* report2 = df::report::find(id2);
            if (!report2)
                break;
            if (report2->type != df::announcement_type::COMBAT_STRIKE_DETAILS)
                break;
            if (!report2->flags.bits.continuation)
                break;
            reportStr += report2->text;
        }

        // get relevant units
        std::unordered_set<int32_t> &relevantUnits = reportToRelevantUnits[report->id];
        if (relevantUnits.size() != 2) {
            return;
        }
        auto iter = relevantUnits.begin();
        // todo: did the order of unit1 vs unit2 matter?
        df::unit* unit1 = df::unit::find(*iter);
        df::unit* unit2 = df::unit::find(*++iter); //todo: we don't check for an error state? (todo: look at logging utils)
        auto getWound = [](df::unit* defender, df::unit* attacker) {
            for (auto wound: defender->body.wounds) {
                // todo/ we might be missing N wounds, we just return the first one we see (albeit N is probably 1 at most)
                if (wound->age <= 1 && wound->attacker_unit_id == attacker->id) {
                    return wound;
                }
            }
            return (df::unit_wound*) nullptr;
        };

        // get the first wound of the first caused by the second
        df::unit_wound* unit1_wound = getWound(unit1, unit2);
        df::unit_wound* unit2_wound = getWound(unit2, unit1);


        // our data, we use it multiple times (pod copy)
        UnitAttackData data{};
        data.report_id = report->id;
        // honestly, I don't know why we are checking these things and constructing these data arrangements (todo: lookup git blame 0.47.05-r5#L980-L1007)
        if (unit1_wound && !alreadyDone[unit1->id][unit2->id]) {
            // emplace a copy of this data, in attackEvents
            data.attacker = unit1->id;
            data.defender = unit2->id;
            data.wound = unit1_wound->id;
            alreadyDone[data.attacker][data.defender] = true;
            attackEvents.emplace(tick, data);
            // we were sending events(0.47.05-r5), now we're just storing; see: https://github.com/DFHack/dfhack/blob/585888c2d36593882f756c5f5de459ec6b42b9ab/library/modules/EventManager.cpp#L980-L1007
        }
        if (unit2_wound && !alreadyDone[unit1->id][unit2->id]) {
            // emplace a copy of this data, in attackEvents
            data.attacker = unit2->id;
            data.defender = unit1->id;
            data.wound = unit2_wound->id;
            alreadyDone[data.attacker][data.defender] = true;
            attackEvents.emplace(tick, data);
        }
        if (Units::isKilled(unit1)) {
            // emplace a copy of this data, in attackEvents
            data.attacker = unit2->id;
            data.defender = unit1->id;
            data.wound = -1;
            alreadyDone[data.attacker][data.defender] = true;
            attackEvents.emplace(tick, data);
        }
        if (Units::isKilled(unit2)) {
            // emplace a copy of this data, in attackEvents
            data.attacker = unit1->id;
            data.defender = unit2->id;
            data.wound = -1;
            alreadyDone[data.attacker][data.defender] = true;
            attackEvents.emplace(tick, data);
        }
        if (!unit1_wound && !unit2_wound) {
            //if ( unit1->flags1.bits.inactive || unit2->flags1.bits.inactive )
            //    continue;
            if (reportStr.find("severed part"))
                return;
            if (Once::doOnce("EventManager neither wound")) {
                out.print("%s, %d: neither wound: %s\n", __FILE__, __LINE__, reportStr.c_str());
            }
        }
    }
};


void DFHack::EventManager::onStateChange(color_ostream& out, state_change_event event) {
    static bool doOnce = false;
//    const string eventNames[] = {"world loaded", "world unloaded", "map loaded", "map unloaded", "viewscreen changed", "core initialized", "begin unload", "paused", "unpaused"};
//    out.print("%s,%d: onStateChange %d: \"%s\"\n", __FILE__, __LINE__, (int32_t)event, eventNames[event].c_str());
    if ( !doOnce ) {
        //TODO: put this somewhere else
        doOnce = true;
        EventHandler buildingHandler(Buildings::updateBuildings, 100);
        DFHack::EventManager::registerListener(EventType::BUILDING, buildingHandler, nullptr);
        //out.print("Registered listeners.\n %d", __LINE__);
    }
    if ( event == DFHack::SC_MAP_UNLOADED ) {
        lastJobId = -1;
        for (auto &prevJob : prevJobs) {
            Job::deleteJobStruct(prevJob.second, true);
        }
        prevJobs.clear();
        tickQueue.clear();
        livingUnits.clear();
        buildings.clear();
        constructions.clear();
        equipmentLog.clear();
        activeUnits.clear();

        Buildings::clearBuildings(out);
        lastReport = -1;
        lastReportUnitAttack = -1;
        gameLoaded = false;

        multimap<Plugin*,EventHandler> copy(handlers[EventType::UNLOAD].begin(), handlers[EventType::UNLOAD].end());
        for (auto &key_value : copy) {
            DEBUG(log,out).print("calling handler for map unloaded state change event\n");
            key_value.second.eventHandler(out, nullptr);
        }
    } else if ( event == DFHack::SC_MAP_LOADED ) {
        /*
        int32_t tick = df::global::world->frame_counter;
        multimap<int32_t,EventHandler> newTickQueue;
        for ( auto i = tickQueue.begin(); i != tickQueue.end(); i++ )
            newTickQueue.insert(pair<int32_t,EventHandler>(tick+(*i).first, (*i).second));
        tickQueue.clear();
        tickQueue.insert(newTickQueue.begin(), newTickQueue.end());
        //out.print("%s,%d: on load, frame_counter = %d\n", __FILE__, __LINE__, tick);
        */
        //tickQueue.clear();
        if (!df::global::item_next_id)
            return;
        if (!df::global::building_next_id)
            return;
        if (!df::global::job_next_id)
            return;
        if (!df::global::plotinfo)
            return;
        if (!df::global::world)
            return;

        nextItem = *df::global::item_next_id;
        nextBuilding = *df::global::building_next_id;
        nextInvasion = df::global::plotinfo->invasions.next_id;
        lastJobId = -1 + *df::global::job_next_id;

        constructions.clear();
        for (auto c : df::global::world->constructions) {
            if ( !c ) {
                if ( Once::doOnce("EventManager.onLoad null constr") ) {
                    out.print("EventManager.onLoad: null construction.\n");
                }
                continue;
            }
            if (c->pos == df::coord() ) {
                if ( Once::doOnce("EventManager.onLoad null position of construction.\n") )
                    out.print("EventManager.onLoad null position of construction.\n");
                continue;
            }
            constructions.emplace(*c);
        }
        for (auto b : df::global::world->buildings.all) {
            Buildings::updateBuildings(out, (void*)intptr_t(b->id));
            buildings.insert(b->id);
        }
        lastSyndromeTime = -1;
        for (auto unit : df::global::world->units.all) {
            if (Units::isActive(unit)) {
                activeUnits.emplace(unit->id);
            }
            for (auto syndrome : unit->syndromes.active) {
                int32_t startTime = syndrome->year*ticksPerYear + syndrome->year_time;
                if ( startTime > lastSyndromeTime )
                    lastSyndromeTime = startTime;
            }
        }
        lastReport = -1;
        if ( !df::global::world->status.reports.empty() ) {
            lastReport = df::global::world->status.reports[df::global::world->status.reports.size()-1]->id;
        }
        lastReportUnitAttack = -1;
        lastReportInteraction = -1;
        reportToRelevantUnitsTime = -1;
        reportToRelevantUnits.clear();
        for (int &last_tick : eventLastTick) {
            last_tick = -1;//-1000000;
        }
        for (auto unit : df::global::world->history.figures) {
            if ( unit->id < 0 && unit->name.language < 0 )
                unit->name.language = 0;
        }

        gameLoaded = true;
    }
}

void DFHack::EventManager::manageEvents(color_ostream& out) {
    static const std::array<eventManager_t, EventType::EVENT_MAX> eventManager = compileManagerArray();
    if ( !gameLoaded ) {
        return;
    }
    if (!df::global::world)
        return;

    CoreSuspender suspender;

    int32_t tick = df::global::world->frame_counter;
    TRACE(log,out).print("processing events at tick %d\n", tick);

    for ( size_t a = 0; a < EventType::EVENT_MAX; a++ ) {
        if ( handlers[a].empty() )
            continue;
        int32_t eventFrequency = -100;
        if ( a != EventType::TICK )
            for (auto &key_value : handlers[a]) {
                EventHandler &handle = key_value.second;
                if (handle.freq < eventFrequency || eventFrequency == -100 )
                    eventFrequency = handle.freq;
            }
        else eventFrequency = 1;

        if ( tick >= eventLastTick[a] && tick - eventLastTick[a] < eventFrequency )
            continue;

        eventManager[a](out);
        eventLastTick[a] = tick;
    }
}

static void manageTickEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    unordered_set<EventHandler> toRemove;
    int32_t tick = df::global::world->frame_counter;
    while ( !tickQueue.empty() ) {
        if ( tick < (*tickQueue.begin()).first )
            break;
        EventHandler &handle = (*tickQueue.begin()).second;
        tickQueue.erase(tickQueue.begin());
        DEBUG(log,out).print("calling handler for tick event\n");
        handle.eventHandler(out, (void*)intptr_t(tick));
        toRemove.insert(handle);
    }
    if ( toRemove.empty() )
        return;
    for ( auto a = handlers[EventType::TICK].begin(); a != handlers[EventType::TICK].end(); ) {
        EventHandler &handle = (*a).second;
        if ( toRemove.find(handle) == toRemove.end() ) {
            a++;
            continue;
        }
        a = handlers[EventType::TICK].erase(a);
        toRemove.erase(handle);
        if ( toRemove.empty() )
            break;
    }
}

static void manageJobInitiatedEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    if (!df::global::job_next_id)
        return;
    if ( lastJobId == -1 ) {
        lastJobId = *df::global::job_next_id - 1;
        return;
    }

    if ( lastJobId+1 == *df::global::job_next_id ) {
        return; //no new jobs
    }
    multimap<Plugin*,EventHandler> copy(handlers[EventType::JOB_INITIATED].begin(), handlers[EventType::JOB_INITIATED].end());

    for ( df::job_list_link* link = &df::global::world->jobs.list; link != nullptr; link = link->next ) {
        if ( link->item == nullptr )
            continue;
        if ( link->item->id <= lastJobId )
            continue;
        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for job initiated event\n");
            handle.eventHandler(out, (void*)link->item);
        }
    }

    lastJobId = *df::global::job_next_id - 1;
}

static void manageJobStartedEvent(color_ostream& out) {
    if (!df::global::world)
        return;

    static unordered_set<int32_t> startedJobs;

    // iterate event handler callbacks
    multimap<Plugin*, EventHandler> copy(handlers[EventType::JOB_STARTED].begin(), handlers[EventType::JOB_STARTED].end());
    for (df::job_list_link* link = df::global::world->jobs.list.next; link != nullptr; link = link->next) {
        df::job* job = link->item;
        if (job && Job::getWorker(job) && !startedJobs.count(job->id)) {
            startedJobs.emplace(job->id);
            for (auto &key_value : copy) {
                auto &handler = key_value.second;
                // the jobs must have a worker to start
                DEBUG(log,out).print("calling handler for job started event\n");
                handler.eventHandler(out, job);
            }
        }
    }
}

//helper function for manageJobCompletedEvent
//static int32_t getWorkerID(df::job* job) {
//    auto ref = findRef(job->general_refs, general_ref_type::UNIT_WORKER);
//    return ref ? ref->getID() : -1;
//}

/*
TODO: consider checking item creation / experience gain just in case
*/
static void manageJobCompletedEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    int32_t tick0 = eventLastTick[EventType::JOB_COMPLETED];
    int32_t tick1 = df::global::world->frame_counter;

    multimap<Plugin*,EventHandler> copy(handlers[EventType::JOB_COMPLETED].begin(), handlers[EventType::JOB_COMPLETED].end());
    map<int32_t, df::job*> nowJobs;
    for ( df::job_list_link* link = &df::global::world->jobs.list; link != nullptr; link = link->next ) {
        if ( link->item == nullptr )
            continue;
        nowJobs[link->item->id] = link->item;
    }

#if 0
    //testing info on job initiation/completion
    //newly allocated jobs
    for ( auto j = nowJobs.begin(); j != nowJobs.end(); j++ ) {
        if ( prevJobs.find((*j).first) != prevJobs.end() )
            continue;

        df::job& job1 = *(*j).second;
        out.print("new job\n"
            "  location         : 0x%X\n"
            "  id               : %d\n"
            "  type             : %d %s\n"
            "  working          : %d\n"
            "  completion_timer : %d\n"
            "  workerID         : %d\n"
            "  time             : %d -> %d\n"
            "\n", job1.list_link->item, job1.id, job1.job_type, ENUM_ATTR(job_type, caption, job1.job_type), job1.flags.bits.working, job1.completion_timer, getWorkerID(&job1), tick0, tick1);
    }
    for ( auto i = prevJobs.begin(); i != prevJobs.end(); i++ ) {
        df::job& job0 = *(*i).second;
        auto j = nowJobs.find((*i).first);
        if ( j == nowJobs.end() ) {
            out.print("job deallocated\n"
                "  location         : 0x%X\n"
                "  id               : %d\n"
                "  type             : %d %s\n"
                "  working          : %d\n"
                "  completion_timer : %d\n"
                "  workerID         : %d\n"
                "  time             : %d -> %d\n"
                ,job0.list_link == NULL ? 0 : job0.list_link->item, job0.id, job0.job_type, ENUM_ATTR(job_type, caption, job0.job_type), job0.flags.bits.working, job0.completion_timer, getWorkerID(&job0), tick0, tick1);
            continue;
        }
        df::job& job1 = *(*j).second;

        if ( job0.flags.bits.working == job1.flags.bits.working &&
               (job0.completion_timer == job1.completion_timer || (job1.completion_timer > 0 && job0.completion_timer-1 == job1.completion_timer)) &&
               getWorkerID(&job0) == getWorkerID(&job1) )
            continue;

        out.print("job change\n"
            "  location         : 0x%X -> 0x%X\n"
            "  id               : %d -> %d\n"
            "  type             : %d -> %d\n"
            "  type             : %s -> %s\n"
            "  working          : %d -> %d\n"
            "  completion timer : %d -> %d\n"
            "  workerID         : %d -> %d\n"
            "  time             : %d -> %d\n"
            "\n",
            job0.list_link->item, job1.list_link->item,
            job0.id, job1.id,
            job0.job_type, job1.job_type,
            ENUM_ATTR(job_type, caption, job0.job_type), ENUM_ATTR(job_type, caption, job1.job_type),
            job0.flags.bits.working, job1.flags.bits.working,
            job0.completion_timer, job1.completion_timer,
            getWorkerID(&job0), getWorkerID(&job1),
            tick0, tick1
        );
    }
#endif

    for (auto &prevJob : prevJobs) {
        //if it happened within a tick, must have been cancelled by the user or a plugin: not completed
        if ( tick1 <= tick0 )
            continue;

        if ( nowJobs.find(prevJob.first) != nowJobs.end() ) {
            //could have just finished if it's a repeat job
            df::job& job0 = *prevJob.second;
            if ( !job0.flags.bits.repeat )
                continue;
            df::job& job1 = *nowJobs[prevJob.first];
            if ( job0.completion_timer != 0 )
                continue;
            if ( job1.completion_timer != -1 )
                continue;

            //still false positive if cancelled at EXACTLY the right time, but experiments show this doesn't happen
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for repeated job completed event\n");
                handle.eventHandler(out, (void*)&job0);
            }
            continue;
        }

        //recently finished or cancelled job
        df::job& job0 = *prevJob.second;
        if ( job0.flags.bits.repeat || job0.completion_timer != 0 )
            continue;

        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for job completed event\n");
            handle.eventHandler(out, (void*)&job0);
        }
    }

    //erase old jobs, copy over possibly altered jobs
    for (auto &prevJob : prevJobs) {
        Job::deleteJobStruct(prevJob.second, true);
    }
    prevJobs.clear();

    //create new jobs
    for (auto &nowJob : nowJobs) {
        /*map<int32_t, df::job*>::iterator i = prevJobs.find((*j).first);
        if ( i != prevJobs.end() ) {
            continue;
        }*/

        df::job* newJob = Job::cloneJobStruct(nowJob.second, true);
        prevJobs[newJob->id] = newJob;
    }
}

static void manageNewUnitActiveEvent(color_ostream& out) {
    if (!df::global::world)
        return;

    multimap<Plugin*,EventHandler> copy(handlers[EventType::UNIT_NEW_ACTIVE].begin(), handlers[EventType::UNIT_NEW_ACTIVE].end());
    // iterate event handler callbacks
    for (auto &key_value : copy) {
        auto &handler = key_value.second;
        for (df::unit* unit : df::global::world->units.active) {
            int32_t id = unit->id;
            if (!activeUnits.count(id)) {
                activeUnits.emplace(id);
                DEBUG(log,out).print("calling handler for new unit event\n");
                handler.eventHandler(out, (void*) intptr_t(id)); // intptr_t() avoids cast from smaller type warning
            }
        }
    }
}


static void manageUnitDeathEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::UNIT_DEATH].begin(), handlers[EventType::UNIT_DEATH].end());
    for (auto unit : df::global::world->units.all) {
        //if ( unit->counters.death_id == -1 ) {
        if ( Units::isActive(unit) ) {
            livingUnits.insert(unit->id);
            continue;
        }
        //dead: if dead since last check, trigger events
        if ( livingUnits.find(unit->id) == livingUnits.end() )
            continue;

        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for unit death event\n");
            handle.eventHandler(out, (void*)intptr_t(unit->id));
        }
        livingUnits.erase(unit->id);
    }
}

static void manageItemCreationEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    if (!df::global::item_next_id)
        return;
    if ( nextItem >= *df::global::item_next_id ) {
        return;
    }

    multimap<Plugin*,EventHandler> copy(handlers[EventType::ITEM_CREATED].begin(), handlers[EventType::ITEM_CREATED].end());
    size_t index = df::item::binsearch_index(df::global::world->items.all, nextItem, false);
    if ( index != 0 ) index--;
    for ( size_t a = index; a < df::global::world->items.all.size(); a++ ) {
        df::item* item = df::global::world->items.all[a];
        //already processed
        if ( item->id < nextItem )
            continue;
        //invaders
        if ( item->flags.bits.foreign )
            continue;
        //traders who bring back your items?
        if ( item->flags.bits.trader )
            continue;
        //migrants
        if ( item->flags.bits.owned )
            continue;
        //spider webs don't count
        if ( item->flags.bits.spider_web )
            continue;
        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for item created event\n");
            handle.eventHandler(out, (void*)intptr_t(item->id));
        }
    }
    nextItem = *df::global::item_next_id;
}

static void manageBuildingEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    if (!df::global::building_next_id)
        return;
    /*
     * TODO: could be faster
     * consider looking at jobs: building creation / destruction
     **/
    multimap<Plugin*,EventHandler> copy(handlers[EventType::BUILDING].begin(), handlers[EventType::BUILDING].end());
    //first alert people about new buildings
    for ( int32_t a = nextBuilding; a < *df::global::building_next_id; a++ ) {
        int32_t index = df::building::binsearch_index(df::global::world->buildings.all, a);
        if ( index == -1 ) {
            //out.print("%s, line %d: Couldn't find new building with id %d.\n", __FILE__, __LINE__, a);
            //the tricky thing is that when the game first starts, it's ok to skip buildings, but otherwise, if you skip buildings, something is probably wrong. TODO: make this smarter
            continue;
        }
        buildings.insert(a);
        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for created building event\n");
            handle.eventHandler(out, (void*)intptr_t(a));
        }
    }
    nextBuilding = *df::global::building_next_id;

    //now alert people about destroyed buildings
    for ( auto a = buildings.begin(); a != buildings.end(); ) {
        int32_t id = *a;
        int32_t index = df::building::binsearch_index(df::global::world->buildings.all,id);
        if ( index != -1 ) {
            a++;
            continue;
        }

        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for destroyed building event\n");
            handle.eventHandler(out, (void*)intptr_t(id));
        }
        a = buildings.erase(a);
    }
}

static void manageConstructionEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    //unordered_set<df::construction*> constructionsNow(df::global::world->constructions.begin(), df::global::world->constructions.end());

    multimap<Plugin*, EventHandler> copy(handlers[EventType::CONSTRUCTION].begin(), handlers[EventType::CONSTRUCTION].end());
    // find & send construction removals
    for (auto iter = constructions.begin(); iter != constructions.end();) {
        auto &construction = *iter;
        // if we can't find it, it was removed
        if (df::construction::find(construction.pos) != nullptr) {
            ++iter;
            continue;
        }
        // send construction to handlers, because it was removed
        for (const auto &key_value: copy) {
            EventHandler handle = key_value.second;
            DEBUG(log,out).print("calling handler for destroyed construction event\n");
            handle.eventHandler(out, (void*) &construction);
        }
        // erase from existent constructions
        iter = constructions.erase(iter);
    }

    // find & send construction additions
    for (auto c: df::global::world->constructions) {
        auto &construction = *c;
        // add construction to constructions, if it isn't already present
        if (constructions.emplace(construction).second) {
            // send construction to handlers, because it is new
            for (const auto &key_value: copy) {
                EventHandler handle = key_value.second;
                DEBUG(log,out).print("calling handler for created construction event\n");
                handle.eventHandler(out, (void*) &construction);
            }
        }
    }
}

static void manageSyndromeEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::SYNDROME].begin(), handlers[EventType::SYNDROME].end());
    int32_t highestTime = -1;
    for (auto unit : df::global::world->units.all) {

/*
        if ( unit->flags1.bits.inactive )
            continue;
*/
        for ( size_t b = 0; b < unit->syndromes.active.size(); b++ ) {
            df::unit_syndrome* syndrome = unit->syndromes.active[b];
            int32_t startTime = syndrome->year*ticksPerYear + syndrome->year_time;
            if ( startTime > highestTime )
                highestTime = startTime;
            if ( startTime <= lastSyndromeTime )
                continue;

            SyndromeData data(unit->id, b);
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for syndrome event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }
    }
    lastSyndromeTime = highestTime;
}

static void manageInvasionEvent(color_ostream& out) {
    if (!df::global::plotinfo)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::INVASION].begin(), handlers[EventType::INVASION].end());

    if ( df::global::plotinfo->invasions.next_id <= nextInvasion )
        return;
    nextInvasion = df::global::plotinfo->invasions.next_id;

    for (auto &key_value : copy) {
        EventHandler &handle = key_value.second;
        DEBUG(log,out).print("calling handler for invasion event\n");
        handle.eventHandler(out, (void*)intptr_t(nextInvasion-1));
    }
}

static void manageEquipmentEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::INVENTORY_CHANGE].begin(), handlers[EventType::INVENTORY_CHANGE].end());

    unordered_map<int32_t, InventoryItem> itemIdToInventoryItem;
    unordered_set<int32_t> currentlyEquipped;
    for (auto unit : df::global::world->units.all) {
        itemIdToInventoryItem.clear();
        currentlyEquipped.clear();
        /*if ( unit->flags1.bits.inactive )
            continue;
        */

        auto oldEquipment = equipmentLog.find(unit->id);
        bool hadEquipment = oldEquipment != equipmentLog.end();
        vector<InventoryItem>* temp;
        if ( hadEquipment ) {
            temp = &((*oldEquipment).second);
        } else {
            temp = new vector<InventoryItem>;
        }
        //vector<InventoryItem>& v = (*oldEquipment).second;
        vector<InventoryItem>& v = *temp;
        for (auto & i : v) {
            itemIdToInventoryItem[i.itemId] = i;
        }
        for ( size_t b = 0; b < unit->inventory.size(); b++ ) {
            df::unit_inventory_item* dfitem_new = unit->inventory[b];
            currentlyEquipped.insert(dfitem_new->item->id);
            InventoryItem item_new(dfitem_new->item->id, *dfitem_new);
            auto c = itemIdToInventoryItem.find(dfitem_new->item->id);
            if ( c == itemIdToInventoryItem.end() ) {
                //new item equipped (probably just picked up)
                InventoryChangeData data(unit->id, nullptr, &item_new);
                for (auto &key_value : copy) {
                    EventHandler &handle = key_value.second;
                    DEBUG(log,out).print("calling handler for new item equipped inventory change event\n");
                    handle.eventHandler(out, (void*)&data);
                }
                continue;
            }
            InventoryItem item_old = (*c).second;

            df::unit_inventory_item& item0 = item_old.item;
            df::unit_inventory_item& item1 = item_new.item;
            if ( item0.mode == item1.mode && item0.body_part_id == item1.body_part_id && item0.wound_id == item1.wound_id )
                continue;
            //some sort of change in how it's equipped

            InventoryChangeData data(unit->id, &item_old, &item_new);
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for inventory change event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }
        //check for dropped items
        for (auto i : v) {
            if ( currentlyEquipped.find(i.itemId) != currentlyEquipped.end() )
                continue;
            //TODO: delete ptr if invalid
            InventoryChangeData data(unit->id, &i, nullptr);
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for dropped item inventory change event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }
        if ( !hadEquipment )
            delete temp;

        //update equipment
        vector<InventoryItem>& equipment = equipmentLog[unit->id];
        equipment.clear();
        for (auto dfitem : unit->inventory) {
            InventoryItem item(dfitem->item->id, *dfitem);
            equipment.push_back(item);
        }
    }
}

static void updateReportToRelevantUnits() {
    if (!df::global::world)
        return;
    if ( df::global::world->frame_counter <= reportToRelevantUnitsTime )
        return;
    reportToRelevantUnitsTime = df::global::world->frame_counter;

    for (auto unit : df::global::world->units.all) {
        for ( int16_t b = df::enum_traits<df::unit_report_type>::first_item_value; b <= df::enum_traits<df::unit_report_type>::last_item_value; b++ ) {
            if ( b == df::unit_report_type::Sparring )
                continue;
            for ( size_t c = 0; c < unit->reports.log[b].size(); c++ ) {
                int32_t report = unit->reports.log[b][c];
                if ( std::find(reportToRelevantUnits[report].begin(), reportToRelevantUnits[report].end(), unit->id) != reportToRelevantUnits[report].end() )
                    continue;
                reportToRelevantUnits[unit->reports.log[b][c]].push_back(unit->id);
            }
        }
    }
}

static void manageReportEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::REPORT].begin(), handlers[EventType::REPORT].end());
    std::vector<df::report*>& reports = df::global::world->status.reports;
    size_t idx = df::report::binsearch_index(reports, lastReport, false);
    // returns the index to the key equal to or greater than the key provided
    while (idx < reports.size() && reports[idx]->id <= lastReport) {
        idx++;
    }
    // returns the index to the key equal to or greater than the key provided

    for ( ; idx < reports.size(); idx++ ) {
        df::report* report = reports[idx];
        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for report event\n");
            handle.eventHandler(out, (void*)intptr_t(report->id));
        }
        lastReport = report->id;
    }
}

static df::unit_wound* getWound(df::unit* attacker, df::unit* defender) {
    for (auto wound : defender->body.wounds) {
        if ( wound->age <= 1 && wound->attacker_unit_id == attacker->id ) {
            return wound;
        }
    }
    return nullptr;
}

static void manageUnitAttackEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::UNIT_ATTACK].begin(), handlers[EventType::UNIT_ATTACK].end());
    std::vector<df::report*>& reports = df::global::world->status.reports;
    size_t idx = df::report::binsearch_index(reports, lastReportUnitAttack, false);
    // returns the index to the key equal to or greater than the key provided
    idx = reports[idx]->id == lastReportUnitAttack ? idx + 1 : idx; // we need the index after (where the new stuff is)

    std::set<int32_t> strikeReports;
    for ( ; idx < reports.size(); idx++ ) {
        df::report* report = reports[idx];
        lastReportUnitAttack = report->id;
        if ( report->flags.bits.continuation )
            continue;
        df::announcement_type type = report->type;
        if ( type == df::announcement_type::COMBAT_STRIKE_DETAILS ) {
            strikeReports.insert(report->id);
        }
    }

    if ( strikeReports.empty() )
        return;
    updateReportToRelevantUnits();
    map<int32_t, map<int32_t, int32_t> > alreadyDone;
    for (int reportId : strikeReports) {
        df::report* report = df::report::find(reportId);
        if ( !report )
            continue; //TODO: error
        std::string reportStr = report->text;
        for ( int32_t b = reportId+1; ; b++ ) {
            df::report* report2 = df::report::find(b);
            if ( !report2 )
                break;
            if ( report2->type != df::announcement_type::COMBAT_STRIKE_DETAILS )
                break;
            if ( !report2->flags.bits.continuation )
                break;
            reportStr += report2->text;
        }

        std::vector<int32_t>& relevantUnits = reportToRelevantUnits[report->id];
        if ( relevantUnits.size() != 2 ) {
            continue;
        }

        df::unit* unit1 = df::unit::find(relevantUnits[0]);
        df::unit* unit2 = df::unit::find(relevantUnits[1]);

        df::unit_wound* wound1 = getWound(unit1,unit2);
        df::unit_wound* wound2 = getWound(unit2,unit1);

        UnitAttackData data{};
        data.report_id = report->id;
        if ( wound1 && !alreadyDone[unit1->id][unit2->id] ) {
            data.attacker = unit1->id;
            data.defender = unit2->id;
            data.wound = wound1->id;

            alreadyDone[data.attacker][data.defender] = 1;
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for unit1 attack unit attack event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }

        if ( wound2 && !alreadyDone[unit1->id][unit2->id] ) {
            data.attacker = unit2->id;
            data.defender = unit1->id;
            data.wound = wound2->id;

            alreadyDone[data.attacker][data.defender] = 1;
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for unit2 attack unit attack event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }

        if ( Units::isKilled(unit1) ) {
            data.attacker = unit2->id;
            data.defender = unit1->id;
            data.wound = -1;
            alreadyDone[data.attacker][data.defender] = 1;
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for unit1 killed unit attack event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }

        if ( Units::isKilled(unit2) ) {
            data.attacker = unit1->id;
            data.defender = unit2->id;
            data.wound = -1;
            alreadyDone[data.attacker][data.defender] = 1;
            for (auto &key_value : copy) {
                EventHandler &handle = key_value.second;
                DEBUG(log,out).print("calling handler for unit2 killed unit attack event\n");
                handle.eventHandler(out, (void*)&data);
            }
        }

        if ( !wound1 && !wound2 ) {
            //if ( unit1->flags1.bits.inactive || unit2->flags1.bits.inactive )
            //    continue;
            if ( reportStr.find("severed part") )
                continue;
            if ( Once::doOnce("EventManager neither wound") ) {
                out.print("%s, %d: neither wound: %s\n", __FILE__, __LINE__, reportStr.c_str());
            }
        }
    }
}

static std::string getVerb(df::unit* unit, const std::string &reportStr) {
    std::string result(reportStr);
    std::string name = unit->name.first_name + " ";
    bool match = strncmp(result.c_str(), name.c_str(), name.length()) == 0;
    if ( match ) {
        result = result.substr(name.length());
        result = result.substr(0,result.length()-1);
        return result;
    }
    //use profession name
    name = "The " + Units::getProfessionName(unit) + " ";
    match = strncmp(result.c_str(), name.c_str(), name.length()) == 0;
    if ( match ) {
        result = result.substr(name.length());
        result = result.substr(0,result.length()-1);
        return result;
    }

    if ( unit->id != 0 ) {
        return "";
    }

    std::string you = "You ";
    match = strncmp(result.c_str(), name.c_str(), name.length()) == 0;
    if ( match ) {
        result = result.substr(name.length());
        result = result.substr(0,result.length()-1);
        return result;
    }
    return "";
}

static InteractionData getAttacker(color_ostream& out, df::report* attackEvent, df::unit* lastAttacker, df::report* defendEvent, vector<df::unit*>& relevantUnits) {
    vector<df::unit*> attackers = relevantUnits;
    vector<df::unit*> defenders = relevantUnits;

    //find valid interactions: TODO
    /*map<int32_t,vector<df::interaction*> > validInteractions;
    for ( size_t a = 0; a < relevantUnits.size(); a++ ) {
        df::unit* unit = relevantUnits[a];
        vector<df::interaction*>& interactions = validInteractions[unit->id];
        for ( size_t b = 0; b < unit->body.
    }*/

    //if attackEvent
    //  attacker must be same location
    //  attacker name must start attack str
    //  attack verb must match valid interaction of this attacker
    std::string attackVerb;
    if ( attackEvent ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
        for ( size_t a = 0; a < attackers.size(); a++ ) {
            if ( attackers[a]->pos != attackEvent->pos ) {
                attackers.erase(attackers.begin()+a);
                a--;
                continue;
            }
            if ( lastAttacker && attackers[a] != lastAttacker ) {
                attackers.erase(attackers.begin()+a);
                a--;
                continue;
            }

            std::string verbC = getVerb(attackers[a], attackEvent->text);
            if ( verbC.length() == 0 ) {
                attackers.erase(attackers.begin()+a);
                a--;
                continue;
            }
            attackVerb = verbC;
        }
    }

    //if defendEvent
    //  defender must be same location
    //  defender name must start defend str
    //  defend verb must match valid interaction of some attacker
    std::string defendVerb;
    if ( defendEvent ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
        for ( size_t a = 0; a < defenders.size(); a++ ) {
            if ( defenders[a]->pos != defendEvent->pos ) {
                defenders.erase(defenders.begin()+a);
                a--;
                continue;
            }
            std::string verbC = getVerb(defenders[a], defendEvent->text);
            if ( verbC.length() == 0 ) {
                defenders.erase(defenders.begin()+a);
                a--;
                continue;
            }
            defendVerb = verbC;
        }
    }

    //keep in mind one attacker zero defenders is perfectly valid for self-cast
    if ( attackers.size() == 1 && defenders.size() == 1 && attackers[0] == defenders[0] ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
    } else {
        if ( defenders.size() == 1 ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
            auto a = std::find(attackers.begin(),attackers.end(),defenders[0]);
            if ( a != attackers.end() )
                attackers.erase(a);
        }
        if ( attackers.size() == 1 ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
            auto a = std::find(defenders.begin(),defenders.end(),attackers[0]);
            if ( a != defenders.end() )
                defenders.erase(a);
        }
    }

    //if trying attack-defend pair and it fails to find attacker, try defend only
    InteractionData result = /*(InteractionData)*/ { std::string(), std::string(), -1, -1, -1, -1 };
    if ( attackers.size() > 1 ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
        if ( Once::doOnce("EventManager interaction ambiguous attacker") ) {
            out.print("%s,%d: ambiguous attacker on report\n \'%s\'\n '%s'\n", __FILE__, __LINE__, attackEvent ? attackEvent->text.c_str() : "", defendEvent ? defendEvent->text.c_str() : "");
        }
    } else if ( attackers.empty() ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
        if ( attackEvent && defendEvent )
            return getAttacker(out, nullptr, nullptr, defendEvent, relevantUnits);
    } else {
//out.print("%s,%d\n",__FILE__,__LINE__);
        //attackers.size() == 1
        result.attacker = attackers[0]->id;
        if ( !defenders.empty() )
            result.defender = defenders[0]->id;
        if ( defenders.size() > 1 ) {
            if ( Once::doOnce("EventManager interaction ambiguous defender") ) {
                out.print("%s,%d: ambiguous defender: shouldn't happen. On report\n \'%s\'\n '%s'\n", __FILE__, __LINE__, attackEvent ? attackEvent->text.c_str() : "", defendEvent ? defendEvent->text.c_str() : "");
            }
        }
        result.attackVerb = attackVerb;
        result.defendVerb = defendVerb;
        if ( attackEvent )
            result.attackReport = attackEvent->id;
        if ( defendEvent )
            result.defendReport = defendEvent->id;
    }
//out.print("%s,%d\n",__FILE__,__LINE__);
    return result;
}

static vector<df::unit*> gatherRelevantUnits(color_ostream& out, df::report* r1, df::report* r2) {
    vector<df::report*> reports;
    if ( r1 == r2 ) r2 = nullptr;
    if ( r1 ) reports.push_back(r1);
    if ( r2 ) reports.push_back(r2);
    vector<df::unit*> result;
    unordered_set<int32_t> ids;
//out.print("%s,%d\n",__FILE__,__LINE__);
    for (auto report : reports) {
//out.print("%s,%d\n",__FILE__,__LINE__);
        vector<int32_t>& units = reportToRelevantUnits[report->id];
        if ( units.size() > 2 ) {
            if ( Once::doOnce("EventManager interaction too many relevant units") ) {
                out.print("%s,%d: too many relevant units. On report\n \'%s\'\n", __FILE__, __LINE__, report->text.c_str());
            }
        }
        for (int & unit_id : units)
            if (ids.find(unit_id) == ids.end() ) {
                ids.insert(unit_id);
                result.push_back(df::unit::find(unit_id));
            }
    }
//out.print("%s,%d\n",__FILE__,__LINE__);
    return result;
}

static void manageInteractionEvent(color_ostream& out) {
    if (!df::global::world)
        return;
    multimap<Plugin*,EventHandler> copy(handlers[EventType::INTERACTION].begin(), handlers[EventType::INTERACTION].end());
    std::vector<df::report*>& reports = df::global::world->status.reports;
    size_t a = df::report::binsearch_index(reports, lastReportInteraction, false);
    while (a < reports.size() && reports[a]->id <= lastReportInteraction) {
        a++;
    }
    if ( a < reports.size() )
        updateReportToRelevantUnits();

    df::report* lastAttackEvent = nullptr;
    df::unit* lastAttacker = nullptr;
    //df::unit* lastDefender = NULL;
    unordered_map<int32_t,unordered_set<int32_t> > history;
    for ( ; a < reports.size(); a++ ) {
        df::report* report = reports[a];
        lastReportInteraction = report->id;
        df::announcement_type type = report->type;
        if ( type != df::announcement_type::INTERACTION_ACTOR && type != df::announcement_type::INTERACTION_TARGET )
            continue;
        if ( report->flags.bits.continuation )
            continue;
        bool attack = type == df::announcement_type::INTERACTION_ACTOR;
        if ( attack ) {
            lastAttackEvent = report;
            lastAttacker = nullptr;
            //lastDefender = NULL;
        }
        vector<df::unit*> relevantUnits = gatherRelevantUnits(out, lastAttackEvent, report);
        InteractionData data = getAttacker(out, lastAttackEvent, lastAttacker, attack ? nullptr : report, relevantUnits);
        if ( data.attacker < 0 )
            continue;
//out.print("%s,%d\n",__FILE__,__LINE__);
        //if ( !attack && lastAttacker && data.attacker == lastAttacker->id && lastDefender && data.defender == lastDefender->id )
        //    continue; //lazy way of preventing duplicates
        if ( attack && a+1 < reports.size() && reports[a+1]->type == df::announcement_type::INTERACTION_TARGET ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
            vector<df::unit*> relevant_units = gatherRelevantUnits(out, lastAttackEvent, reports[a + 1]);
            InteractionData data2 = getAttacker(out, lastAttackEvent, lastAttacker, reports[a+1], relevant_units);
            if ( data.attacker == data2.attacker && (data.defender == -1 || data.defender == data2.defender) ) {
//out.print("%s,%d\n",__FILE__,__LINE__);
                data = data2;
                a++;
            }
        }
        {
#define HISTORY_ITEM 1
#if HISTORY_ITEM
            unordered_set<int32_t>& b = history[data.attacker];
            if ( b.find(data.defender) != b.end() )
                continue;
            history[data.attacker].insert(data.defender);
            //b.insert(data.defender);
#else
            unordered_set<int32_t>& b = history[data.attackReport];
            if ( b.find(data.defendReport) != b.end() )
                continue;
            history[data.attackReport].insert(data.defendReport);
            //b.insert(data.defendReport);
#endif
        }
//out.print("%s,%d\n",__FILE__,__LINE__);
        lastAttacker = df::unit::find(data.attacker);
        //lastDefender = df::unit::find(data.defender);
        //fire event
        for (auto &key_value : copy) {
            EventHandler &handle = key_value.second;
            DEBUG(log,out).print("calling handler for interaction event\n");
            handle.eventHandler(out, (void*)&data);
        }
        //TODO: deduce attacker from latest defend event first
    }
}
