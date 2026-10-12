# Pinned third-party source dependencies

These libraries are compiled from source by `CMakeLists.txt` with the engine toolchain and
linked statically. Only the files needed to build them (plus their licenses) are kept.
Update a dependency by replacing its whole folder, recording the new archive hash here, and
rerunning the engine and asset I/O/scripting tests.

| Library | Version | Upstream archive | SHA-256 of archive | License |
| --- | --- | --- | --- | --- |
| Lua | 5.5.1 (2026-07-24) | https://www.lua.org/ftp/lua-5.5.1.tar.gz | `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` (matches lua.org) | MIT, `lua-5.5.1/LICENSE.txt` |
| PhysicsFS | 3.2.0 (tag `release-3.2.0`) | https://github.com/icculus/physfs/archive/refs/tags/release-3.2.0.tar.gz | `1991500eaeb8d5325e3a8361847ff3bf8e03ec89252b7915e1f25b3f8ab5d560` | zlib, `physfs-3.2.0/LICENSE.txt` |

Kept from the archives: Lua `src/` (minus `Makefile`), `README`, `doc/readme.html`; PhysicsFS
`src/` (minus `Makefile.os2`), `LICENSE.txt`, `README.txt`. `lua-5.5.1/LICENSE.txt` is the
notice from the end of `lua.h`, copied verbatim. The standalone `lua.c`/`luac.c` programs are
not compiled into the engine.

## Patches and build notes

- Lua 5.5.1: no source patches. The one upstream-listed 5.5.1 bug (negative parameter left-shifted
  in `luaO_applyparam`, www.lua.org/bugs.html) is unreachable in NOX: the only call path is
  `lua_gc` with the constant 100. Lua is compiled as C++ (`LANGUAGE CXX`) so errors unwind with
  exceptions; see `docs/lua_api.md`.
- PhysicsFS 3.2.0: no source patches. Only the ZIP and directory archivers are compiled
  (`PHYSFS_SUPPORTS_DEFAULT=0`, `PHYSFS_SUPPORTS_ZIP=1`, `PHYSFS_NO_CDROM_SUPPORT=1`).
