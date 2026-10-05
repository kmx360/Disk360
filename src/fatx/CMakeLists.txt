add_library(fatx fatx.cpp partitions.cpp)
target_include_directories(fatx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fatx PUBLIC diskio)
