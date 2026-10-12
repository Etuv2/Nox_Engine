#pragma once
// SDL_RWops adapter over AssetIO buffers. Use for IMG_Load_RW, TTF_OpenFontRW and Mix_LoadWAV_RW.
// The returned RWops owns a shared reference to the bytes; SDL_RWclose releases it. Pass freesrc=1 to
// the SDL loaders so the RWops is closed with the resource (TTF fonts keep it until TTF_CloseFont).
#include "AssetIO.h"

#include <SDL/SDL.h>

#include <string>
#include <string_view>

namespace AssetIO {
	SDL_RWops* OpenRWopsFromBuffer(Buffer data);
	// Reads a logical path through AssetIO and wraps it. nullptr on failure (error set).
	SDL_RWops* OpenRWops(std::string_view path, std::string* error = nullptr);
}
