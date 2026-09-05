add_executable(slang-netlist driver.cpp)

target_include_directories(
  slang-netlist PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/include)

target_link_libraries(slang-netlist PUBLIC netlist slang::slang fmt::fmt
                                           nlohmann_json::nlohmann_json)
target_compile_definitions(
  slang-netlist
  PRIVATE
    SLANG_NETLIST_VERSION="${PROJECT_VERSION}"
    SLANG_NETLIST_SKILL_SOURCE="${PROJECT_SOURCE_DIR}/skills/slang-netlist")

install(TARGETS slang-netlist)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/skills/slang-netlist/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/slang-netlist/skill)
