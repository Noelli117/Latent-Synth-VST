set(torch_dir ${CMAKE_CURRENT_BINARY_DIR}/torch)

set(torch_lib_name torch)

set(torch_dir ${CMAKE_CURRENT_BINARY_DIR}/torch)
set(torch_lib_name torch)
find_library(torch_lib
  NAMES ${torch_lib_name}
  PATHS ${torch_dir}/libtorch/lib
)

if (DEFINED torch_version)
  message("setting torch version : ${torch_version}")
else()
  set(torch_version "1.11.0")
  message("torch version : ${torch_version}")
endif()

set(is_apple_arm64 FALSE)
if (APPLE)
  if (CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(is_apple_arm64 TRUE)
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(is_apple_arm64 TRUE)
  endif()
endif()

if (NOT torch_lib)
  message(STATUS "Downloading torch C API pre-built")

  # Download
  if (UNIX AND NOT APPLE)  # Linux
    set(torch_url
        "https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-${torch_version}%2Bcpu.zip")
  elseif (UNIX AND APPLE)  # OSX
    if (is_apple_arm64)
      if (NOT DEFINED torch_version_arm64)
        set(torch_version_arm64 "2.2.2")
      endif()
      message(STATUS "Apple arm64 detected: using libtorch version ${torch_version_arm64}")
      set(torch_url
          "https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-${torch_version_arm64}.zip")
    else()
      set(torch_url
          "https://download.pytorch.org/libtorch/cpu/libtorch-macos-${torch_version}.zip")
    endif()
  else()                   # Windows
    set(torch_url
        "https://download.pytorch.org/libtorch/cpu/libtorch-win-shared-with-deps-${torch_version}%2Bcpu.zip")
  endif()

  file(DOWNLOAD
    ${torch_url}
    ${torch_dir}/torch_cc.zip
    SHOW_PROGRESS
  )
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar -xf torch_cc.zip
                  WORKING_DIRECTORY ${torch_dir})

	  file(REMOVE ${torch_dir}/torch_cc.zip)

endif()

# Find the libraries again
find_library(torch_lib
  NAMES ${torch_lib_name}
  PATHS ${torch_dir}/libtorch/lib
)
if (NOT torch_lib)
  message(FATAL_ERROR "torch could not be included")
endif()
