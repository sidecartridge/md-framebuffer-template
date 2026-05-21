# Create a library for the httpc files
add_library(httpc
    httpc.c
)

# Add the include directory (for httpc.h)
target_include_directories(httpc PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..   # add parent folder if lwipopts.h is there
)

# Link necessary libraries
target_link_libraries(httpc PRIVATE
    pico_stdlib    # Core Pico SDK library
    pico_lwip_http
    pico_lwip_mbedtls
    pico_mbedtls
)

# Conditionally link the CYW43 architecture library if supported
if (PICO_CYW43_SUPPORTED)
    if (DEFINED PICO_CYW43_ARCH_POLL AND PICO_CYW43_ARCH_POLL)
        message(STATUS "Using pico_cyw43_arch_lwip_poll backend")
        target_link_libraries(httpc PRIVATE pico_cyw43_arch_lwip_poll)
    else()
        message(STATUS "Using pico_cyw43_arch_lwip_threadsafe_background backend")
        target_link_libraries(httpc PRIVATE pico_cyw43_arch_lwip_threadsafe_background)
    endif()
else()
    message(WARNING "CYW43 architecture not supported")
endif()
