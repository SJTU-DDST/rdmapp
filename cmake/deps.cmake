include(FetchContent)

message("-- Find asio: ${ASIO_INCLUDE}")

fetchcontent_declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.16.0
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
fetchcontent_declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG 12.0.0
)
fetchcontent_makeavailable(fmt)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

fetchcontent_makeavailable(
  fmt
  spdlog
)

