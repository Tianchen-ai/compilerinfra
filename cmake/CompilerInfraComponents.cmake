include_guard(GLOBAL)
include(GNUInstallDirs)

# Entry functions have the signature void compilerinfra_components::NAME(Assembly&).
function(compilerinfra_add_component name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ENTRY;REGISTER_PASSES" "SOURCES;LINK_LIBRARIES")
  if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_SOURCES OR NOT ARG_ENTRY MATCHES "^[A-Za-z_][A-Za-z_0-9]*$")
    message(FATAL_ERROR "compilerinfra_add_component needs ENTRY <C++ identifier> and SOURCES")
  endif()
  add_library(${name} STATIC ${ARG_SOURCES})
  set_target_properties(${name} PROPERTIES COMPILERINFRA_ENTRY "${ARG_ENTRY}"
    EXPORT_PROPERTIES COMPILERINFRA_ENTRY
    POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(${name} PUBLIC CompilerInfra::Core PRIVATE ${ARG_LINK_LIBRARIES})
  target_compile_definitions(${name} PRIVATE "COMPILERINFRA_COMPONENT_NAME=\"${name}\"")
  target_compile_options(${name} PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=>")
  if(ARG_REGISTER_PASSES)
    if(NOT ARG_REGISTER_PASSES MATCHES "^[A-Za-z_][A-Za-z_0-9]*$")
      message(FATAL_ERROR "REGISTER_PASSES needs a C++ identifier")
    endif()
    set(wrapper "${ARG_ENTRY}_registerPassesOnce")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/compilerinfra-generated/components/${name}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT
"namespace compilerinfra_components {
void ${ARG_REGISTER_PASSES}();
void ${wrapper}() {
  static const bool registered = [] { ${ARG_REGISTER_PASSES}(); return true; }();
  (void)registered;
}
}
")
    target_sources(${name} PRIVATE "${source}")
    set_property(TARGET ${name} PROPERTY COMPILERINFRA_PASS_ENTRY "${wrapper}")
    set_property(TARGET ${name} APPEND PROPERTY EXPORT_PROPERTIES COMPILERINFRA_PASS_ENTRY)
  endif()
endfunction()

# Generate strong references so ordinary static linking retains every chosen component.
function(compilerinfra_assemble name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "COMPONENTS")
  if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_COMPONENTS OR NOT name MATCHES "^[A-Za-z_][A-Za-z_0-9]*$")
    message(FATAL_ERROR "compilerinfra_assemble needs a C++ identifier and COMPONENTS")
  endif()
  set(entries "")
  set(declarations "")
  set(calls "")
  set(pass_calls "")
  foreach(component IN LISTS ARG_COMPONENTS)
    if(NOT TARGET ${component})
      message(FATAL_ERROR "Unknown compilerinfra component ${component}")
    endif()
    get_target_property(entry ${component} COMPILERINFRA_ENTRY)
    if(NOT entry OR entry IN_LIST entries)
      message(FATAL_ERROR "Missing or duplicate entry for component ${component}: ${entry}")
    endif()
    list(APPEND entries "${entry}")
    string(APPEND declarations "void ${entry}(compilerinfra::Assembly &);\n")
    string(APPEND calls "  compilerinfra_components::${entry}(assembly);\n")
    get_target_property(pass_entry ${component} COMPILERINFRA_PASS_ENTRY)
    if(pass_entry)
      string(APPEND declarations "void ${pass_entry}();\n")
      string(APPEND pass_calls "  compilerinfra_components::${pass_entry}();\n")
    endif()
  endforeach()

  set(root "${CMAKE_CURRENT_BINARY_DIR}/compilerinfra-generated/${name}")
  set(header "${name}/Components.h")
  file(GENERATE OUTPUT "${root}/${header}" CONTENT
"#pragma once
namespace compilerinfra { class Assembly; }
namespace ${name} {
void registerComponents(compilerinfra::Assembly &);
void registerPasses();
}
")
  file(GENERATE OUTPUT "${root}/Components.cpp" CONTENT
"#include \"${header}\"
namespace compilerinfra_components {
${declarations}}
void ${name}::registerComponents(compilerinfra::Assembly &assembly) {
${calls}}
void ${name}::registerPasses() {
${pass_calls}}
")
  add_library(${name} STATIC "${root}/Components.cpp")
  target_link_libraries(${name} PUBLIC CompilerInfra::Core PRIVATE ${ARG_COMPONENTS})
  target_include_directories(${name} PUBLIC "$<BUILD_INTERFACE:${root}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON
    COMPILERINFRA_ASSEMBLY_NAMESPACE "${name}" EXPORT_PROPERTIES COMPILERINFRA_ASSEMBLY_NAMESPACE
    PUBLIC_HEADER "${root}/${header}")
endfunction()

# A product with MLIR input gets a complete component-aware product driver.
# Native language frontends can supply their own importer via Assembly::link.
function(compilerinfra_add_driver name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ASSEMBLY;INPUT_MODEL" "")
  if(ARG_UNPARSED_ARGUMENTS OR NOT TARGET "${ARG_ASSEMBLY}" OR
     NOT ARG_INPUT_MODEL MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR "compilerinfra_add_driver needs ASSEMBLY and INPUT_MODEL")
  endif()
  get_target_property(ns ${ARG_ASSEMBLY} COMPILERINFRA_ASSEMBLY_NAMESPACE)
  if(NOT ns)
    message(FATAL_ERROR "${ARG_ASSEMBLY} is not a compilerinfra assembly")
  endif()
  set(source "${CMAKE_CURRENT_BINARY_DIR}/compilerinfra-generated/drivers/${name}.cpp")
  file(GENERATE OUTPUT "${source}" CONTENT
"#include <compilerinfra/Assembly.h>
#include <compilerinfra/Driver.h>
#include <${ns}/Components.h>
int main(int argc, char **argv) {
  ${ns}::registerPasses();
  return compilerinfra::driverMain(argc, argv, [] {
    compilerinfra::Assembly assembly;
    ${ns}::registerComponents(assembly);
    return assembly.link(compilerinfra::importMLIR, \"${ARG_INPUT_MODEL}\");
  });
}
")
  add_executable(${name} "${source}")
  target_link_libraries(${name} PRIVATE ${ARG_ASSEMBLY} CompilerInfra::Driver)
endfunction()
