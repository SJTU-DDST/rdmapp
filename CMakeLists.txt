cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
project(rdmapp CXX)
set(CMAKE_CXX_STANDARD 20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)
find_package(ibverbs REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

string(LENGTH "${CMAKE_SOURCE_DIR}/" SOURCE_PATH_LENGTH)
add_definitions("-DSOURCE_PATH_LENGTH=${SOURCE_PATH_LENGTH}")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  option(RDMAPP_BUILD_EXAMPLES "Build examples" ON)
else()
  option(RDMAPP_BUILD_EXAMPLES "Build examples" OFF)
  message(STATUS "[rdmapp] rdmapp is imported as subdirectory")
endif()

option(RDMAPP_BUILD_DOCS "Build docs" OFF)
option(RDMAPP_ASAN "Build with AddressSanitizer" OFF)
option(RDMAPP_BUILD_EXAMPLES_PYBIND "Build pybind11 example" OFF)
option(RDMAPP_BUILD_NORTTI "Build without RTTI" ON)
option(RDMAPP_BUILD_PIC "Build with -fPIC" OFF)

function(print_options_compact)
  message(STATUS "[rdmapp] config:")
  message(STATUS "  RDMAPP_BUILD_DOCS: ${RDMAPP_BUILD_DOCS}")
  message(STATUS "  RDMAPP_ASAN: ${RDMAPP_ASAN}")
  message(STATUS "  RDMAPP_BUILD_EXAMPLES: ${RDMAPP_BUILD_EXAMPLES}")
  message(STATUS "  RDMAPP_BUILD_EXAMPLES_PYBIND: ${RDMAPP_BUILD_EXAMPLES_PYBIND}")
  message(STATUS "  RDMAPP_BUILD_NORTTI: ${RDMAPP_BUILD_NORTTI}")
  message(STATUS "  RDMAPP_BUILD_PIC: ${RDMAPP_BUILD_PIC}")
endfunction()

if(RDMAPP_BUILD_EXAMPLES_PYBIND)
  message(STATUS "[rdmapp/deps] pybind: looking for pybind11")
  find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
  find_package(pybind11 CONFIG REQUIRED)

  message(STATUS "[rdmapp/examples] build pybind example: using -fPIC")
  set(RDMAPP_BUILD_PIC ON)

  message(STATUS "[rdmapp/examples] build pybind example: using RTTI")
  set(RDMAPP_BUILD_NORTTI OFF)
endif()

if(RDMAPP_BUILD_PIC)
  message(STATUS "[rdmapp/deps] spdlog: using -fPIC for spdlog")
  set(SPDLOG_BUILD_PIC ON)
endif()

print_options_compact()

include(cmake/deps.cmake)

if(RDMAPP_BUILD_DOCS)
  # check if Doxygen is installed
  find_package(Doxygen)

  if(DOXYGEN_FOUND)
    # set input and output files
    set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)

    # request to configure the file
    message("Doxygen build started")

    # note the option ALL which allows to build the docs together with the application
    add_custom_target(doc_doxygen ALL
      COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Generating API documentation with Doxygen"
      VERBATIM)
  else(DOXYGEN_FOUND)
    message("Doxygen need to be installed to generate the doxygen documentation")
  endif(DOXYGEN_FOUND)
endif()

set(RDMAPP_SOURCE_FILES
  src/device.cc
  src/pd.cc
  src/cq.cc
  src/qp.cc
  src/srq.cc
  src/cq_poller.cc
  src/executor.cc
  src/mr.cc
)

set(RDMAPP_LINK_LIBRARIES ibverbs Threads::Threads spdlog)

add_library(rdmapp STATIC ${RDMAPP_SOURCE_FILES})

list(APPEND
  RDMAPP_COMPILE_OPTIONS
  PRIVATE
  -Wall
  -Wextra
  -pedantic
)
list(APPEND
  RDMAPP_LINK_OPTIONS
  PRIVATE
)
if(RDMAPP_BUILD_NORTTI)
  message(STATUS "[rdmapp] build: build with -fno-rtti and disable asio typeid")
  list(APPEND RDMAPP_COMPILE_OPTIONS PUBLIC -fno-rtti -DASIO_NO_TYPEID)
else()
  message(STATUS "[rdmapp] build: build with RTTI")
endif()

if(RDMAPP_BUILD_PIC)
  list(APPEND RDMAPP_COMPILE_OPTIONS -fPIC)
  message(STATUS "[rdmapp] build: using -fPIC for rdmapp")
else()
endif()

if(RDMAPP_ASAN)
  list(APPEND RDMAPP_COMPILE_OPTIONS PRIVATE -fno-omit-frame-pointer -fsanitize=address)
  list(APPEND RDMAPP_LINK_OPTIONS -fno-omit-frame-pointer -fsanitize=address)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "17")
    list(APPEND RDMAPP_COMPILE_OPTIONS
      PUBLIC
      -fcoroutines-ts
    )
  endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES GNU)
  if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "11")
    list(APPEND RDMAPP_COMPILE_OPTIONS
      PUBLIC
      -fcoroutines
    )
  endif()
endif()

target_compile_options(rdmapp PRIVATE ${RDMAPP_COMPILE_OPTIONS})
target_link_libraries(rdmapp PUBLIC ${RDMAPP_LINK_LIBRARIES})
target_include_directories(rdmapp PUBLIC include)
target_link_libraries(rdmapp PUBLIC asio)

find_program(iwyu_path NAMES include-what-you-use iwyu)

if(iwyu_path)
  message(STATUS "Using include-what-you-use ${iwyu_path}")
  set_property(TARGET rdmapp PROPERTY CXX_INCLUDE_WHAT_YOU_USE ${iwyu_path})
endif()

if(RDMAPP_BUILD_EXAMPLES)
  set(RDMAPP_EXAMPLES_LIB_SOURCE_FILES
    examples/qp_transmission.cc
    examples/qp_acceptor.cc
    examples/qp_connector.cc
    examples/helloworld_handler.cc
  )

  add_library(rdmapp_examples STATIC ${RDMAPP_EXAMPLES_LIB_SOURCE_FILES})
  target_compile_options(rdmapp_examples ${RDMAPP_COMPILE_OPTIONS})
  target_include_directories(rdmapp_examples PUBLIC examples/include)
  target_link_libraries(rdmapp_examples PUBLIC rdmapp)
  target_link_options(rdmapp_examples ${RDMAPP_LINK_OPTIONS})
  set(RDMAPP_EXAMPLES helloworld send_bw write_bw rpc)

  foreach(EXAMPLE IN LISTS RDMAPP_EXAMPLES)
    add_executable(${EXAMPLE} examples/${EXAMPLE}.cc)
    target_link_libraries(${EXAMPLE} rdmapp_examples)
    target_link_libraries(${EXAMPLE} spdlog)
    target_compile_options(${EXAMPLE} ${RDMAPP_COMPILE_OPTIONS})
  endforeach()

  if(RDMAPP_BUILD_EXAMPLES_PYBIND)
    # NOTE: this is how to configure a pybind cmake project with rdmapp
    pybind11_add_module(
      rdmapp_py
      examples/pybind11/helloworld.cc
      examples/qp_transmission.cc
      examples/qp_acceptor.cc
      examples/qp_connector.cc
      examples/helloworld_handler.cc
    )

    # NOTE: must enable RTTI for pybind11!
    # if -fno-rtti is used for sources in pybind11, there will be really complicated bugs
    # for example: you could not link rdmapp_py to library:rdampp_examples(which disable ftti).
    # in the linking stage, it surely will work, but for pybind11 program, there will be really
    # fucking tricky problems.
    target_include_directories(rdmapp_py PUBLIC examples/include)
    target_compile_options(rdmapp_py PRIVATE -Wall -Wextra -pedantic)
    target_link_libraries(rdmapp_py PRIVATE rdmapp)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS rdmapp EXPORT rdmapp ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/rdmapp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
