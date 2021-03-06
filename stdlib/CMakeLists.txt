set(SCANNER_PATH "${CMAKE_SOURCE_DIR}")
include(${SCANNER_PATH}/cmake/Util/Op.cmake)

option(BUILD_IMGPROC_OPS "" ON)
option(BUILD_CAFFE_OPS "" ON)
option(BUILD_CPM2_OPS "" OFF)
option(BUILD_MOTION_OPS "" ON)
option(BUILD_OPENFACE_OPS "" ON)

set(STDLIB_LIBRARIES)
set(OPENCV_MAJOR_VERSION 3)
set(OPENCV_COMPONENTS)

set(HALIDE_TARGETS)
macro(add_halide_target SRC TARGET)
  if (NOT HALIDE_FOUND)
    find_package(Halide REQUIRED)
    include(${CMAKE_SOURCE_DIR}/cmake/Util/HalideGenerator.cmake)
    include_directories("${HALIDE_INCLUDE_DIR}")
    include_directories("${PROJECT_BINARY_DIR}/generator_genfiles")
  endif()

  get_filename_component(NAME ${SRC} NAME_WE)
  set(GENERATOR ${NAME}.generator)
  halide_add_generator(${GENERATOR}
    SRCS ${SRC})
  halide_add_aot_library(${NAME}
    GENERATOR_TARGET ${GENERATOR}
    GENERATOR_ARGS target=${TARGET})
  list(APPEND HALIDE_TARGETS ${NAME})
  set(HALIDE_TARGETS ${HALIDE_TARGETS} PARENT_SCOPE)
endmacro()

if (BUILD_CUDA)
  find_package(CUDA REQUIRED)
  add_definitions(-DHAVE_CUDA)
  include_directories(${CUDA_INCLUDE_DIRS})
  if(COMPILER_SUPPORTS_CXX1Y)
    set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS} -std=c++11")
  endif()
  list(APPEND STDLIB_LIBRARIES "${CUDA_LIBRARIES}")
else()
  add_definitions(-DCPU_ONLY)
endif()

set(TARGETS)
if (BUILD_CAFFE_OPS)
  add_subdirectory(caffe)
  list(APPEND TARGETS caffe)
endif()

if (BUILD_IMGPROC_OPS)
  add_subdirectory(imgproc)
  list(APPEND TARGETS imgproc)
endif()

if (BUILD_OPENFACE_OPS)
  add_subdirectory(openface)
  list(APPEND TARGETS openface)
endif()

if (BUILD_MOTION_OPS)
  add_subdirectory(motion)
  list(APPEND TARGETS motion)
endif()

set(SOURCES)
foreach (TARGET ${TARGETS})
  list(APPEND SOURCES $<TARGET_OBJECTS:${TARGET}>)
endforeach()

build_op(
  LIB_NAME stdlib
  CPP_SRCS ${SOURCES}
  PROTO_SRC stdlib.proto
  NO_FLAGS ON)
add_dependencies(stdlib scanner)

find_package(OpenCV REQUIRED COMPONENTS "${OPENCV_COMPONENTS}")
list(APPEND STDLIB_LIBRARIES "${OpenCV_LIBRARIES}")

foreach (TARGET ${TARGETS})
  target_include_directories(${TARGET} PUBLIC ${OpenCV_INCLUDE_DIRS})
  add_dependencies(${TARGET} stdlib_proto_files)
endforeach()

target_link_libraries(stdlib PUBLIC "${STDLIB_LIBRARIES}")

if (HALIDE_TARGETS)
  foreach(HALIDE_TARGET ${HALIDE_TARGETS})
    add_dependencies(stdlib "${HALIDE_TARGET}.exec_generator")
    halide_add_aot_library_dependency(stdlib ${HALIDE_TARGET})
  endforeach()
  if (BUILD_CUDA)
    add_dependencies(stdlib scanner_halide)
    target_link_libraries(stdlib PRIVATE scanner_halide)
  endif()
endif()
