#include "Core.h"
#include "DataFuncs.h"
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>
#include "cxxrandom.h"

CXXRNG* CXXRNG::instance = nullptr;

using namespace DFHack;

DFHACK_PLUGIN("cxxrandom");
#define PLUGIN_VERSION 1.0
//REQUIRE_GLOBAL(world);

command_result cxxrandom (color_ostream &out, std::vector <std::string> & parameters);

DFhackCExport command_result plugin_init (color_ostream &out, std::vector <PluginCommand> &commands)
{
    commands.push_back(PluginCommand(
        "cxxrandom", "C++xx Random Numbers", cxxrandom, false,
        "  This plugin cannot be used on the commandline.\n"
		"  Import into a lua script to have access to exported RNG functions:\n"
		"    local rng = require('plugins.cxxrandom')\n\n"
		"  Exported Functions:\n"
		"    rng.RollInt(min, max, [(optional)string ref])\n"
        "    rng.RollDouble(min, max, [(optional)string ref])\n"
		"    rng.RollNormal(mean, std_deviation, [(optional)string ref])\n"
        "    rng.RollBool(chance_of_true, [(optional)string ref])\n\n"
    ));
	CXXRNG::Instance();
    return CR_OK;
}

DFhackCExport command_result plugin_shutdown (color_ostream &out)
{
	CXXRNG::Instance().BlastDistributions();
    return CR_OK;
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
	return CR_OK;
}

union TwoShorts
{
	struct
	{
		short A;
		short B;
	};
	int value;
};

union TwoFloats
{
	struct
	{
		float A;
		float B;
	};
	double value;
};

int CXXRNG::GenInteger(short min, short max)
{
	TwoShorts merger;
	merger.A = min;
	merger.B = max;
	int ref = merger.value;
	auto iter = ND_int.find(ref);
	if(iter == ND_int.end() || iter->second.min() != min || iter->second.max() != max)
	{
		if(iter != ND_int.end())
			ND_int.erase(iter);

		iter = ND_int.emplace(ref, std::uniform_int_distribution<int>(min, max)).first;
	}
	return iter->second(RNG);
}

double CXXRNG::GenDouble(float min, float max)
{
	TwoFloats merger;
	merger.A = min;
	merger.B = max;
	double ref = merger.value;
	auto iter = ND_double.find(ref);
	if(iter == ND_double.end() || iter->second.min() != min || iter->second.max() != max)
	{
		if(iter != ND_double.end())
			ND_double.erase(iter);

		iter = ND_double.emplace(ref, std::uniform_real_distribution<double>(min, max)).first;
	}
	return iter->second(RNG);
}

double CXXRNG::GenNormal(float mean, float stddev)
{
	TwoFloats merger;
	merger.A = mean;
	merger.B = stddev;
	double ref = merger.value;
	auto iter = ND_normal.find(ref);
	if(iter == ND_normal.end() || iter->second.mean() != mean || iter->second.stddev() != stddev)
	{
		if(iter != ND_normal.end())
			ND_normal.erase(iter);

		iter = ND_normal.emplace(ref, std::normal_distribution<double>(mean, stddev)).first;
	}
	return iter->second(RNG);
}

bool CXXRNG::GenBool(float p)
{
	auto iter = ND_bool.find(p);
	if(iter == ND_bool.end() || iter->second.p() != p)
	{
		if(iter != ND_bool.end())
			ND_bool.erase(iter);

		iter = ND_bool.emplace(p, std::bernoulli_distribution(p)).first;
	}
	return iter->second(RNG);
}

void CXXRNG::BlastDistributions()
{
	ND_int.clear();
	ND_double.clear();
	ND_normal.clear();
	ND_bool.clear();
}


int RollInt(float min, float max)
{
	return CXXRNG::Instance().GenInteger((short)min, (short)max);
}

double RollDouble(float min, float max)
{
	return CXXRNG::Instance().GenDouble(min, max);
}

double GetNormalDouble(float mean, float stddev)
{
	return CXXRNG::Instance().GenNormal(mean, stddev);
}

bool RollBool(float p)
{
	return CXXRNG::Instance().GenBool(p);
}

void BlastDistributions()
{
	CXXRNG::Instance().BlastDistributions();
}

DFHACK_PLUGIN_LUA_FUNCTIONS {
    DFHACK_LUA_FUNCTION(RollInt),
    DFHACK_LUA_FUNCTION(RollDouble),
    DFHACK_LUA_FUNCTION(GetNormalDouble),
    DFHACK_LUA_FUNCTION(RollBool),
	DFHACK_LUA_FUNCTION(BlastDistributions),
    DFHACK_LUA_END
};

static command_result cxxrandom (color_ostream &out, std::vector <std::string> & parameters)
{
	return CR_WRONG_USAGE;
    /*if (!parameters.empty())
    CoreSuspender suspend;
    out.print("blah");
    return CR_OK;*/
}