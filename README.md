[![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/OpenKJ/OpenKJ.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/OpenKJ/OpenKJ/context:cpp)
[![Copr build status](https://copr.fedorainfracloud.org/coprs/openkj/OpenKJ-unstable/package/openkjtools/status_image/last_build.png)](https://copr.fedorainfracloud.org/coprs/openkj/OpenKJ-unstable/package/openkjtools/)
[![Windows Build](https://github.com/OpenKJ/OpenKJ/actions/workflows/windows-test.yml/badge.svg)](https://github.com/OpenKJ/OpenKJ/actions/workflows/windows-test.yml)
[![Test building on macOS](https://github.com/OpenKJ/OpenKJ/actions/workflows/macos-test.yml/badge.svg)](https://github.com/OpenKJ/OpenKJ/actions/workflows/macos-test.yml)

**Downloads**  
If you are looking for installers for Windows or macOS, please visit the Downloads section at https://openkj.org

Linux users can grab OpenKJ stable versions from flathub: https://flathub.org/apps/details/org.openkj.OpenKJ

If you would like to install Linux versions of the unstable builds, please refer to the OpenKJ documentation wiki.

Documentation can be found at https://docs.openkj.org

If you need help with OpenKJ, you can reach out to support@openkj.org via email.

OpenKJ
======

Cross-platform open source karaoke show hosting software.

OpenKJ is a fully featured karaoke hosting program.
A few features:
* Save/track/load regular singers
* Key changer
* Tempo control
* EQ
* End of track silence detection (after last CDG draw command)
* Rotation ticker on the CDG display
* Option to use a custom background or display a rotating slide show on the CDG output dialog while idle
* Fades break music in and out automatically when karaoke tracks start/end
* Remote request server integration allowing singers to look up and submit songs via the web or mobile apps
* Automatic performance recording
* Autoplay karaoke mode
* Classic mode for legacy remote request workflows
* Local Mode with embedded LAN API, local user accounts, and a mobile/admin web UI integration path
* Lots of other little things

It currently handles media+g zip files (zip files containing an mp3, wav, or ogg file and a cdg file) and paired mp3 and cdg files.  I'll be adding others in the future if anyone expresses interest.  It also can play non-cdg based video files (mkv, mp4, mpg, avi) for both break music and karaoke.

Database entries for the songs are based on the file naming scheme.  I've included the common ones I've come across which should cover 90% of what's out there. Custom patterns can be also defined in the program using regular expressions.



**Requirements to build OpenKJ:**

* Qt 6.x
* CMake 3.24+
* Current GStreamer 1.x development packages
* spdlog
* taglib

**Linux**

Build using CMake from the command line or in your IDE of choice.

**Mac**

macOS is supported as a developer compile-check environment. Install a Qt 6 toolchain, `cmake`, `pkg-config`, GStreamer, taglib, and spdlog before configuring.


**Windows**

Windows 11 x64 is the primary runtime and validation target. Use a Qt 6 kit that matches your compiler, current GStreamer SDK packages, and CMake/Ninja or Qt Creator.

## Operating modes

OpenKJ now separates request behavior into two modes:

* `Classic`: legacy request-server workflow and existing openkj.org-compatible venue/account behavior
* `Local Mode`: OpenKJ hosts an embedded LAN API for the separate Karaoke UI project and stores local users in the OpenKJ database

## Local Mode API

When Local Mode is active and the embedded API is enabled, OpenKJ serves:

* `POST /api.php`
* `POST /browse`
* `GET /health`
* `GET /stats`
* `GET/POST /local/...` endpoints for queue, users, admin auth, capabilities, and event settings

The separate Karaoke UI project remains standalone. OpenKJ provides the LAN API, while OpenKJ's own admin and configuration flow remains native.
