# Collect all source files
set(FLASH_SRC
    nvram/nvol3.c
    qshell/registrycmd.c
    registry.c
)


# Add the library target
add_library(qoraal-flash STATIC ${FLASH_SRC})

# Link necessary libraries
target_link_libraries(qoraal-flash qoraal)

