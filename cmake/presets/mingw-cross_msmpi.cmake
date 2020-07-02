set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
set(BUILD_MPI ON CACHE BOOL "" FORCE)
set(MPI_GUESS_LIBRARY_NAME "MSMPI" CACHE STRING "" FORCE)
set(LAMMPS_MACHINE "msmpi" CACHE STRING "" FORCE)
#set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "" FORCE)

set(FFMPEG_EXECUTABLE "/development/FFMPEG" on CACHE BOOL "" FORCE)

set(WIN_PACKAGES ASPHERE BODY CLASS2 COLLOID COMPRESS CORESHELL DIPOLE GPU
                 GRANULAR KSPACE LATTE MANYBODY MC MISC MOLECULE OPT PERI
                 POEMS QEQ REPLICA RIGID SHOCK SNAP SPIN SRD VORONOI
                 USER-ATC USER-AWPMD USER-BOCS USER-CGDNA USER-CGSDK
                 USER-COLVARS USER-DIFFRACTION USER-DPD USER-DRUDE USER-EFF
                 USER-FEP USER-INTEL USER-MANIFOLD USER-MEAMC USER-MESODPD
                 USER-MESONT USER-MISC USER-MGPT USER-MOFFF USER-MOLFILE USER-OMP
                 USER-PHONON USER-PTM USER-QTB USER-REACTION USER-REAXC
                 USER-SDPD USER-SMD USER-SMTBQ USER-SPH USER-TALLY USER-UEF
                 USER-YAFF)

foreach(PKG ${WIN_PACKAGES})
  set(PKG_${PKG} ON CACHE BOOL "" FORCE)
endforeach()

# these two packages require a full MPI implementation
if(BUILD_MPI)
  set(PKG_MPIIO ON CACHE BOOL "" FORCE)
  set(PKG_USER-LB ON CACHE BOOL "" FORCE)
endif()

set(DOWNLOAD_VORO ON CACHE BOOL "" FORCE)
set(DOWNLOAD_EIGEN3 ON CACHE BOOL "" FORCE)
set(LAMMPS_MEMALIGN "0" CACHE STRING "" FORCE)
set(CMAKE_TUNE_FLAGS "-Wno-missing-include-dirs" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "-Wl,--enable-stdcall-fixup" CACHE STRING "" FORCE)
##set(CMAKE_EXE_LINKER_FLAGS "-Wl,-Bstatic -Wl,--enable-stdcall-fixup -static" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,--enable-stdcall-fixup " CACHE STRING "" FORCE)
set(BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/lammps-installer")
