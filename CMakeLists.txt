cmake_minimum_required(VERSION 3.28.3)

project(peeves
    VERSION 0.0.0
    LANGUAGES C
)

add_subdirectory(vendor)

add_executable(peeves
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cmdline.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/logger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
)

target_link_libraries(peeves
    PRIVATE
        mpscq
)

if(LOG_ENABLED)
    target_compile_definitions(peeves
        PRIVATE LOG_ENABLED
    )
endif()

target_compile_features(peeves
    PRIVATE
        c_std_11
)
