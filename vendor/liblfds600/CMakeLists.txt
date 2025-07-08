file(GLOB LIB_SOURCES_ABSTRACTION "${CMAKE_CURRENT_SOURCE_DIR}/src/lfds600_abstraction/*.c")
file(GLOB LIB_SOURCES_FREELIST "${CMAKE_CURRENT_SOURCE_DIR}/src/lfds600_freelist/*.c")
file(GLOB LIB_SOURCES_QUEUE "${CMAKE_CURRENT_SOURCE_DIR}/src/lfds600_queue/*.c")

add_library(liblfds600 OBJECT
    ${LIB_SOURCES_ABSTRACTION}
    ${LIB_SOURCES_FREELIST}
    ${LIB_SOURCES_QUEUE}
)

target_include_directories(liblfds600
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
