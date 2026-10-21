#pragma once
// Animator marker events for Lua: the markers the animation system fired since the last update are queued as the
// "anim.event" world event (nox.on("anim.event", function(ctx, e) ... end)). Payload: { entity = handle,
// name = marker name, clip = clip name, phase = marker time in [0, 1) }. Contract: docs/animation.md, docs/lua_api.md.

namespace NoxLua {

	class LuaWorld;

	// Moves the Animator marker events collected since the last call into the world's event queue. Called by
	// LuaWorld::Update before its events are dispatched.
	void PumpAnimationEvents(LuaWorld& world);
}
