#include "TextureArrayBuilder.h"
#include "stb_image.h"
#include "assets/AssetIOImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

namespace {

	float SrgbToLinear(float c) {
		return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
	}

	const std::array<float, 256>& DecodeLut() {
		static const std::array<float, 256> lut = [] {
			std::array<float, 256> t{};
			for (int i = 0; i < 256; ++i) t[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
			return t;
		}();
		return lut;
	}

	// Linear [0,1] -> sRGB 8-bit, 16k-entry table (fine enough for premultiplied dark fringes).
	constexpr int kEncodeLutSize = 16384;
	const std::vector<uint8_t>& EncodeLut() {
		static const std::vector<uint8_t> lut = [] {
			std::vector<uint8_t> t(kEncodeLutSize + 1);
			for (int i = 0; i <= kEncodeLutSize; ++i) {
				const float l = static_cast<float>(i) / kEncodeLutSize;
				const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
				t[i] = static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			return t;
		}();
		return lut;
	}

	uint8_t EncodeSrgb(float linear) {
		return EncodeLut()[static_cast<int>(std::clamp(linear, 0.0f, 1.0f) * kEncodeLutSize + 0.5f)];
	}

	struct ImageF {
		int w = 0, h = 0;
		std::vector<glm::vec4> px;   // linear RGBA (premultiplied when requested)
		const glm::vec4& At(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
	};

	bool Decode(const std::string& path, bool premultiply, float alphaFloor, ImageF& out) {
		stbi_set_flip_vertically_on_load_thread(0);
		int w = 0, h = 0, n = 0;
		unsigned char* data = AssetIO::LoadImageFile(path, &w, &h, &n, 4);
		if (!data) return false;
		const auto& lut = DecodeLut();
		const float floorA = std::clamp(alphaFloor, 0.0f, 0.5f);
		out.w = w;
		out.h = h;
		out.px.resize(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < out.px.size(); ++i) {
			const unsigned char* p = data + i * 4;
			const float a = n == 4 ? std::max((p[3] / 255.0f - floorA) / (1.0f - floorA), 0.0f) : 1.0f;
			const float m = premultiply ? a : 1.0f;
			out.px[i] = glm::vec4(lut[p[0]] * m, lut[p[1]] * m, lut[p[2]] * m, a);
		}
		stbi_image_free(data);
		return true;
	}

	struct Tap { int index; float weight; };

	// Per-destination taps for one axis over the source region [x0, x1) in pixel-edge coordinates:
	// exact box (area) filter when minifying, bilinear when magnifying.
	std::vector<std::vector<Tap>> AxisTaps(float x0, float x1, int srcN, int dstN) {
		std::vector<std::vector<Tap>> taps(dstN);
		const float scale = (x1 - x0) / static_cast<float>(dstN);
		const int lo = std::clamp(static_cast<int>(std::floor(x0)), 0, srcN - 1);
		const int hi = std::clamp(static_cast<int>(std::ceil(x1)) - 1, 0, srcN - 1);
		for (int i = 0; i < dstN; ++i) {
			std::vector<Tap>& t = taps[i];
			if (scale >= 1.0f) {
				const float a = x0 + i * scale, b = a + scale;
				float sum = 0.0f;
				for (int p = static_cast<int>(std::floor(a)); p < static_cast<int>(std::ceil(b)); ++p) {
					const float wgt = std::min(b, p + 1.0f) - std::max(a, static_cast<float>(p));
					if (wgt <= 0.0f) continue;
					t.push_back({ std::clamp(p, lo, hi), wgt });
					sum += wgt;
				}
				for (Tap& tp : t) tp.weight /= std::max(sum, 1e-6f);
			}
			else {
				const float c = std::clamp(x0 + (i + 0.5f) * scale - 0.5f, static_cast<float>(lo), static_cast<float>(hi));
				const int i0 = static_cast<int>(std::floor(c));
				const float f = c - i0;
				t.push_back({ std::clamp(i0, lo, hi), 1.0f - f });
				t.push_back({ std::clamp(i0 + 1, lo, hi), f });
			}
		}
		return taps;
	}

	ImageF Resample(const ImageF& src, const glm::vec4& rect, int size) {
		const auto tx = AxisTaps(rect.x * src.w, rect.z * src.w, src.w, size);
		const auto ty = AxisTaps(rect.y * src.h, rect.w * src.h, src.h, size);
		ImageF tmp;
		tmp.w = size;
		tmp.h = src.h;
		tmp.px.assign(static_cast<size_t>(size) * src.h, glm::vec4(0.0f));
		std::vector<uint8_t> rowNeeded(src.h, 0);
		for (const auto& col : ty) for (const Tap& t : col) rowNeeded[t.index] = 1;
		for (int y = 0; y < src.h; ++y) {
			if (!rowNeeded[y]) continue;
			for (int x = 0; x < size; ++x) {
				glm::vec4 acc(0.0f);
				for (const Tap& t : tx[x]) acc += src.At(t.index, y) * t.weight;
				tmp.px[static_cast<size_t>(y) * size + x] = acc;
			}
		}
		ImageF out;
		out.w = size;
		out.h = size;
		out.px.assign(static_cast<size_t>(size) * size, glm::vec4(0.0f));
		for (int y = 0; y < size; ++y) {
			for (int x = 0; x < size; ++x) {
				glm::vec4 acc(0.0f);
				for (const Tap& t : ty[y]) acc += tmp.At(x, t.index) * t.weight;
				out.px[static_cast<size_t>(y) * size + x] = acc;
			}
		}
		return out;
	}

	ImageF Downsample2x(const ImageF& src) {
		ImageF out;
		out.w = std::max(src.w / 2, 1);
		out.h = std::max(src.h / 2, 1);
		out.px.resize(static_cast<size_t>(out.w) * out.h);
		for (int y = 0; y < out.h; ++y) {
			for (int x = 0; x < out.w; ++x) {
				const int x0 = std::min(x * 2, src.w - 1), x1 = std::min(x * 2 + 1, src.w - 1);
				const int y0 = std::min(y * 2, src.h - 1), y1 = std::min(y * 2 + 1, src.h - 1);
				out.px[static_cast<size_t>(y) * out.w + x] =
					(src.At(x0, y0) + src.At(x1, y0) + src.At(x0, y1) + src.At(x1, y1)) * 0.25f;
			}
		}
		return out;
	}

	std::vector<std::vector<uint8_t>> EncodeMips(ImageF level, int levels) {
		std::vector<std::vector<uint8_t>> mips;
		for (int l = 0; l < levels; ++l) {
			std::vector<uint8_t> bytes(level.px.size() * 4);
			for (size_t i = 0; i < level.px.size(); ++i) {
				const glm::vec4& p = level.px[i];
				bytes[i * 4 + 0] = EncodeSrgb(p.r);
				bytes[i * 4 + 1] = EncodeSrgb(p.g);
				bytes[i * 4 + 2] = EncodeSrgb(p.b);
				bytes[i * 4 + 3] = static_cast<uint8_t>(std::clamp(p.a, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			mips.push_back(std::move(bytes));
			if (l + 1 < levels) level = Downsample2x(level);
		}
		return mips;
	}

	int MipLevels(int size) {
		int levels = 1;
		while ((size >> levels) > 0) ++levels;
		return levels;
	}

	std::string CacheKey(const std::vector<TextureArrayBuilder::Layer>& layers, const TextureArrayBuilder::Options& o) {
		std::ostringstream key;
		key << o.size << '|' << o.premultiply << '|' << o.alphaFloor << '|' << o.anisotropic;
		for (const auto& l : layers) key << '|' << l.file << '@' << l.rect.x << ',' << l.rect.y << ',' << l.rect.z << ',' << l.rect.w;
		return key.str();
	}

	rhi::TextureHandle Build(const std::vector<TextureArrayBuilder::Layer>& layers, const TextureArrayBuilder::Options& o) {
		const int size = std::max(o.size, 1);
		const int levels = MipLevels(size);
		std::map<std::string, std::shared_future<std::shared_ptr<ImageF>>> decoded;
		for (const auto& l : layers) {
			if (decoded.count(l.file)) continue;
			decoded[l.file] = std::async(std::launch::async, [file = l.file, o]() -> std::shared_ptr<ImageF> {
				auto img = std::make_shared<ImageF>();
				if (!Decode(file, o.premultiply, o.alphaFloor, *img)) return nullptr;
				return img;
			}).share();
		}
		std::vector<std::future<std::vector<std::vector<uint8_t>>>> encoded;
		for (const auto& l : layers) {
			auto src = decoded[l.file];
			encoded.push_back(std::async(std::launch::async, [src, rect = l.rect, size, levels]() {
				std::shared_ptr<ImageF> img = src.get();
				if (!img) return std::vector<std::vector<uint8_t>>{};
				return EncodeMips(Resample(*img, rect, size), levels);
			}));
		}
		std::vector<std::vector<std::vector<uint8_t>>> data;
		for (size_t i = 0; i < encoded.size(); ++i) {
			data.push_back(encoded[i].get());
			if (data.back().empty()) {
				std::cerr << "[TextureArrayBuilder] " << o.debugName << ": cannot read " << layers[i].file << std::endl;
				return rhi::TextureHandle{};
			}
		}

		rhi::Device& gpu = rhi::Gpu();
		rhi::TextureDesc desc;
		desc.type = rhi::TextureType::Tex2DArray;
		desc.format = rhi::Format::SRGB8_A8;
		desc.width = size;
		desc.height = size;
		desc.depth = static_cast<int>(layers.size());
		desc.mipLevels = levels;
		desc.immutable = true;   // fixed storage (glTexStorage3D)
		const rhi::TextureHandle tex = gpu.CreateTexture(desc);
		for (size_t layer = 0; layer < data.size(); ++layer) {
			for (int l = 0; l < levels; ++l) {
				const int s = std::max(size >> l, 1);
				rhi::TextureRegion region;
				region.level = l;
				region.offset = glm::ivec3(0, 0, static_cast<int>(layer));
				region.extent = glm::ivec3(s, s, 1);
				gpu.UpdateTexture(tex, region, rhi::PixelFormat::RGBA, rhi::PixelType::UByte, data[layer][l].data(), 1);
			}
		}
		gpu.SetTextureFilter(tex, rhi::Filter::LinearMipLinear, rhi::Filter::Linear);
		gpu.SetTextureWrapAxis(tex, 0, rhi::Wrap::ClampToEdge);
		gpu.SetTextureWrapAxis(tex, 1, rhi::Wrap::ClampToEdge);
		if (o.anisotropic && gpu.Limits().anisotropicFiltering) {
			const float maxAniso = gpu.Limits().maxAnisotropy;
			gpu.SetTextureAnisotropy(tex, std::min(maxAniso, 16.0f));
		}
		std::cout << "[TextureArrayBuilder] " << o.debugName << ": " << layers.size() << " layers, " << size << "^2, "
			<< levels << " mips" << std::endl;
		return tex;
	}

} // namespace

namespace TextureArrayBuilder {

	rhi::TextureHandle GetOrBuild(const std::vector<Layer>& layers, const Options& options) {
		static std::map<std::string, rhi::TextureHandle> cache;
		if (layers.empty()) return rhi::TextureHandle{};
		const std::string key = CacheKey(layers, options);
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		return cache[key] = Build(layers, options);
	}

}
