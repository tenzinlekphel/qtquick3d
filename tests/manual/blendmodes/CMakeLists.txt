# Generated from blendmodes.pro.

#####################################################################
## blendmodes Binary:
#####################################################################

qt_add_manual_test(blendmodes
    GUI
    SOURCES
        main.cpp
    PUBLIC_LIBRARIES
        Qt::Gui
        Qt::Quick
        Qt::Quick3D
)

# Resources:
set(qml_resource_files
    "BackgroundView.qml"
    "ForegroundView.qml"
    "main.qml"
)

qt_add_resource(blendmodes "qml"
    PREFIX
        "/"
    FILES
        ${qml_resource_files}
)


#### Keys ignored in scope 1:.:.:blendmodes.pro:<TRUE>:
# INSTALLS = "target"
# OTHER_FILES = "doc/src/*.*"
# target.path = "$$[QT_INSTALL_EXAMPLES]/quick3d/blendmodes"
