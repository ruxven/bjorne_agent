cmake_minimum_required(VERSION 3.16)
project(bjorne_agent VERSION 0.1.0 LANGUAGES CXX)

set(BJORNE_AGENT_OPENSSL "static" CACHE STRING "OpenSSL linkage: static, shared, or disabled")
set(BJORNE_AGENT_TLS "openssl" CACHE STRING "TLS backend for the model client: openssl or none")
option(BJORNE_AGENT_COVERAGE "Instrument BJORNE_AGENT targets with gcov coverage" OFF)

# Static linking is its own switch. The deployment modes, a static OpenSSL and
# no OpenSSL at all, default to a fully static executable with no dynamic
# dependencies. A shared OpenSSL build and a coverage build stay dynamic,
# because both need to resolve their libraries at run time.
if(BJORNE_AGENT_COVERAGE)
  set(BJORNE_AGENT_STATIC_DEFAULT OFF)
elseif(BJORNE_AGENT_OPENSSL STREQUAL "shared")
  set(BJORNE_AGENT_STATIC_DEFAULT OFF)
else()
  set(BJORNE_AGENT_STATIC_DEFAULT ON)
endif()
option(BJORNE_AGENT_STATIC "Link a fully static executable with no dynamic dependencies" ${BJORNE_AGENT_STATIC_DEFAULT})

# The warning, optimisation and link flags below are GNU and Clang spellings.
# MSVC rejects them, so the Windows builds leave them out and keep the
# toolchain defaults.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(BJORNE_AGENT_WARNING_FLAGS -Wall -Wextra)
  set(BJORNE_AGENT_RELEASE_FLAGS -O2)
  set(BJORNE_AGENT_COVERAGE_FLAGS -O0 -g)
  set(BJORNE_AGENT_STATIC_LINK_FLAG -static)
  set(BJORNE_AGENT_STRIP_LINK_FLAG -s)
else()
  set(BJORNE_AGENT_WARNING_FLAGS "")
  set(BJORNE_AGENT_RELEASE_FLAGS "")
  set(BJORNE_AGENT_COVERAGE_FLAGS "")
  set(BJORNE_AGENT_STATIC_LINK_FLAG "")
  set(BJORNE_AGENT_STRIP_LINK_FLAG "")
endif()

# A fully static executable needs the GNU link flag. Without it the option
# would silently produce a dynamic binary, so it is turned off instead.
if(BJORNE_AGENT_STATIC AND NOT BJORNE_AGENT_STATIC_LINK_FLAG)
  message(WARNING "BJORNE_AGENT_STATIC needs a GNU or Clang toolchain; building without the static link")
  set(BJORNE_AGENT_STATIC OFF)
endif()

add_library(bjorne_agent_core STATIC
  src/json/Json.cpp
  src/cli/Invocation.cpp
  src/config/Config.cpp
  src/config/ConfigPaths.cpp
  src/http/DnsResponse.cpp
  src/http/HttpClient.cpp
  src/http/Resolver.cpp
  src/http/Url.cpp
  src/log/Log.cpp
  src/model/ModelClient.cpp
  src/context/ContextManager.cpp
  src/tools/Tools.cpp
  src/agent/AgentLoop.cpp
)

target_compile_features(bjorne_agent_core PUBLIC cxx_std_20)
target_compile_options(bjorne_agent_core PRIVATE ${BJORNE_AGENT_WARNING_FLAGS})
target_include_directories(bjorne_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(BJORNE_AGENT_COVERAGE)
  target_compile_options(bjorne_agent_core PRIVATE ${BJORNE_AGENT_COVERAGE_FLAGS} --coverage)
  target_link_options(bjorne_agent_core INTERFACE --coverage)
else()
  target_compile_options(bjorne_agent_core PRIVATE ${BJORNE_AGENT_RELEASE_FLAGS})
endif()

add_executable(bjorne_agent src/main.cpp)
target_compile_features(bjorne_agent PRIVATE cxx_std_20)
target_compile_options(bjorne_agent PRIVATE ${BJORNE_AGENT_WARNING_FLAGS})
target_link_libraries(bjorne_agent PRIVATE bjorne_agent_core)

if(BJORNE_AGENT_COVERAGE)
  target_compile_options(bjorne_agent PRIVATE ${BJORNE_AGENT_COVERAGE_FLAGS} --coverage)
endif()

if(NOT BJORNE_AGENT_OPENSSL MATCHES "^(static|shared|disabled)$")
  message(FATAL_ERROR "BJORNE_AGENT_OPENSSL must be static, shared, or disabled, not '${BJORNE_AGENT_OPENSSL}'")
endif()
if(NOT BJORNE_AGENT_TLS MATCHES "^(openssl|none)$")
  message(FATAL_ERROR "BJORNE_AGENT_TLS must be openssl or none, not '${BJORNE_AGENT_TLS}'")
endif()

# Both switches can turn TLS off: the backend says whether TLS exists at all,
# the linkage says whether OpenSSL is linked. Disabled means no OpenSSL headers
# and no OpenSSL library, static or shared.
set(BJORNE_AGENT_WITH_OPENSSL OFF)
if(BJORNE_AGENT_TLS STREQUAL "openssl" AND NOT BJORNE_AGENT_OPENSSL STREQUAL "disabled")
  set(BJORNE_AGENT_WITH_OPENSSL ON)
endif()

if(BJORNE_AGENT_WITH_OPENSSL)
  if(BJORNE_AGENT_OPENSSL STREQUAL "static")
    set(OPENSSL_USE_STATIC_LIBS ON)
  endif()
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(bjorne_agent_core PUBLIC BJORNE_AGENT_HAS_OPENSSL=1)
  target_link_libraries(bjorne_agent PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
  target_compile_definitions(bjorne_agent_core PUBLIC BJORNE_AGENT_HAS_OPENSSL=0)
  message(STATUS "TLS disabled: building without OpenSSL, https endpoints are rejected")
endif()

if(BJORNE_AGENT_STATIC AND BJORNE_AGENT_OPENSSL STREQUAL "shared")
  message(FATAL_ERROR "BJORNE_AGENT_STATIC=ON cannot work with a shared OpenSSL; use static or disabled")
endif()

enable_testing()

find_package(Threads REQUIRED)

add_library(catch2_main STATIC third_party/catch2/catch_amalgamated.cpp)
target_compile_features(catch2_main PRIVATE cxx_std_20)
target_compile_options(catch2_main PRIVATE ${BJORNE_AGENT_RELEASE_FLAGS} ${BJORNE_AGENT_WARNING_FLAGS})
target_include_directories(catch2_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2)

add_executable(bjorne_agent_tests
  tests/json/JsonTest.cpp
  tests/config/ConfigTest.cpp
  tests/config/ConfigPathsTest.cpp
  tests/cli/InvocationTest.cpp
  tests/http/UrlTest.cpp
  tests/http/ResolverTest.cpp
  tests/http/DnsResponseTest.cpp
  tests/http/HttpClientTest.cpp
  tests/log/LogTest.cpp
  tests/model/ModelClientTest.cpp
  tests/context/ContextManagerTest.cpp
  tests/tools/ToolsTest.cpp
  tests/agent/AgentLoopTest.cpp
)

target_compile_features(bjorne_agent_tests PRIVATE cxx_std_20)
target_compile_options(bjorne_agent_tests PRIVATE ${BJORNE_AGENT_WARNING_FLAGS})
target_include_directories(bjorne_agent_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(bjorne_agent_tests PRIVATE bjorne_agent_core catch2_main Threads::Threads)

if(BJORNE_AGENT_COVERAGE)
  target_compile_options(bjorne_agent_tests PRIVATE ${BJORNE_AGENT_COVERAGE_FLAGS} --coverage)
else()
  target_compile_options(bjorne_agent_tests PRIVATE ${BJORNE_AGENT_RELEASE_FLAGS})
endif()

if(BJORNE_AGENT_WITH_OPENSSL)
  target_link_libraries(bjorne_agent_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(BJORNE_AGENT_STATIC)
  message(STATUS "Static link: bjorne_agent will have no dynamic dependencies")
  target_link_options(bjorne_agent PRIVATE ${BJORNE_AGENT_STATIC_LINK_FLAG})
  target_link_options(bjorne_agent_tests PRIVATE ${BJORNE_AGENT_STATIC_LINK_FLAG})
endif()

# Ship without a symbol table. Coverage builds keep theirs so gcov can map
# instrumented lines back to source; the test binary is never shipped.
if(NOT BJORNE_AGENT_COVERAGE)
  target_link_options(bjorne_agent PRIVATE ${BJORNE_AGENT_STRIP_LINK_FLAG})
endif()

# A static glibc cannot call getaddrinfo: it needs the NSS modules, which are
# shared objects. The static build resolves through /etc/hosts and the libc stub
# resolver instead, so the binary never references getaddrinfo at all.
if(BJORNE_AGENT_STATIC)
  target_compile_definitions(bjorne_agent_core PUBLIC BJORNE_AGENT_USE_STUB_RESOLVER=1)
  message(STATUS "Static link: name resolution uses /etc/hosts and the stub resolver, not nsswitch")
else()
  target_compile_definitions(bjorne_agent_core PUBLIC BJORNE_AGENT_USE_STUB_RESOLVER=0)
endif()

# Install configuration prompts (compression markdown, etc.) relative to config dir
install(DIRECTORY config/prompts/
  DESTINATION
  share/bjorne_agent/prompts
  COMPONENT prompts)

# Install the runtime binary and its static library.
install(TARGETS bjorne_agent
  RUNTIME DESTINATION bin
  COMPONENT runtime)
install(TARGETS bjorne_agent_core
  ARCHIVE DESTINATION lib
  COMPONENT runtime)

# The license travels with the packaged artifacts.
install(FILES LICENSE
  DESTINATION share/licenses/bjorne_agent
  COMPONENT runtime)

add_test(NAME bjorne_agent_tests COMMAND bjorne_agent_tests)

# Packaging for the release job. The archive name carries the project name and
# version so the uploaded artifacts are unambiguous.
set(CPACK_PACKAGE_NAME "bjorne_agent")
set(CPACK_PACKAGE_VENDOR "bjorne_agent")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low dependency AI coding agent")
set(CPACK_PACKAGE_FILE_NAME "bjorne_agent-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "bjorne_agent")
# The Debian and RPM generators refuse to run without a contact address.
set(CPACK_PACKAGE_CONTACT "Ruxven <ruxven@protonmail.ch>")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ruxven <ruxven@protonmail.ch>")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_GENERATOR "TGZ;DEB;RPM")
include(CPack)
