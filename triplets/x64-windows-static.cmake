# vcpkg's own x64-windows-static triplet (static libraries, static CRT), plus build options that keep
# the AV1 libraries small: dav1d decodes AVIF, so aom is built as an encoder only and libavif doesn't
# wire up aom's decoder. vcpkg.json points vcpkg here ("overlay-triplets").
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "aom")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DCONFIG_AV1_DECODER=0)
endif()
if(PORT STREQUAL "libavif")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS -DAVIF_CODEC_AOM_DECODE=OFF)
endif()
