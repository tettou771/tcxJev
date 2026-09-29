# =============================================================================
# tcxJev - client for TypeSafe's Jev model
# =============================================================================
# Has its own CMakeLists (rather than auto-collection) because it links the
# tcxCurl addon, which provides the default HTTPS transport. tcxCurl is linked
# PRIVATE: tcxJev.h does not include it, and its libraries (libcurl) still
# reach the app's link line.
#
# tcxCurl must be loaded by the project too: list it in addons.make (addon.json
# declares the dependency, so `trusscli addon add tcxJev` adds it for you).
# =============================================================================

cmake_minimum_required(VERSION 3.20)

set(ADDON_NAME tcxJev)

file(GLOB ADDON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB ADDON_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h)

add_library(${ADDON_NAME} STATIC ${ADDON_SOURCES} ${ADDON_HEADERS})
target_include_directories(${ADDON_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(${ADDON_NAME} PUBLIC TrussC)
# By target name, so the order of lines in addons.make doesn't matter.
target_link_libraries(${ADDON_NAME} PRIVATE tcxCurl)

# Fail at configure time with a clear message (instead of a missing
# tcxCurl.h at compile time) when the project doesn't load tcxCurl. Checked at
# the end of the top-level directory, after every addons.make line was loaded.
function(_tcxjev_require_tcxcurl)
    if(NOT TARGET tcxCurl)
        message(FATAL_ERROR
            "[tcxJev] needs the tcxCurl addon. Add a line 'tcxCurl' to this "
            "project's addons.make (`trusscli addon add tcxJev` does it for you).")
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _tcxjev_require_tcxcurl)

add_library(tc::tcxJev ALIAS tcxJev)
