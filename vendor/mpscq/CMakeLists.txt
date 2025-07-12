add_library(mpscq OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/mpsc.c)

target_include_directories(mpscq
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
