# Collect all test source files
set(TEST_SRCS
    main.c
    platform.c
    system.c
)

add_executable(flashtest ${TEST_SRCS})

# set options for compiling the qoraal library
target_compile_options(qoraal PRIVATE -O0 -g)
target_compile_definitions(qoraal PRIVATE CFG_OS_POSIX)
target_compile_options(qoraal-flash PRIVATE -O0 -g)
target_compile_definitions(qoraal-flash PRIVATE CFG_OS_POSIX CONFIG_QORAAL_FLASH_TALLIES=1)
target_compile_options(flashtest PRIVATE -O0 -g)
target_compile_definitions(flashtest PRIVATE CFG_OS_POSIX)

# Link the qoraal library and pthread
if(WIN32)
    target_link_libraries(flashtest qoraal qoraal-flash pthread ws2_32)
else()
    target_link_libraries(flashtest qoraal qoraal-flash pthread)
endif()


# Generate a map file
set_target_properties(flashtest PROPERTIES LINK_FLAGS "-Wl,-Map=output.map -T ${CMAKE_SOURCE_DIR}/test/posix.ld")

# nvol3 regression tests - standalone, no RTOS, self-contained FLASH simulation
add_executable(nvol3test nvol3_test.c)
target_compile_options(nvol3test PRIVATE -O0 -g)
target_compile_definitions(nvol3test PRIVATE CFG_OS_POSIX)
if(WIN32)
    target_link_libraries(nvol3test qoraal qoraal-flash pthread ws2_32)
else()
    target_link_libraries(nvol3test qoraal qoraal-flash pthread)
endif()

set_target_properties(nvol3test PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/test/posix.ld")

# tallies regression tests - same standalone shape as nvol3test
add_executable(talliestest tallies_test.c)
target_compile_options(talliestest PRIVATE -O0 -g)
target_compile_definitions(talliestest PRIVATE CFG_OS_POSIX CONFIG_QORAAL_FLASH_TALLIES=1)
# the generated block includes the module's own list file by name, so the
# directory holding it has to be on the include path
target_include_directories(talliestest PRIVATE ${CMAKE_SOURCE_DIR}/test)
if(WIN32)
    target_link_libraries(talliestest qoraal qoraal-flash pthread ws2_32)
else()
    target_link_libraries(talliestest qoraal qoraal-flash pthread)
endif()

set_target_properties(talliestest PROPERTIES LINK_FLAGS "-T ${CMAKE_SOURCE_DIR}/test/posix.ld")