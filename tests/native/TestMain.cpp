// Runner for the headless native tests (see TestFramework.h).
#include "TestFramework.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {
	struct TestCase {
		std::string suite;
		std::string name;
		std::function<void()> body;
	};

	std::vector<TestCase>& Tests()
	{
		static std::vector<TestCase> tests;
		return tests;
	}

	int g_failures = 0;
	bool g_currentFailed = false;
}

namespace NoxTest {
	void Register(const char* suite, const char* name, std::function<void()> body)
	{
		Tests().push_back({ suite, name, std::move(body) });
	}

	void ReportFailure(const char* file, int line, const std::string& message, bool fatal)
	{
		g_currentFailed = true;
		std::printf("    %s(%d): %s\n", file, line, message.c_str());
		if (fatal) throw Failure{};
	}

	const std::filesystem::path& ScratchDir()
	{
		static const std::filesystem::path dir = [] {
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			std::filesystem::path p = std::filesystem::temp_directory_path() / ("nox_tests_" + std::to_string(stamp));
			std::filesystem::create_directories(p);
			return p;
		}();
		return dir;
	}

	const std::filesystem::path& RepoRoot()
	{
		static const std::filesystem::path root = [] {
			std::error_code ec;
			std::filesystem::path dir = std::filesystem::current_path(ec);
			for (int i = 0; i < 8 && !dir.empty(); ++i) {
				if (std::filesystem::exists(dir / "CMakeLists.txt", ec) && std::filesystem::exists(dir / "src", ec)) return dir;
				if (dir == dir.parent_path()) break;
				dir = dir.parent_path();
			}
			return std::filesystem::current_path(ec);
		}();
		return root;
	}
}

int main(int argc, char* argv[])
{
	std::vector<std::string> filters(argv + 1, argv + argc);
	int run = 0;
	for (const TestCase& test : Tests()) {
		const std::string full = test.suite + "." + test.name;
		if (!filters.empty()) {
			bool match = false;
			for (const std::string& f : filters) match = match || full.find(f) != std::string::npos;
			if (!match) continue;
		}
		++run;
		g_currentFailed = false;
		std::printf("[ RUN  ] %s\n", full.c_str());
		std::fflush(stdout);
		try { test.body(); }
		catch (const NoxTest::Failure&) {}
		catch (const std::exception& e) { NoxTest::ReportFailure(__FILE__, __LINE__, std::string("exception: ") + e.what(), false); }
		catch (...) { NoxTest::ReportFailure(__FILE__, __LINE__, "unknown exception", false); }
		if (g_currentFailed) ++g_failures;
		std::printf("[ %s ] %s\n", g_currentFailed ? "FAIL" : "  OK", full.c_str());
		std::fflush(stdout);
	}
	std::printf("%d test(s), %d failed\n", run, g_failures);
	if (g_failures == 0) {
		std::error_code ec;
		std::filesystem::remove_all(NoxTest::ScratchDir(), ec);
	}
	return g_failures == 0 ? 0 : 1;
}
