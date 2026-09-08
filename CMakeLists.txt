cmake_minimum_required(VERSION 3.16)
project(rtt_http VERSION 0.1.0 LANGUAGES CXX)

find_package(OROCOS-RTT REQUIRED)
find_package(Boost 1.84 REQUIRED COMPONENTS json)
find_package(Threads REQUIRED)
find_path(RTT_HTTP_HTTPLIB_INCLUDE_DIR httplib.h REQUIRED)
option(RTT_HTTP_TLS "Build optional HTTPS listener support with OpenSSL" ON)
if(RTT_HTTP_TLS)
  find_package(OpenSSL REQUIRED)
endif()
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES "${RTT_HTTP_HTTPLIB_INCLUDE_DIR}")
set(CMAKE_REQUIRED_DEFINITIONS -DCPPHTTPLIB_NO_DEFAULT_SIGPIPE -DCPPHTTPLIB_OWNED_SERVER_SOCKETS)
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
if(WIN32)
  list(APPEND CMAKE_REQUIRED_LIBRARIES ws2_32 crypt32)
endif()
check_cxx_source_compiles("#include <httplib.h>
#if CPPHTTPLIB_SERVER_CONNECTION_SUPPORT != 1 || CPPHTTPLIB_SIGPIPE_POLICY_SUPPORT != 1 || CPPHTTPLIB_OWNED_LISTENER_SUPPORT != 1
#error Required maintained cpp-httplib capabilities are absent
#endif
int main() { return 0; }" RTT_HTTP_HAS_OWNED_CONNECTIONS)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT RTT_HTTP_HAS_OWNED_CONNECTIONS)
  message(FATAL_ERROR "rtt_http requires the maintained cpp-httplib connection ownership/SIGPIPE patch")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

add_library(rtt_http SHARED src/codec_context.cpp src/type_protocol.cpp src/canonical_types.cpp
  src/reflected_codec.cpp src/object_model.cpp src/operation_executor.cpp src/server.cpp)
add_library(rtt_http::rtt_http ALIAS rtt_http)
target_compile_features(rtt_http PUBLIC cxx_std_20)
target_compile_definitions(rtt_http PRIVATE RTT_HTTP_BUILD
  PUBLIC OROCOS_TARGET=${OROCOS_TARGET})
target_include_directories(rtt_http PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/orocos>)
target_include_directories(rtt_http SYSTEM PUBLIC
  "$<BUILD_INTERFACE:${OROCOS-RTT_INCLUDE_DIRS}>")
target_compile_options(rtt_http PUBLIC ${OROCOS-RTT_DEFINITIONS})
target_link_libraries(rtt_http PUBLIC ${OROCOS-RTT_LIBRARIES} Boost::json)
target_include_directories(rtt_http SYSTEM PRIVATE "${RTT_HTTP_HTTPLIB_INCLUDE_DIR}")
target_compile_definitions(rtt_http PRIVATE CPPHTTPLIB_NO_DEFAULT_SIGPIPE CPPHTTPLIB_OWNED_SERVER_SOCKETS)
target_link_libraries(rtt_http PRIVATE Threads::Threads)
if(RTT_HTTP_TLS)
  target_compile_definitions(rtt_http PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(rtt_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(WIN32)
  target_link_libraries(rtt_http PRIVATE ws2_32 crypt32)
else()
  target_compile_definitions(rtt_http PRIVATE CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL)
endif()
set_target_properties(rtt_http PROPERTIES
  OUTPUT_NAME orocos-rtt-http-${OROCOS_TARGET}
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(MSVC)
  target_compile_options(rtt_http PRIVATE /W4 /WX)
else()
  target_compile_options(rtt_http PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(rtt-transport-http MODULE src/type_transport_plugin.cpp)
target_link_libraries(rtt-transport-http PRIVATE rtt_http::rtt_http)
set_target_properties(rtt-transport-http PROPERTIES OUTPUT_NAME rtt-transport-http-${OROCOS_TARGET})
install(TARGETS rtt-transport-http
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/orocos/${OROCOS_TARGET}/rtt_http/types
  RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/orocos/${OROCOS_TARGET}/rtt_http/types)
install(FILES package.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/rtt_http)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rtt_http)

install(TARGETS rtt_http EXPORT rtt_httpTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/rtt/http DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orocos/rtt)
install(EXPORT rtt_httpTargets NAMESPACE rtt_http::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtt_http)
configure_package_config_file(cmake/rtt_httpConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/rtt_httpConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtt_http)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/rtt_httpConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rtt_httpConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/rtt_httpConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtt_http)
configure_file(cmake/rtt_http.pc.in rtt_http-${OROCOS_TARGET}.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rtt_http-${OROCOS_TARGET}.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
  add_executable(server_contract_test tests/server_contract_test.cpp)
  target_link_libraries(server_contract_test PRIVATE rtt_http::rtt_http ${OROCOS-RTT_TYPEKIT_LIBRARIES} Threads::Threads)
  target_include_directories(server_contract_test SYSTEM PRIVATE "${RTT_HTTP_HTTPLIB_INCLUDE_DIR}")
  target_compile_definitions(server_contract_test PRIVATE CPPHTTPLIB_NO_DEFAULT_SIGPIPE CPPHTTPLIB_OWNED_SERVER_SOCKETS)
  if(WIN32)
    target_link_libraries(server_contract_test PRIVATE ws2_32 crypt32)
  endif()
  add_test(NAME server_contract COMMAND server_contract_test)
  set_tests_properties(server_contract PROPERTIES TIMEOUT 30)
  add_executable(json_contract_test tests/json_contract_test.cpp)
  target_link_libraries(json_contract_test PRIVATE rtt_http::rtt_http)
  add_test(NAME json_contract COMMAND json_contract_test)
  set_tests_properties(json_contract PROPERTIES TIMEOUT 10)
  add_executable(type_contract_test tests/type_contract_test.cpp)
  target_link_libraries(type_contract_test PRIVATE rtt_http::rtt_http ${OROCOS-RTT_TYPEKIT_LIBRARIES})
  foreach(case values missing_dependency cycle)
    add_test(NAME type_contract_${case} COMMAND type_contract_test ${case})
    set_tests_properties(type_contract_${case} PROPERTIES TIMEOUT 10)
  endforeach()
endif()
