# Used by both trusscli's project and the standalone checkout build.
target_link_libraries(${PROJECT_NAME} PRIVATE tcxCurl)
enable_testing()
add_test(NAME liveRunnerOffline COMMAND $<TARGET_FILE:${PROJECT_NAME}> --self-test)
set_tests_properties(liveRunnerOffline PROPERTIES TIMEOUT 30 ENVIRONMENT "TYPESAFE_API_KEY=")
add_test(NAME liveRunnerOfflineErrors COMMAND $<TARGET_FILE:${PROJECT_NAME}> --self-test-errors)
set_tests_properties(liveRunnerOfflineErrors PROPERTIES TIMEOUT 30 ENVIRONMENT "TYPESAFE_API_KEY="
    FAIL_REGULAR_EXPRESSION "OFFLINE_HEADER_ECHO_SENTINEL;Authorization:;Bearer ")
add_test(NAME liveRunnerSkipUnset COMMAND ${CMAKE_COMMAND} -E env --unset=TYPESAFE_API_KEY $<TARGET_FILE:${PROJECT_NAME}>)
add_test(NAME liveRunnerSkipEmpty COMMAND ${CMAKE_COMMAND} -E env "TYPESAFE_API_KEY=" $<TARGET_FILE:${PROJECT_NAME}>)
set_tests_properties(liveRunnerSkipUnset liveRunnerSkipEmpty PROPERTIES TIMEOUT 10
    PASS_REGULAR_EXPRESSION "SKIP: TYPESAFE_API_KEY not set")
