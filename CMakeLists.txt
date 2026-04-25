cmake_minimum_required(VERSION 3.22)

project(DuplicateImageFinder LANGUAGES CXX)

add_executable(DuplicateImageFinder WIN32
    src/main.cpp
    src/app.cpp
    src/scanner.cpp
    src/hash.cpp
    src/delete.cpp
    resources/app.rc
)

target_compile_features(DuplicateImageFinder PRIVATE cxx_std_20)

target_compile_definitions(DuplicateImageFinder PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(DuplicateImageFinder PRIVATE src resources)

target_link_libraries(DuplicateImageFinder PRIVATE
    comctl32
    gdiplus
    bcrypt
    shell32
    shlwapi
    ole32
    uxtheme
)

if(MSVC)
    set_property(TARGET DuplicateImageFinder PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(DuplicateImageFinder PRIVATE /W4 /permissive- /utf-8)
    target_link_options(DuplicateImageFinder PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
        "/STACK:8388608")
endif()
