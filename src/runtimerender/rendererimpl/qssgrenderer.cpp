/****************************************************************************
**
** Copyright (C) 2008-2012 NVIDIA Corporation.
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Quick 3D.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtQuick3DRuntimeRender/private/qssgrenderer_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercontextcore_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercamera_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderlight_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderimage_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderbuffermanager_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercontextcore_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendereffect_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderresourcemanager_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercustommaterialsystem_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendershadercodegenerator_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderdefaultmaterialshadergenerator_p.h>
#include <QtQuick3DRuntimeRender/private/qssgperframeallocator_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrhiquadrenderer_p.h>

#include <QtQuick3DUtils/private/qssgdataref_p.h>
#include <QtQuick3DUtils/private/qssgutils_p.h>

#include <cstdlib>
#include <algorithm>

#ifdef Q_CC_MSVC
#pragma warning(disable : 4355)
#endif

// Quick tests you can run to find performance problems

//#define QSSG_RENDER_DISABLE_HARDWARE_BLENDING 1
//#define QSSG_RENDER_DISABLE_LIGHTING 1
//#define QSSG_RENDER_DISABLE_TEXTURING 1
//#define QSSG_RENDER_DISABLE_TRANSPARENCY 1
//#define QSSG_RENDER_DISABLE_FRUSTUM_CULLING 1

// If you are fillrate bound then sorting opaque objects can help in some circumstances
//#define QSSG_RENDER_DISABLE_OPAQUE_SORT 1

QT_BEGIN_NAMESPACE

struct QSSGRenderableImage;
struct QSSGSubsetRenderable;

void QSSGRenderer::releaseResources()
{
    delete m_rhiQuadRenderer; // TODO: pointer to incomplete type!
    m_rhiQuadRenderer = nullptr;

    m_rhiShaders.clear();
    m_instanceRenderMap.clear();
}

QSSGRenderer::QSSGRenderer(QSSGRenderContextInterface *ctx)
    : m_contextInterface(ctx)
    , m_bufferManager(ctx->bufferManager())
    , m_currentLayer(nullptr)
    , m_layerGPuProfilingEnabled(false)
    , m_progressiveAARenderRequest(false)
{
}

QSSGRenderer::~QSSGRenderer()
{
    m_contextInterface = nullptr;
    releaseResources();
}

void QSSGRenderer::childrenUpdated(QSSGRenderNode &inParent)
{   
    if (inParent.type == QSSGRenderGraphObject::Type::Layer) {
        const QSSGRenderLayer *theLayer = layerForNode(inParent);
        auto theIter = m_instanceRenderMap.find(theLayer);
        if (theIter != m_instanceRenderMap.end()) {
            theIter.value()->cameras.clear();
            theIter.value()->lights.clear();
            theIter.value()->renderableNodes.clear();
        }
    } else if (inParent.parent) {
        childrenUpdated(*inParent.parent);
    }
}

static inline QSSGRenderLayer *getNextLayer(QSSGRenderLayer &inLayer)
{
    if (inLayer.nextSibling && inLayer.nextSibling->type == QSSGRenderGraphObject::Type::Layer)
        return static_cast<QSSGRenderLayer *>(inLayer.nextSibling);
    return nullptr;
}

// Found by fair roll of the dice (in practice we'll never have more then 1 layer!).
using QSSGRenderLayerList = QVarLengthArray<QSSGRenderLayer *, 4>;

static inline void maybePushLayer(QSSGRenderLayer &inLayer, QSSGRenderLayerList &outLayerList)
{
    inLayer.calculateGlobalVariables();
    if (inLayer.flags.testFlag(QSSGRenderLayer::Flag::GloballyActive) && inLayer.flags.testFlag(QSSGRenderLayer::Flag::LayerRenderToTarget))
        outLayerList.push_back(&inLayer);
}

bool QSSGRenderer::prepareLayerForRender(QSSGRenderLayer &inLayer,
                                               const QSize &surfaceSize)
{

    QSSGRenderLayerList renderableLayers;
    maybePushLayer(inLayer, renderableLayers);

    bool retval = false;

    auto iter = renderableLayers.crbegin();
    const auto end = renderableLayers.crend();
    for (; iter != end; ++iter) {
        // Store the previous state of if we were rendering a layer.
        QSSGRenderLayer *theLayer = *iter;
        QSSGRef<QSSGLayerRenderData> theRenderData = getOrCreateLayerRenderDataForNode(*theLayer);

        if (Q_LIKELY(theRenderData)) {
            theRenderData->prepareForRender(surfaceSize);
            retval = retval || theRenderData->layerPrepResult->flags.wasDirty();
        } else {
            Q_ASSERT(false);
        }
    }

    return retval;
}

void QSSGRenderer::rhiPrepare(QSSGRenderLayer &inLayer)
{
    QSSGRenderLayerList renderableLayers;
    maybePushLayer(inLayer, renderableLayers);

    auto iter = renderableLayers.crbegin();
    const auto end = renderableLayers.crend();

    for (iter = renderableLayers.crbegin(); iter != end; ++iter) {
        QSSGRenderLayer *theLayer = *iter;
        const QSSGRef<QSSGLayerRenderData> &theRenderData = getOrCreateLayerRenderDataForNode(*theLayer);

        if (Q_LIKELY(theRenderData)) {
            if (theRenderData->layerPrepResult->isLayerVisible())
                theRenderData->rhiPrepare();
        } else {
            Q_ASSERT(false);
        }
    }
}

void QSSGRenderer::rhiRender(QSSGRenderLayer &inLayer)
{
    QSSGRenderLayerList renderableLayers;
    maybePushLayer(inLayer, renderableLayers);

    auto iter = renderableLayers.crbegin();
    const auto end = renderableLayers.crend();

    for (iter = renderableLayers.crbegin(); iter != end; ++iter) {
        QSSGRenderLayer *theLayer = *iter;
        const QSSGRef<QSSGLayerRenderData> &theRenderData = getOrCreateLayerRenderDataForNode(*theLayer);

        if (Q_LIKELY(theRenderData)) {
            if (theRenderData->layerPrepResult->isLayerVisible())
                theRenderData->rhiRender();
        } else {
            Q_ASSERT(false);
        }
    }
}

QSSGRenderLayer *QSSGRenderer::layerForNode(const QSSGRenderNode &inNode) const
{
    if (inNode.type == QSSGRenderGraphObject::Type::Layer)
        return &const_cast<QSSGRenderLayer &>(static_cast<const QSSGRenderLayer &>(inNode));

    if (inNode.parent)
        return layerForNode(*inNode.parent);

    return nullptr;
}

QSSGRef<QSSGLayerRenderData> QSSGRenderer::getOrCreateLayerRenderDataForNode(const QSSGRenderNode &inNode)
{
    const QSSGRenderLayer *theLayer = layerForNode(inNode);
    if (theLayer) {
        auto it = m_instanceRenderMap.constFind(theLayer);
        if (it != m_instanceRenderMap.cend())
            return it.value();

        it = m_instanceRenderMap.insert(theLayer, new QSSGLayerRenderData(const_cast<QSSGRenderLayer &>(*theLayer), this));

        return *it;
    }
    return nullptr;
}

void QSSGRenderer::addMaterialDirtyClear(QSSGRenderGraphObject *material)
{
    m_materialClearDirty.insert(material);
}

void QSSGRenderer::beginFrame()
{
    for (int idx = 0, end = m_lastFrameLayers.size(); idx < end; ++idx)
        m_lastFrameLayers[idx]->resetForFrame();
    m_lastFrameLayers.clear();
    for (auto *matObj : qAsConst(m_materialClearDirty)) {
        if (matObj->type == QSSGRenderGraphObject::Type::CustomMaterial)
            static_cast<QSSGRenderCustomMaterial *>(matObj)->updateDirtyForFrame();
        else if (matObj->type == QSSGRenderGraphObject::Type::DefaultMaterial)
            static_cast<QSSGRenderDefaultMaterial *>(matObj)->dirty.updateDirtyForFrame();
    }
    m_materialClearDirty.clear();
}

void QSSGRenderer::endFrame()
{
}

inline bool pickResultLessThan(const QSSGRenderPickResult &lhs, const QSSGRenderPickResult &rhs)
{
    return lhs.m_cameraDistanceSq < rhs.m_cameraDistanceSq;
}

QSSGPickResultProcessResult QSSGRenderer::processPickResultList(bool inPickEverything)
{
    Q_UNUSED(inPickEverything)
    if (m_lastPickResults.empty())
        return QSSGPickResultProcessResult();
    // Things are rendered in a particular order and we need to respect that ordering.
    std::stable_sort(m_lastPickResults.begin(), m_lastPickResults.end(), pickResultLessThan);

    // We need to pick against sub objects basically somewhat recursively
    // but if we don't hit any sub objects and the parent isn't pickable then
    // we need to move onto the next item in the list.
    // We need to keep in mind that theQuery->Pick will enter this method in a later
    // stack frame so *if* we get to sub objects we need to pick against them but if the pick
    // completely misses *and* the parent object locally pickable is false then we need to move
    // onto the next object.

    const int numToCopy = m_lastPickResults.size();
    Q_ASSERT(numToCopy >= 0);
    size_t numCopyBytes = size_t(numToCopy) * sizeof(QSSGRenderPickResult);
    QSSGRenderPickResult *thePickResults = reinterpret_cast<QSSGRenderPickResult *>(
            m_contextInterface->perFrameAllocator().allocate(numCopyBytes));
    ::memcpy(thePickResults, m_lastPickResults.data(), numCopyBytes);
    m_lastPickResults.clear();
    QSSGPickResultProcessResult thePickResult(thePickResults[0]);
    return thePickResult;
}

QSSGRenderPickResult QSSGRenderer::pick(QSSGRenderLayer &inLayer,
                                                const QVector2D &inViewportDimensions,
                                                const QVector2D &inMouseCoords,
                                                bool inPickSiblings,
                                                bool inPickEverything)
{
    m_lastPickResults.clear();

    QSSGRenderLayer *theLayer = &inLayer;
    // Stepping through how the original runtime did picking it picked layers in order
    // stopping at the first hit.  So objects on the top layer had first crack at the pick
    // vector itself.
    do {
        if (theLayer->flags.testFlag(QSSGRenderLayer::Flag::Active)) {
            const auto theIter = m_instanceRenderMap.constFind(theLayer);
            if (theIter != m_instanceRenderMap.cend()) {
                m_lastPickResults.clear();
                getLayerHitObjectList(*theIter.value(), inViewportDimensions, inMouseCoords, inPickEverything, m_lastPickResults);
                QSSGPickResultProcessResult retval(processPickResultList(inPickEverything));
                if (retval.m_wasPickConsumed)
                    return retval;
            } else {
                // Q_ASSERT( false );
            }
        }

        if (inPickSiblings)
            theLayer = getNextLayer(*theLayer);
        else
            theLayer = nullptr;
    } while (theLayer != nullptr);

    return QSSGRenderPickResult();
}

QSSGRenderPickResult QSSGRenderer::syncPick(const QSSGRenderLayer &layer,
                                                const QSSGRef<QSSGBufferManager> &bufferManager,
                                                const QVector2D &inViewportDimensions,
                                                const QVector2D &inMouseCoords)
{
    using PickResultList = QVarLengthArray<QSSGRenderPickResult, 20>; // Lets assume most items are filtered out already
    static const auto processResults = [](PickResultList &pickResults) {
        if (pickResults.empty())
            return QSSGPickResultProcessResult();
        // Things are rendered in a particular order and we need to respect that ordering.
        std::stable_sort(pickResults.begin(), pickResults.end(), [](const QSSGRenderPickResult &lhs, const QSSGRenderPickResult &rhs) {
            return lhs.m_cameraDistanceSq < rhs.m_cameraDistanceSq;
        });
        return QSSGPickResultProcessResult{ pickResults.at(0), true };
    };

    PickResultList pickResults;
    if (layer.flags.testFlag(QSSGRenderLayer::Flag::Active)) {
        getLayerHitObjectList(layer, bufferManager, inViewportDimensions, inMouseCoords, false, pickResults);
        QSSGPickResultProcessResult retval = processResults(pickResults);
        if (retval.m_wasPickConsumed)
            return retval;
    }

    return QSSGPickResultProcessResult();
}

QSSGOption<QVector2D> QSSGRenderer::facePosition(QSSGRenderNode &inNode,
                                                         QSSGBounds3 inBounds,
                                                         const QMatrix4x4 &inGlobalTransform,
                                                         const QVector2D &inViewportDimensions,
                                                         const QVector2D &inMouseCoords,
                                                         QSSGDataView<QSSGRenderGraphObject *> inMapperObjects,
                                                         QSSGRenderBasisPlanes inPlane)
{
    Q_UNUSED(inMapperObjects)
    const QSSGRef<QSSGLayerRenderData> &theLayerData = getOrCreateLayerRenderDataForNode(inNode);
    if (theLayerData == nullptr)
        return QSSGEmpty();
    // This function assumes the layer was rendered to the scene itself.  There is another
    // function
    // for completely offscreen layers that don't get rendered to the scene.
    bool wasRenderToTarget(theLayerData->layer.flags.testFlag(QSSGRenderLayer::Flag::LayerRenderToTarget));
    if (!wasRenderToTarget || theLayerData->camera == nullptr || !theLayerData->layerPrepResult.hasValue())
        return QSSGEmpty();

    QVector2D theMouseCoords(inMouseCoords);
    QVector2D theViewportDimensions(inViewportDimensions);

    const auto camera = theLayerData->layerPrepResult->camera();
    const auto viewport = theLayerData->layerPrepResult->viewport();
    QSSGOption<QSSGRenderRay> theHitRay = QSSGLayerRenderHelper::pickRay(*camera, viewport, theMouseCoords, theViewportDimensions, false);
    if (!theHitRay.hasValue())
        return QSSGEmpty();

    // Scale the mouse coords to change them into the camera's numerical space.
    QSSGRenderRay thePickRay = *theHitRay;
    QSSGOption<QVector2D> newValue = thePickRay.relative(inGlobalTransform, inBounds, inPlane);
    return newValue;
}

QVector3D QSSGRenderer::unprojectToPosition(QSSGRenderNode &inNode, QVector3D &inPosition, const QVector2D &inMouseVec) const
{
    // Translate mouse into layer's coordinates
    const QSSGRef<QSSGLayerRenderData> &theData = const_cast<QSSGRenderer &>(*this).getOrCreateLayerRenderDataForNode(inNode);
    if (theData == nullptr || theData->camera == nullptr) {
        return QVector3D(0, 0, 0);
    } // Q_ASSERT( false ); return QVector3D(0,0,0); }

    QSize theWindow = m_contextInterface->windowDimensions();
    QVector2D theDims(float(theWindow.width()), float(theWindow.height()));

    QSSGLayerRenderPreparationResult &thePrepResult(*theData->layerPrepResult);
    const auto camera = thePrepResult.camera();
    const auto viewport = thePrepResult.viewport();
    QSSGRenderRay theRay = QSSGLayerRenderHelper::pickRay(*camera, viewport, inMouseVec, theDims, true);

    return theData->camera->unprojectToPosition(inPosition, theRay);
}

QVector3D QSSGRenderer::unprojectWithDepth(QSSGRenderNode &inNode, QVector3D &, const QVector3D &inMouseVec) const
{
    // Translate mouse into layer's coordinates
    const QSSGRef<QSSGLayerRenderData> &theData = const_cast<QSSGRenderer &>(*this).getOrCreateLayerRenderDataForNode(inNode);
    if (theData == nullptr || theData->camera == nullptr) {
        return QVector3D(0, 0, 0);
    } // Q_ASSERT( false ); return QVector3D(0,0,0); }

    // Flip the y into gl coordinates from window coordinates.
    QVector2D theMouse(inMouseVec.x(), inMouseVec.y());
    float theDepth = inMouseVec.z();

    QSSGLayerRenderPreparationResult &thePrepResult(*theData->layerPrepResult);
    QSize theWindow = m_contextInterface->windowDimensions();
    const auto camera = thePrepResult.camera();
    const auto viewport = thePrepResult.viewport();
    QSSGRenderRay theRay = QSSGLayerRenderHelper::pickRay(*camera, viewport, theMouse, QVector2D(float(theWindow.width()), float(theWindow.height())), true);
    QVector3D theTargetPosition = theRay.origin + theRay.direction * theDepth;
    if (inNode.parent != nullptr && inNode.parent->type != QSSGRenderGraphObject::Type::Layer)
        theTargetPosition = mat44::transform(inNode.parent->globalTransform.inverted(), theTargetPosition);
    return theTargetPosition;
}

QVector3D QSSGRenderer::projectPosition(QSSGRenderNode &inNode, const QVector3D &inPosition) const
{
    // Translate mouse into layer's coordinates
    const QSSGRef<QSSGLayerRenderData> &theData = const_cast<QSSGRenderer &>(*this).getOrCreateLayerRenderDataForNode(inNode);
    if (theData == nullptr || theData->camera == nullptr) {
        return QVector3D(0, 0, 0);
    }

    QMatrix4x4 viewProj;
    theData->camera->calculateViewProjectionMatrix(viewProj);
    QVector4D projPos = mat44::transform(viewProj, QVector4D(inPosition, 1.0f));
    projPos.setX(projPos.x() / projPos.w());
    projPos.setY(projPos.y() / projPos.w());

    QRectF theViewport = theData->layerPrepResult->viewport();
    QVector2D theDims(float(theViewport.width()), float(theViewport.height()));
    projPos.setX(projPos.x() + 1.0f);
    projPos.setY(projPos.y() + 1.0f);
    projPos.setX(projPos.x() * 0.5f);
    projPos.setY(projPos.y() * 0.5f);
    QVector3D cameraToObject = theData->camera->getGlobalPos() - inPosition;
    projPos.setZ(sqrtf(QVector3D::dotProduct(cameraToObject, cameraToObject)));
    QVector3D mouseVec = QVector3D(projPos.x(), projPos.y(), projPos.z());
    mouseVec.setX(mouseVec.x() * theDims.x());
    mouseVec.setY(mouseVec.y() * theDims.y());

    mouseVec.setX(mouseVec.x() + float(theViewport.x()));
    mouseVec.setY(mouseVec.y() + float(theViewport.y()));

    // Flip the y into window coordinates so it matches the mouse.
    QSize theWindow = m_contextInterface->windowDimensions();
    mouseVec.setY(theWindow.height() - mouseVec.y());

    return mouseVec;
}

QSSGRhiQuadRenderer *QSSGRenderer::rhiQuadRenderer()
{
    if (!m_contextInterface->rhiContext()->isValid())
        return nullptr;

    if (!m_rhiQuadRenderer)
        m_rhiQuadRenderer = new QSSGRhiQuadRenderer;

    return m_rhiQuadRenderer;
}

void QSSGRenderer::layerNeedsFrameClear(QSSGLayerRenderData &inLayer)
{
    m_lastFrameLayers.push_back(&inLayer);
}

void QSSGRenderer::beginLayerDepthPassRender(QSSGLayerRenderData &inLayer)
{
    m_currentLayer = &inLayer;
}

void QSSGRenderer::endLayerDepthPassRender()
{
    m_currentLayer = nullptr;
}

void QSSGRenderer::beginLayerRender(QSSGLayerRenderData &inLayer)
{
    m_currentLayer = &inLayer;
}
void QSSGRenderer::endLayerRender()
{
    m_currentLayer = nullptr;
}

//void QSSGRenderer::prepareImageForIbl(QSSGRenderImage &inImage)
//{
//    if (inImage.m_textureData.m_texture && inImage.m_textureData.m_texture->numMipmaps() < 1)
//        inImage.m_textureData.m_texture->generateMipmaps();
//}

bool nodeContainsBoneRoot(QSSGRenderNode &childNode, qint32 rootID)
{
    for (QSSGRenderNode *childChild = childNode.firstChild; childChild != nullptr; childChild = childChild->nextSibling) {
        if (childChild->skeletonId == rootID)
            return true;
    }

    return false;
}

void fillBoneIdNodeMap(QSSGRenderNode &childNode, QHash<long, QSSGRenderNode *> &ioMap)
{
    if (childNode.skeletonId >= 0)
        ioMap[childNode.skeletonId] = &childNode;
    for (QSSGRenderNode *childChild = childNode.firstChild; childChild != nullptr; childChild = childChild->nextSibling)
        fillBoneIdNodeMap(*childChild, ioMap);
}

QSSGOption<QVector2D> QSSGRenderer::getLayerMouseCoords(QSSGLayerRenderData &inLayerRenderData,
                                                                const QVector2D &inMouseCoords,
                                                                const QVector2D &inViewportDimensions,
                                                                bool forceImageIntersect) const
{
    if (inLayerRenderData.layerPrepResult.hasValue()) {
        const auto viewport = inLayerRenderData.layerPrepResult->viewport();
        return QSSGLayerRenderHelper::layerMouseCoords(viewport, inMouseCoords, inViewportDimensions, forceImageIntersect);
    }
    return QSSGEmpty();
}

bool QSSGRenderer::rendererRequestsFrames() const
{
    return m_progressiveAARenderRequest;
}

void QSSGRenderer::getLayerHitObjectList(QSSGLayerRenderData &inLayerRenderData,
                                               const QVector2D &inViewportDimensions,
                                               const QVector2D &inPresCoords,
                                               bool inPickEverything,
                                               TPickResultArray &outIntersectionResult)
{
    // This function assumes the layer was rendered to the scene itself. There is another
    // function for completely offscreen layers that don't get rendered to the scene.
    bool wasRenderToTarget(inLayerRenderData.layer.flags.testFlag(QSSGRenderLayer::Flag::LayerRenderToTarget));
    if (wasRenderToTarget && inLayerRenderData.camera != nullptr) {
        QSSGOption<QSSGRenderRay> theHitRay;
        if (inLayerRenderData.layerPrepResult.hasValue()) {
            const auto camera = inLayerRenderData.layerPrepResult->camera();
            const auto viewport = inLayerRenderData.layerPrepResult->viewport();
            theHitRay = QSSGLayerRenderHelper::pickRay(*camera, viewport, inPresCoords, inViewportDimensions, false);
        }

        if (theHitRay.hasValue()) {
            // Scale the mouse coords to change them into the camera's numerical space.
            QSSGRenderRay thePickRay = *theHitRay;
            for (int idx = inLayerRenderData.opaqueObjects.size(), end = 0; idx > end; --idx) {
                QSSGRenderableObject *theRenderableObject = inLayerRenderData.opaqueObjects.at(idx - 1).obj;
                if (inPickEverything || theRenderableObject->renderableFlags.isPickable())
                    intersectRayWithSubsetRenderable(thePickRay, *theRenderableObject, outIntersectionResult);
            }
            for (int idx = inLayerRenderData.transparentObjects.size(), end = 0; idx > end; --idx) {
                QSSGRenderableObject *theRenderableObject = inLayerRenderData.transparentObjects.at(idx - 1).obj;
                if (inPickEverything || theRenderableObject->renderableFlags.isPickable())
                    intersectRayWithSubsetRenderable(thePickRay, *theRenderableObject, outIntersectionResult);
            }
        }
    }
}

using RenderableList = QVarLengthArray<const QSSGRenderNode *>;
static void dfs(const QSSGRenderNode &node, RenderableList &renderables)
{
    if (node.isRenderableType())
        renderables.push_back(&node);

    for (QSSGRenderNode *child = node.firstChild; child != nullptr; child = child->nextSibling)
        dfs(*child, renderables);
}

void QSSGRenderer::getLayerHitObjectList(const QSSGRenderLayer &layer,
                                             const QSSGRef<QSSGBufferManager> &bufferManager,
                                             const QVector2D &inViewportDimensions,
                                             const QVector2D &inPresCoords,
                                             bool inPickEverything,
                                             PickResultList &outIntersectionResult)
{

    // This function assumes the layer was rendered to the scene itself. There is another
    // function for completely offscreen layers that don't get rendered to the scene.
    const bool wasRenderToTarget(layer.flags.testFlag(QSSGRenderLayer::Flag::LayerRenderToTarget));
    if (wasRenderToTarget && layer.renderedCamera != nullptr) {
        const auto camera = layer.renderedCamera;
        // TODO: Need to make sure we get the right Viewport rect here.
        const auto viewport = QRectF(QPointF(), QSizeF(qreal(inViewportDimensions.x()), qreal(inViewportDimensions.y())));
        const QSSGOption<QSSGRenderRay> hitRay = QSSGLayerRenderHelper::pickRay(*camera, viewport, inPresCoords, inViewportDimensions, false);
        if (hitRay.hasValue()) {
            // Scale the mouse coords to change them into the camera's numerical space.
            RenderableList renderables;
            for (QSSGRenderNode *childNode = layer.firstChild; childNode; childNode = childNode->nextSibling)
                dfs(*childNode, renderables);

            for (int idx = renderables.size(), end = 0; idx > end; --idx) {
                const auto &pickableObject = renderables.at(idx - 1);
                if (inPickEverything || pickableObject->flags.testFlag(QSSGRenderNode::Flag::LocallyPickable))
                    intersectRayWithSubsetRenderable(bufferManager, *hitRay, *pickableObject, outIntersectionResult);
            }
        }
    }
}

void QSSGRenderer::intersectRayWithSubsetRenderable(const QSSGRef<QSSGBufferManager> &bufferManager,
                                                        const QSSGRenderRay &inRay,
                                                        const QSSGRenderNode &node,
                                                        QSSGRenderer::PickResultList &outIntersectionResultList)
{
    if (node.type != QSSGRenderGraphObject::Type::Model)
        return;

    const QSSGRenderModel &model = static_cast<const QSSGRenderModel &>(node);

    // TODO: Technically we should have some guard here, as the meshes are usually loaded on a different thread,
    // so this isn't really nice (assumes all meshes are loaded before picking and none are removed, which currently should be the case).
    auto mesh = bufferManager->getMesh(model.meshPath);
    if (!mesh)
        return;

    const auto &globalTransform = model.globalTransform;
    auto rayData = QSSGRenderRay::createRayData(globalTransform, inRay);
    const auto &subMeshes = mesh->subsets;
    QSSGBounds3 modelBounds = QSSGBounds3::empty();
    for (const auto &subMesh : subMeshes)
        modelBounds.include(subMesh.bounds);

    if (modelBounds.isEmpty())
        return;

    auto hit = QSSGRenderRay::intersectWithAABBv2(rayData, modelBounds);

    // If we don't intersect with the model at all, then there's no need to go furher down!
    if (!hit.intersects())
        return;

    // Check each submesh to find the closest intersection point
    float minRayLength = std::numeric_limits<float>::max();
    QSSGRenderRay::IntersectionResult intersectionResult;
    QVector<QSSGRenderRay::IntersectionResult> results;

    for (const auto &subMesh : subMeshes) {
        QSSGRenderRay::IntersectionResult result;
        if (subMesh.bvhRoot) {
            hit = QSSGRenderRay::intersectWithAABBv2(rayData, subMesh.bvhRoot->boundingData);
            if (hit.intersects()) {
                results.clear();
                inRay.intersectWithBVH(rayData, subMesh.bvhRoot, mesh, results);
                float subMeshMinRayLength = std::numeric_limits<float>::max();
                for (const auto &subMeshResult : qAsConst(results)) {
                    if (subMeshResult.rayLengthSquared < subMeshMinRayLength) {
                        result = subMeshResult;
                        subMeshMinRayLength = result.rayLengthSquared;
                    }
                }
            }
        } else {
            hit = QSSGRenderRay::intersectWithAABBv2(rayData, subMesh.bounds);
            if (hit.intersects())
                result = QSSGRenderRay::createIntersectionResult(rayData, hit);
        }
        if (result.intersects && result.rayLengthSquared < minRayLength) {
            intersectionResult = result;
            minRayLength = intersectionResult.rayLengthSquared;
        }
    }

    if (!intersectionResult.intersects)
        return;

    outIntersectionResultList.push_back(
                                QSSGRenderPickResult(model,
                                                     intersectionResult.rayLengthSquared,
                                                     intersectionResult.relXY,
                                                     intersectionResult.scenePosition));
}

void QSSGRenderer::intersectRayWithSubsetRenderable(const QSSGRenderRay &inRay,
                                                        QSSGRenderableObject &inRenderableObject,
                                                        TPickResultArray &outIntersectionResultList)
{
    QSSGRenderRay::IntersectionResult intersectionResult = QSSGRenderRay::intersectWithAABB(inRenderableObject.globalTransform, inRenderableObject.bounds, inRay);
    if (!intersectionResult.intersects)
        return;

    // Leave the coordinates relative for right now.
    const QSSGRenderGraphObject *thePickObject = nullptr;
    if (inRenderableObject.renderableFlags.isDefaultMaterialMeshSubset())
        thePickObject = &static_cast<QSSGSubsetRenderable *>(&inRenderableObject)->modelContext.model;
    else if (inRenderableObject.renderableFlags.isCustomMaterialMeshSubset())
        thePickObject = &static_cast<QSSGCustomMaterialRenderable *>(&inRenderableObject)->modelContext.model;

    if (thePickObject != nullptr) {
        outIntersectionResultList.push_back(
                                    QSSGRenderPickResult(*thePickObject,
                                                         intersectionResult.rayLengthSquared,
                                                         intersectionResult.relXY,
                                                         intersectionResult.scenePosition));
    }
}

QSSGRef<QSSGRhiShaderStagesWithResources> QSSGRenderer::getRhiShadersWithResources(QSSGSubsetRenderable &inRenderable,
                                                                                       const ShaderFeatureSetList &inFeatureSet)
{
    if (Q_UNLIKELY(m_currentLayer == nullptr)) {
        Q_ASSERT(false);
        return nullptr;
    }
    auto shaderIt = m_rhiShaders.constFind(inRenderable.shaderDescription);
    if (shaderIt == m_rhiShaders.cend()) {
        const QSSGRef<QSSGRhiShaderStages> &shaderStages(generateRhiShaderStages(inRenderable, inFeatureSet));
        if (shaderStages) {
            QSSGRef<QSSGRhiShaderStagesWithResources> generatedShaders = QSSGRhiShaderStagesWithResources::fromShaderStages(shaderStages);
            shaderIt = m_rhiShaders.insert(inRenderable.shaderDescription, generatedShaders);
        } else {
            // We still insert something because we don't to attempt to generate the same bad shader
            // twice.
            shaderIt = m_rhiShaders.insert(inRenderable.shaderDescription, nullptr);
        }
    }

    if (!shaderIt->isNull()) {
        if (m_currentLayer && m_currentLayer->camera) {
            QSSGRenderCamera &theCamera(*m_currentLayer->camera);
            if (!m_currentLayer->cameraDirection.hasValue())
                m_currentLayer->cameraDirection = theCamera.getScalingCorrectDirection();
        }
    }
    return *shaderIt;
}

QSSGLayerGlobalRenderProperties QSSGRenderer::getLayerGlobalRenderProperties()
{
    QSSGLayerRenderData &theData = *m_currentLayer;
    const QSSGRenderLayer &theLayer = theData.layer;
    if (!theData.cameraDirection.hasValue())
        theData.cameraDirection = theData.camera->getScalingCorrectDirection();

    const bool isYUpInFramebuffer = m_contextInterface->rhiContext()->isValid()
            ? m_contextInterface->rhiContext()->rhi()->isYUpInFramebuffer()
            : true;

    return QSSGLayerGlobalRenderProperties{ theLayer,
                                              *theData.camera,
                                              *theData.cameraDirection,
                                              theData.globalLights,
                                              theData.lightDirections,
                                              theData.shadowMapManager,
                                              theData.m_rhiDepthTexture.texture,
                                              theData.m_rhiAoTexture.texture,
                                              theLayer.lightProbe,
                                              theLayer.lightProbe2,
                                              theLayer.probeHorizon,
                                              theLayer.probeBright,
                                              theLayer.probe2Window,
                                              theLayer.probe2Pos,
                                              theLayer.probe2Fade,
                                              theLayer.probeFov,
                                              isYUpInFramebuffer };
}

const QSSGRef<QSSGProgramGenerator> &QSSGRenderer::getProgramGenerator()
{
    return m_contextInterface->shaderProgramGenerator();
}

QSSGOption<QVector2D> QSSGRenderer::getLayerMouseCoords(QSSGRenderLayer &inLayer,
                                                                const QVector2D &inMouseCoords,
                                                                const QVector2D &inViewportDimensions,
                                                                bool forceImageIntersect) const
{
    QSSGRef<QSSGLayerRenderData> theData = const_cast<QSSGRenderer &>(*this).getOrCreateLayerRenderDataForNode(inLayer);
    return getLayerMouseCoords(*theData, inMouseCoords, inViewportDimensions, forceImageIntersect);
}

QSSGRenderPickResult::QSSGRenderPickResult(const QSSGRenderGraphObject &inHitObject,
                                           float inCameraDistance,
                                           const QVector2D &inLocalUVCoords,
                                           const QVector3D &scenePosition)
    : m_hitObject(&inHitObject)
    , m_cameraDistanceSq(inCameraDistance)
    , m_localUVCoords(inLocalUVCoords)
    , m_scenePosition(scenePosition)
{
}

QT_END_NAMESPACE
