#include "AssetIOSdl.h"

#include <cstring>

namespace AssetIO {
namespace {

	struct MemoryState {
		Buffer data;
		uint64_t pos = 0;
	};

	MemoryState* State(SDL_RWops* ops)
	{
		return static_cast<MemoryState*>(ops->hidden.unknown.data1);
	}

	Sint64 MemSize(SDL_RWops* ops)
	{
		MemoryState* s = State(ops);
		return s && s->data ? static_cast<Sint64>(s->data->size()) : -1;
	}

	Sint64 MemSeek(SDL_RWops* ops, Sint64 offset, int whence)
	{
		MemoryState* s = State(ops);
		if (!s || !s->data) return -1;
		const Sint64 size = static_cast<Sint64>(s->data->size());
		Sint64 base = 0;
		if (whence == RW_SEEK_CUR) base = static_cast<Sint64>(s->pos);
		else if (whence == RW_SEEK_END) base = size;
		else if (whence != RW_SEEK_SET) return -1;
		const Sint64 target = base + offset;
		if (target < 0 || target > size) return -1;
		s->pos = static_cast<uint64_t>(target);
		return target;
	}

	size_t MemRead(SDL_RWops* ops, void* ptr, size_t size, size_t maxnum)
	{
		MemoryState* s = State(ops);
		if (!s || !s->data || size == 0 || maxnum == 0) return 0;
		const uint64_t available = s->pos < s->data->size() ? s->data->size() - s->pos : 0;
		const uint64_t wanted = static_cast<uint64_t>(size) * maxnum;
		const uint64_t count = wanted < available ? wanted : available;
		if (count > 0) std::memcpy(ptr, s->data->data() + s->pos, static_cast<size_t>(count));
		s->pos += count;
		return static_cast<size_t>(count / size);
	}

	size_t MemWrite(SDL_RWops*, const void*, size_t, size_t)
	{
		return 0; // read-only
	}

	int MemClose(SDL_RWops* ops)
	{
		delete State(ops);
		ops->hidden.unknown.data1 = nullptr;
		SDL_FreeRW(ops);
		return 0;
	}

} // namespace

	SDL_RWops* OpenRWopsFromBuffer(Buffer data)
	{
		if (!data) return nullptr;
		SDL_RWops* ops = SDL_AllocRW();
		if (!ops) return nullptr;
		ops->size = &MemSize;
		ops->seek = &MemSeek;
		ops->read = &MemRead;
		ops->write = &MemWrite;
		ops->close = &MemClose;
		ops->type = SDL_RWOPS_UNKNOWN;
		auto* state = new MemoryState();
		state->data = std::move(data);
		ops->hidden.unknown.data1 = state;
		return ops;
	}

	SDL_RWops* OpenRWops(std::string_view path, std::string* error)
	{
		Buffer data = ReadAll(path, error);
		return data ? OpenRWopsFromBuffer(std::move(data)) : nullptr;
	}

} // namespace AssetIO
