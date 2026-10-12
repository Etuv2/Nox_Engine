"""Checks that the legacy Nox_Engine.vcxproj (and .vcxproj.filters) list the sources CMake builds.

CMakeLists.txt is the authority. The expected sets are derived the way CMakeLists.txt derives them:

- Nox_Engine.exe: src/main.cpp plus NoxEngineCore (recursive glob of src/*.cpp and
  include/IMGUI/*.cpp, minus main.cpp and the racing list) plus NoxRacingLegacy (the explicit
  NOX_RACING_LEGACY_SOURCES list, existing files only), plus the explicit nox_physfs list and the
  nox_lua glob (external/lua-5.5.1/src/*.c minus lua.c and luac.c).
- ClInclude: every src/**/*.h and src/**/*.hpp.

Excluded on purpose (other CMake targets, not Nox_Engine.exe): tools/cook/main.cpp (nox_cook) and
tests/native/*.cpp (nox_tests).

Also checked: every listed item has a <Filter> entry in the .filters file and the reverse; racing
sources and their headers sit in the "Racing (legacy)" filter; external sources sit under "External";
Lua sources are compiled as C++ (CompileAs CompileAsCpp).

Exit status: 0 when everything matches, 1 with a list of differences otherwise.

    python tools/CheckVcxprojSources.py
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NS = '{http://schemas.microsoft.com/developer/msbuild/2003}'
RACING_FILTER = 'Racing (legacy)'
EXTERNAL_FILTER_PREFIX = 'External'


def rel(path):
    return os.path.relpath(path, ROOT).replace('\\', '/')


def read_text(path):
    with open(path, 'r', encoding='utf-8-sig', newline='') as fh:
        return fh.read()


def cmake_lists():
    text = read_text(os.path.join(ROOT, 'CMakeLists.txt'))
    racing = re.search(r'set\(NOX_RACING_LEGACY_SOURCES(.*?)\)', text, re.S).group(1)
    racing = [m.group(1) for m in re.finditer(r'\$\{NOX_ENGINE_SRC_DIR\}/([^"\s]+)', racing)]
    physfs_block = re.search(r'add_library\(nox_physfs STATIC(.*?)\)', text, re.S).group(1)
    physfs = [m.group(1) for m in re.finditer(r'\$\{NOX_PHYSFS_DIR\}/([^"\s]+)', physfs_block)]
    return racing, physfs


def expected():
    racing_names, physfs_names = cmake_lists()
    src = os.path.join(ROOT, 'src')
    engine = {rel(p) for p in glob.glob(os.path.join(src, '**', '*.cpp'), recursive=True)}
    engine |= {rel(p) for p in glob.glob(os.path.join(ROOT, 'include', 'IMGUI', '**', '*.cpp'), recursive=True)}
    engine.discard('src/main.cpp')
    racing = {'src/' + n for n in racing_names if os.path.exists(os.path.join(src, n))}
    engine -= racing
    physfs = {'external/physfs-3.2.0/src/' + n for n in physfs_names}
    lua_dir = os.path.join(ROOT, 'external', 'lua-5.5.1', 'src')
    lua = {rel(p) for p in glob.glob(os.path.join(lua_dir, '*.c'))}
    lua -= {'external/lua-5.5.1/src/lua.c', 'external/lua-5.5.1/src/luac.c'}
    compile_set = engine | racing | physfs | lua | {'src/main.cpp'}
    headers = {rel(p) for ext in ('*.h', '*.hpp') for p in glob.glob(os.path.join(src, '**', ext), recursive=True)}
    return compile_set, headers, racing, lua | physfs


def vcxproj_items(path):
    tree = ET.parse(path)
    compile_items, include_items = {}, {}
    for group in tree.getroot().findall(NS + 'ItemGroup'):
        for child in group:
            tag = child.tag.replace(NS, '')
            if tag not in ('ClCompile', 'ClInclude'):
                continue
            name = child.get('Include').replace('\\', '/')
            target = compile_items if tag == 'ClCompile' else include_items
            target[name] = child
    return compile_items, include_items


def filters_map(path):
    tree = ET.parse(path)
    result = {}
    for group in tree.getroot().findall(NS + 'ItemGroup'):
        for child in group:
            tag = child.tag.replace(NS, '')
            if tag not in ('ClCompile', 'ClInclude'):
                continue
            name = child.get('Include').replace('\\', '/')
            filt = child.find(NS + 'Filter')
            result[name] = filt.text if filt is not None else ''
    return result


def main():
    compile_set, headers, racing, external = expected()
    proj_compile, proj_include = vcxproj_items(os.path.join(ROOT, 'Nox_Engine.vcxproj'))
    filt = filters_map(os.path.join(ROOT, 'Nox_Engine.vcxproj.filters'))
    problems = []

    def diff(label, want, have):
        for name in sorted(want - have):
            problems.append('%s missing from vcxproj: %s' % (label, name))
        for name in sorted(have - want):
            problems.append('%s in vcxproj but not built by CMake: %s' % (label, name))

    diff('ClCompile', compile_set, set(proj_compile))
    diff('ClInclude', headers, set(proj_include))

    # Filter bookkeeping: every item has a filter, and the filters file lists no other items.
    for name in sorted(set(proj_compile) | set(proj_include)):
        if name not in filt:
            problems.append('no <Filter> entry in .filters: %s' % name)
    for name in sorted(set(filt) - set(proj_compile) - set(proj_include)):
        problems.append('.filters lists an item missing from the vcxproj: %s' % name)

    for name in sorted(racing):
        base = name[:-4]
        for ext in ('.cpp', '.h'):
            item = base + ext
            if item in filt and not (filt[item] == RACING_FILTER or filt[item].startswith(RACING_FILTER + '\\')):
                problems.append('racing source/header not in "%s" filter: %s (filter %r)' % (RACING_FILTER, item, filt[item]))
    for name in sorted(external):
        if name in proj_compile and not filt.get(name, '').startswith(EXTERNAL_FILTER_PREFIX):
            problems.append('external source not in "External" filter: %s' % name)
    for name in sorted(external):
        if name in proj_compile and name.endswith('.c') and 'lua-5.5.1' in name:
            compile_as = proj_compile[name].find(NS + 'CompileAs')
            if compile_as is None or compile_as.text != 'CompileAsCpp':
                problems.append('Lua source not compiled as C++ (CompileAs): %s' % name)

    if problems:
        print('vcxproj source check FAILED (%d problem(s)):' % len(problems))
        for line in problems:
            print('  ' + line)
        return 1
    print('vcxproj source check OK: %d ClCompile, %d ClInclude entries match CMakeLists.txt'
          % (len(compile_set), len(headers)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
