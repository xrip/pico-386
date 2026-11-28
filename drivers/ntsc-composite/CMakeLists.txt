add_library(ntsc-composite INTERFACE)

target_sources(ntsc-composite INTERFACE ${CMAKE_CURRENT_LIST_DIR}/ntsc-composite.c)
target_compile_definitions(ntsc-composite INTERFACE NTSC_TV=1)
target_link_libraries(ntsc-composite INTERFACE graphics hardware_dma hardware_gpio hardware_irq hardware_pio)
target_include_directories(ntsc-composite INTERFACE ${CMAKE_CURRENT_LIST_DIR})

# The scanline IRQ must not call flash-resident memset.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/ntsc-composite.c
        DIRECTORY ${PROJECT_SOURCE_DIR}
        PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
