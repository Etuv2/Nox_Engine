#include "FrameProfiler.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;
	struct State {
		int interval = 0;
		int frames = 0;
		Clock::time_point last{};
		std::vector<std::string> order;
		std::map<std::string, float> current;
		std::map<std::string, std::vector<float>> samples;
	};
	State& S()
	{
		static State state = [] {
			State s;
			if (const char* v = std::getenv("NOX_LOG_FRAME_PHASES")) s.interval = std::max(0, std::atoi(v));
			return s;
		}();
		return state;
	}
	float Pct(std::vector<float>& v, float p)
	{
		if (v.empty()) return 0.0f;
		std::sort(v.begin(), v.end());
		return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5f))];
	}
}

bool FrameProfiler::Enabled() { return S().interval > 0; }

void FrameProfiler::BeginFrame()
{
	State& s = S();
	if (!s.interval) return;
	s.last = Clock::now();
	s.current.clear();
}

void FrameProfiler::Mark(const char* phase)
{
	State& s = S();
	if (!s.interval) return;
	const Clock::time_point now = Clock::now();
	const std::string name(phase);
	if (std::find(s.order.begin(), s.order.end(), name) == s.order.end()) s.order.push_back(name);
	s.current[name] += std::chrono::duration<float, std::milli>(now - s.last).count();
	s.last = now;
}

void FrameProfiler::EndFrame()
{
	State& s = S();
	if (!s.interval) return;
	float total = 0.0f;
	for (const std::string& name : s.order) {
		const float ms = s.current.count(name) ? s.current[name] : 0.0f;
		s.samples[name].push_back(ms);
		total += ms;
	}
	s.samples["total"].push_back(total);
	if (++s.frames % s.interval) return;
	std::ostringstream line;
	line << "[FramePhases] frames=" << s.interval << std::fixed << std::setprecision(2);
	for (const std::string& name : s.order) line << ' ' << name << '=' << Pct(s.samples[name], 0.5f) << '/' << Pct(s.samples[name], 0.95f);
	line << " total=" << Pct(s.samples["total"], 0.5f) << '/' << Pct(s.samples["total"], 0.95f);
	std::cout << line.str() << std::endl;
	s.samples.clear();
}
