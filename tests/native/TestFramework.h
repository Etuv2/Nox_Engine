#pragma once
// Minimal headless test harness for tests/native (no external dependency).
//
//   NOX_TEST(AssetIO, NormalizesSeparators) { NOX_CHECK(a == b); NOX_REQUIRE(ptr); }
//
// nox_tests runs every test, or only those whose "Suite.Name" contains an argument:
//   build\vs2022-x64\Release\nox_tests.exe AssetIO Lua
// Tests run from the repository root (ctest sets the working directory). Scratch files go under
// NoxTest::ScratchDir(), which is unique per run and removed on success.
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>

namespace NoxTest {
	struct Failure {};

	void Register(const char* suite, const char* name, std::function<void()> body);
	void ReportFailure(const char* file, int line, const std::string& message, bool fatal);
	// Per-run scratch directory (created on first use).
	const std::filesystem::path& ScratchDir();
	// Repository root (the directory holding CMakeLists.txt), found from the working directory.
	const std::filesystem::path& RepoRoot();

	struct Registrar {
		Registrar(const char* suite, const char* name, std::function<void()> body) { Register(suite, name, std::move(body)); }
	};
}

#define NOX_TEST_CAT2(a, b) a##b
#define NOX_TEST_CAT(a, b) NOX_TEST_CAT2(a, b)
#define NOX_TEST(suite, name) \
	static void NOX_TEST_CAT(NoxTest_##suite##_##name, _Body)(); \
	static NoxTest::Registrar NOX_TEST_CAT(NoxTest_##suite##_##name, _Reg)(#suite, #name, &NOX_TEST_CAT(NoxTest_##suite##_##name, _Body)); \
	static void NOX_TEST_CAT(NoxTest_##suite##_##name, _Body)()

#define NOX_CHECK(cond) do { if (!(cond)) NoxTest::ReportFailure(__FILE__, __LINE__, "CHECK failed: " #cond, false); } while (0)
#define NOX_REQUIRE(cond) do { if (!(cond)) NoxTest::ReportFailure(__FILE__, __LINE__, "REQUIRE failed: " #cond, true); } while (0)
#define NOX_CHECK_EQ(a, b) do { const auto& nox_a_ = (a); const auto& nox_b_ = (b); if (!(nox_a_ == nox_b_)) { \
	std::ostringstream nox_s_; nox_s_ << "CHECK_EQ failed: " #a " == " #b " (" << nox_a_ << " vs " << nox_b_ << ")"; \
	NoxTest::ReportFailure(__FILE__, __LINE__, nox_s_.str(), false); } } while (0)
#define NOX_FAIL(msg) NoxTest::ReportFailure(__FILE__, __LINE__, (msg), true)
