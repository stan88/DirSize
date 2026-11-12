# Installer language files (third party)

These files are **not** part of DirSize. They are copied unchanged from the
Inno Setup source repository and are used only by the installer
(`installer/dirsize.iss`).

| File | Source | SHA-256 |
|---|---|---|
| `ChineseTraditional.isl` | [jrsoftware/issrc `Files/Languages/ChineseTraditional.isl`](https://github.com/jrsoftware/issrc/blob/6ef32198ef1f7b7b375cd4b6b90896c2a58eb4c2/Files/Languages/ChineseTraditional.isl), commit `6ef32198ef1f7b7b375cd4b6b90896c2a58eb4c2` | `031684FC769259291FD563338B5ABE20B7753C88AB5A2976B83A80788DEB8455` |
| `INNO-SETUP-LICENSE.txt` | [jrsoftware/issrc `license.txt`](https://github.com/jrsoftware/issrc/blob/6ef32198ef1f7b7b375cd4b6b90896c2a58eb4c2/license.txt), same commit | `2E5346868C2A18434489824E11D65C3031620F792FEFC415D05F19CD441ABF5C` |

- `ChineseTraditional.isl`: the Traditional Chinese messages of the Inno
  Setup wizard, an official translation listed at
  <https://jrsoftware.org/files/istrans/>. Its translators are named in the
  file's header. The file states no license of its own; it is distributed
  in the Inno Setup repository, whose license is `INNO-SETUP-LICENSE.txt`.
- Inno Setup 6.7.3, the version used to build the installer, did not ship
  this file yet (it was an unofficial translation at that release). The copy
  here compiles with 6.7.3 without warnings.
- Line endings are kept as published (`.gitattributes`), so the SHA-256
  values above stay valid.

To update: download both files from a newer commit, check that the
installer compiles without warnings, and update the table.
