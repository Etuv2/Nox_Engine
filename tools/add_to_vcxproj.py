"""Add source files to the legacy root Nox_Engine.vcxproj and .filters (the CMake build globs src/
by itself). Idempotent. Run tools/CheckVcxprojSources.py afterwards.

    python tools/add_to_vcxproj.py src/Foo.cpp src/Foo.h [--filter "Source Files\\passes"]
"""
import argparse
import io
import os
import re
import uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def enc(path):
    with open(path, 'rb') as fh:
        return 'utf-8-sig' if fh.read(3) == b'\xef\xbb\xbf' else 'utf-8'


def insert(path, block_tag, entry, anchor_pattern):
    text = io.open(path, encoding='utf-8-sig', newline='').read()
    nl = '\r\n' if '\r\n' in text else '\n'
    if entry.split(nl)[0] in text:
        return False
    m = None
    for m in re.finditer(anchor_pattern, text):
        pass
    if m is None:
        raise SystemExit('no anchor %s in %s' % (anchor_pattern, path))
    end = text.index('</%s>' % block_tag, m.end()) + len('</%s>' % block_tag) if block_tag in ('ClCompile', 'ClInclude') and text[m.end() - 2:m.end()] != '/>' else m.end()
    text = text[:end] + nl + entry + text[end:]
    io.open(path, 'w', encoding=enc(path), newline='').write(text)
    return True


def default_filter(rel, tag):
    # Mirrors the layout of Nox_Engine.vcxproj.filters: src/<dir> maps to a filter per directory.
    parts = rel.split('\\')
    if parts[0] == 'src':
        base = 'Header Files' if tag == 'ClInclude' else 'Source Files'
        return base + ''.join('\\' + p for p in parts[1:-1])
    if len(parts) > 1 and parts[0] == 'include' and parts[1].upper() == 'IMGUI':
        return 'External\\imgui'
    return 'Source Files' if tag == 'ClCompile' else 'Header Files'


def ensure_filter(text, name, nl):
    # Adds a <Filter> definition (and its parents) when the filters file does not have one yet.
    if '<Filter Include="%s">' % name in text:
        return text
    parent, _, _ = name.rpartition('\\')
    if parent:
        text = ensure_filter(text, parent, nl)
    guid = '{%s}' % str(uuid.uuid4()).upper()
    block = ('  <ItemGroup>%s    <Filter Include="%s">%s      <UniqueIdentifier>%s</UniqueIdentifier>%s'
             '    </Filter>%s  </ItemGroup>%s') % (nl, name, nl, guid, nl, nl, nl)
    first = text.index('  <ItemGroup>')
    return text[:first] + block + text[first:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--filter', default=None)
    a = ap.parse_args()
    proj = os.path.join(ROOT, 'Nox_Engine.vcxproj')
    filt = os.path.join(ROOT, 'Nox_Engine.vcxproj.filters')
    for f in a.files:
        rel = f.replace('/', '\\')
        tag = 'ClCompile' if rel.endswith(('.cpp', '.c')) else 'ClInclude'
        flt = a.filter or default_filter(rel, tag)
        added = insert(proj, tag, '    <%s Include="%s" />' % (tag, rel), r'    <%s Include="[^"]+" />' % tag)
        text = io.open(filt, encoding='utf-8-sig', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        if ('<%s Include="%s">' % (tag, rel)) not in text:
            text = ensure_filter(text, flt, nl)
            entry = '    <%s Include="%s">%s      <Filter>%s</Filter>%s    </%s>' % (tag, rel, nl, flt, nl, tag)
            last = text.rindex('    </%s>' % tag) + len('    </%s>' % tag)
            text = text[:last] + nl + entry + text[last:]
            io.open(filt, 'w', encoding=enc(filt), newline='').write(text)
        print(('added ' if added else 'present ') + rel)


if __name__ == '__main__':
    main()
