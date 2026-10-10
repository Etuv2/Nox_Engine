#pragma once
#include "rhi/RHI.h"
#include <glm/glm.hpp>
#include <string>
#include <vector>

/*
* Builds 2D array texture resources from image files (one layer per entry, optionally a cropped
* region of a file). Colour images are decoded as sRGB, optionally premultiplied in linear space
* (with a residual-alpha floor), area-resampled to a square layer size, given premultiplied box mips
* (alpha-aware filtering) and stored as SRGB8_ALPHA8 with clamp-to-edge sampling. Shaders read linear
* (premultiplied when requested) colour. Decoding and filtering run on worker threads; the upload runs
* on the calling GL thread. Results are cached per (layers, options) for the process lifetime.
*/
namespace TextureArrayBuilder {

	struct Layer {
		std::string file;
		glm::vec4 rect{ 0.0f, 0.0f, 1.0f, 1.0f };  // normalised source region (left, top, right, bottom), top-left origin
	};

	struct Options {
		int size = 1024;               // layer edge in texels
		bool premultiply = true;       // store linear-premultiplied colour (straight-alpha sources)
		float alphaFloor = 2.0f / 255.0f;  // alpha below this is treated as zero (residual canvas alpha)
		bool anisotropic = true;
		std::string debugName = "texture array";
	};

	// Returns 0 (logged once) when any file cannot be read.
	rhi::TextureHandle GetOrBuild(const std::vector<Layer>& layers, const Options& options);
}
