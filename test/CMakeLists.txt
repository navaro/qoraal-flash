# Collect all test source files
set(TEST_SRCS
    main.c
    services/shell.c
    platform/platform.c
)

# Add the executable target
add_executable(regtest ${TEST_SRCS})

# Link the qoraal library and pthread
target_link_libraries(regtest qoraal qoraal-flash pthread)
target_compile_options(regtest PRIVATE -O0 -g)
target_compile_options(qoraal PRIVATE -O0 -g)
target_compile_options(qoraal-flash PRIVATE -O0 -g)

target_link_options(regtest PRIVATE
    "-Wl,--whole-archive"
    "$<TARGET_FILE:qoraal>"
    "$<TARGET_FILE:qoraal-flash>"
    "-Wl,--no-whole-archive"
)

# Generate a map file
set_target_properties(regtest PROPERTIES LINK_FLAGS "-Wl,-Map=output.map -T ${CMAKE_SOURCE_DIR}/test/platform/posix.ld")