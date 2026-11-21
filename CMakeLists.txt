cmake_minimum_required(VERSION 3.20)
project(CompilerInfra VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET MLIRIR)
  find_package(MLIR REQUIRED CONFIG)
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
add_library(CompilerInfraCore lib/Assembly.cpp lib/Catalog.cpp lib/Comparison.cpp lib/Compiler.cpp lib/Decision.cpp lib/Preset.cpp lib/Session.cpp)
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
  target_link_libraries(CompilerInfraCore PUBLIC MLIRParser MLIRBytecodeWriter MLIRPass MLIRIR LLVMSupport)
endif()

add_library(CompilerInfraDriver lib/Driver.cpp)
add_library(CompilerInfra::Driver ALIAS CompilerInfraDriver)
set_target_properties(CompilerInfraDriver PROPERTIES EXPORT_NAME Driver POSITION_INDEPENDENT_CODE ON)
target_link_libraries(CompilerInfraDriver PUBLIC CompilerInfraCore)

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
install(TARGETS CompilerInfraCore CompilerInfraDriver ${development_target} EXPORT CompilerInfraTargets
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/compilerinfra DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfigVersion.cmake"
  cmake/CompilerInfraLayout.cmake cmake/product-layout.json DESTINATION "${config_dir}")
install(EXPORT CompilerInfraTargets NAMESPACE CompilerInfra:: DESTINATION "${config_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CompilerInfraConfig.cmake" cmake/CompilerInfraComponents.cmake DESTINATION "${config_dir}")
