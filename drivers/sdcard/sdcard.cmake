if (NOT TARGET sdcard)
    add_library(sdcard INTERFACE)

    target_sources(sdcard INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/diskio_async.c
            ${CMAKE_CURRENT_LIST_DIR}/sdcard_async.c
    )
    target_compile_definitions(sdcard INTERFACE SDCARD_ASYNC_ENABLE_STATS=0)

    target_link_libraries(sdcard INTERFACE fatfs pico_stdlib hardware_spi hardware_dma)
    target_include_directories(sdcard INTERFACE ${CMAKE_CURRENT_LIST_DIR})
endif()
