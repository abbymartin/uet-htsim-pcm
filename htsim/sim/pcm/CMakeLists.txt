# Programmable Congestion Management (UE spec v1.1 §3.6.18).
# See docs/pcm-plan.md.

add_library(htsim_pcm STATIC
    pcm_accumulator.cpp
    pcm_registry.cpp
    pcm_runtime.cpp
    pcm_signal_state.cpp
    pcm_signals.cpp
    pcm_spec.cpp
    pcm_trigger.cpp
)
target_include_directories(htsim_pcm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(htsim_pcm PRIVATE yaml-cpp::yaml-cpp)

if ( ENABLE_TESTS )
    set(PCM_UNIT_TESTS
        pcm_accumulator_test
        pcm_runtime_test
        pcm_signal_state_test
        pcm_spec_test
        pcm_trigger_test
    )
    foreach(TEST_NAME ${PCM_UNIT_TESTS})
        add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
        target_link_libraries(${TEST_NAME} PRIVATE htsim_pcm gtest_main gmock)
        gtest_discover_tests(${TEST_NAME}
            PROPERTIES LABELS "unit"
            PROPERTIES TIMEOUT 60
        )
    endforeach()
endif()
