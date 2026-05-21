# Create the u8g2 library
file(GLOB U8G2_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/*.c
)

# Add library target
add_library(u8g2 STATIC ${U8G2_SOURCES})

# Add the include directory
target_include_directories(u8g2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link necessary libraries
target_link_libraries(u8g2
    pico_stdlib    # Core Pico SDK library
) 
