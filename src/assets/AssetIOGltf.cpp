#include "AssetIOGltf.h"

#include "AssetIO.h"

#include <cstring>

namespace AssetIO {
namespace {

	bool GltfFileExists(const std::string& path, void*)
	{
		if (path.empty()) return false;
		auto st = AssetIO::Stat(path);
		return st.has_value() && !st->isDirectory;
	}

	std::string GltfExpandFilePath(const std::string& path, void*)
	{
		// Invalid references (absolute paths, drive letters, URIs, root escapes) expand to "" and never match.
		return AssetIO::Normalize(path).value_or(std::string());
	}

	bool GltfReadWholeFile(std::vector<unsigned char>* out, std::string* err, const std::string& path, void*)
	{
		std::string error;
		Buffer buffer = AssetIO::ReadAll(path, &error);
		if (!buffer) {
			if (err) *err = error;
			return false;
		}
		out->assign(buffer->begin(), buffer->end());
		return true;
	}

	bool GltfWriteWholeFile(std::string* err, const std::string& path, const std::vector<unsigned char>&, void*)
	{
		if (err) *err = "glTF writes are not allowed through asset I/O: " + path;
		return false;
	}

	bool GltfGetFileSize(size_t* sizeOut, std::string* err, const std::string& path, void*)
	{
		auto st = AssetIO::Stat(path);
		if (!st || st->isDirectory) {
			if (err) *err = "not a file: " + path;
			return false;
		}
		*sizeOut = static_cast<size_t>(st->size);
		return true;
	}

} // namespace

	tinygltf::FsCallbacks MakeGltfFsCallbacks()
	{
		tinygltf::FsCallbacks callbacks{};
		callbacks.FileExists = &GltfFileExists;
		callbacks.ExpandFilePath = &GltfExpandFilePath;
		callbacks.ReadWholeFile = &GltfReadWholeFile;
		callbacks.WriteWholeFile = &GltfWriteWholeFile;
		callbacks.GetFileSizeInBytes = &GltfGetFileSize;
		callbacks.user_data = nullptr;
		return callbacks;
	}

} // namespace AssetIO
