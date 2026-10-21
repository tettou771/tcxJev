# Shared by generated TrussC test projects and the standalone local build.
target_compile_definitions(tcxJev PRIVATE TCX_JEV_TESTING)
target_compile_definitions(${PROJECT_NAME} PRIVATE TCX_JEV_TESTING)
enable_testing()
add_test(NAME tcxJevTests COMMAND $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(tcxJevTests PROPERTIES TIMEOUT 60)
