#include <inlines.h>
#include <on-tick.h>
#include <plugin.h>
#include <modules/EventManager.h>

extern DFHack::Plugin* plugin_self;

namespace CSP {
    extern OnTick tick_it_master;
    extern void UnpauseEvent(bool full_scan);
}

void cancel(DFHack::color_ostream&, void*);
void dig(DFHack::color_ostream&, void*);
void resurrect(DFHack::color_ostream&, void*);
void refresh(DFHack::color_ostream&, void*);

EventManager::EventHandler cancelHandler(plugin_self, cancel, 0);
EventManager::EventHandler digHandler(plugin_self, dig, 0);
EventManager::EventHandler resurrectHandler(plugin_self, resurrect, 0);
EventManager::EventHandler refreshHandler(plugin_self, refresh, 0);

void cancel(DFHack::color_ostream&, void*) {
    CSP::tick_it_master.cancel_queued = false;
    CSP::active_job_manager.handle_cancellation();
}

void resurrect(DFHack::color_ostream &out, void*) {
    CSP::tick_it_master.resurrect_queued = false;
    CSP::active_job_manager.handle_resurrect(out);
    if (CSP::active_job_manager.needs_resurrect_queued()) {
        EventManager::registerTick(resurrectHandler, 1);
        CSP::tick_it_master.resurrect_queued = true;
    }
}

void dig(DFHack::color_ostream &out, void*) {
    CSP::tick_it_master.dig_queued = false;
    if (!config.insta_dig) {
        CSP::dignow_queue.clear();
        return;
    }
    TRACE(monitor).print(" -> evaluate dignow queue\n");
    for (auto pos : CSP::dignow_queue) {
        dig_now(out, pos);
        ChannelManager::Get().erase(pos);
        CSP::active_job_manager.cancel(pos);
    }
    CSP::dignow_queue.clear();
}

void refresh(DFHack::color_ostream&, void*) {
    CSP::UnpauseEvent(true);
    EventManager::registerTick(refreshHandler, config.refresh_freq);
}
