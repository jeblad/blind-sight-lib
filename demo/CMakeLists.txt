# SPDX-License-Identifier: GPL-3.0-only
find_package(PkgConfig REQUIRED)
include(FetchContent)

pkg_check_modules(GTKMM4 REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
# gexiv2: reads IPTC/EXIF/XMP metadata from opened images (John, 2026-09-12:
# "jeg tror vi legger opp til bruk av IPTC i billedboblene") — the standard
# GNOME-ecosystem wrapper around Exiv2. Plain GObject, no gtkmm binding of
# its own — used entirely via its raw C API, same as libadwaita elsewhere in
# this file, deliberately never through Glib::wrap() (a foreign GType with
# no gtkmm wrap-table registration would hit the exact same
# "wrap_func_table != nullptr" crash documented earlier in notes.md).
# Its pkg-config name carries the API version from 0.16 on (Ubuntu
# replaced plain "gexiv2", 0.14, with "gexiv2-0.16"); first found wins.
pkg_search_module(GEXIV2 REQUIRED IMPORTED_TARGET gexiv2-0.16 gexiv2)

# GStreamer: microphone capture during Record, over whatever PipeWire
# exposes as the default input (John, 2026-09-13: "Mikrofonen vi bruker er
# bestemt av OS'et, vi bruker det vi får over pipewire") — the standard,
# well-trodden way to do this on modern Linux, rather than the raw
# PipeWire C API directly (much lower-level for the same result). Plain C
# API here too, same reasoning as gexiv2 above — gstreamermm exists but is
# unmaintained/uncommon; not worth taking on for one narrow use.
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)

# GUdev: hotplug detection for the Surface Dial (John, 2026-09-23) — the
# previous one-shot find_evdev_by_name() + open() in on_activate() never
# noticed the device reappearing after a disconnect (confirmed by reading
# the code, not assumed: the GLib IO source is torn down for good on
# HUP/ERR, and nothing ever re-runs the lookup). GUdev is the GLib/GObject
# binding for udev — the standard way to watch for device add/remove on
# Linux, same mechanism systemd-logind itself clearly uses already (see
# its own "Watching system buttons on /dev/input/eventN" log lines).
pkg_check_modules(GUDEV REQUIRED IMPORTED_TARGET gudev-1.0)

# OpenEXR: reads the precomputed depth file (Z channel, half-float) for
# the "distance at pointer" readout (John, 2026-09-24) — the same format
# demo/depth/generate_depth.py writes on the Python side, now read back
# in C++. Plain C++ API, no gtkmm/giomm wrapper exists or is needed for
# this — same "use the library's own API directly" precedent as
# gexiv2/GStreamer above.
pkg_check_modules(OPENEXR REQUIRED IMPORTED_TARGET OpenEXR)

# libsecret: reads the HuggingFace access token diarization needs, out of
# GNOME Keyring (John, 2026-09-26: "libsecret er på alle gnome-maskiner, så
# burde være safe") — rejected storing it via HuggingFace's own
# ~/.cache/huggingface/token as a plaintext, XDG-cache-semantics file
# ("`.cache` er ikke et sikkert sted å lagre data" — see notes.md,
# "Diarization setup"). The app only ever reads (secret_password_lookup_
# sync()); the token itself is stored once, by the user, via the separate
# `secret-tool` CLI (package libsecret-tools, not linked here — a setup-time
# tool, not a library dependency). Plain C API, same "no gtkmm/giomm wrapper
# exists or is needed" precedent as OpenEXR/GStreamer/gexiv2 above.
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# libpipewire: real-time synthesized audio playback (John, 2026-09-30,
# first take on the acoustic model's noise-pulse carrier) — native
# PipeWire, not GStreamer (which this file already links above, but only
# ever for bounded file operations: mic capture, Opus transcode, recorded-
# file playback, never a persistent real-time stream). notes.md's own
# architecture decision already reads "Audio out: PipeWire native
# (libpipewire) — the established path for continuous low-latency
# output" — confirmed directly with John rather than assumed, given the
# latency stakes ("det er viktig at vi får forsinkelsen under kontroll"):
# "Gå rett på libpipewire." See demo/noise_pulse.cpp for the real-time-
# safety design this depends on.
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# Glaze: JSON (de)serialization for the record/playback log — demo-only, see
# notes.md "Demo-only, same as the rest of the sensor-emulation layer." The
# library never links this. Version pinned to match ~/spaces/gnome-agent-proxy
# (inherited caution around a CUDA-interop inconsistency in a third project;
# worth staying in step with since CUDA is a possible future compute backend
# here too).
FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v7.5.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glaze)

add_executable(blind-sight-demo
    main.cpp
    recording.cpp
    protocol.cpp
    sessions.cpp
    images.cpp
    tracks.cpp
    monitoring.cpp
    noise_pulse.cpp
    transcription.cpp
    preferences.cpp
    depth.cpp
    audio.cpp
    audio_devices.cpp
    audio_routing.cpp
    panorama.cpp
    textures.cpp
    collection_editor.cpp
    collection_menu.cpp
    project_dialogs.cpp
    session_dialogs.cpp
    image_dialogs.cpp
    track_dialogs.cpp
    monitoring_dialogs.cpp
    trace_delay.cpp
    bt_output_check.cpp
)

# The per-microphone voice leveller (AGC + volume + mute), a LADSPA plugin
# that PipeWire's filter-chain module loads by absolute path — see
# voice_agc.cpp and audio_routing.cpp. Needs only ladspa.h to build.
find_path(LADSPA_INCLUDE_DIR ladspa.h)
if(NOT LADSPA_INCLUDE_DIR)
    message(FATAL_ERROR "ladspa.h not found — install it first (Ubuntu/Debian: ladspa-sdk)")
endif()
add_library(blind-sight-voice MODULE voice_agc.cpp)
target_include_directories(blind-sight-voice PRIVATE ${LADSPA_INCLUDE_DIR})
set_target_properties(blind-sight-voice PROPERTIES
    PREFIX ""
    OUTPUT_NAME "blind_sight_voice"
)
find_package(Threads REQUIRED)
target_link_libraries(blind-sight-voice PRIVATE Threads::Threads)
add_dependencies(blind-sight-demo blind-sight-voice)

# Runtime prerequisites. The app runs these as separate processes, or
# PipeWire loads them on its behalf, so the build succeeds without them —
# but audio routing, voice levelling or recording won't work. CMake can't
# install system packages, so it only reports what's missing, naming the
# Ubuntu/Debian package that provides it. See README.md, "Building and
# running".
set(BLIND_SIGHT_MISSING_RUNTIME "")
function(blind_sight_require_program program package)
    find_program(BLIND_SIGHT_PROGRAM_${program} ${program})
    if(NOT BLIND_SIGHT_PROGRAM_${program})
        list(APPEND BLIND_SIGHT_MISSING_RUNTIME "${program} (${package})")
        set(BLIND_SIGHT_MISSING_RUNTIME "${BLIND_SIGHT_MISSING_RUNTIME}" PARENT_SCOPE)
    endif()
endfunction()
function(blind_sight_require_file file package)
    if(NOT EXISTS "${file}")
        list(APPEND BLIND_SIGHT_MISSING_RUNTIME "${file} (${package})")
        set(BLIND_SIGHT_MISSING_RUNTIME "${BLIND_SIGHT_MISSING_RUNTIME}" PARENT_SCOPE)
    endif()
endfunction()

# Audio routing, device lists, and the voice levellers' filter chains.
foreach(program pipewire pw-loopback pw-link pw-cli pw-dump)
    blind_sight_require_program(${program} pipewire-bin)
endforeach()
blind_sight_require_program(wpctl wireplumber)
blind_sight_require_program(python3 python3)

# What a voice leveller's filter chain loads: PipeWire modules, and SPA
# plugins for format conversion and for hosting a LADSPA plugin.
pkg_get_variable(PIPEWIRE_MODULE_DIR libpipewire-0.3 moduledir)
pkg_get_variable(SPA_PLUGIN_DIR libspa-0.2 plugindir)
foreach(module filter-chain protocol-native client-node adapter)
    blind_sight_require_file("${PIPEWIRE_MODULE_DIR}/libpipewire-module-${module}.so"
                             libpipewire-0.3-modules)
endforeach()
foreach(plugin audioconvert/libspa-audioconvert support/libspa-support
               filter-graph/libspa-filter-graph-plugin-ladspa)
    blind_sight_require_file("${SPA_PLUGIN_DIR}/${plugin}.so" libspa-0.2-modules)
endforeach()

# GStreamer elements for recording and playback.
find_program(GST_INSPECT gst-inspect-1.0)
if(GST_INSPECT)
    foreach(element_package IN ITEMS pipewiresrc:gstreamer1.0-pipewire
                                     pipewiresink:gstreamer1.0-pipewire
                                     wavenc:gstreamer1.0-plugins-good
                                     wavparse:gstreamer1.0-plugins-good
                                     opusenc:gstreamer1.0-plugins-base
                                     oggmux:gstreamer1.0-plugins-base
                                     playbin:gstreamer1.0-plugins-base)
        string(REPLACE ":" ";" pair "${element_package}")
        list(GET pair 0 element)
        list(GET pair 1 package)
        execute_process(COMMAND ${GST_INSPECT} --exists ${element} RESULT_VARIABLE missing
                        OUTPUT_QUIET ERROR_QUIET)
        if(missing)
            list(APPEND BLIND_SIGHT_MISSING_RUNTIME "GStreamer element ${element} (${package})")
        endif()
    endforeach()
else()
    list(APPEND BLIND_SIGHT_MISSING_RUNTIME "gst-inspect-1.0 (gstreamer1.0-tools) — GStreamer elements not checked")
endif()

if(BLIND_SIGHT_MISSING_RUNTIME)
    list(JOIN BLIND_SIGHT_MISSING_RUNTIME "\n  " missing_text)
    message(WARNING "Missing at runtime (the build still works):\n  ${missing_text}")
endif()

target_link_libraries(blind-sight-demo
    PRIVATE
        PkgConfig::GTKMM4
        PkgConfig::LIBADWAITA
        PkgConfig::GEXIV2
        PkgConfig::GSTREAMER
        PkgConfig::GUDEV
        PkgConfig::OPENEXR
        PkgConfig::LIBSECRET
        PkgConfig::PIPEWIRE
        glaze::glaze
)

# Dev-time resource lookup: point at the source tree's resource/ directory so
# the demo can be run straight from the build directory. Not a packaging
# story — revisit (GResource, or an install-time data dir) once the app does
# more than show a splash image. resource/ itself is for things the app uses
# internally (currently just the splash image) — the sample photosphere/
# panorama images a tester opens live in images/ instead (John,
# 2026-09-12: "Resource-katalogen bør vel bare inneholde ting vi bruker
# internt... tenkte å lage en examples-katalog" — resource/ had started
# mixing the two; that directory was later renamed examples/ -> images/).
target_compile_definitions(blind-sight-demo
    PRIVATE
        BLIND_SIGHT_VERSION="${PROJECT_VERSION}"
        BLIND_SIGHT_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resource"
        BLIND_SIGHT_IMAGES_DIR="${CMAKE_SOURCE_DIR}/images"
        BLIND_SIGHT_DEPTH_DIR="${CMAKE_SOURCE_DIR}/demo/depth"
        BLIND_SIGHT_TRANSCRIBE_DIR="${CMAKE_SOURCE_DIR}/demo/transcribe"
        BLIND_SIGHT_TRACE_DELAY_SCRIPT="${CMAKE_SOURCE_DIR}/demo/trace_delay.py"
        BLIND_SIGHT_CHECK_BT_OUTPUT_SCRIPT="${CMAKE_SOURCE_DIR}/demo/check_bt_output.py"
        BLIND_SIGHT_DUMP_AUDIO_NODES_SCRIPT="${CMAKE_SOURCE_DIR}/demo/dump_pipewire_audio_nodes.py"
        BLIND_SIGHT_VOICE_PLUGIN="$<TARGET_FILE:blind-sight-voice>"
)

# Gettext: every po/<lang>.po is compiled with msgfmt into the build tree's
# locale/<lang>/LC_MESSAGES/, so the catalogs always match the .po files and
# nothing generated lands in the source tree. Updating the .po files from the
# sources (xgettext/msgmerge, see po/POTFILES.in) stays a manual step.
# Swap the lookup for CMAKE_INSTALL_FULL_LOCALEDIR once this target has a
# real install() step.
find_program(MSGFMT_EXECUTABLE msgfmt)
if(NOT MSGFMT_EXECUTABLE)
    message(FATAL_ERROR "msgfmt not found — install it first (Ubuntu/Debian: gettext)")
endif()
file(GLOB BLIND_SIGHT_PO_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/po/*.po")
set(BLIND_SIGHT_MO_FILES "")
foreach(po_file IN LISTS BLIND_SIGHT_PO_FILES)
    get_filename_component(lang "${po_file}" NAME_WE)
    set(mo_dir "${CMAKE_BINARY_DIR}/locale/${lang}/LC_MESSAGES")
    set(mo_file "${mo_dir}/blind-sight-demo.mo")
    add_custom_command(
        OUTPUT "${mo_file}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${mo_dir}"
        COMMAND ${MSGFMT_EXECUTABLE} --check -o "${mo_file}" "${po_file}"
        DEPENDS "${po_file}"
        COMMENT "Compiling ${lang} translation"
    )
    list(APPEND BLIND_SIGHT_MO_FILES "${mo_file}")
endforeach()
add_custom_target(blind-sight-demo-translations ALL DEPENDS ${BLIND_SIGHT_MO_FILES})
add_dependencies(blind-sight-demo blind-sight-demo-translations)
target_compile_definitions(blind-sight-demo
    PRIVATE
        BLIND_SIGHT_LOCALE_DIR="${CMAKE_BINARY_DIR}/locale"
)

# GSettings: the schema must be compiled, or Gio::Settings::create() fails
# outright. Compiled into demo/data/, next to its source, where
# GSETTINGS_SCHEMA_DIR (set at runtime in main(), see BLIND_SIGHT_SCHEMA_DIR
# below) points GLib at it without installing the schema system-wide.
find_program(GLIB_COMPILE_SCHEMAS_EXECUTABLE glib-compile-schemas REQUIRED)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/data/gschemas.compiled
    COMMAND ${GLIB_COMPILE_SCHEMAS_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/data
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/io.github.jeblad.blind-sight-demo.gschema.xml
    COMMENT "Compiling GSettings schema"
)
add_custom_target(blind-sight-demo-schema ALL
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/gschemas.compiled
)
add_dependencies(blind-sight-demo blind-sight-demo-schema)

target_compile_definitions(blind-sight-demo
    PRIVATE
        BLIND_SIGHT_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
