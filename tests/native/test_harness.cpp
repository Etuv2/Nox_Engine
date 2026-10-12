// Self-check for the native test harness.
#include "TestFramework.h"

NOX_TEST(Harness, FindsRepoRoot)
{
	NOX_CHECK(std::filesystem::exists(NoxTest::RepoRoot() / "CMakeLists.txt"));
	NOX_CHECK(std::filesystem::exists(NoxTest::ScratchDir()));
}
