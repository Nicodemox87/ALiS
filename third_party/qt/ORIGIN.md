# Qt minimal platform plugin — 5.15.2

Copyright (C) 2016 The Qt Company Ltd. and other contributors identified in the source.

ALiS redistributes unmodified `plugins/platforms/qminimal.dll` from the Qt 5.15.2 MSVC2019_64 SDK. It lets the separate block worker use Qt Widgets internally without opening a visible processing window. Other Qt libraries are supplied by the separately installed CloudCompare host.

The minimal plugin is available under LGPL-3.0, with the GPL alternatives stated in its source headers. Original `LICENSE.LGPL3`, `LICENSE.GPL2` and `LICENSE.GPL3` are retained here. This notice does not relicense Qt.

Corresponding source: `qtbase-everywhere-src-5.15.2.tar.xz`, available as an additional asset of the ALiS alpha.5.7 release and from [the Qt archive](https://download.qt.io/archive/qt/5.15/5.15.2/submodules/qtbase-everywhere-src-5.15.2.tar.xz).

SHA-256: `909fad2591ee367993a75d7e2ea50ad4db332f05e1c38dd7a5a274e156a4e0f8`.

Implementation: `src/plugins/platforms/minimal`. No ALiS modifications to Qt are required. Build Qt Base with the matching Windows MSVC x64 shared-library configuration, or build this plugin using the matching SDK qmake followed by nmake. Preserve the platform plugin's ABI and dependencies. The resulting DLL may replace `CloudCompare/platforms/qminimal.dll` while the application is closed. ALiS imposes no restriction on replacing compatible LGPL libraries or reverse engineering for debugging modifications to them. See the included licences for the full terms.
