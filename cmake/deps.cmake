include(FetchContent)

if(RDMAPP_BUILD_EXAMPLES_PYBIND)
  message("-- [examples] build pybind example: using -fPIC")
  set(RDMAPP_BUILD_PIC ON)
  message("-- [examples] build pybind example: using RTTI")
  set(RDMAPP_BUILD_NORTTI OFF)
endif()

if(RDMAPP_BUILD_PIC)
  message("-- [deps] spdlog: using -fPIC for spdlog")
  set(SPDLOG_BUILD_PIC ON)
endif()


fetchcontent_declare(
  asio
  GIT_REPOSITORY git@github.com:chriskohlhoff/asio.git
  GIT_TAG asio-1-36-0
  GIT_SHALLOW 1
)

fetchcontent_declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
  GIT_SHALLOW 1
)

if(RDMAPP_BUILD_PIC)
  set(CMAKE_PIC_ORIGIN ${CMAKE_POSITION_INDEPENDENT_CODE})
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
fetchcontent_declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 12.0.0
  GIT_SHALLOW 1
)
fetchcontent_makeavailable(fmt)
if(RDMAPP_BUILD_PIC)
  set(CMAKE_POSITION_INDEPENDENT_CODE ${CMAKE_PIC_ORIGIN})
endif()

fetchcontent_makeavailable(
  asio
  spdlog
)

add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
