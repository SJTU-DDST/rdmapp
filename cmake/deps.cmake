include(FetchContent)

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

fetchcontent_makeavailable(
  asio
  spdlog
)

add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_HAS_STD_COROUTINE)
