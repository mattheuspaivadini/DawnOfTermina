# DekUI modules (third-party)

Headers copied **unmodified** from [DekUI](https://github.com/tgacquin/mm3d-dekui) (`src/mm3d/`),
by Thomas (tgacquin) and the "MM3D presentation contributors", licensed GPL-2.0-or-later
(see each file's SPDX line; DawnOfTermina as a whole is GPL-3.0-or-later). They operate on game
memory through a small `Read`/`Write` interface; the glue that calls them from the recompiled
game is `src/recomp/dekui.cpp`. Keep them verbatim so updates can be diffed against upstream.

Exception: the message-text data headers (`single_screen_text_data.h`,
`single_screen_localized_data.h`, `classic_*text*.h`) are copied by `tools/dekui_strip_text.py`,
which empties the verbatim game text they carry (the originals of the replaced messages and the
Classic Saving dialogues); DekUI's replacement text, sizes and hashes stay.
