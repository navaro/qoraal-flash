# Collect all source files
set(FLASH_SRC
    nvram/nvol3.c
    nvram/nlog2.c
    qshell/registrycmd.c
    qshell/syslogcmd.c
    registry.c
    syslog.c
    qoraal.c
)


# Add the library target
add_library(qoraal-flash STATIC ${FLASH_SRC})

# Link necessary libraries
target_link_libraries(qoraal-flash qoraal)

# Set target include dirs
target_include_directories(qoraal-flash PUBLIC ${PROJECT_SOURCE_DIR}/include)