add_subdirectory(backends)

add_executable(tdb main.cc)
target_link_libraries(tdb PRIVATE backend)
target_compile_options(tdb PRIVATE -Wall)
