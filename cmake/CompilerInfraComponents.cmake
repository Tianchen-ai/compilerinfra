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

# Read optional arrays without mistaking a malformed manifest for an empty plugin.
function(_compilerinfra_plugin_strings out json field file)
  string(JSON type ERROR_VARIABLE error TYPE "${json}" "${field}")
  set(items "")
  if(NOT error)
    if(NOT type STREQUAL "ARRAY")
      message(FATAL_ERROR "${file}: ${field} must be an array of names")
    endif()
    string(JSON count LENGTH "${json}" "${field}")
    while(count GREATER 0)
      math(EXPR count "${count}-1")
      string(JSON type TYPE "${json}" "${field}" ${count})
      string(JSON item GET "${json}" "${field}" ${count})
      if(NOT type STREQUAL "STRING" OR NOT item MATCHES "^[A-Za-z0-9_.:+-]+$")
        message(FATAL_ERROR "${file}: invalid name in ${field}")
      endif()
      list(PREPEND items "${item}")
    endwhile()
  endif()
  set(${out} "${items}" PARENT_SCOPE)
endfunction()

# Generate strong references so ordinary static linking retains every chosen component.
function(compilerinfra_assemble name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PRODUCT;PRESET" "COMPONENTS;PLUGINS")
  if(ARG_PRODUCT)
    if(ARG_PRESET OR ARG_PLUGINS)
      message(FATAL_ERROR "PRODUCT owns the plugin selection and default preset")
    endif()
    get_filename_component(product "${ARG_PRODUCT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(product_dir "${product}" DIRECTORY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${product}")
    file(READ "${product}" config)
    string(JSON count ERROR_VARIABLE error LENGTH "${config}" plugins)
    if(NOT error AND count GREATER 0)
      math(EXPR last "${count}-1")
      foreach(index RANGE 0 ${last})
        string(JSON descriptor GET "${config}" plugins ${index})
        list(APPEND ARG_PLUGINS "${product_dir}/${descriptor}")
      endforeach()
    endif()
    string(JSON preset ERROR_VARIABLE error GET "${config}" build preset)
    if(NOT error)
      set(ARG_PRESET "${product_dir}/${preset}")
    endif()
  endif()
  set(plugins "")
  foreach(plugin IN LISTS ARG_PLUGINS)
    get_filename_component(descriptor "${plugin}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${descriptor}")
    file(READ "${descriptor}" plugin_json)
    string(JSON id GET "${plugin_json}" id)
    string(JSON type TYPE "${plugin_json}" id)
    if(NOT type STREQUAL "STRING" OR NOT id MATCHES "^[A-Za-z0-9_.+-]+$" OR id IN_LIST plugins)
      message(FATAL_ERROR "${descriptor}: invalid or duplicate plugin id '${id}'")
    endif()
    list(APPEND plugins "${id}")
    _compilerinfra_plugin_strings(deps "${plugin_json}" dependencies "${descriptor}")
    set("deps_${id}" "${deps}")
    _compilerinfra_plugin_strings(native_components "${plugin_json}" native "${descriptor}")
    list(APPEND ARG_COMPONENTS ${native_components})
  endforeach()
  set(pending "${plugins}")
  while(pending)
    set(before "${pending}")
    foreach(id IN LISTS before)
      set(deps "${deps_${id}}")
      foreach(dep IN LISTS deps)
        if(dep MATCHES "^compilerinfra[.](runtime|development|measurement)$")
          list(REMOVE_ITEM deps "${dep}")
          continue()
        endif()
        if(NOT dep IN_LIST plugins)
          message(FATAL_ERROR "Plugin ${id} requires missing package ${dep}")
        endif()
        if(NOT dep IN_LIST pending)
          list(REMOVE_ITEM deps "${dep}")
        endif()
      endforeach()
      if(NOT deps)
        list(REMOVE_ITEM pending "${id}")
      endif()
    endforeach()
    if(before STREQUAL pending)
      message(FATAL_ERROR "Cyclic compilerinfra plugin dependencies: ${pending}")
    endif()
  endwhile()
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
  set(preset_bytes "123,125,")
  if(ARG_PRESET)
    get_filename_component(preset "${ARG_PRESET}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${preset}")
    file(READ "${preset}" preset_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," preset_bytes "${preset_hex}")
  endif()
  file(GENERATE OUTPUT "${root}/${header}" CONTENT
"#pragma once
#include <compilerinfra/Compiler.h>
namespace compilerinfra { class Assembly; }
namespace ${name} {
void registerComponents(compilerinfra::Assembly &);
void registerPasses();
llvm::Expected<compilerinfra::Compiler> link(compilerinfra::Assembly &, compilerinfra::Import, llvm::StringRef);
}
")
  file(GENERATE OUTPUT "${root}/Components.cpp" CONTENT
"#include \"${header}\"
#include <compilerinfra/Assembly.h>
namespace compilerinfra_components {
${declarations}}
void ${name}::registerComponents(compilerinfra::Assembly &assembly) {
${calls}}
void ${name}::registerPasses() {
${pass_calls}}
llvm::Expected<compilerinfra::Compiler> ${name}::link(compilerinfra::Assembly &a, compilerinfra::Import importer, llvm::StringRef model) {
  static const unsigned char defaults[] = {${preset_bytes}0};
  auto v=llvm::json::parse(llvm::StringRef(reinterpret_cast<const char*>(defaults),sizeof(defaults)-1));
  if(!v)return v.takeError();
  auto p=compilerinfra::parsePreset(*v);if(!p)return p.takeError();
  return a.link(std::move(importer),model,*p);
}
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
    return ${ns}::link(assembly, compilerinfra::importMLIR, \"${ARG_INPUT_MODEL}\");
  });
}
")
  add_executable(${name} "${source}")
  target_link_libraries(${name} PRIVATE ${ARG_ASSEMBLY} CompilerInfra::Driver)
endfunction()
