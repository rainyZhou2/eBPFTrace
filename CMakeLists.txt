cmake_minimum_required(VERSION 3.25)
project(etrace LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(ETRACE_VERSION "dev" CACHE STRING "Version reported by `etrace version`")
option(ETRACE_BUILD_TESTS "Build unit tests" ON)
set(ETRACE_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

find_package(protobuf CONFIG REQUIRED)
# platform/grpc.cpp uses grpc::experimental::InMemoryCertificateProvider and
# IdentityKeyOrSignerCertPair, added in gRPC 1.80. Distribution gRPC (for
# example Debian 13's 1.51) is too old; build with the builder image.
find_package(gRPC 1.80 CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBBPF REQUIRED libbpf)

# clickhouse-cpp ships no CMake package in vcpkg; link its archive and the
# libraries it was built against.
find_path(CLICKHOUSE_INCLUDE_DIR clickhouse/client.h REQUIRED)
find_library(CLICKHOUSE_LIB clickhouse-cpp-lib REQUIRED)
find_library(CITYHASH_LIB cityhash REQUIRED)
find_library(LZ4_LIB lz4 REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)
add_library(etrace_clickhouse_cpp INTERFACE)
target_include_directories(etrace_clickhouse_cpp INTERFACE ${CLICKHOUSE_INCLUDE_DIR})
target_link_libraries(etrace_clickhouse_cpp INTERFACE ${CLICKHOUSE_LIB} ${CITYHASH_LIB} ${LZ4_LIB} ${ZSTD_LIB} absl::int128)

add_library(etrace_libbpf INTERFACE)
target_include_directories(etrace_libbpf INTERFACE ${LIBBPF_STATIC_INCLUDE_DIRS})
target_link_libraries(etrace_libbpf INTERFACE ${LIBBPF_STATIC_LDFLAGS})

set(ETRACE_WARNINGS -Wall -Wextra -Wno-missing-field-initializers)
if(ETRACE_SANITIZE)
  add_compile_options(-fsanitize=${ETRACE_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ETRACE_SANITIZE})
endif()

function(etrace_library name)
  cmake_parse_arguments(L "" "" "SOURCES;DEPS" ${ARGN})
  add_library(${name} STATIC ${L_SOURCES})
  target_include_directories(${name} PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(${name} PRIVATE ${ETRACE_WARNINGS})
  target_link_libraries(${name} PUBLIC ${L_DEPS})
endfunction()

# etrace_embed(<target-var> <namespace> <symbol> <input>) generates a source
# exposing the input bytes as `const std::string_view <namespace>::<symbol>`.
function(etrace_embed out_var namespace symbol input)
  set(output ${CMAKE_BINARY_DIR}/embed/${symbol}.cpp)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DNAMESPACE=${namespace} -DSYMBOL=${symbol}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${input} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  set(${out_var} ${output} PARENT_SCOPE)
endfunction()

# Wire contract.
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/api/proto)
set(PROTO_OUT ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PROTO_OUT})
set(PROTO_SRCS ${PROTO_OUT}/etrace/v1/ingest.pb.cc ${PROTO_OUT}/etrace/v1/ingest.grpc.pb.cc)
add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_OUT}/etrace/v1/ingest.pb.h ${PROTO_OUT}/etrace/v1/ingest.grpc.pb.h
  COMMAND $<TARGET_FILE:protobuf::protoc> -I ${PROTO_DIR} --cpp_out=${PROTO_OUT} --grpc_out=${PROTO_OUT}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> ${PROTO_DIR}/etrace/v1/ingest.proto
  DEPENDS ${PROTO_DIR}/etrace/v1/ingest.proto
  VERBATIM)
add_library(etrace_proto STATIC ${PROTO_SRCS})
target_include_directories(etrace_proto PUBLIC ${PROTO_OUT})
target_link_libraries(etrace_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

# BPF programs, compiled for the build host's architecture and embedded.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(BPF_ARCH arm64)
else()
  set(BPF_ARCH x86)
endif()
set(BPF_SRC ${CMAKE_SOURCE_DIR}/src/agent/source/offcpu/bpf/offcpu.bpf.c)
set(BPF_CLANG clang CACHE STRING "clang used for BPF programs")
execute_process(COMMAND uname -m OUTPUT_VARIABLE HOST_MACHINE OUTPUT_STRIP_TRAILING_WHITESPACE)
set(BPF_CFLAGS -g -O2 -Wall -Werror -target bpfel -D__TARGET_ARCH_${BPF_ARCH} -I/usr/include/${HOST_MACHINE}-linux-gnu)
set(BPF_FP ${CMAKE_BINARY_DIR}/bpf/offcpu_${BPF_ARCH}.bpf.o)
set(BPF_DWARF ${CMAKE_BINARY_DIR}/bpf/offcpu_dwarf_${BPF_ARCH}.bpf.o)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bpf)
add_custom_command(OUTPUT ${BPF_FP} COMMAND ${BPF_CLANG} ${BPF_CFLAGS} -c ${BPF_SRC} -o ${BPF_FP} DEPENDS ${BPF_SRC} VERBATIM)
add_custom_command(OUTPUT ${BPF_DWARF} COMMAND ${BPF_CLANG} ${BPF_CFLAGS} -DETRACE_DWARF_UNWIND -c ${BPF_SRC} -o ${BPF_DWARF}
                   DEPENDS ${BPF_SRC} VERBATIM)
etrace_embed(BPF_FP_SRC etrace::agent::offcpu::objects offcpu_fp ${BPF_FP})
etrace_embed(BPF_DWARF_SRC etrace::agent::offcpu::objects offcpu_dwarf ${BPF_DWARF})

etrace_embed(UI_INDEX etrace::query::ui::assets index_html ${CMAKE_SOURCE_DIR}/src/query/ui/index.html)
etrace_embed(UI_JS etrace::query::ui::assets app_js ${CMAKE_SOURCE_DIR}/src/query/ui/app.js)
etrace_embed(UI_CSS etrace::query::ui::assets app_css ${CMAKE_SOURCE_DIR}/src/query/ui/app.css)

etrace_library(etrace_platform
  SOURCES src/platform/time.cpp src/platform/log.cpp src/platform/config.cpp src/platform/backoff.cpp
          src/platform/spool.cpp src/platform/lifecycle.cpp src/platform/flags.cpp src/platform/url.cpp
          src/platform/tls.cpp src/platform/http.cpp src/platform/telemetry.cpp src/platform/grpc.cpp
          src/platform/auth.cpp src/platform/ratelimit.cpp src/platform/compress.cpp
  DEPS yaml-cpp::yaml-cpp httplib::httplib nlohmann_json::nlohmann_json prometheus-cpp::core gRPC::grpc++
       OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

etrace_library(etrace_model SOURCES src/model/model.cpp DEPS etrace_platform)
etrace_library(etrace_wire SOURCES src/wire/wire.cpp DEPS etrace_model etrace_proto)
etrace_library(etrace_storage SOURCES src/storage/storage.cpp src/storage/memory/memory.cpp DEPS etrace_model)
etrace_library(etrace_storage_clickhouse
  SOURCES src/storage/clickhouse/clickhouse.cpp src/storage/clickhouse/schema.cpp
  DEPS etrace_storage etrace_clickhouse_cpp)
etrace_library(etrace_storage_drivers SOURCES src/storage/drivers.cpp DEPS etrace_storage etrace_storage_clickhouse)
etrace_library(etrace_tracing SOURCES src/tracing/tracing.cpp src/tracing/jaeger.cpp DEPS etrace_platform)
etrace_library(etrace_query
  SOURCES src/query/flame.cpp src/query/pprof.cpp src/query/json.cpp src/query/service.cpp src/query/http.cpp
          src/query/auth.cpp src/query/ui.cpp ${UI_INDEX} ${UI_JS} ${UI_CSS}
  DEPS etrace_storage etrace_tracing)
etrace_library(etrace_collector
  SOURCES src/collector/service.cpp src/collector/grpc.cpp src/collector/auth.cpp
  DEPS etrace_storage etrace_wire)
etrace_library(etrace_symbol
  SOURCES src/symbol/elf.cpp src/symbol/table.cpp src/symbol/resolver.cpp src/symbol/debuginfod.cpp
  DEPS etrace_platform)
etrace_library(etrace_unwind SOURCES src/unwind/unwind.cpp DEPS etrace_symbol)
etrace_library(etrace_agent
  SOURCES src/agent/pipeline.cpp src/agent/plugins.cpp src/agent/exporter/grpc.cpp src/agent/exporter/pyroscope.cpp
          src/agent/processor/container.cpp src/agent/processor/symbolize.cpp
          src/agent/source/offcpu/options.cpp src/agent/source/offcpu/decode.cpp src/agent/source/offcpu/bpf_object.cpp
          src/agent/source/offcpu/uprobes.cpp src/agent/source/offcpu/gpu.cpp src/agent/source/offcpu/unwinder.cpp
          src/agent/source/offcpu/source.cpp
          ${BPF_FP_SRC} ${BPF_DWARF_SRC}
  DEPS etrace_wire etrace_query etrace_symbol etrace_unwind etrace_libbpf)
etrace_library(etrace_doctor
  SOURCES src/doctor/checks.cpp src/doctor/session.cpp src/doctor/report.cpp src/doctor/render.cpp
  DEPS etrace_agent etrace_query)
etrace_library(etrace_app
  SOURCES src/app/common.cpp src/app/agent.cpp src/app/collector.cpp src/app/query.cpp src/app/doctor.cpp
  DEPS etrace_agent etrace_collector etrace_query etrace_doctor etrace_storage_drivers)
target_compile_definitions(etrace_app PRIVATE ETRACE_VERSION="${ETRACE_VERSION}")

add_executable(etrace src/main.cpp)
target_compile_options(etrace PRIVATE ${ETRACE_WARNINGS})
target_link_libraries(etrace PRIVATE etrace_app -static-libstdc++ -static-libgcc)

if(ETRACE_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  file(GLOB TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/test/unit/*.cpp)
  add_executable(etrace_tests ${TEST_SRCS})
  target_compile_options(etrace_tests PRIVATE ${ETRACE_WARNINGS})
  target_compile_definitions(etrace_tests PRIVATE ETRACE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                             ETRACE_BPF_FP_OBJECT="${BPF_FP}" ETRACE_BPF_DWARF_OBJECT="${BPF_DWARF}")
  target_link_libraries(etrace_tests PRIVATE etrace_app GTest::gtest GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(etrace_tests DISCOVERY_MODE PRE_TEST)

  # Loads BPF programs; run it privileged in the host PID namespace.
  add_executable(etrace_kernel_tests test/kernel/offcpu_kernel_test.cpp)
  target_compile_options(etrace_kernel_tests PRIVATE ${ETRACE_WARNINGS})
  target_link_libraries(etrace_kernel_tests PRIVATE etrace_agent GTest::gtest GTest::gtest_main -static-libstdc++ -static-libgcc)
endif()
