#pragma once
#include <mutex>
#include <string>
#include <vector>

// Captures std::cout / std::cerr (still forwarded to the original streams) for the editor console.
class ConsoleLog {
public:
	struct Line { std::string text; int level = 0; double time = 0.0; }; // level: 0 info, 1 warning, 2 error
	static ConsoleLog& Get();
	void Install();
	void Uninstall();
	void Add(const std::string& text, int level);
	// Thread-safe copy of the lines added since 'from'.
	std::vector<Line> Since(size_t from, size_t& outTotal) const;
	size_t Count() const;
	void Clear();
	size_t ClearedOffset() const { return m_cleared; }

private:
	mutable std::mutex m_mutex;
	std::vector<Line> m_lines;
	size_t m_cleared = 0;
	bool m_installed = false;
};
