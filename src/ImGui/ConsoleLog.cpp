#include "ConsoleLog.h"

#include <SDL/SDL.h>
#include <cctype>
#include <iostream>
#include <streambuf>

namespace {
	class TeeBuf : public std::streambuf {
	public:
		TeeBuf(std::streambuf* original, int level) : m_original(original), m_level(level) {}
		std::streambuf* Original() const { return m_original; }
	protected:
		int overflow(int c) override {
			if (c == EOF) return 0;
			if (m_original) m_original->sputc(static_cast<char>(c));
			if (c == '\n') Flush();
			else m_pending.push_back(static_cast<char>(c));
			return c;
		}
		std::streamsize xsputn(const char* s, std::streamsize n) override {
			if (m_original) m_original->sputn(s, n);
			for (std::streamsize i = 0; i < n; ++i) {
				if (s[i] == '\n') Flush();
				else m_pending.push_back(s[i]);
			}
			return n;
		}
		int sync() override { return m_original ? m_original->pubsync() : 0; }
	private:
		void Flush() {
			if (!m_pending.empty() && m_pending.back() == '\r') m_pending.pop_back();
			int level = m_level;
			if (level == 0) {
				const std::string lower = [&] { std::string t = m_pending; for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return t; }();
				if (lower.find("error") != std::string::npos || lower.find("failed") != std::string::npos) level = 2;
				else if (lower.find("warning") != std::string::npos) level = 1;
			}
			ConsoleLog::Get().Add(m_pending, level);
			m_pending.clear();
		}
		std::streambuf* m_original;
		int m_level;
		std::string m_pending;
	};
	TeeBuf* g_out = nullptr;
	TeeBuf* g_err = nullptr;
}

ConsoleLog& ConsoleLog::Get()
{
	static ConsoleLog log;
	return log;
}

void ConsoleLog::Install()
{
	if (m_installed) return;
	g_out = new TeeBuf(std::cout.rdbuf(), 0);
	g_err = new TeeBuf(std::cerr.rdbuf(), 2);
	std::cout.rdbuf(g_out);
	std::cerr.rdbuf(g_err);
	m_installed = true;
}

void ConsoleLog::Uninstall()
{
	if (!m_installed) return;
	std::cout.flush();
	std::cerr.flush();
	std::cout.rdbuf(g_out->Original());
	std::cerr.rdbuf(g_err->Original());
	m_installed = false;
	// The tee buffers are intentionally leaked: static destructors may still print.
}

void ConsoleLog::Add(const std::string& text, int level)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_lines.push_back({ text, level, SDL_GetTicks() / 1000.0 });
	if (m_lines.size() > 20000) {
		m_lines.erase(m_lines.begin(), m_lines.begin() + 5000);
		m_cleared += 5000;
	}
}

std::vector<ConsoleLog::Line> ConsoleLog::Since(size_t from, size_t& outTotal) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	outTotal = m_cleared + m_lines.size();
	std::vector<Line> out;
	const size_t start = from > m_cleared ? from - m_cleared : 0;
	for (size_t i = start; i < m_lines.size(); ++i) out.push_back(m_lines[i]);
	return out;
}

size_t ConsoleLog::Count() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_cleared + m_lines.size();
}

void ConsoleLog::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cleared += m_lines.size();
	m_lines.clear();
}
