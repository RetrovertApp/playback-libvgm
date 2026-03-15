# Vendored zlib build (with gzip file I/O for libvgm)
add_library(zlib_static STATIC
    adler32.c
    compress.c
    crc32.c
    deflate.c
    gzclose.c
    gzlib.c
    gzread.c
    gzwrite.c
    infback.c
    inffast.c
    inflate.c
    inftrees.c
    trees.c
    uncompr.c
    zutil.c
)
target_include_directories(zlib_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(zlib_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Suppress warnings in vendored code (gz* files use implicit POSIX declarations)
if(MSVC)
    target_compile_options(zlib_static PRIVATE /w)
else()
    target_compile_options(zlib_static PRIVATE -w)
endif()

# Provide ZLIB::ZLIB target so plugins can use standard CMake convention
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib_static)
endif()
