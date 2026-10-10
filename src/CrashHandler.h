#pragma once

// Installs a process-wide unhandled-exception filter that prints the exception code and a
// symbolized call stack (function, file:line from the PDB) to stderr before the process exits.
// Lets crashes in scripted runs (benchmarks, captures, CI) be diagnosed without a debugger.
namespace CrashHandler {
	void Install();
}
