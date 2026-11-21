include_guard(GLOBAL)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/CompilerInfraLayout.cmake")

# Entry functions have the signature void compilerinfra_components::NAME(Assembly&).
function(compilerinfra_add_component name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "ENTRY;REGISTER_PASSES;LAYER" "SOURCES;LINK_LIBRARIES")
  if(ARG_LAYER STREQUAL "runtime" AND NOT ARG_ENTRY)
    set(ARG_ENTRY "_")
  endif()
  if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_SOURCES OR NOT ARG_ENTRY MATCHES "^[A-Za-z_][A-Za-z_0-9]*$")
    message(FATAL_ERROR "compilerinfra_add_component needs ENTRY <C++ identifier> and SOURCES")
  endif()
  _compilerinfra_owner("${name}" "${ARG_LAYER}" owner)
  _compilerinfra_check_sources("${name}" "${owner}" ${ARG_SOURCES})
  add_library(${name} STATIC ${ARG_SOURCES})
  set_target_properties(${name} PROPERTIES COMPILERINFRA_ENTRY "${ARG_ENTRY}"
    COMPILERINFRA_LAYER "${ARG_LAYER}" COMPILERINFRA_DIRECTORY "${owner}"
    EXPORT_PROPERTIES "COMPILERINFRA_ENTRY;COMPILERINFRA_LAYER;COMPILERINFRA_DIRECTORY"
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
    _compilerinfra_policy(policy)
    string(JSON expected GET "${policy}" version)
    string(JSON version ERROR_VARIABLE missing_version GET "${config}" compilerinfra)
    if(missing_version OR NOT version STREQUAL expected)
      message(FATAL_ERROR "${product}: expected compilerinfra=${expected}; see doc/organization.md")
    endif()
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
    set("descriptor_${id}" "${descriptor}")
    _compilerinfra_plugin_strings(native_components "${plugin_json}" native "${descriptor}")
    set("native_${id}" "${native_components}")
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
        if(native_${id})
          get_filename_component(unit_dir "${descriptor_${id}}" DIRECTORY)
          file(RELATIVE_PATH unit "${PROJECT_SOURCE_DIR}" "${unit_dir}")
          if(NOT unit MATCHES "^[a-z]+/[a-z][a-z0-9_-]*$")
            message(FATAL_ERROR "${descriptor_${id}}: native descriptor must live at <layer>/<unit>/plugin.json")
          endif()
          list(GET native_${id} 0 first_target)
          if(NOT TARGET "${first_target}")
            add_subdirectory("${unit_dir}" "${PROJECT_BINARY_DIR}/${unit}")
          endif()
          foreach(member IN LISTS native_${id})
            if(NOT TARGET "${member}")
              message(FATAL_ERROR "${descriptor_${id}}: missing native component ${member}")
            endif()
            get_target_property(directory "${member}" COMPILERINFRA_DIRECTORY)
            if(NOT unit STREQUAL directory)
              message(FATAL_ERROR "${descriptor_${id}}: ${member} belongs to ${directory}")
            endif()
          endforeach()
        endif()
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
  _compilerinfra_policy(layout)
  string(JSON layout SET "${layout}" components "{}")
  set(entries "")
  set(declarations "")
  set(calls "")
  set(pass_calls "")
  foreach(component IN LISTS ARG_COMPONENTS)
    if(NOT TARGET ${component})
      message(FATAL_ERROR "Unknown compilerinfra component ${component}")
    endif()
    get_target_property(layer ${component} COMPILERINFRA_LAYER)
    get_target_property(owner ${component} COMPILERINFRA_DIRECTORY)
    if(NOT layer OR NOT owner)
      message(FATAL_ERROR "${component}: missing v0.2 component ownership")
    endif()
    _compilerinfra_dependencies("${component}" "${ARG_COMPONENTS}" dependencies)
    set(edges "[]")
    foreach(dep IN LISTS dependencies)
      string(JSON n LENGTH "${edges}")
      string(JSON edges SET "${edges}" ${n} "\"${dep}\"")
    endforeach()
    string(JSON layout SET "${layout}" components "${component}"
      "{\"layer\":\"${layer}\",\"directory\":\"${owner}\",\"dependencies\":${edges}}")
    get_target_property(entry ${component} COMPILERINFRA_ENTRY)
    if(entry STREQUAL "_" AND layer STREQUAL "runtime")
      continue()
    endif()
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
  string(HEX "${layout}" layout_hex)
  string(REGEX REPLACE "(..)" "0x\\1," layout_bytes "${layout_hex}")
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
  static const unsigned char organization[] = {${layout_bytes}0};
  auto layout = llvm::json::parse(llvm::StringRef(reinterpret_cast<const char*>(organization),sizeof(organization)-1));
  if (!layout) return layout.takeError();
  a.setLayout(std::move(*layout->getAsObject()));
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
