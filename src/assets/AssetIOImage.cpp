#include "AssetIOImage.h"

#include "AssetIO.h"
#include "stb_image.h"

#include <cstdint>
#include <limits>

namespace AssetIO {

	unsigned char* LoadImageFile(std::string_view path, int* x, int* y, int* channelsInFile, int desiredChannels,
		std::string* error)
	{
		Buffer bytes = ReadAll(path, error);
		if (!bytes) return nullptr;
		if (bytes->size() > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
			if (error) *error = "image too large for stb_image: " + std::string(path);
			return nullptr;
		}
		return stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), x, y, channelsInFile, desiredChannels);
	}

	float* LoadImageFileF(std::string_view path, int* x, int* y, int* channelsInFile, int desiredChannels,
		std::string* error)
	{
		Buffer bytes = ReadAll(path, error);
		if (!bytes) return nullptr;
		if (bytes->size() > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
			if (error) *error = "image too large for stb_image: " + std::string(path);
			return nullptr;
		}
		return stbi_loadf_from_memory(bytes->data(), static_cast<int>(bytes->size()), x, y, channelsInFile, desiredChannels);
	}

} // namespace AssetIO
