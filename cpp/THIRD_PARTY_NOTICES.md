# Third-party notices

The report wire format and portions of the formatting/result semantics are
adapted from Robot Framework 6.1.1, Copyright 2008–2015 Nokia Networks and
Copyright 2016– Robot Framework Foundation, licensed under Apache License 2.0.
See ROBOT_FRAMEWORK_LICENSE.txt. The C++ implementation is a modified work and
is not an official Robot Framework distribution.

The existing `resources/robot/htmldata` files are embedded unchanged. Their
original copyright and license notices are retained, including bundled
jQuery, jQuery plugins, and JSXCompressor notices. `NOTICE` text inside
embedded resources is not removed.

The executable also links to libxml2 and zlib under their respective upstream
licenses. These are system dependencies on the verified macOS build; this
module does not vendor either library.

The desktop application uses shared Qt 6 Widgets, Gui, Core, Concurrent and
platform/image plugins. CI uses Qt 6.11.2. Qt's applicable LGPLv3/GPLv3 license
texts are included in `licenses/`; source is available at
https://code.qt.io/cgit/qt/qtbase.git/ and the Qt source release archives.
Qt remains dynamically linked, including inside the Windows self-extracting
package. The Windows package can be extracted with the launcher source to
replace runtime libraries and rebuild the payload.

libxlsxwriter 1.2.4 (John McNamara, BSD-2-Clause) and miniz 3.1.2 (MIT) are
downloaded from pinned upstream archives with SHA256 verification and linked
statically. Their original license files are copied into the application
package. libxlsxwriter's vendored minizip/tmpfileplus/MD5/dtoa notices are
retained in its source archive:

- https://github.com/jmcnamara/libxlsxwriter/tree/v1.2.4
- https://github.com/richgel999/miniz/tree/3.1.2

Python, PyQt6, Robot Framework, openpyxl, Node and Playwright are development
and comparison tools only and are not shipped in the native runtime.
