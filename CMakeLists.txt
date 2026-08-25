cmake_minimum_required(VERSION 3.22)
project(examvan_cpp VERSION 2.7.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)

option(ENABLE_SANITIZERS "Build with ASan+UBSan" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

option(WITH_UWEBSOCKETS "Fetch uWebSockets/uSockets" OFF)
if(WITH_UWEBSOCKETS)
  FetchContent_Declare(uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets
    GIT_TAG v0.8.8
  )
  FetchContent_Declare(uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets
    GIT_TAG v20.71.0
  )
  FetchContent_MakeAvailable(uSockets uWebSockets)
  add_compile_definitions(HAS_UWEBSOCKETS=1)
endif()

find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
  add_compile_definitions(HAS_LIBPQ=1)
endif()
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(HIREDIS QUIET hiredis)
  pkg_check_modules(LIBCURL QUIET libcurl)
  if(HIREDIS_FOUND)
    add_compile_definitions(HAS_HIREDIS=1)
  endif()
  if(LIBCURL_FOUND)
    add_compile_definitions(HAS_LIBCURL=1)
  endif()
endif()

add_library(examvan_core STATIC
  src/config/config.cpp
  src/utils/sanitize.cpp
  src/session/cookie.cpp
  src/session/csrf.cpp
  src/websocket/hub.cpp
  src/websocket/socketio.cpp
  src/db/pool.cpp
  src/redis/client.cpp
  src/http/router.cpp
  src/http/handlers.cpp
  src/http/router_full.cpp
  src/helpers/utils.cpp
  src/services/examtoken/examtoken.cpp
  src/handlers/r2/r2.cpp
  src/middleware/cors.cpp
  src/middleware/ratelimit.cpp
  src/middleware/auth.cpp
  src/middleware/version.cpp
  src/middleware/body_limit.cpp
  src/middleware/turnstile.cpp
  src/middleware/scoring.cpp
  src/handlers/public/hasil.cpp
  src/handlers/public/download.cpp
  src/handlers/api/exams.cpp
  src/handlers/api/webhook.cpp
  src/handlers/admin/dashboard.cpp
  src/handlers/admin/users.cpp
  src/handlers/admin/vouchers.cpp
  src/handlers/admin/exams.cpp
  src/handlers/admin/settings.cpp
  src/handlers/admin/pengawas.cpp
  src/handlers/admin/submissions.cpp
  src/handlers/admin/export.cpp
  src/queue/submission_queue.cpp
  src/jobs/jobs.cpp
  src/server/server.cpp
  src/db/pool_real.cpp
  src/redis/redis_real.cpp
)

target_include_directories(examvan_core PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(examvan_core PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(PostgreSQL_FOUND)
  target_link_libraries(examvan_core PUBLIC PostgreSQL::PostgreSQL)
  target_include_directories(examvan_core PUBLIC ${PostgreSQL_INCLUDE_DIRS})
endif()
if(HIREDIS_FOUND)
  target_link_libraries(examvan_core PUBLIC ${HIREDIS_LIBRARIES})
  target_include_directories(examvan_core PUBLIC ${HIREDIS_INCLUDE_DIRS})
endif()
if(LIBCURL_FOUND)
  target_link_libraries(examvan_core PUBLIC ${LIBCURL_LIBRARIES})
  target_include_directories(examvan_core PUBLIC ${LIBCURL_INCLUDE_DIRS})
endif()

add_executable(examvan-server src/main.cpp)
target_link_libraries(examvan-server PRIVATE examvan_core)

enable_testing()
add_executable(examvan-tests
  tests/test_sanitize.cpp
  tests/test_socketio.cpp
  tests/test_session.cpp
  tests/test_hub.cpp
  tests/test_config.cpp
  tests/test_csrf.cpp
  tests/test_handlers.cpp
  tests/test_models.cpp
  tests/test_middleware.cpp
  tests/test_api.cpp
  tests/test_public.cpp
  tests/test_r2.cpp
  tests/test_new_coverage.cpp
  tests/test_queue.cpp
  tests/test_export.cpp
  tests/test_characterization.cpp
  tests/test_scoring.cpp
  tests/test_server_live.cpp
  tests/test_f4_hasil.cpp
)
target_link_libraries(examvan-tests PRIVATE examvan_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(examvan-tests)
