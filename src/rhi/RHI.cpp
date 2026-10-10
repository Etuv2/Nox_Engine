#include "RHI.h"
#include "gl/GLDevice.h"

#include <cassert>
#include <iostream>

namespace rhi {

	bool IsDepthFormat(Format f) {
		switch (f) {
		case Format::D16: case Format::D24: case Format::D32F: case Format::D24S8: case Format::D32FS8: return true;
		default: return false;
		}
	}

	bool HasStencil(Format f) { return f == Format::D24S8 || f == Format::D32FS8; }

	bool IsIntegerFormat(Format f) {
		switch (f) {
		case Format::R8UI: case Format::R16UI: case Format::R32I: case Format::R32UI:
		case Format::RG8UI: case Format::RG16UI: case Format::RG32UI:
		case Format::RGB8UI: case Format::RGB16UI: case Format::RGB32UI:
		case Format::RGBA8UI: case Format::RGBA16UI: case Format::RGBA32UI:
			return true;
		default:
			return false;
		}
	}

	uint32_t FormatChannelCount(Format f) {
		switch (f) {
		case Format::R8: case Format::R8UI: case Format::R16: case Format::R16F: case Format::R16UI:
		case Format::R32F: case Format::R32I: case Format::R32UI:
		case Format::D16: case Format::D24: case Format::D32F:
			return 1;
		case Format::RG8: case Format::RG8UI: case Format::RG16: case Format::RG16F: case Format::RG16UI:
		case Format::RG32F: case Format::RG32UI: case Format::D24S8: case Format::D32FS8:
			return 2;
		case Format::RGB8: case Format::RGB8UI: case Format::RGB16F: case Format::RGB16UI: case Format::RGB32F:
		case Format::RGB32UI: case Format::R11G11B10F: case Format::SRGB8:
			return 3;
		default:
			return 4;
		}
	}

	const char* FormatName(Format f) {
		switch (f) {
		case Format::Unknown: return "Unknown";
		case Format::R8: return "R8"; case Format::R8UI: return "R8UI"; case Format::R16: return "R16";
		case Format::R16F: return "R16F"; case Format::R16UI: return "R16UI"; case Format::R32F: return "R32F";
		case Format::R32I: return "R32I"; case Format::R32UI: return "R32UI";
		case Format::RG8: return "RG8"; case Format::RG8UI: return "RG8UI"; case Format::RG16: return "RG16";
		case Format::RG16F: return "RG16F"; case Format::RG16UI: return "RG16UI"; case Format::RG32F: return "RG32F";
		case Format::RG32UI: return "RG32UI";
		case Format::RGB8: return "RGB8"; case Format::RGB8UI: return "RGB8UI"; case Format::RGB16F: return "RGB16F";
		case Format::RGB16UI: return "RGB16UI"; case Format::RGB32F: return "RGB32F"; case Format::RGB32UI: return "RGB32UI";
		case Format::R11G11B10F: return "R11G11B10F";
		case Format::RGBA8: return "RGBA8"; case Format::RGBA8UI: return "RGBA8UI"; case Format::RGBA16: return "RGBA16";
		case Format::RGBA16F: return "RGBA16F"; case Format::RGBA16UI: return "RGBA16UI"; case Format::RGBA32F: return "RGBA32F";
		case Format::RGBA32UI: return "RGBA32UI"; case Format::RGB10A2: return "RGB10A2";
		case Format::SRGB8: return "SRGB8"; case Format::SRGB8_A8: return "SRGB8_A8";
		case Format::D16: return "D16"; case Format::D24: return "D24"; case Format::D32F: return "D32F";
		case Format::D24S8: return "D24S8"; case Format::D32FS8: return "D32FS8";
		}
		return "?";
	}

	uint32_t PrepareWindow(Backend backend, bool debug) {
		switch (backend) {
		case Backend::OpenGL: return gl::PrepareWindow(debug);
		}
		return 0;
	}

	std::unique_ptr<Device> CreateDevice(const DeviceCreateInfo& info) {
		switch (info.backend) {
		case Backend::OpenGL: return gl::CreateDevice(info);
		}
		return nullptr;
	}

	namespace {
		Device* g_device = nullptr;
	}

	Device& Gpu() {
		assert(g_device && "rhi::Gpu() used before the device was created");
		return *g_device;
	}

	bool HasGpu() { return g_device != nullptr; }

	void SetGpu(Device* device) { g_device = device; }

} // namespace rhi
