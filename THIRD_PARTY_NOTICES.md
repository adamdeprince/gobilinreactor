# GoblinReactor component notices

GoblinReactor includes software from the following projects. Original copyrights,
license conditions and disclaimers remain in the supplied source and notice files.
The app's **Open-source notices** menu exposes the packaged license texts.

| Component | License information | Source identity |
| --- | --- | --- |
| Original application and adapters | GPL-3.0-or-later, with component exceptions in LICENSE | Release project source |
| Linux UML kernel and execution stub | GPL-2.0-only; kernel SPDX notices and syscall exceptions apply to their respective files | `uml/sources.lock.json`, `uml/port-patches/`, `uml/patches/`, `uml/patch-kernel.py` |
| passt and port helper | GPL-2.0-or-later and BSD-3-Clause, per file | `uml/sources.lock.json`, `uml/patch-passt.py` |
| kitty terminal | GPL-3.0-only | `terminal/sources.lock.json`, `terminal/patches/kitty-android.patch` |
| CPython | Python/PSF license and included historical notices | `terminal/sources.lock.json` |
| FreeType, HarfBuzz, libpng, Little CMS, xxHash, SIMDe, Cairo, Pixman, Fontconfig, Expat | Their individual licenses, included under `terminal-licenses/dependencies/` in the APK | `terminal/sources.lock.json` |
| Android Python dependencies: bzip2, libffi, OpenSSL, SQLite, XZ | Their individual licenses; Android patches and build recipes accompany the sources | `terminal/python-deps.lock.json`, `release/extra-sources.lock.json` |
| Debian seed, deployment packages and boot tools | Per-package copyright and licensing; includes GPL, LGPL, BSD and other terms | `release/debian-sources.lock.json`, `release/boot-packages.lock.json` |
| Android NDK runtime code | Android and LLVM component notices, including applicable runtime exceptions | NDK 29.0.14206865; `release/notices/android/` |

The separately executed kernel and Debian guest retain their own licenses.
The Android terminal incorporating kitty is distributed under GPLv3. Application
source is supplied so recipients can modify and rebuild it, including relinking
bundled libraries. The source handoff includes upstream source archives, Debian
source packages with packaging changes, local modifications, and build scripts.

Debian copyright files are also present inside the guest under
`/usr/share/doc/PACKAGE/copyright`; shared license texts are under
`/usr/share/common-licenses`. The handoff retains notices for every bundled
version, including seed packages later superseded by deployment packages.

Distribute the matching corresponding-source archive and NOTICES archive beside
the APK and App Bundle, with equal access and no additional charge. Keep their
SHA256SUMS and SOURCE-MANIFEST.json together. A link to a moving upstream branch
is not a substitute for the matching source handoff.

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.
