# Generated from effectlib.pro.

#####################################################################
## qtquick3deffectplugin Plugin:
#####################################################################

qt_add_qml_module(qtquick3deffectplugin
    URI "QtQuick3D.Effects"
    VERSION "1.${CMAKE_PROJECT_VERSION_MINOR}"
    DESIGNER_SUPPORTED
    CLASSNAME QtQuick3DEffectPlugin
    DEPENDENCIES
        QtQuick3D/1.15
        QtQuick.Window/2.1
    SOURCES
        plugin.cpp
    PUBLIC_LIBRARIES
        Qt::Qml
        Qt::Quick
        Qt::Quick3DPrivate
)

# Resources:
set(qteffectlibrary_resource_files
    "maps/brushnoise.png"
    "maps/white.png"
    "shaders/additivecolorgradient.frag"
    "shaders/blend.frag"
    "shaders/blur.frag"
    "shaders/blurhorizontal.vert"
    "shaders/blurvertical.vert"
    "shaders/brushstrokes.frag"
    "shaders/chromaticaberration.frag"
    "shaders/colormaster.frag"
    "shaders/combiner.frag"
    "shaders/depthoffieldblur.frag"
    "shaders/depthoffieldblur.vert"
    "shaders/desaturate.frag"
    "shaders/distortion.vert"
    "shaders/distortionripple.frag"
    "shaders/distortionsphere.frag"
    "shaders/distortionspiral.frag"
    "shaders/downsample.frag"
    "shaders/downsample.vert"
    "shaders/downsampletiltshift.frag"
    "shaders/edgedetect.frag"
    "shaders/edgedetect.vert"
    "shaders/emboss.frag"
    "shaders/flip.frag"
    "shaders/fxaaBlur.frag"
    "shaders/fxaaRgbl.frag"
    "shaders/gaussianblur.frag"
    "shaders/luminosity.frag"
    "shaders/luminosity.vert"
    "shaders/motionblurhorizontal.frag"
    "shaders/motionblurhorizontal.vert"
    "shaders/motionblurvertical.frag"
    "shaders/motionblurvertical.vert"
    "shaders/poissonblur.frag"
    "shaders/poissonblur.vert"
    "shaders/poissonblurtiltshift.frag"
    "shaders/poissonblurtiltshift.vert"
    "shaders/scatter.frag"
    "shaders/scurvetonemap.frag"
    "shaders/vignette.frag"
)

qt_add_resource(qtquick3deffectplugin "qteffectlibrary"
    PREFIX
        "/"
    FILES
        ${qteffectlibrary_resource_files}
)


#### Keys ignored in scope 2:.:.:plugin.pro:<TRUE>:
# CXX_MODULE = "qml"
# DISTFILES = "qmldir"
# EFFECT_IMAGE_FILES = "maps/brushnoise.png" "maps/white.png"
# IMPORT_VERSION = "1.$$QT_MINOR_VERSION"
# OTHER_FILES = "$$QML_FILES"
# QML_FILES = "Vignette.qml" "TiltShift.qml" "SCurveTonemap.qml" "Scatter.qml" "MotionBlur.qml" "HDRBloomTonemap.qml" "GaussianBlur.qml" "Fxaa.qml" "Flip.qml" "Emboss.qml" "EdgeDetect.qml" "DistortionSpiral.qml" "DistortionSphere.qml" "DistortionRipple.qml" "Desaturate.qml" "DepthOfFieldHQBlur.qml" "ColorMaster.qml" "ChromaticAberration.qml" "BrushStrokes.qml" "Blur.qml" "AdditiveColorGradient.qml" "$$EFFECT_IMAGE_FILES"
# TARGETPATH = "QtQuick3D/Effects"

set(qml_files
    "Vignette.qml"
    "TiltShift.qml"
    "SCurveTonemap.qml"
    "Scatter.qml"
    "MotionBlur.qml"
    "HDRBloomTonemap.qml"
    "GaussianBlur.qml"
    "Fxaa.qml"
    "Flip.qml"
    "Emboss.qml"
    "EdgeDetect.qml"
    "DistortionSpiral.qml"
    "DistortionSphere.qml"
    "DistortionRipple.qml"
    "Desaturate.qml"
    "DepthOfFieldHQBlur.qml"
    "ColorMaster.qml"
    "ChromaticAberration.qml"
    "BrushStrokes.qml"
    "Blur.qml"
    "AdditiveColorGradient.qml"
    "maps/brushnoise.png"
    "maps/white.png"
)
set_source_files_properties(Vignette.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(TiltShift.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(SCurveTonemap.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Scatter.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(MotionBlur.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(HDRBloomTonemap.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(GaussianBlur.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Fxaa.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Flip.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Emboss.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(EdgeDetect.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(DistortionSpiral.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(DistortionSphere.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(DistortionRipple.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Desaturate.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(DepthOfFieldHQBlur.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(ColorMaster.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(ChromaticAberration.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(BrushStrokes.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(Blur.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(AdditiveColorGradient.qml PROPERTIES
    QT_QML_SOURCE_VERSION "1.0"
)
set_source_files_properties(maps/brushnoise.png PROPERTIES
    QT_QML_SKIP_QMLDIR_ENTRY TRUE
)
set_source_files_properties(maps/white.png PROPERTIES
    QT_QML_SKIP_QMLDIR_ENTRY TRUE
)

qt6_target_qml_files(qtquick3deffectplugin
    FILES
        ${qml_files}
)
