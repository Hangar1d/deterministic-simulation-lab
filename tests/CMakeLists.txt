add_executable(simlab_tests test_simulation.cpp)
target_link_libraries(simlab_tests PRIVATE simlab_codec simlab_build_options)
foreach(case IN ITEMS golden determinism seed_mapping engine_vectors rejection ordering
        unsorted_arrivals empty boundary invalid_schedule overflow_atomic snapshot_recovery
        snapshot_ownership invalid_restore malformed_plan malformed_snapshot record_codec
        locale job_limit conservation)
    add_test(NAME core.${case} COMMAND simlab_tests ${case})
endforeach()

foreach(case IN ITEMS reproducible record_replay tampered_record snapshot_resume invalid_io invalid_snapshot)
    add_test(NAME cli.${case} COMMAND "${CMAKE_COMMAND}"
        "-DSIMLAB=$<TARGET_FILE:simlab>"
        "-DSOURCE=${PROJECT_SOURCE_DIR}"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-${case}"
        "-DCASE=${case}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")
endforeach()
