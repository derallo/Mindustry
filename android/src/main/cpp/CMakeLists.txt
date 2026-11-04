# Mindustry VR — native OpenXR bridge
#
# The Khronos OpenXR loader is vendored (src/main/cpp/openxr + openxr/lib/<abi>/libopenxr_loader.so)
# rather than consumed as a prefab AAR: AGP 8.2's prefab dependency propagation proved
# unreliable for flavor-scoped dependencies, and vendoring keeps the build deterministic.
cmake_minimum_required(VERSION 3.22.1)
project(mindustryvr CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../../../openxr/lib/${ANDROID_ABI}/libopenxr_loader.so")

add_library(mindustryvr SHARED
        vr_jni.cpp
        vr_session.cpp
)

target_include_directories(mindustryvr PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/openxr/include
)

target_compile_definitions(mindustryvr PRIVATE
        XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_OPENGL_ES
)

target_link_libraries(mindustryvr PRIVATE
        ${OPENXR_LIB}
        android
        log
        EGL
        GLESv3
)

target_compile_options(mindustryvr PRIVATE -Wall -Wextra -Wno-unused-parameter)
