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

# The algorithm handlers. They register themselves at program start and nothing
# calls them directly, so they are built as an object library: from a static
# library the linker would drop them.
add_library(htsim_pcm_algorithms OBJECT
    algorithms/constant.cpp
    algorithms/dctcp.cpp
    algorithms/dctcp_legacy.cpp
    algorithms/nscc.cpp
    algorithms/dcqcn.cpp
    algorithms/swift.cpp
    algorithms/timely.cpp
    algorithms/bbr.cpp
)
target_link_libraries(htsim_pcm_algorithms PUBLIC htsim_pcm)

if ( ENABLE_TESTS )
    set(PCM_UNIT_TESTS
        pcm_accumulator_test
        pcm_runtime_test
        pcm_signal_state_test
        pcm_spec_test
        pcm_timer_test
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

    # Tests of the algorithms in algorithms/, which load their YAML files.
    add_executable(pcm_dctcp_test tests/pcm_dctcp_test.cpp)
    target_link_libraries(pcm_dctcp_test PRIVATE htsim_pcm_algorithms gtest_main)
    target_compile_definitions(pcm_dctcp_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms")
    gtest_discover_tests(pcm_dctcp_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    # NSCC algorithm tests.
    add_executable(pcm_nscc_test tests/pcm_nscc_test.cpp)
    target_link_libraries(pcm_nscc_test PRIVATE
        htsim_pcm_algorithms gtest_main)
    target_compile_definitions(pcm_nscc_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms")
    gtest_discover_tests(pcm_nscc_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )

    # DCQCN algorithm tests.
    add_executable(pcm_dcqcn_test tests/pcm_dcqcn_test.cpp)
    target_link_libraries(pcm_dcqcn_test PRIVATE
        htsim_pcm_algorithms gtest_main)
    target_compile_definitions(pcm_dcqcn_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms")
    gtest_discover_tests(pcm_dcqcn_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
        # Swift algorithm tests.
    add_executable(pcm_swift_test tests/pcm_swift_test.cpp)
    target_link_libraries(pcm_swift_test PRIVATE
        htsim_pcm_algorithms gtest_main)
    target_compile_definitions(pcm_swift_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms")
    gtest_discover_tests(pcm_swift_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
    # TIMELY algorithm tests.
    add_executable(pcm_timely_test tests/pcm_timely_test.cpp)
    target_link_libraries(pcm_timely_test PRIVATE
        htsim_pcm_algorithms gtest_main)
    target_compile_definitions(pcm_timely_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms")
    gtest_discover_tests(pcm_timely_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
    # BBR algorithm tests.
    add_executable(pcm_bbr_test tests/pcm_bbr_test.cpp)

    target_link_libraries(pcm_bbr_test PRIVATE
        htsim_pcm_algorithms
        gtest_main
    )

    target_compile_definitions(pcm_bbr_test PRIVATE
        PCM_ALGORITHMS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/algorithms"
    )

    gtest_discover_tests(pcm_bbr_test
        PROPERTIES LABELS "unit"
        PROPERTIES TIMEOUT 60
    )
endif()
