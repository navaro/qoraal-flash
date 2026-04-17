# Zephyr module integration for Qoraal-http

zephyr_library()

# Includes
zephyr_include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/..
    ${CMAKE_CURRENT_LIST_DIR}/../include
    ${ZEPHYR_QORAAL_MODULE_DIR}/include
)

# Core sources (adjust if needed)
zephyr_library_sources_ifdef(CONFIG_QORAAL_FLASH
    ${CMAKE_CURRENT_LIST_DIR}/../src/nvram/nvol3.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/nvram/nlog2.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/qshell/registrycmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/qshell/syslogcmd.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/registry.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/regenum.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/syslog.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/qoraal.c

)

# Optional compile defs (keep minimal for now)
zephyr_compile_definitions(
    CFG_OS_ZEPHYR=1
)




