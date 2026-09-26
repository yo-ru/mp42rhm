cmake_minimum_required(VERSION 3.24)
project(mp42rhm VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This prototype uses Win32 process pipes and requires Windows.")
endif()

include(FetchContent)
FetchContent_Declare(rhmparse
  GIT_REPOSITORY https://github.com/yo-ru/rhmParse.git
  GIT_TAG c25f0cfb95d9573b578125317bb72e1e46aa2172
)
FetchContent_MakeAvailable(rhmparse)

add_library(converter src/converter.cpp src/process.cpp src/archive.cpp src/note_order.cpp)
target_compile_features(converter PUBLIC cxx_std_17)
target_include_directories(converter PUBLIC src PRIVATE ${rhmparse_SOURCE_DIR}/src)
target_compile_definitions(converter PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(converter PRIVATE rhmParse::rhmParse bcrypt)

add_executable(mp42rhm src/main.cpp)
target_link_libraries(mp42rhm PRIVATE converter)

if(MSVC)
  target_compile_options(converter PRIVATE /W4 /utf-8)
  target_compile_options(mp42rhm PRIVATE /W4 /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(converter_tests tests/converter_tests.cpp)
  add_dependencies(converter_tests mp42rhm)
  target_include_directories(converter_tests PRIVATE ${rhmparse_SOURCE_DIR}/src)
  target_link_libraries(converter_tests PRIVATE converter rhmParse::rhmParse)
  add_test(NAME converter_tests COMMAND converter_tests)
endif()
