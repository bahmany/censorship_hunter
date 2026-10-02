# Stage 4 B: runtime watchdog / failover sources and test suites.
# Included from CMakeLists.txt after hunter_core is defined.
set(HUNTER_RUNTIME_HEALTH_SOURCES
    src/proxy/process_runner.cpp
    src/proxy/health_monitor.cpp
)

function(hunter_add_runtime_health_tests)
    add_executable(fake_engine tests/fake_engine.cpp)
    foreach(_t test_health_monitor test_process_runner test_proxy_manager)
        add_executable(${_t} tests/${_t}.cpp)
        target_link_options(${_t} PRIVATE -static-libgcc -static-libstdc++)
        target_link_libraries(${_t} PRIVATE hunter_core)
        target_compile_definitions(${_t} PRIVATE HUNTER_FAKE_ENGINE_PATH="$<TARGET_FILE:fake_engine>")
        add_dependencies(${_t} fake_engine)
        add_test(NAME ${_t} COMMAND ${_t})
        set_tests_properties(${_t} PROPERTIES TIMEOUT 180)
    endforeach()
    if(HUNTER_INTEGRATION_TESTS)
        add_executable(test_runtime_integration tests/test_runtime_integration.cpp)
        target_link_options(test_runtime_integration PRIVATE -static-libgcc -static-libstdc++)
        target_link_libraries(test_runtime_integration PRIVATE hunter_core)
        add_test(NAME test_runtime_integration COMMAND test_runtime_integration)
        set_tests_properties(test_runtime_integration PROPERTIES LABELS integration TIMEOUT 240 SKIP_RETURN_CODE 77)
    endif()
endfunction()
