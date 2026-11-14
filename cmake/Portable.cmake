if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "LiteMusic supports Windows, Linux and macOS")
endif()

include(FetchContent)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_BACKEND_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG release-1.4.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fltk)

find_package(Threads REQUIRED)
add_executable(LiteMusic MACOSX_BUNDLE
    src/main_portable.cpp src/music.cpp src/music_unix.cpp src/settings_unix.cpp third_party/cjson/cJSON.c)
target_compile_features(LiteMusic PRIVATE cxx_std_20)
target_include_directories(LiteMusic PRIVATE third_party/cjson)
target_link_libraries(LiteMusic PRIVATE fltk::fltk Threads::Threads)

if(APPLE)
    find_library(SYSTEM_CURL curl REQUIRED)
    set_target_properties(LiteMusic PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.artS3rg.LiteMusic
        MACOSX_BUNDLE_BUNDLE_NAME LiteMusic
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_ICON_FILE LiteMusic.icns)
    target_sources(LiteMusic PRIVATE src/audio_mac.mm src/desktop_mac.mm LICENSE THIRD_PARTY_NOTICES "${fltk_SOURCE_DIR}/COPYING")
    set_source_files_properties(LICENSE THIRD_PARTY_NOTICES "${fltk_SOURCE_DIR}/COPYING" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_compile_options(LiteMusic PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(LiteMusic PRIVATE "${SYSTEM_CURL}" "-framework AVFoundation" "-framework CoreMedia" "-framework Security")
    find_program(ICONUTIL iconutil REQUIRED)
    find_program(SIPS sips REQUIRED)
    add_custom_command(TARGET LiteMusic POST_BUILD
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/packaging/mac-icon.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/icon.png" "${CMAKE_CURRENT_BINARY_DIR}/LiteMusic.iconset"
            "$<TARGET_BUNDLE_CONTENT_DIR:LiteMusic>/Resources/LiteMusic.icns" "${SIPS}" "${ICONUTIL}"
        VERBATIM)
else()
    find_package(PkgConfig REQUIRED)
    find_package(X11 REQUIRED)
    pkg_check_modules(NATIVE REQUIRED IMPORTED_TARGET libcurl openssl uuid gstreamer-1.0 libsecret-1)
    target_sources(LiteMusic PRIVATE src/audio_linux.cpp src/desktop_linux.cpp)
    target_link_libraries(LiteMusic PRIVATE PkgConfig::NATIVE X11::X11)
    install(TARGETS LiteMusic RUNTIME DESTINATION bin)
    install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION share/licenses/LiteMusic)
    install(FILES "${fltk_SOURCE_DIR}/COPYING" DESTINATION share/licenses/LiteMusic RENAME FLTK-COPYING)
    install(FILES docs/icon.png DESTINATION share/icons/hicolor/256x256/apps RENAME litemusic.png)
    install(FILES packaging/litemusic.desktop DESTINATION share/applications)
endif()
