# Generated from assetimporters.pro.

# special case begin
qt_feature_module_begin(
    NO_MODULE
    PUBLIC_FILE "qtassetimporters-config.h"
    PRIVATE_FILE "qtassetimporters-config_p.h"
)
include("configure.cmake")
qt_feature_module_end(NO_MODULE)
# special case end

add_subdirectory(uip)
if(QT_FEATURE_assimp)
    add_subdirectory(assimp)
endif()
