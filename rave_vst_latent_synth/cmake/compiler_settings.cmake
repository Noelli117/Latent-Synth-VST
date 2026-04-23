function (setup_compilers)
  if(APPLE)
    if (NOT CMAKE_C_COMPILER)
      find_program(LATENT_SYNTH_CLANG
        NAMES clang
        PATHS
          /opt/homebrew/opt/llvm/bin
          /usr/local/opt/llvm/bin
      )
      if (LATENT_SYNTH_CLANG)
        set(CMAKE_C_COMPILER "${LATENT_SYNTH_CLANG}" CACHE FILEPATH
            "C compiler for Latent Synth" FORCE)
      endif()
    endif()

    if (NOT CMAKE_CXX_COMPILER)
      find_program(LATENT_SYNTH_CLANGXX
        NAMES clang++
        PATHS
          /opt/homebrew/opt/llvm/bin
          /usr/local/opt/llvm/bin
      )
      if (LATENT_SYNTH_CLANGXX)
        set(CMAKE_CXX_COMPILER "${LATENT_SYNTH_CLANGXX}" CACHE FILEPATH
            "C++ compiler for Latent Synth" FORCE)
      endif()
    endif()

    set(MACOSX_RPATH TRUE)
  endif(APPLE)
  if (MSVC)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /permissive- ")
  endif(MSVC)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  # Keep runtime lookup inside the plugin/app bundle (avoid absolute build paths).
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
  set(CMAKE_INSTALL_RPATH
      "@loader_path/../torch/libtorch;@loader_path/../Resources/libtorch;@executable_path/../torch/libtorch;@executable_path/../Resources/libtorch")

  set(CMAKE_CXX_FLAGS "-Wall")
  set(CMAKE_CXX_FLAGS_DEBUG "-g")
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endfunction()
