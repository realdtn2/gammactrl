cmake_minimum_required(VERSION 3.16)
project(gammactrl
    VERSION 0.1.0
    DESCRIPTION "Gamma control for KDE Plasma Wayland"
    LANGUAGES C
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(gammactrl src/main.c)

target_link_libraries(gammactrl
    PRIVATE
        PkgConfig::GTK4
        m
)

target_compile_options(gammactrl PRIVATE -Wall -Wextra -O2)

# Install binary
install(TARGETS gammactrl
    RUNTIME DESTINATION bin
)

# Install .desktop file
install(FILES data/org.gammactrl.desktop
    DESTINATION share/applications
)

# Install icon
install(FILES data/org.gammactrl.svg
    DESTINATION share/icons/hicolor/scalable/apps
)
