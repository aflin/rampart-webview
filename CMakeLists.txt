cmake_minimum_required(VERSION 3.16)
project(rampart-webview C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Detect MSYS/Cygwin compiler (CMAKE_SYSTEM_NAME may be forced to Windows)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -dumpmachine
        OUTPUT_VARIABLE GCC_TARGET_TRIPLE OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(GCC_TARGET_TRIPLE MATCHES "cygwin|msys")
        set(MSYS_COMPILER TRUE)
    endif()
endif()

# Rampart paths
set(RAMPART_INCLUDE "/usr/local/rampart/include" CACHE PATH "Rampart include directory")
set(RAMPART_BIN "/usr/local/rampart/bin" CACHE PATH "Rampart bin directory (for import library)")

# Webview paths
set(WEBVIEW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/webview")
set(WEBVIEW_INCLUDE_DIR "${WEBVIEW_SOURCE_DIR}/core/include")

# Module filename suffix, used ONLY by the Linux docker ("oven") builds to emit
# the per-WebKitGTK variants: -DMODSUFFIX=_wk41 -> rampart-webview_wk41.so.
# Empty (the default) keeps the plain rampart-webview.so that native macOS /
# FreeBSD / Linux builds produce -- those are deliberately unchanged.
# Named MODSUFFIX, not SUFFIX, to avoid confusion with the
# set_target_properties(... SUFFIX ".so") shared-library extension below.
set(MODSUFFIX "" CACHE STRING "module filename suffix, e.g. _wk41 (docker variant builds)")

# The module shared library
if(APPLE)
    add_library(rampart-webview SHARED rampart-webview.c snapshot_macos.m)
    # ARC is required: async callbacks (snapshot, cookies) set __block ObjC
    # object pointers that must be retained across run-loop pumping.
    set_source_files_properties(snapshot_macos.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    add_library(rampart-webview SHARED rampart-webview.c)
endif()

# Output is rampart-webview.so on every platform for a native build; the Linux
# ovens set MODSUFFIX to emit rampart-webview_wk40.so / _wk41.so alongside the
# rampart-webview.js selector.
# Use C++ linker because libwebview.a contains C++ object code.
set_target_properties(rampart-webview PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "rampart-webview${MODSUFFIX}"
    LINKER_LANGUAGE CXX
)

target_include_directories(rampart-webview PRIVATE
    ${RAMPART_INCLUDE}
    ${WEBVIEW_INCLUDE_DIR}
)

target_compile_definitions(rampart-webview PRIVATE WEBVIEW_STATIC)
target_compile_options(rampart-webview PRIVATE -Wall -O2)

# ---- Build webview as a subproject ----

# Configure webview subproject: static lib only, no extras
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_AMALGAMATION OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)

if(WIN32 OR CYGWIN OR MSYS)
    set(WEBVIEW_USE_COMPAT_MINGW ON CACHE BOOL "" FORCE)
    # MSYS2/Cygwin report CMAKE_SYSTEM_NAME as "MSYS" or "CYGWIN"
    # rather than "Windows", causing the webview cmake to look for
    # WebKitGTK instead of WebView2.  Force the correct platform.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_SYSTEM_NAME "Windows")
    endif()
endif()

add_subdirectory(webview)

# webview::core_static transitively brings in platform dependencies
# (GTK/WebKitGTK on Linux, WebKit/Cocoa on macOS, WebView2/Win32 on Windows)
target_link_libraries(rampart-webview PRIVATE webview::core_static)

if(APPLE)
    # macOS: allow unresolved symbols for Rampart module loading
    set_target_properties(rampart-webview PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
    # macOS: link JavaScriptCore.framework for headless JSC support
    target_link_libraries(rampart-webview PRIVATE "-framework JavaScriptCore")
elseif(MSYS_COMPILER)
    # Link against rampart import library to resolve duktape symbols
    target_link_libraries(rampart-webview PRIVATE "${RAMPART_BIN}/librampart.dll.a")
endif()

# ---- Copy test scripts and library fixtures to build directory ----

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/test_webview.js"
    "${CMAKE_CURRENT_BINARY_DIR}/test_webview.js"
    COPYONLY
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/webview-jsc-test.js"
    "${CMAKE_CURRENT_BINARY_DIR}/webview-jsc-test.js"
    COPYONLY
)

file(GLOB JSC_TEST_LIBS "${CMAKE_CURRENT_SOURCE_DIR}/libs/*.js")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/libs")
foreach(libfile ${JSC_TEST_LIBS})
    get_filename_component(fname "${libfile}" NAME)
    configure_file("${libfile}" "${CMAKE_CURRENT_BINARY_DIR}/libs/${fname}" COPYONLY)
endforeach()

# ---- Install ----

# Try to detect rampart modules path
execute_process(
    COMMAND rampart -c "console.log(process.modulesPath)"
    OUTPUT_VARIABLE RAMPART_MODULES_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE RAMPART_MODULES_RESULT
)
if(NOT RAMPART_MODULES_RESULT EQUAL 0)
    # Fallback (e.g. the oven, where `rampart` isn't on PATH to query): derive the
    # modules dir from RAMPART_INCLUDE's parent instead of hardcoding, so it
    # follows a custom prefix (e.g. /usr/local/rampart-ml).
    get_filename_component(_rp_root "${RAMPART_INCLUDE}" DIRECTORY)
    set(RAMPART_MODULES_PATH "${_rp_root}/modules")
endif()

install(TARGETS rampart-webview
    LIBRARY DESTINATION "${RAMPART_MODULES_PATH}"
)

# Variant (Linux docker) builds also ship the selector shim, which picks the
# .so matching the host's WebKitGTK at first require().  The loader tries
# .js before .so (src/duktape/core/module.c), so rampart-webview.js
# transparently fronts require("rampart-webview").
#
# Native builds (MODSUFFIX empty) install a plain rampart-webview.so and must
# NOT get the shim -- it would look for _wk40/_wk41 variants that don't exist.
if(NOT MODSUFFIX STREQUAL "")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/rampart-webview.js"
        DESTINATION "${RAMPART_MODULES_PATH}"
    )
endif()
