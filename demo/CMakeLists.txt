# SPDX-License-Identifier: GPL-3.0-only
find_package(PkgConfig REQUIRED)

pkg_check_modules(GTKMM4 REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)

add_executable(blind-sight-demo
    main.cpp
)

target_link_libraries(blind-sight-demo
    PRIVATE
        PkgConfig::GTKMM4
        PkgConfig::LIBADWAITA
)

# Dev-time resource lookup: point at the source tree's resource/ directory so
# the demo can be run straight from the build directory. Not a packaging
# story — revisit (GResource, or an install-time data dir) once the app does
# more than show a splash image.
target_compile_definitions(blind-sight-demo
    PRIVATE
        BLIND_SIGHT_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resource"
)

# Gettext: dev-time locale lookup points straight at the source tree's
# locale/ (po/*.po compiled via msgfmt, see po/POTFILES.in), same approach as
# the resource dir above. Swap for CMAKE_INSTALL_FULL_LOCALEDIR once this
# target has a real install() step.
target_compile_definitions(blind-sight-demo
    PRIVATE
        BLIND_SIGHT_LOCALE_DIR="${CMAKE_SOURCE_DIR}/locale"
)
