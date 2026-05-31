add_library(symbols STATIC elf.cc)
target_include_directories(symbols PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
