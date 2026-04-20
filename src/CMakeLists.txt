add_library(event_loop STATIC event_loop.cc)
target_include_directories(event_loop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(backends)
add_subdirectory(frontends)

add_executable(tdb main.cc)
target_link_libraries(tdb PRIVATE frontend)
target_compile_options(tdb PRIVATE -Wall)
