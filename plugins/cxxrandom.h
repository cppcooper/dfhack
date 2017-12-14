#include <random>
#include <unordered_map>
#include <chrono>

class CXXRNG
{
private:
	CXXRNG() { RNG = std::default_random_engine(std::chrono::system_clock::now().time_since_epoch().count()); }
	static CXXRNG* instance;

protected:
	std::default_random_engine											RNG;
	std::unordered_map<int, std::uniform_int_distribution<int>>			ND_int;
	std::unordered_map<double, std::uniform_real_distribution<double>>	ND_double;
	std::unordered_map<double, std::normal_distribution<double>>		ND_normal;
	std::unordered_map<float, std::bernoulli_distribution>				ND_bool;

public:
	static CXXRNG& Instance()
	{
		if(!instance)
		{
			instance = new CXXRNG();
		}
		return *instance;
	}
	int GenInteger(short min, short max);
	double GenDouble(float min, float max);
	double GenNormal(float mean, float stddev);
	bool GenBool(float p);
	void BlastDistributions();
};