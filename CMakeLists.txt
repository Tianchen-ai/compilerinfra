cmake_minimum_required(VERSION 3.20)
project(CompilerInfra VERSION 0.5.3 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET MLIRIR)
  find_package(MLIR REQUIRED CONFIG)
endif()
if(LLVM_ENABLE_ZSTD AND NOT TARGET zstd::libzstd_shared)
  find_library(_ci_zstd_lib NAMES zstd libzstd.so.1)
  if(_ci_zstd_lib)
    add_library(zstd::libzstd_shared SHARED IMPORTED)
    set_target_properties(zstd::libzstd_shared PROPERTIES IMPORTED_LOCATION "${_ci_zstd_lib}")
  else()
    add_library(zstd::libzstd_shared INTERFACE IMPORTED)
  endif()
endif()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(standalone ON)
else()
  set(standalone OFF)
endif()
set(shared_mlir_default "${MLIR_LINK_MLIR_DYLIB}")
if(standalone AND NOT DEFINED MLIR_LINK_MLIR_DYLIB AND TARGET MLIR AND TARGET LLVM)
  set(shared_mlir_default ON)
endif()
option(COMPILERINFRA_LINK_MLIR_DYLIB "Match the product's shared MLIR/LLVM linkage" ${shared_mlir_default})
option(COMPILERINFRA_INSTALL "Install the CompilerInfra SDK" ${standalone})
if(NOT COMPILERINFRA_LINK_MLIR_DYLIB AND TARGET LLVMSupport)
  get_property(_ci_imported DIRECTORY PROPERTY IMPORTED_TARGETS)
  foreach(_ci_tgt IN LISTS _ci_imported)
    if(_ci_tgt MATCHES "^MLIR")
      get_target_property(_ci_iface ${_ci_tgt} INTERFACE_LINK_LIBRARIES)
      if(_ci_iface AND "LLVM" IN_LIST _ci_iface)
        list(TRANSFORM _ci_iface REPLACE "^LLVM$" "LLVMCore;LLVMBinaryFormat;LLVMSupport;LLVMDemangle")
        set_target_properties(${_ci_tgt} PROPERTIES INTERFACE_LINK_LIBRARIES "${_ci_iface}")
      endif()
    endif()
  endforeach()
endif()
add_library(CompilerInfraCore STATIC lib/Assembly.cpp lib/Contract.cpp lib/Cost.cpp lib/Compiler.cpp lib/Decision.cpp lib/Proofs.cpp lib/Session.cpp lib/Terminal.cpp)
add_library(CompilerInfra::Core ALIAS CompilerInfraCore)
set_target_properties(CompilerInfraCore PROPERTIES EXPORT_NAME Core POSITION_INDEPENDENT_CODE ON)
target_compile_features(CompilerInfraCore PUBLIC cxx_std_17)
target_include_directories(CompilerInfraCore PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_include_directories(CompilerInfraCore SYSTEM PUBLIC
  "$<BUILD_INTERFACE:${MLIR_INCLUDE_DIRS}>" "$<BUILD_INTERFACE:${LLVM_INCLUDE_DIRS}>")
if(COMPILERINFRA_LINK_MLIR_DYLIB)
  target_link_libraries(CompilerInfraCore PUBLIC MLIR LLVM)
else()
  target_link_libraries(CompilerInfraCore PUBLIC
    MLIRParser MLIRBytecodeWriter MLIRPass MLIRAnalysis MLIRArithDialect
    MLIRInferIntRangeInterface MLIRLoopLikeInterface MLIRSideEffectInterfaces
    MLIRViewLikeInterface MLIRIR LLVMSupport)
endif()

option(COMPILERINFRA_BUILD_TESTS "Build CompilerInfra SDK unit tests" OFF)
if(standalone AND COMPILERINFRA_BUILD_TESTS)
  enable_testing()
  add_executable(ci-v05-tests test/V05Tests.cpp)
  if(COMPILERINFRA_LINK_MLIR_DYLIB)
    target_link_libraries(ci-v05-tests PRIVATE CompilerInfraCore MLIR LLVM)
  else()
    target_link_libraries(ci-v05-tests PRIVATE CompilerInfraCore
      MLIRFuncDialect MLIRMemRefDialect MLIRSCFDialect MLIRTensorDialect
      MLIRVectorDialect)
  endif()
  add_test(NAME ci-v05-tests COMMAND ci-v05-tests)
endif()

add_library(CompilerInfraRuntime INTERFACE)
add_library(CompilerInfra::Runtime ALIAS CompilerInfraRuntime)
set_target_properties(CompilerInfraRuntime PROPERTIES EXPORT_NAME Runtime)
target_include_directories(CompilerInfraRuntime INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

# Host-side action transport is separate from the offline JIT runtime.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(CompilerInfraDevelopment lib/Development.cpp)
  add_library(CompilerInfra::Development ALIAS CompilerInfraDevelopment)
  set_target_properties(CompilerInfraDevelopment PROPERTIES EXPORT_NAME Development POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(CompilerInfraDevelopment PUBLIC CompilerInfraCore)
  set(development_target CompilerInfraDevelopment)
endif()

include(cmake/CompilerInfraComponents.cmake)
if(NOT COMPILERINFRA_INSTALL)
  return()
endif()
set(config_dir "${CMAKE_INSTALL_LIBDIR}/cmake/CompilerInfra")
configure_package_config_file(cmake/CompilerInfraConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfig.cmake"
  INSTALL_DESTINATION "${config_dir}")
install(TARGETS CompilerInfraCore CompilerInfraRuntime ${development_target} EXPORT CompilerInfraTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/compilerinfra DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/compilerinfra")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfigVersion.cmake"
  cmake/CompilerInfraLayout.cmake cmake/tree_build.py cmake/ProductABI.cpp cmake/Main.cpp DESTINATION "${config_dir}")
install(DIRECTORY python/compilerinfra DESTINATION "${config_dir}/python" PATTERN "__pycache__" EXCLUDE)
set(COMPILERINFRA_PYTHON_INSTALL_DIR "lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages" CACHE STRING "Python package directory relative to installation")
install(DIRECTORY python/compilerinfra DESTINATION "${COMPILERINFRA_PYTHON_INSTALL_DIR}" PATTERN "__pycache__" EXCLUDE)
install(EXPORT CompilerInfraTargets NAMESPACE CompilerInfra:: DESTINATION "${config_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfig.cmake" cmake/CompilerInfraComponents.cmake DESTINATION "${config_dir}")
