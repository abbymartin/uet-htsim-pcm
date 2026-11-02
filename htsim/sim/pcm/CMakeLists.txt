# Programmable Congestion Management (UE spec v1.1 §3.6.18).
# See docs/pcm-plan.md.

add_library(htsim_pcm STATIC
    pcm_signals.cpp
    pcm_spec.cpp
)
target_include_directories(htsim_pcm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(htsim_pcm PRIVATE yaml-cpp::yaml-cpp)

if ( ENABLE_TESTS )
    set(PCM_UNIT_TESTS
        pcm_spec_test
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
