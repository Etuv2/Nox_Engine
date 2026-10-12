#pragma once
// stb_image decoding of logical asset files. Bytes come from AssetIO and are decoded from memory, so loose
// and packed content decode identically. Arguments match stbi_load / stbi_loadf; release results with
// stbi_image_free. Flip and thread-local flag settings are still owned by the caller.
#include <string>
#include <string_view>

namespace AssetIO {
	unsigned char* LoadImageFile(std::string_view path, int* x, int* y, int* channelsInFile, int desiredChannels,
		std::string* error = nullptr);
	float* LoadImageFileF(std::string_view path, int* x, int* y, int* channelsInFile, int desiredChannels,
		std::string* error = nullptr);
}
