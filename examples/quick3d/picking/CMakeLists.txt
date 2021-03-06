# Generated from picking.pro.

cmake_minimum_required(VERSION 3.14)
project(picking LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(INSTALL_EXAMPLEDIR "examples/quick3d/picking")

find_package(Qt6 COMPONENTS Core)
find_package(Qt6 COMPONENTS Gui)
find_package(Qt6 COMPONENTS Quick)
find_package(Qt6 COMPONENTS Quick3D)

add_qt_gui_executable(picking
    main.cpp
)
target_link_libraries(picking PUBLIC
    Qt::Core
    Qt::Gui
    Qt::Quick
    Qt::Quick3D
)


# Resources:
set(qml_resource_files
    "main.qml"
)

qt6_add_resources(picking "qml"
    PREFIX
        "/"
    FILES
        ${qml_resource_files}
)
set(materials_resource_files
    "maps/roughness.jpg"
)

qt6_add_resources(picking "materials"
    PREFIX
        "/"
    FILES
        ${materials_resource_files}
)

install(TARGETS picking
    RUNTIME DESTINATION "${INSTALL_EXAMPLEDIR}"
    BUNDLE DESTINATION "${INSTALL_EXAMPLEDIR}"
    LIBRARY DESTINATION "${INSTALL_EXAMPLEDIR}"
)
