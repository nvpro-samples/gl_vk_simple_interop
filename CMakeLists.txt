cmake_minimum_required(VERSION 3.5)

get_filename_component(PROJNAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)
Project(${PROJNAME})
Message(STATUS "-------------------------------")
Message(STATUS "Processing Project ${PROJNAME}:")

#####################################################################################
# look for nvpro_core 1) as a sub-folder 2) at some other locations
# this cannot be put anywhere else since we still didn't find setup.cmake yet
#
if(NOT BASE_DIRECTORY)
  find_path(BASE_DIRECTORY
    NAMES nvpro_core/cmake/setup.cmake
    PATHS ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR}/../.. 
    REQUIRED
    DOC "Directory containing nvpro_core"
    )
endif()
if(EXISTS ${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
  include(${BASE_DIRECTORY}/nvpro_core/cmake/setup.cmake)
else()
  message(FATAL_ERROR "could not find base directory, please set BASE_DIRECTORY to folder containing nvpro_core")
endif()
    
_add_project_definitions(${PROJNAME})

#####################################################################################
# additions from packages needed for this sample
# add refs  in LIBRARIES_OPTIMIZED
# add refs  in LIBRARIES_DEBUG
# add files in PACKAGE_SOURCE_FILES

set( EXENAME ${PROJNAME} )
_add_package_OpenGL()
_add_package_VulkanSDK()
_add_package_IMGUI()


#####################################################################################
# process the rest of some cmake code that needs to be done *after* the packages add
_add_nvpro_core_lib()

#####################################################################################
# Source files for this project
#
file(GLOB SOURCE_FILES *.cpp *.hpp *.inl *.h *.c)
file(GLOB GLSL_FILES *.glsl)

#####################################################################################
# GLSL to SPIR-V custom build
#
#more than one file can be given: _compile_GLSL("GLSL_mesh.vert;GLSL_mesh.frag" "GLSL_mesh.spv" GLSL_SOURCES)
# the SpirV validator is fine as long as files are for different pipeline stages (entry points still need to be main())
#_compile_GLSL(<source(s)> <target spv> <LIST where files are appended>)
UNSET(GLSL_SOURCES)
UNSET(SPV_OUTPUT)
_compile_GLSL("shaders/shader.comp" "shaders/shader.comp.spv" GLSL_SOURCES SPV_OUTPUT)
source_group(GLSL_Files FILES ${GLSL_SOURCES})

#####################################################################################
# Executable
#
add_executable(${EXENAME} ${SOURCE_FILES} ${COMMON_SOURCE_FILES} ${PACKAGE_SOURCE_FILES} ${GLSL_SOURCES})
set_property(TARGET ${EXENAME} PROPERTY CXX_STANDARD 20)

#####################################################################################
# common source code needed for this sample
#
source_group(common FILES 
  ${COMMON_SOURCE_FILES}
  ${PACKAGE_SOURCE_FILES}
)
source_group("Source Files" FILES ${SOURCE_FILES})

#####################################################################################
# Linkage
#
target_link_libraries(${EXENAME} ${PLATFORM_LIBRARIES} nvpro_core)

foreach(DEBUGLIB ${LIBRARIES_DEBUG})
  target_link_libraries(${EXENAME} debug ${DEBUGLIB})
endforeach(DEBUGLIB)

foreach(RELEASELIB ${LIBRARIES_OPTIMIZED})
  target_link_libraries(${EXENAME} optimized ${RELEASELIB})
endforeach(RELEASELIB)

#####################################################################################
# copies binaries that need to be put next to the exe files (ZLib, etc.)
#

_finalize_target( ${EXENAME} )

install(FILES ${SPV_OUTPUT} CONFIGURATIONS Release DESTINATION "bin_${ARCH}/${PROJNAME}/shaders")
install(FILES ${SPV_OUTPUT} CONFIGURATIONS Debug DESTINATION "bin_${ARCH}_debug/${PROJNAME}/shaders")
