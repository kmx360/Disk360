find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

add_executable(disk360-gui WIN32 main.cpp mainwindow.cpp)
target_link_libraries(disk360-gui PRIVATE fatx Qt6::Widgets)
