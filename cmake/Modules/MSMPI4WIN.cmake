# Download and configure custom MS-MPI files for Windows
message(STATUS "Downloading and configuring MS-MPI for Windows")
include(ExternalProject)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  ExternalProject_Add(msmpi4win_build
    URL http://repo.msys2.org/mingw/x86_64/mingw-w64-x86_64-msmpi-10.1.1-2-any.pkg.tar.zst
    URL_MD5 ac6438c71ab1f838b052a5f859474986
    CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <SOURCE_DIR>/mingw64/lib/libmsmpi.a)
else()
  ExternalProject_Add(msmpi4win_build
    URL http://repo.msys2.org/mingw/i686/mingw-w64-i686-msmpi-10.1.1-2-any.pkg.tar.zst
    URL_MD5 d7b8c0dc9655f2327a60e7d2746313fd
    CONFIGURE_COMMAND "" BUILD_COMMAND "" INSTALL_COMMAND ""
    BUILD_BYPRODUCTS <SOURCE_DIR>/lib/libmpi.a)
endif()

ExternalProject_get_property(msmpi4win_build SOURCE_DIR)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  file(MAKE_DIRECTORY "${SOURCE_DIR}/mingw64/include")
  add_library(MPI::MPI_CXX UNKNOWN IMPORTED)
  set_target_properties(MPI::MPI_CXX PROPERTIES
    IMPORTED_LOCATION "${SOURCE_DIR}/mingw64/lib/libmsmpi.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SOURCE_DIR}/mingw64/include"
    INTERFACE_COMPILE_DEFINITIONS "MPI_SKIP_MPICXX")
  add_dependencies(MPI::MPI_CXX msmpi4win_build)
else()
  file(MAKE_DIRECTORY "${SOURCE_DIR}/mingw32/include")
  add_library(MPI::MPI_CXX UNKNOWN IMPORTED)
  set_target_properties(MPI::MPI_CXX PROPERTIES
    IMPORTED_LOCATION "${SOURCE_DIR}/mingw32/lib/libmsmpi.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SOURCE_DIR}/mingw32/include"
    INTERFACE_COMPILE_DEFINITIONS "MPI_SKIP_MPICXX")
  add_dependencies(MPI::MPI_CXX msmpi4win_build)
endif()

# set variables for status reporting at the end of CMake run
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  set(MPI_CXX_INCLUDE_PATH "${SOURCE_DIR}/mingw64/include")
  set(MPI_CXX_COMPILE_DEFINITIONS "MPI_SKIP_MPICXX")
  set(MPI_CXX_LIBRARIES "${SOURCE_DIR}/mingw64/lib/libmsmpi.a")
else()
  set(MPI_CXX_INCLUDE_PATH "${SOURCE_DIR}/mingw32/include")
  set(MPI_CXX_COMPILE_DEFINITIONS "MPI_SKIP_MPICXX")
  set(MPI_CXX_LIBRARIES "${SOURCE_DIR}/mingw32/lib/libmsmpi.a")
endif()
