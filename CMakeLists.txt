cmake_minimum_required(VERSION 3.22)

# Get the name of the current directory
get_filename_component(PROJECT_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

#-------------------------------------------------------------------------------
project(${PROJECT_NAME} LANGUAGES CXX)

# Turn off libraries we don't need
option(NVPRO2_ENABLE_nvaftermath off)
option(NVPRO2_ENABLE_nvapp off)
option(NVPRO2_ENABLE_nvgpu_monitor off)
option(NVPRO2_ENABLE_nvgui off)
option(NVPRO2_ENABLE_nvimageformats off)
option(NVPRO2_ENABLE_nvshaders_host off)
option(NVPRO2_ENABLE_nvslang off)
option(NVPRO2_ENABLE_nvvkgltf off)



list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(NvproCore2 REQUIRED)
# Sources and headers/all_in_one/sample_gl_vk_interop/src/main.cpp
file(GLOB CPP_SOURCES "*.cpp" "*.h" "*.hpp")
source_group("Source" FILES ${CPP_SOURCES})

# Compile shaders
file(GLOB SHADER_FILES "shaders/*.glsl")

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/_autogen")

compile_glsl(
   "${SHADER_FILES}"
   "${SHADER_OUTPUT_DIR}"
   GENERATED_SHADER_GLSL_HEADERS
   EXTRA_FLAGS "-I${NVSHADERS_DIR} -I${COMMON_DIR}"
 )


# Add shader files to the project
source_group("Shaders" FILES ${SHADER_FILES})
source_group("Shaders/Compiled" FILES ${GENERATED_SHADER_GLSL_HEADERS})

add_executable(${PROJECT_NAME} ${CPP_SOURCES})

target_sources(${PROJECT_NAME} PRIVATE ${SHADER_FILES} ${GENERATED_SHADER_GLSL_HEADERS})

target_link_libraries(${PROJECT_NAME} PRIVATE
  nvpro2::nvutils
  nvpro2::nvvk
  nvpro2::nvgl
  nvpro2::nvvkglsl
)

add_project_definitions(${PROJECT_NAME})

copy_to_runtime_and_install(${PROJECT_NAME}
  LOCAL_DIRS "${CMAKE_CURRENT_LIST_DIR}/shaders"
  AUTO
)
