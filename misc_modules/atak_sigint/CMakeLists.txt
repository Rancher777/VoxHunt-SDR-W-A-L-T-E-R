cmake_minimum_required(VERSION 3.13)
project(atak_sigint)

# Download Whisper model
# Download Whisper model
file(DOWNLOAD
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"
    "${CMAKE_CURRENT_SOURCE_DIR}/ggml-tiny.en.bin"
    STATUS download_status
            EXPECTED_HASH SHA256=921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f)
if(NOT download_status EQUAL 0)
    message(FATAL_ERROR "Failed to download ggml-tiny.en.bin: ${download_status}")
endif()

# Add whisper.cpp dependency
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "")
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "")
add_subdirectory(vendor)

file(GLOB SRC "src/*.cpp")

# This creates the 'atak_sigint' target
include(${SDRPP_MODULE_CMAKE})

# Pass the module's source directory to the C++ code as a preprocessor macro
target_compile_definitions(atak_sigint PRIVATE -DMODULE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Now that the target exists, we can link it and add its include directories
target_include_directories(atak_sigint PRIVATE vendor)
target_link_libraries(atak_sigint PRIVATE whisper)

install(FILES ggml-tiny.en.bin DESTINATION bin)
