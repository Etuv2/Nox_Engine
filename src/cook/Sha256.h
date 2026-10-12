#pragma once
// SHA-256 (FIPS 180-4) for content keys and input fingerprints. Portable, no external dependency.
#include <cstdint>
#include <string>
#include <string_view>

namespace Sha256 {
	// Lower-case hex digest (64 characters).
	std::string HexOf(std::string_view bytes);

	class Hasher {
	public:
		Hasher();
		void Update(std::string_view bytes);
		std::string FinishHex();
	private:
		void Block(const uint8_t* block);
		uint32_t m_state[8];
		uint8_t m_buffer[64];
		size_t m_bufferSize = 0;
		uint64_t m_totalBytes = 0;
	};
}
