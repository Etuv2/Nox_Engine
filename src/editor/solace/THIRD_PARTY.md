# Third-party notices for the Solace UI kit

This folder (`src/editor/solace/`) and `fonts/solace/` adapt code, font files and icon
geometry from **Free-Solace-ImGui-Interface** by Pondot.

- Source repository: https://github.com/poncippg-spec/Free-Solace-ImGui-Interface
- Pinned revision: `6b88e2ed65b06ffbfa26b1aa4561579707fbeb4f`
- Licence of the Solace code: MIT, Copyright (c) 2026 Pondot (full text below)

Solace's own third-party components are used as follows:

| Component | Used by NOX as | Licence |
| --- | --- | --- |
| Geist (Vercel, basement.studio) | `fonts/solace/Geist-*.ttf`, copied from the byte arrays in Solace `src/generated/fonts/geist_data.cpp` | SIL Open Font License 1.1 (full text in `fonts/solace/OFL-Geist.txt`, copied below) |
| Lucide (Lucide Contributors) | Stroke path data for the icon set in `SolaceIcons.cpp` | ISC (full text below) |

Solace's Dear ImGui, FreeType and stb_image components are not used. NOX uses its own vendored
Dear ImGui 1.91.9-docking (`include/IMGUI`) and stb_truetype.

## Adapted files

| NOX file | Adapted from Solace | Notes |
| --- | --- | --- |
| `SolaceSvg.cpp` | `src/ui/foundation/svg.cpp`, `svg.h` | Path parser and stroker, unchanged apart from includes and declarations moved to `SolaceInternal.h`. |
| `SolaceIcons.cpp` | `src/ui/foundation/icons.cpp` | Path data for search, eye, eye-off, settings, sun, moon, check, cross, info, alert, panel-left, user, mail, lock, sparkles, bell, command, chevron-down/right. Other icons were written from the Lucide set. |
| `SolaceTheme.cpp` | `src/ui/foundation/theme.h`, `theme.cpp`, `src/ui/foundation/runtime.cpp` (`apply_style`) | Palette values, spacing and rounding scale, colour roles. ImGuiCol mapping and the TTF font loader are new. |
| `SolaceControls.cpp` | `src/ui/controls/widgets.cpp`, `form_controls.cpp`, `src/ui/screens/shell_layout.cpp` | Button, icon button, toolbar button, toggle, segmented control, search field, section header, property grid, card, nav item. Rebuilt on ImGui items and draw lists. |
| `SolaceFeedback.cpp` | `src/ui/controls/widgets.cpp` (badge, toast), `src/ui/controls/loader.cpp` | Badge, spinner, progress bars, dirty dot, tooltip, toasts. |
| `SolaceCore.cpp` | `src/ui/foundation/motion/motion.h`, `primitives.cpp`, `runtime.h` (animation store) | Colour mix helpers and per-ID animation store; the motion curves are reimplemented as exponential approach, not Solace springs. |
| `fonts/solace/Geist-*.ttf` | `src/generated/fonts/geist_data.cpp` | Same font data, written out as files. |

Every adapted block carries an "Adapted from Solace <file>" comment.

## Solace MIT licence (copied verbatim from LICENSE)

MIT License

Copyright (c) 2026 Pondot

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Geist licence (SIL Open Font License 1.1, copied from thirdparty/geist/LICENSE.txt)

```
Copyright (c) 2023 Vercel, in collaboration with basement.studio

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION AND CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Lucide licence (ISC, copied from thirdparty/lucide/LICENSE)

```
ISC License

Copyright (c) 2022, Lucide Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```
