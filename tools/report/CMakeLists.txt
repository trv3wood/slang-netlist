add_executable(slang-report report.cpp)

target_include_directories(
  slang-report PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_libraries(slang-report PRIVATE slang::slang fmt::fmt
                                           nlohmann_json::nlohmann_json)
target_compile_definitions(slang-report
                           PRIVATE SLANG_NETLIST_VERSION="${PROJECT_VERSION}")

install(TARGETS slang-report)
