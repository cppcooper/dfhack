#pragma once
#include "DFHackVersion.h"

namespace DFHack {
    class Plugin;
}

struct OnTick {
    bool cancel_queued = false;
    bool dig_queued = false;
    bool resurrect_queued = false;
};
