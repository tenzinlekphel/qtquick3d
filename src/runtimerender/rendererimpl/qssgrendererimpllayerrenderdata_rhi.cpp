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

#include "qssgrendererimpllayerrenderdata_p.h"

#include <QtQuick3DRuntimeRender/private/qssgrenderer_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrenderlight_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrendercustommaterialrendercontext_p.h>
#include <QtQuick3DRuntimeRender/private/qssgrhiquadrenderer_p.h>
#include <QtQuick/private/qsgtexture_p.h>

QT_BEGIN_NAMESPACE

static constexpr float QSSG_PI = float(M_PI);
static constexpr float QSSG_HALFPI = float(M_PI_2);

static const QRhiShaderResourceBinding::StageFlags VISIBILITY_ALL =
        QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;

QSSGLayerRenderData::QSSGLayerRenderData(QSSGRenderLayer &inLayer, const QSSGRef<QSSGRenderer> &inRenderer)
    : QSSGLayerRenderPreparationData(inLayer, inRenderer)
    , m_depthBufferFormat(QSSGRenderTextureFormat::Unknown)
    , m_progressiveAAPassIndex(0)
    , m_temporalAAPassIndex(0)
    , m_textScale(1.0f)
    , m_zPrePassPossible(true)
{
}

QSSGLayerRenderData::~QSSGLayerRenderData()
{
    m_rhiDepthTexture.reset();
    m_rhiAoTexture.reset();
}

void QSSGLayerRenderData::prepareForRender(const QSize &inViewportDimensions)
{
    QSSGLayerRenderPreparationData::prepareForRender(inViewportDimensions);
    QSSGLayerRenderPreparationResult &thePrepResult(*layerPrepResult);
    const QSSGRef<QSSGResourceManager> &theResourceManager(renderer->contextInterface()->resourceManager());

    // Generate all necessary lighting keys

    if (thePrepResult.flags.wasLayerDataDirty()) {
        m_progressiveAAPassIndex = 0;
    }

    renderer->layerNeedsFrameClear(*this);

    // Clean up the texture cache if layer dimensions changed
    if (inViewportDimensions.width() != m_previousDimensions.width()
            || inViewportDimensions.height() != m_previousDimensions.height()) {

        m_previousDimensions.setWidth(inViewportDimensions.width());
        m_previousDimensions.setHeight(inViewportDimensions.height());

        theResourceManager->destroyFreeSizedResources();

    }
}

static void fillTargetBlend(QRhiGraphicsPipeline::TargetBlend *targetBlend, QSSGRenderDefaultMaterial::MaterialBlendMode materialBlend)
{
    // Assuming default values in the other TargetBlend fields
    switch (materialBlend) {
    case QSSGRenderDefaultMaterial::MaterialBlendMode::Screen:
        targetBlend->srcColor = QRhiGraphicsPipeline::SrcAlpha;
        targetBlend->dstColor = QRhiGraphicsPipeline::One;
        targetBlend->srcAlpha = QRhiGraphicsPipeline::One;
        targetBlend->dstAlpha = QRhiGraphicsPipeline::One;
        break;
    case QSSGRenderDefaultMaterial::MaterialBlendMode::Multiply:
        targetBlend->srcColor = QRhiGraphicsPipeline::DstColor;
        targetBlend->dstColor = QRhiGraphicsPipeline::Zero;
        targetBlend->srcAlpha = QRhiGraphicsPipeline::One;
        targetBlend->dstAlpha = QRhiGraphicsPipeline::One;
        break;
    default:
        // Use SourceOver for everything else
        targetBlend->srcColor = QRhiGraphicsPipeline::SrcAlpha;
        targetBlend->dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        targetBlend->srcAlpha = QRhiGraphicsPipeline::One;
        targetBlend->dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        break;
    }
}

static void rhiPrepareRenderable(QSSGRhiContext *rhiCtx,
                                 QSSGLayerRenderData &inData,
                                 QSSGRenderableObject &inObject,
                                 const QVector2D &inCameraProps,
                                 quint32 indexLight,
                                 const QSSGRenderCamera &inCamera)
{
    Q_UNUSED(indexLight);

    QSSGRhiGraphicsPipelineState *ps = rhiCtx->graphicsPipelineState(&inData);
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();

    if (inObject.renderableFlags.isDefaultMaterialMeshSubset()) {
        QSSGSubsetRenderable &subsetRenderable(static_cast<QSSGSubsetRenderable &>(inObject));
        const QSSGRef<QSSGRenderer> &generator(subsetRenderable.generator);
        const ShaderFeatureSetList &featureSet(inData.getShaderFeatureSet());

        QSSGRef<QSSGRhiShaderStagesWithResources> shaderPipeline = generator->getRhiShadersWithResources(subsetRenderable, featureSet);
        if (shaderPipeline) {
            ps->shaderStages = shaderPipeline->stages();
            const auto &defMatGen
                    = generator->contextInterface()->defaultMaterialShaderGenerator();
            defMatGen->setRhiMaterialProperties(shaderPipeline,
                                                ps,
                                                subsetRenderable.material,
                                                inCameraProps,
                                                subsetRenderable.modelContext.modelViewProjection,
                                                subsetRenderable.modelContext.normalMatrix,
                                                subsetRenderable.modelContext.model.globalTransform,
                                                subsetRenderable.firstImage,
                                                subsetRenderable.opacity,
                                                generator->getLayerGlobalRenderProperties(),
                                                subsetRenderable.renderableFlags.receivesShadows());

            // shaderPipeline->dumpUniforms();

            ps->samples = rhiCtx->mainPassSampleCount();

            ps->cullMode = QSSGRhiGraphicsPipelineState::toCullMode(subsetRenderable.material.cullMode);
            fillTargetBlend(&ps->targetBlend, subsetRenderable.material.blendMode);

            ps->ia = subsetRenderable.subset.rhi.ia;
            ps->ia.bakeVertexInputLocations(*shaderPipeline);

            QRhiResourceUpdateBatch *resourceUpdates = rhiCtx->rhi()->nextResourceUpdateBatch();

            // Unlike the subsetRenderable (which is allocated per frame so is
            // not persistent in any way), the model reference is persistent in
            // the sense that it references the model node in the scene graph.
            // Combined with the layer node (multiple View3Ds may share the
            // same scene!), this is suitable as a key to get the uniform
            // buffers that were used with the rendering of the same model in
            // the previous frame.
            const void *layerNode = &inData.layer;
            const void *modelNode = &subsetRenderable.modelContext.model;

            QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet({ layerNode, modelNode,
                                                                               &subsetRenderable.material, QSSGRhiUniformBufferSetKey::Main }));
            shaderPipeline->bakeMainUniformBuffer(&uniformBuffers.ubuf, resourceUpdates);
            QRhiBuffer *ubuf = uniformBuffers.ubuf;

            QRhiBuffer *lightsUbuf = nullptr;
            if (shaderPipeline->isLightingEnabled(QSSGRhiShaderStagesWithResources::LightBuffer0)) {
                shaderPipeline->bakeLightsUniformBuffer(QSSGRhiShaderStagesWithResources::LightBuffer0, &uniformBuffers.lightsUbuf0, resourceUpdates);
                lightsUbuf = uniformBuffers.lightsUbuf0;
            }

            cb->resourceUpdate(resourceUpdates);

            QSSGRhiContext::ShaderResourceBindingList bindings;
            bindings.append(QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf));

            if (lightsUbuf)
                bindings.append(QRhiShaderResourceBinding::uniformBuffer(1, VISIBILITY_ALL, lightsUbuf));

            // Texture maps
            auto *renderableImage = subsetRenderable.firstImage;
            if (renderableImage) {
                int imageNumber = 0;
                while (renderableImage) {
                    // TODO: optimize this! We're looking for the sampler corresponding to imageNumber, and currently the
                    // only information we have is the name of the form "image0_sampler"
                    QByteArray samplerName = QByteArrayLiteral("image%_sampler").replace('%', QByteArray::number(imageNumber));
                    int samplerBinding = shaderPipeline->bindingForTexture(samplerName);
                    if (samplerBinding >= 0) {
                        QRhiTexture *texture = renderableImage->m_image.m_textureData.m_rhiTexture;
                        if (samplerBinding >= 0 && texture) {
                            const bool mipmapped = texture->flags().testFlag(QRhiTexture::MipMapped);
                            QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear,
                                                                     mipmapped ? QRhiSampler::Linear : QRhiSampler::None,
                                                                     toRhi(renderableImage->m_image.m_horizontalTilingMode),
                                                                     toRhi(renderableImage->m_image.m_verticalTilingMode) });
                            bindings.append(QRhiShaderResourceBinding::sampledTexture(samplerBinding,
                                                                                      QRhiShaderResourceBinding::FragmentStage,
                                                                                      texture, sampler));
                        }
                    } else {
                        qWarning("Could not find sampler for image");
                    }
                    renderableImage = renderableImage->m_nextImage;
                    imageNumber++;
                }
            }

            // Shadow map textures
            if (shaderPipeline->isLightingEnabled(QSSGRhiShaderStagesWithResources::LightBuffer0)) {
                const int shadowMapCount = shaderPipeline->shadowMapCount();
                for (int i = 0; i < shadowMapCount; ++i) {
                    QSSGRhiShadowMapProperties &shadowMapProperties(shaderPipeline->shadowMapAt(i));
                    QRhiTexture *texture = shadowMapProperties.shadowMapTexture;
                    QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
                    Q_ASSERT(texture && sampler);
                    const QLatin1String name(shadowMapProperties.shadowMapTextureUniformName);
                    if (shadowMapProperties.cachedBinding < 0)
                        shadowMapProperties.cachedBinding = shaderPipeline->bindingForTexture(name);
                    if (shadowMapProperties.cachedBinding < 0) {
                        qWarning("No combined image sampler for shadow map texture '%s'", name.data());
                        continue;
                    }
                    bindings.append(QRhiShaderResourceBinding::sampledTexture(shadowMapProperties.cachedBinding,
                                                                              QRhiShaderResourceBinding::FragmentStage,
                                                                              texture,
                                                                              sampler));
                }
            }

            // Light probe texture
            if (shaderPipeline->lightProbeTexture()) {
                int binding = shaderPipeline->bindingForTexture(QLatin1String("lightProbe"));
                if (binding >= 0) {
                    auto tiling = shaderPipeline->lightProbeTiling();
                    QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear, // enables mipmapping
                                                             toRhi(tiling.first), toRhi(tiling.second) });
                    bindings.append(QRhiShaderResourceBinding::sampledTexture(binding,
                                                                              QRhiShaderResourceBinding::FragmentStage,
                                                                              shaderPipeline->lightProbeTexture(), sampler));
                } else {
                    qWarning("Could not find sampler for lightprobe");
                }
            }

            // Depth texture
            if (shaderPipeline->depthTexture()) {
                int binding = shaderPipeline->bindingForTexture(QLatin1String("depthTexture"));
                if (binding >= 0) {
                     // nearest min/mag, no mipmap
                     QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
                     bindings.append(QRhiShaderResourceBinding::sampledTexture(binding,
                                                                               QRhiShaderResourceBinding::FragmentStage,
                                                                               shaderPipeline->depthTexture(), sampler));
                 } // else ignore, not an error
            }

            // SSAO texture
            if (shaderPipeline->ssaoTexture()) {
                int binding = shaderPipeline->bindingForTexture(QLatin1String("aoTexture"));
                if (binding >= 0) {
                    // linear min/mag, no mipmap
                    QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
                    bindings.append(QRhiShaderResourceBinding::sampledTexture(binding,
                                                                              QRhiShaderResourceBinding::FragmentStage,
                                                                              shaderPipeline->ssaoTexture(), sampler));
                } // else ignore, not an error
            }

            QRhiShaderResourceBindings *srb = rhiCtx->srb(bindings);

            const QSSGGraphicsPipelineStateKey pipelineKey { *ps, rhiCtx->mainRenderPassDescriptor(), srb };
            subsetRenderable.rhiRenderData.mainPass.pipeline = rhiCtx->pipeline(pipelineKey);
            subsetRenderable.rhiRenderData.mainPass.srb = srb;

        }
    } else if (inObject.renderableFlags.isCustomMaterialMeshSubset()) {
        QSSGCustomMaterialRenderable &renderable(static_cast<QSSGCustomMaterialRenderable &>(inObject));
        const QSSGRenderCustomMaterial &material(renderable.material);
        QSSGMaterialSystem &customMaterialSystem(*renderable.generator->contextInterface()->customMaterialSystem().data());

        if (!inData.layer.lightProbe && renderable.material.m_iblProbe) {
            inData.setShaderFeature(QSSGShaderDefines::asString(QSSGShaderDefines::LightProbe),
                                    renderable.material.m_iblProbe->m_textureData.m_rhiTexture != nullptr);
        } else if (inData.layer.lightProbe) {
            inData.setShaderFeature(QSSGShaderDefines::asString(QSSGShaderDefines::LightProbe),
                                    inData.layer.lightProbe->m_textureData.m_rhiTexture != nullptr);
        }
        const ShaderFeatureSetList &featureSet(inData.getShaderFeatureSet());
        QSSGCustomMaterialRenderContext customMaterialContext(inData.layer,
                                                              inData,
                                                              inData.globalLights,
                                                              inCamera,
                                                              renderable.modelContext.model,
                                                              renderable.subset,
                                                              renderable.modelContext.modelViewProjection,
                                                              renderable.globalTransform,
                                                              renderable.modelContext.normalMatrix,
                                                              material,
                                                              inData.m_rhiDepthTexture.texture,
                                                              inData.m_rhiAoTexture.texture,
                                                              renderable.shaderDescription,
                                                              renderable.firstImage,
                                                              renderable.opacity);
        customMaterialSystem.prepareRhiSubset(customMaterialContext, featureSet, ps, renderable);
    } else {
        Q_ASSERT(false);
    }
}

static bool rhiPrepareDepthPassForObject(QSSGRhiContext *rhiCtx,
                                         QSSGLayerRenderData &inData,
                                         QSSGRenderableObject *obj,
                                         QRhiRenderPassDescriptor *rpDesc,
                                         QSSGRhiGraphicsPipelineState *ps,
                                         QRhiResourceUpdateBatch *rub,
                                         QSSGRhiUniformBufferSetKey::Selector ubufSel)
{
    QRhi *rhi = rhiCtx->rhi();
    const QMatrix4x4 correctionMatrix = rhi->clipSpaceCorrMatrix();

    // material specific have to be done differently for default and custom materials
    if (obj->renderableFlags.isDefaultMaterialMeshSubset()) {
        // We need to know the cull mode.
        QSSGSubsetRenderable &subsetRenderable(static_cast<QSSGSubsetRenderable &>(*obj));

        ps->cullMode = QSSGRhiGraphicsPipelineState::toCullMode(subsetRenderable.material.cullMode);
    } else if (obj->renderableFlags.isCustomMaterialMeshSubset()) {
        QSSGCustomMaterialRenderable &renderable(static_cast<QSSGCustomMaterialRenderable &>(*obj));
        ps->cullMode = QSSGRhiGraphicsPipelineState::toCullMode(renderable.material.cullMode);
    }

    // the rest is common
    if (obj->renderableFlags.isDefaultMaterialMeshSubset() || obj->renderableFlags.isCustomMaterialMeshSubset()) {
        QSSGSubsetRenderableBase *subsetRenderable(static_cast<QSSGSubsetRenderableBase *>(obj));
        QSSGRef<QSSGRhiShaderStagesWithResources> shaderStages = subsetRenderable->generator->getRhiDepthPrepassShader();
        if (!shaderStages)
            return false;

        ps->shaderStages = shaderStages->stages();
        ps->ia = subsetRenderable->subset.rhi.iaDepth;

        // the depth prepass shader takes the mvp matrix, nothing else
        const int UBUF_SIZE = 64;
        const QSSGRhiUniformBufferSetKey ubufKey = { &inData.layer, &subsetRenderable->modelContext.model, nullptr, ubufSel };
        QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet(ubufKey));
        QRhiBuffer *&ubuf = uniformBuffers.ubuf;
        if (!ubuf) {
            ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, UBUF_SIZE);
            ubuf->create();
        } else if (ubuf->size() < UBUF_SIZE) {
            ubuf->setSize(UBUF_SIZE);
            ubuf->create();
        }

        const QMatrix4x4 mvp = correctionMatrix * subsetRenderable->modelContext.modelViewProjection;
        rub->updateDynamicBuffer(ubuf, 0, 64, mvp.constData());

        QSSGRhiContext::ShaderResourceBindingList bindings;
        bindings.append(QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf));
        QRhiShaderResourceBindings *srb = rhiCtx->srb(bindings);
        const QSSGGraphicsPipelineStateKey pipelineKey { *ps, rpDesc, srb };
        QRhiGraphicsPipeline *pipeline = rhiCtx->pipeline(pipelineKey);

        subsetRenderable->rhiRenderData.depthPrePass.pipeline = pipeline;
        subsetRenderable->rhiRenderData.depthPrePass.srb = srb;
    }

    return true;
}

static bool rhiPrepareDepthPass(QSSGRhiContext *rhiCtx,
                                const QSSGRhiGraphicsPipelineState &basePipelineState,
                                QRhiRenderPassDescriptor *rpDesc,
                                QSSGLayerRenderData &inData,
                                const QVector<QSSGRenderableObjectHandle> &sortedOpaqueObjects,
                                const QVector<QSSGRenderableObjectHandle> &sortedTransparentObjects,
                                const QVector<QSSGRenderableNodeEntry> &item2Ds,
                                QSSGRhiUniformBufferSetKey::Selector ubufSel,
                                int samples)
{
    // Phase 1 (prepare) for the Z prepass or the depth texture generation.
    // These renders opaque (Z prepass), or opaque and transparent (depth
    // texture), objects with depth test/write enabled, and color write
    // disabled, using a very simple set of shaders.

    // ### depth prepass is not currently supported for 2D item quads - these
    // are both opaque and semi-transparent at the same time (?!) so are part
    // of a Z prepass but that's not implemented yet
    if (!item2Ds.isEmpty())
        return false;

    QRhi *rhi = rhiCtx->rhi();
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
    QSSGRhiGraphicsPipelineState ps = basePipelineState; // viewport and others are filled out already
    // We took a copy of the pipeline state since we do not want to conflict
    // with what rhiPrepare() collects for its own use. So here just change
    // whatever we need.

    ps.samples = samples;
    ps.depthTestEnable = true;
    ps.depthWriteEnable = true;
    ps.targetBlend.colorWrite = {};

    QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();

    for (const QSSGRenderableObjectHandle &handle : sortedOpaqueObjects) {
        if (!rhiPrepareDepthPassForObject(rhiCtx, inData, handle.obj, rpDesc, &ps, rub, ubufSel))
            return false;
    }

    for (const QSSGRenderableObjectHandle &handle : sortedTransparentObjects) {
        if (!rhiPrepareDepthPassForObject(rhiCtx, inData, handle.obj, rpDesc, &ps, rub, ubufSel))
            return false;
    }

    cb->resourceUpdate(rub);

    return true;
}

static bool rhiPrepareDepthTexture(QSSGRhiContext *rhiCtx, const QSize &size, QSSGRhiRenderableTexture *renderableTex)
{
    QRhi *rhi = rhiCtx->rhi();
    bool needsBuild = false;

    if (!renderableTex->texture) {
        QRhiTexture::Format format = QRhiTexture::D32F;
        if (!rhi->isTextureFormatSupported(format))
            format = QRhiTexture::D16;
        if (!rhi->isTextureFormatSupported(format))
            qWarning("Depth texture not supported");
        // the depth texture is always non-msaa, even if multisampling is used in the main pass
        renderableTex->texture = rhiCtx->rhi()->newTexture(format, size, 1, QRhiTexture::RenderTarget);
        needsBuild = true;
    } else if (renderableTex->texture->pixelSize() != size) {
        renderableTex->texture->setPixelSize(size);
        needsBuild = true;
    }

    if (needsBuild) {
        if (!renderableTex->texture->create()) {
            qWarning("Failed to build depth texture (size %dx%d, format %d)",
                     size.width(), size.height(), int(renderableTex->texture->format()));
            renderableTex->reset();
            return false;
        }

        delete renderableTex->rt;
        delete renderableTex->rpDesc;
        QRhiTextureRenderTargetDescription rtDesc;
        rtDesc.setDepthTexture(renderableTex->texture);
        renderableTex->rt = rhi->newTextureRenderTarget(rtDesc);
        renderableTex->rpDesc = renderableTex->rt->newCompatibleRenderPassDescriptor();
        renderableTex->rt->setRenderPassDescriptor(renderableTex->rpDesc);
        if (!renderableTex->rt->create()) {
            qWarning("Failed to build render target for depth texture");
            renderableTex->reset();
            return false;
        }
    }

    return true;
}

static void rhiRenderDepthPassForObject(QSSGRhiContext *rhiCtx,
                                        QSSGLayerRenderData &inData,
                                        QSSGRenderableObject *obj,
                                        bool *needsSetViewport)
{
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();

    // casts to SubsetRenderableBase so it works for both default and custom materials
    if (obj->renderableFlags.isDefaultMaterialMeshSubset() || obj->renderableFlags.isCustomMaterialMeshSubset()) {
        QSSGSubsetRenderableBase *subsetRenderable(static_cast<QSSGSubsetRenderableBase *>(obj));

        QRhiBuffer *vertexBuffer = subsetRenderable->subset.rhi.iaDepth.vertexBuffer->buffer();
        QRhiBuffer *indexBuffer = subsetRenderable->subset.rhi.iaDepth.indexBuffer
                ? subsetRenderable->subset.rhi.iaDepth.indexBuffer->buffer()
                : nullptr;

        QRhiGraphicsPipeline *ps = subsetRenderable->rhiRenderData.depthPrePass.pipeline;
        if (!ps)
            return;

        QRhiShaderResourceBindings *srb = subsetRenderable->rhiRenderData.depthPrePass.srb;
        if (!srb)
            return;

        cb->setGraphicsPipeline(ps);
        cb->setShaderResources(srb);

        if (*needsSetViewport) {
            cb->setViewport(rhiCtx->graphicsPipelineState(&inData)->viewport);
            *needsSetViewport = false;
        }

        QRhiCommandBuffer::VertexInput vb(vertexBuffer, 0);
        if (indexBuffer) {
            cb->setVertexInput(0, 1, &vb, indexBuffer, 0, subsetRenderable->subset.rhi.iaDepth.indexBuffer->indexFormat());
            cb->drawIndexed(subsetRenderable->subset.count, 1, subsetRenderable->subset.offset);
        } else {
            cb->setVertexInput(0, 1, &vb);
            cb->draw(subsetRenderable->subset.count, 1, subsetRenderable->subset.offset);
        }
    }
}

static void rhiRenderDepthPass(QSSGRhiContext *rhiCtx,
                               QSSGLayerRenderData &inData,
                               const QVector<QSSGRenderableObjectHandle> &sortedOpaqueObjects,
                               const QVector<QSSGRenderableObjectHandle> &sortedTransparentObjects,
                               const QVector<QSSGRenderableNodeEntry> &item2Ds,
                               bool *needsSetViewport)
{
    Q_UNUSED(item2Ds);

    for (const QSSGRenderableObjectHandle &handle : sortedOpaqueObjects)
        rhiRenderDepthPassForObject(rhiCtx, inData, handle.obj, needsSetViewport);

    for (const QSSGRenderableObjectHandle &handle : sortedTransparentObjects)
        rhiRenderDepthPassForObject(rhiCtx, inData, handle.obj, needsSetViewport);
}

static void computeFrustumBounds(const QSSGRenderCamera &inCamera,
                                 const QRectF &inViewPort,
                                 QVector3D &ctrBound,
                                 QVector3D camVerts[8])
{
    QVector3D camEdges[4];

    const float *dataPtr(inCamera.globalTransform.constData());
    QVector3D camX(dataPtr[0], dataPtr[1], dataPtr[2]);
    QVector3D camY(dataPtr[4], dataPtr[5], dataPtr[6]);
    QVector3D camZ(dataPtr[8], dataPtr[9], dataPtr[10]);

    float tanFOV = tanf(inCamera.verticalFov(inViewPort) * 0.5f);
    float asTanFOV = tanFOV * inViewPort.width() / inViewPort.height();
    camEdges[0] = -asTanFOV * camX + tanFOV * camY + camZ;
    camEdges[1] = asTanFOV * camX + tanFOV * camY + camZ;
    camEdges[2] = asTanFOV * camX - tanFOV * camY + camZ;
    camEdges[3] = -asTanFOV * camX - tanFOV * camY + camZ;

    for (int i = 0; i < 4; ++i) {
        camEdges[i].setX(-camEdges[i].x());
        camEdges[i].setY(-camEdges[i].y());
    }

    camVerts[0] = inCamera.position + camEdges[0] * inCamera.clipNear;
    camVerts[1] = inCamera.position + camEdges[0] * inCamera.clipFar;
    camVerts[2] = inCamera.position + camEdges[1] * inCamera.clipNear;
    camVerts[3] = inCamera.position + camEdges[1] * inCamera.clipFar;
    camVerts[4] = inCamera.position + camEdges[2] * inCamera.clipNear;
    camVerts[5] = inCamera.position + camEdges[2] * inCamera.clipFar;
    camVerts[6] = inCamera.position + camEdges[3] * inCamera.clipNear;
    camVerts[7] = inCamera.position + camEdges[3] * inCamera.clipFar;

    ctrBound = camVerts[0];
    for (int i = 1; i < 8; ++i) {
        ctrBound += camVerts[i];
    }
    ctrBound *= 0.125f;
}

static QSSGBounds3 calculateShadowCameraBoundingBox(const QVector3D *points,
                                                    const QVector3D &forward,
                                                    const QVector3D &up,
                                                    const QVector3D &right)
{
    float minDistanceZ = std::numeric_limits<float>::max();
    float maxDistanceZ = -std::numeric_limits<float>::max();
    float minDistanceY = std::numeric_limits<float>::max();
    float maxDistanceY = -std::numeric_limits<float>::max();
    float minDistanceX = std::numeric_limits<float>::max();
    float maxDistanceX = -std::numeric_limits<float>::max();
    for (int i = 0; i < 8; ++i) {
        float distanceZ = QVector3D::dotProduct(points[i], forward);
        if (distanceZ < minDistanceZ)
            minDistanceZ = distanceZ;
        if (distanceZ > maxDistanceZ)
            maxDistanceZ = distanceZ;
        float distanceY = QVector3D::dotProduct(points[i], up);
        if (distanceY < minDistanceY)
            minDistanceY = distanceY;
        if (distanceY > maxDistanceY)
            maxDistanceY = distanceY;
        float distanceX = QVector3D::dotProduct(points[i], right);
        if (distanceX < minDistanceX)
            minDistanceX = distanceX;
        if (distanceX > maxDistanceX)
            maxDistanceX = distanceX;
    }
    return QSSGBounds3(QVector3D(minDistanceX, minDistanceY, minDistanceZ),
                       QVector3D(maxDistanceX, maxDistanceY, maxDistanceZ));
}

static void setupCameraForShadowMap(const QRectF &inViewport,
                                    const QSSGRenderCamera &inCamera,
                                    const QSSGRenderLight *inLight,
                                    QSSGRenderCamera &theCamera,
                                    QVector3D *scenePoints = nullptr)
{
    // setup light matrix
    quint32 mapRes = 1 << inLight->m_shadowMapRes;
    QRectF theViewport(0.0f, 0.0f, (float)mapRes, (float)mapRes);
    theCamera.clipNear = 1.0f;
    theCamera.clipFar = inLight->m_shadowMapFar;
    // Setup camera projection
    QVector3D inLightPos = inLight->getGlobalPos();
    QVector3D inLightDir = inLight->getDirection();

    inLightPos -= inLightDir * inCamera.clipNear;
    theCamera.fov = qDegreesToRadians(90.f);

    if (inLight->m_lightType == QSSGRenderLight::Type::Directional) {
        QVector3D frustumPoints[8], boundCtr, sceneCtr;
        computeFrustumBounds(inCamera, inViewport, boundCtr, frustumPoints);

        if (scenePoints) {
            sceneCtr = QVector3D(0, 0, 0);
            for (int i = 0; i < 8; ++i)
                sceneCtr += scenePoints[i];
            sceneCtr *= 0.125f;
        }

        QVector3D forward = inLightDir;
        forward.normalize();
        QVector3D right;
        if (!qFuzzyCompare(qAbs(forward.y()), 1.0f))
            right = QVector3D::crossProduct(forward, QVector3D(0, 1, 0));
        else
            right = QVector3D::crossProduct(forward, QVector3D(1, 0, 0));
        right.normalize();
        QVector3D up = QVector3D::crossProduct(right, forward);
        up.normalize();

        // Calculate bounding box of the scene camera frustum
        QSSGBounds3 bounds = calculateShadowCameraBoundingBox(frustumPoints, forward, up, right);
        inLightPos = boundCtr;
        if (scenePoints) {
            QSSGBounds3 sceneBounds = calculateShadowCameraBoundingBox(scenePoints, forward, up,
                                                                       right);
            if (sceneBounds.extents().x() * sceneBounds.extents().y() * sceneBounds.extents().z()
                    < bounds.extents().x() * bounds.extents().y() * bounds.extents().z()) {
                bounds = sceneBounds;
                inLightPos = sceneCtr;
            }
        }

        // Apply bounding box parameters to shadow map camera projection matrix
        // so that the whole scene is fit inside the shadow map
        theViewport.setHeight(bounds.extents().y() * 2);
        theViewport.setWidth(bounds.extents().x() * 2);
        theCamera.clipNear = -bounds.extents().z() * 2;
        theCamera.clipFar = bounds.extents().z() * 2;
    }

    theCamera.flags.setFlag(QSSGRenderCamera::Flag::Orthographic, inLight->m_lightType == QSSGRenderLight::Type::Directional);
    theCamera.parent = nullptr;
    theCamera.pivot = inLight->pivot;

    if (inLight->m_lightType != QSSGRenderLight::Type::Point) {
        theCamera.lookAt(inLightPos, QVector3D(0, 1.0, 0), inLightPos + inLightDir);
    } else {
        theCamera.lookAt(inLightPos, QVector3D(0, 1.0, 0), QVector3D(0, 0, 0));
    }

    theCamera.calculateGlobalVariables(theViewport);
}

static void setupCubeShadowCameras(const QSSGRenderLight *inLight, QSSGRenderCamera inCameras[6])
{
    // setup light matrix
    quint32 mapRes = 1 << inLight->m_shadowMapRes;
    QRectF theViewport(0.0f, 0.0f, (float)mapRes, (float)mapRes);
    QQuaternion rotOfs[6];

    Q_ASSERT(inLight != nullptr);
    Q_ASSERT(inLight->m_lightType != QSSGRenderLight::Type::Directional);

    const QVector3D inLightPos = inLight->getGlobalPos();

    rotOfs[0] = QQuaternion::fromEulerAngles(0.f, qRadiansToDegrees(-QSSG_HALFPI), qRadiansToDegrees(QSSG_PI));
    rotOfs[1] = QQuaternion::fromEulerAngles(0.f, qRadiansToDegrees(QSSG_HALFPI), qRadiansToDegrees(QSSG_PI));
    rotOfs[2] = QQuaternion::fromEulerAngles(qRadiansToDegrees(QSSG_HALFPI), 0.f, 0.f);
    rotOfs[3] = QQuaternion::fromEulerAngles(qRadiansToDegrees(-QSSG_HALFPI), 0.f, 0.f);
    rotOfs[4] = QQuaternion::fromEulerAngles(0.f, qRadiansToDegrees(QSSG_PI), qRadiansToDegrees(-QSSG_PI));
    rotOfs[5] = QQuaternion::fromEulerAngles(0.f, 0.f, qRadiansToDegrees(QSSG_PI));

    for (int i = 0; i < 6; ++i) {
        inCameras[i].flags.setFlag(QSSGRenderCamera::Flag::Orthographic, false);
        inCameras[i].parent = nullptr;
        inCameras[i].pivot = inLight->pivot;
        inCameras[i].clipNear = 1.0f;
        inCameras[i].clipFar = qMax<float>(2.0f, inLight->m_shadowMapFar);
        inCameras[i].fov = qDegreesToRadians(90.f);

        inCameras[i].position = inLightPos;
        inCameras[i].rotation = rotOfs[i];
        inCameras[i].calculateGlobalVariables(theViewport);
    }

    /*
        if ( inLight->m_LightType == RenderLightTypes::Point ) return;

        QVector3D viewDirs[6];
        QVector3D viewUp[6];
        QMatrix3x3 theDirMatrix( inLight->m_GlobalTransform.getUpper3x3() );

        viewDirs[0] = theDirMatrix.transform( QVector3D( 1.f, 0.f, 0.f ) );
        viewDirs[2] = theDirMatrix.transform( QVector3D( 0.f, -1.f, 0.f ) );
        viewDirs[4] = theDirMatrix.transform( QVector3D( 0.f, 0.f, 1.f ) );
        viewDirs[0].normalize();  viewDirs[2].normalize();  viewDirs[4].normalize();
        viewDirs[1] = -viewDirs[0];
        viewDirs[3] = -viewDirs[2];
        viewDirs[5] = -viewDirs[4];

        viewUp[0] = viewDirs[2];
        viewUp[1] = viewDirs[2];
        viewUp[2] = viewDirs[5];
        viewUp[3] = viewDirs[4];
        viewUp[4] = viewDirs[2];
        viewUp[5] = viewDirs[2];

        for (int i = 0; i < 6; ++i)
        {
                inCameras[i].LookAt( inLightPos, viewUp[i], inLightPos + viewDirs[i] );
                inCameras[i].CalculateGlobalVariables( theViewport, QVector2D( theViewport.m_Width,
        theViewport.m_Height ) );
        }
        */
}

static const int ORTHO_SHADOW_UBUF_ENTRY_SIZE = 64 + 8;
static const int CUBE_SHADOW_UBUF_ENTRY_SIZE = 64 + 64 + 16 + 8;

static void rhiPrepareResourcesForShadowMap(QSSGRhiContext *rhiCtx,
                                            QSSGLayerRenderData &inData,
                                            QSSGShadowMapEntry *pEntry,
                                            float depthAdjust[2],
                                            const QVector<QSSGRenderableObjectHandle> &sortedOpaqueObjects,
                                            const QSSGRenderCamera &inCamera,
                                            bool orthographic,
                                            int cubeFace)
{
    QRhi *rhi = rhiCtx->rhi();
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
    const QMatrix4x4 correctionMatrix = rhi->clipSpaceCorrMatrix();

    // For the orthographic case (just one 2D texture) a single set of uniform
    // values is enough, but the cubemap approach runs 6 renderpasses, one for
    // each face, and it's good if we can use the same uniform buffer, just
    // with a different dynamic offset. Hence 6 times the size in that case.
    const int UBUF_ENTRY_SIZE = orthographic ? ORTHO_SHADOW_UBUF_ENTRY_SIZE : CUBE_SHADOW_UBUF_ENTRY_SIZE;
    const int UBUF_SIZE = orthographic ? UBUF_ENTRY_SIZE : 6 * rhi->ubufAligned(UBUF_ENTRY_SIZE);
    const int ubufBaseOffset = cubeFace * rhi->ubufAligned(UBUF_ENTRY_SIZE);

    for (const auto &handle : sortedOpaqueObjects) {
        QSSGRenderableObject *theObject = handle.obj;
        if (!theObject->renderableFlags.castsShadows())
            continue;

        QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();
        if (theObject->renderableFlags.isDefaultMaterialMeshSubset() || theObject->renderableFlags.isCustomMaterialMeshSubset()) {
            QSSGSubsetRenderableBase *subsetRenderable(static_cast<QSSGSubsetRenderableBase *>(theObject));

            const QSSGRhiUniformBufferSetKey ubufKey = { &inData.layer, &subsetRenderable->modelContext.model,
                                                         pEntry, QSSGRhiUniformBufferSetKey::Shadow };
            QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet(ubufKey));
            QRhiBuffer *&ubuf = uniformBuffers.ubuf;
            if (!ubuf) {
                ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, UBUF_SIZE);
                ubuf->create();
            } else if (ubuf->size() < UBUF_SIZE) {
                ubuf->setSize(UBUF_SIZE);
                ubuf->create();
            }

            const QMatrix4x4 modelViewProjection = correctionMatrix * pEntry->m_lightVP * subsetRenderable->globalTransform;
            // mat4 modelViewProjection
            rub->updateDynamicBuffer(ubuf, ubufBaseOffset, 64, modelViewProjection.constData());

            if (orthographic) {
                // vec2 depthAdjust
                rub->updateDynamicBuffer(ubuf, ubufBaseOffset + 64, 8, depthAdjust);
            } else {
                // mat4 modelMatrix
                rub->updateDynamicBuffer(ubuf, ubufBaseOffset + 64, 64, subsetRenderable->globalTransform.constData());
                // vec3 cameraPosition
                rub->updateDynamicBuffer(ubuf, ubufBaseOffset + 128, 12, &inCamera.position);
                // vec2 cameraProperties
                float cameraProps[] = { inCamera.clipNear, inCamera.clipFar };
                rub->updateDynamicBuffer(ubuf, ubufBaseOffset + 144, 8, cameraProps); // vec2 is aligned to 8, hence the 144
            }
        }
        cb->resourceUpdate(rub);
    }

}

static void rhiRenderOneShadowMap(QSSGRhiContext *rhiCtx,
                                  QSSGLayerRenderData &inData,
                                  QSSGShadowMapEntry *pEntry,
                                  QSSGRhiGraphicsPipelineState *ps,
                                  const QVector<QSSGRenderableObjectHandle> &sortedOpaqueObjects,
                                  bool orthographic,
                                  int cubeFace)
{
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
    const int UBUF_ENTRY_SIZE = orthographic ? ORTHO_SHADOW_UBUF_ENTRY_SIZE : CUBE_SHADOW_UBUF_ENTRY_SIZE;
    const int ubufBaseOffset = cubeFace * rhiCtx->rhi()->ubufAligned(UBUF_ENTRY_SIZE);
    bool needsSetViewport = true;

    for (const auto &handle : sortedOpaqueObjects) {
        QSSGRenderableObject *theObject = handle.obj;
        if (!theObject->renderableFlags.castsShadows())
            continue;

        if (theObject->renderableFlags.isDefaultMaterialMeshSubset() || theObject->renderableFlags.isCustomMaterialMeshSubset()) {
            QSSGSubsetRenderableBase *subsetRenderable(static_cast<QSSGSubsetRenderableBase *>(theObject));
            QSSGRef<QSSGRhiShaderStagesWithResources> shaderStages = orthographic
                    ? subsetRenderable->generator->getRhiOrthographicShadowDepthShader()
                    : subsetRenderable->generator->getRhiCubemapShadowDepthShader();
            if (!shaderStages)
                continue;

            ps->shaderStages = shaderStages->stages();
            ps->ia = subsetRenderable->subset.rhi.iaDepth;

            QRhiBuffer *vertexBuffer = subsetRenderable->subset.rhi.iaDepth.vertexBuffer->buffer();
            QRhiBuffer *indexBuffer = subsetRenderable->subset.rhi.iaDepth.indexBuffer
                    ? subsetRenderable->subset.rhi.iaDepth.indexBuffer->buffer()
                    : nullptr;

            const QSSGRhiUniformBufferSetKey ubufKey = { &inData.layer, &subsetRenderable->modelContext.model,
                                                         pEntry, QSSGRhiUniformBufferSetKey::Shadow };
            QRhiBuffer *ubuf = rhiCtx->uniformBufferSet(ubufKey).ubuf;

            QSSGRhiContext::ShaderResourceBindingList bindings;
            if (orthographic) {
                bindings.append(QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf));
            } else {
                bindings.append(QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                    0, VISIBILITY_ALL, ubuf, CUBE_SHADOW_UBUF_ENTRY_SIZE));
            }

            QRhiShaderResourceBindings *srb = rhiCtx->srb(bindings);
            const QSSGGraphicsPipelineStateKey pipelineKey { *ps, pEntry->m_rhiRenderPassDesc, srb };
            cb->setGraphicsPipeline(rhiCtx->pipeline(pipelineKey));
            if (orthographic) {
                cb->setShaderResources(srb);
            } else {
                const QRhiCommandBuffer::DynamicOffset dynOfs = { 0, quint32(ubufBaseOffset) };
                cb->setShaderResources(srb, 1, &dynOfs);
            }

            if (needsSetViewport) {
                cb->setViewport(ps->viewport);
                needsSetViewport = false;
            }

            QRhiCommandBuffer::VertexInput vb(vertexBuffer, 0);
            if (indexBuffer) {
                cb->setVertexInput(0, 1, &vb, indexBuffer, 0, subsetRenderable->subset.rhi.iaDepth.indexBuffer->indexFormat());
                cb->drawIndexed(subsetRenderable->subset.count, 1, subsetRenderable->subset.offset);
            } else {
                cb->setVertexInput(0, 1, &vb);
                cb->draw(subsetRenderable->subset.count, 1, subsetRenderable->subset.offset);
            }
        }
    }
}

static void rhiBlurShadowMap(QSSGRhiContext *rhiCtx,
                             QSSGShadowMapEntry *pEntry,
                             const QSSGRef<QSSGRenderer> &renderer,
                             float shadowFilter,
                             float shadowMapFar,
                             bool orthographic)
{
    // may not be able to do the blur pass if the number of max color
    // attachments is the gl/vk spec mandated minimum of 4, and we need 6.
    // (applicable only to !orthographic, whereas orthographic always works)
    if (!pEntry->m_rhiBlurRenderTarget0 || !pEntry->m_rhiBlurRenderTarget1)
        return;

    QRhi *rhi = rhiCtx->rhi();
    QSSGRhiGraphicsPipelineState ps;
    QRhiTexture *map = orthographic ? pEntry->m_rhiDepthMap : pEntry->m_rhiDepthCube;
    QRhiTexture *workMap = orthographic ? pEntry->m_rhiDepthCopy : pEntry->m_rhiCubeCopy;
    const QSize size = map->pixelSize();
    ps.viewport = QRhiViewport(0, 0, float(size.width()), float(size.height()));

    QSSGRef<QSSGRhiShaderStagesWithResources> shaderPipeline = orthographic ? renderer->getRhiOrthographicShadowBlurXShader()
                                                                            : renderer->getRhiCubemapShadowBlurXShader();
    if (!shaderPipeline)
        return;
    ps.shaderStages = shaderPipeline->stages();

    ps.colorAttachmentCount = orthographic ? 1 : 6;

    // construct a key that is unique for this frame (we use a dynamic buffer
    // so even if the same key gets used in the next frame, just doing
    // updateDynamicBuffer() on the same QRhiBuffer is ok due to QRhi's
    // internal double buffering)
    QSSGRhiUniformBufferSetKey ubufKey = { map, nullptr, nullptr, QSSGRhiUniformBufferSetKey::ShadowBlurX };

    QSSGRhiUniformBufferSet &ubufContainer = rhiCtx->uniformBufferSet(ubufKey);
    if (!ubufContainer.ubuf) {
        ubufContainer.ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64 + 8);
        ubufContainer.ubuf->create();
    }
    QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();

    // the blur also needs Y reversed in order to get correct results (while
    // the second blur step would end up with the correct orientation without
    // this too, but we need to blur the correct fragments in the second step
    // hence the flip is important)
    QMatrix4x4 flipY;
    // correct for D3D and Metal but not for Vulkan because there the Y is down
    // in NDC so that kind of self-corrects...
    if (rhi->isYUpInFramebuffer() != rhi->isYUpInNDC())
        flipY.data()[5] = -1.0f;
    rub->updateDynamicBuffer(ubufContainer.ubuf, 0, 64, flipY.constData());
    float cameraProperties[2] = { shadowFilter, shadowMapFar };
    rub->updateDynamicBuffer(ubufContainer.ubuf, 64, 8, cameraProperties);

    QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
    Q_ASSERT(sampler);

    QSSGRhiContext::ShaderResourceBindingList bindings = {
        QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubufContainer.ubuf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, map, sampler)
    };
    QRhiShaderResourceBindings *srb = rhiCtx->srb(bindings);

    QSSGRhiQuadRenderer::Flags quadFlags;
    if (orthographic) // orthoshadowshadowblurx and y have attr_uv as well
        quadFlags |= QSSGRhiQuadRenderer::UvCoords;
    renderer->rhiQuadRenderer()->prepareQuad(rhiCtx, rub);
    renderer->rhiQuadRenderer()->recordRenderQuadPass(rhiCtx, &ps, srb, pEntry->m_rhiBlurRenderTarget0, quadFlags);

    // repeat for blur Y, now depthCopy -> depthMap or cubeCopy -> depthCube

    shaderPipeline = orthographic ? renderer->getRhiOrthographicShadowBlurYShader()
                                  : renderer->getRhiCubemapShadowBlurYShader();
    if (!shaderPipeline)
        return;
    ps.shaderStages = shaderPipeline->stages();

    ubufKey = { map, nullptr, nullptr, QSSGRhiUniformBufferSetKey::ShadowBlurY };

    bindings = {
        QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubufContainer.ubuf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, workMap, sampler)
    };
    srb = rhiCtx->srb(bindings);

    renderer->rhiQuadRenderer()->prepareQuad(rhiCtx, nullptr);
    renderer->rhiQuadRenderer()->recordRenderQuadPass(rhiCtx, &ps, srb, pEntry->m_rhiBlurRenderTarget1, quadFlags);
}

static void rhiRenderShadowMap(QSSGRhiContext *rhiCtx,
                               QSSGLayerRenderData &inData,
                               QSSGRef<QSSGRenderShadowMap> &shadowMapManager,
                               const QRectF &viewViewport,
                               const QSSGRenderCamera &camera,
                               const QVector<QSSGRenderLight *> &globalLights,
                               const QVector<QSSGRenderableObjectHandle> &sortedOpaqueObjects,
                               const QSSGRef<QSSGRenderer> &renderer)
{
    QRhi *rhi = rhiCtx->rhi();
    QRhiCommandBuffer *cb = rhiCtx->commandBuffer();

    QSSGRhiGraphicsPipelineState ps;
    ps.depthTestEnable = true;
    ps.depthWriteEnable = true;

    // We need to deal with a clip depth range of [0, 1] or
    // [-1, 1], depending on the graphics API underneath.
    float depthAdjust[2]; // (d + depthAdjust[0]) * depthAdjust[1] = d mapped to [0, 1]
    if (rhi->isClipDepthZeroToOne()) {
        // d is [0, 1] so no need for any mapping
        depthAdjust[0] = 0.0f;
        depthAdjust[1] = 1.0f;
    } else {
        // d is [-1, 1]
        depthAdjust[0] = 1.0f;
        depthAdjust[1] = 0.5f;
    }

    // Try reducing self-shadowing and artifacts.
    ps.depthBias = 2;
    ps.slopeScaledDepthBias = 1.5f;

    auto bounds = camera.parent->getBounds(renderer->contextInterface()->bufferManager());

    QVector3D scenePoints[8];
    scenePoints[0] = bounds.minimum;
    scenePoints[1] = QVector3D(bounds.maximum.x(), bounds.minimum.y(), bounds.minimum.z());
    scenePoints[2] = QVector3D(bounds.minimum.x(), bounds.maximum.y(), bounds.minimum.z());
    scenePoints[3] = QVector3D(bounds.maximum.x(), bounds.maximum.y(), bounds.minimum.z());
    scenePoints[4] = QVector3D(bounds.minimum.x(), bounds.minimum.y(), bounds.maximum.z());
    scenePoints[5] = QVector3D(bounds.maximum.x(), bounds.minimum.y(), bounds.maximum.z());
    scenePoints[6] = QVector3D(bounds.minimum.x(), bounds.maximum.y(), bounds.maximum.z());
    scenePoints[7] = bounds.maximum;

    for (int i = 0, ie = globalLights.count(); i != ie; ++i) {
        if (!globalLights[i]->m_castShadow)
            continue;

        QSSGShadowMapEntry *pEntry = shadowMapManager->getShadowMapEntry(i);
        if (!pEntry)
            continue;

        Q_ASSERT(pEntry->m_rhiDepthStencil);
        const bool orthographic = pEntry->m_rhiDepthMap && pEntry->m_rhiDepthCopy;
        if (orthographic) {
            const QSize size = pEntry->m_rhiDepthMap->pixelSize();
            ps.viewport = QRhiViewport(0, 0, float(size.width()), float(size.height()));

            QSSGRenderCamera theCamera;
            setupCameraForShadowMap(viewViewport, camera, globalLights[i], theCamera, scenePoints);
            theCamera.calculateViewProjectionMatrix(pEntry->m_lightVP);
            pEntry->m_lightView = theCamera.globalTransform.inverted(); // pre-calculate this for the material

            rhiPrepareResourcesForShadowMap(rhiCtx, inData, pEntry, depthAdjust,
                                            sortedOpaqueObjects, theCamera, true, 0);

            // Render into the 2D texture pEntry->m_rhiDepthMap, using
            // pEntry->m_rhiDepthStencil as the (throwaway) depth/stencil buffer.
            QRhiTextureRenderTarget *rt = pEntry->m_rhiRenderTargets[0];
            cb->beginPass(rt, Qt::white, { 1.0f, 0 });
            rhiRenderOneShadowMap(rhiCtx, inData, pEntry, &ps, sortedOpaqueObjects, true, 0);
            cb->endPass();

            rhiBlurShadowMap(rhiCtx, pEntry, renderer, globalLights[i]->m_shadowFilter, globalLights[i]->m_shadowMapFar, true);
        } else {
            Q_ASSERT(pEntry->m_rhiDepthCube && pEntry->m_rhiCubeCopy);
            const QSize size = pEntry->m_rhiDepthCube->pixelSize();
            ps.viewport = QRhiViewport(0, 0, float(size.width()), float(size.height()));

            QSSGRenderCamera theCameras[6];
            setupCubeShadowCameras(globalLights[i], theCameras);
            pEntry->m_lightView = QMatrix4x4();

            const bool swapYFaces = !rhi->isYUpInFramebuffer();
            for (int face = 0; face < 6; ++face) {
                theCameras[face].calculateViewProjectionMatrix(pEntry->m_lightVP);
                pEntry->m_lightCubeView[face] = theCameras[face].globalTransform.inverted(); // pre-calculate this for the material

                rhiPrepareResourcesForShadowMap(rhiCtx, inData, pEntry, depthAdjust,
                                                sortedOpaqueObjects, theCameras[face], false, face);

                // Render into one face of the cubemap texture pEntry->m_rhiDephCube, using
                // pEntry->m_rhiDepthStencil as the (throwaway) depth/stencil buffer.

                int outFace = face;
                // FACE  S  T               GL
                // +x   -z, -y   right
                // -x   +z, -y   left
                // +y   +x, +z   top
                // -y   +x, -z   bottom
                // +z   +x, -y   front
                // -z   -x, -y   back
                // FACE  S  T               D3D
                // +x   -z, +y   right
                // -x   +z, +y   left
                // +y   +x, -z   bottom
                // -y   +x, +z   top
                // +z   +x, +y   front
                // -z   -x, +y   back
                if (swapYFaces) {
                    // +Y and -Y faces get swapped (D3D, Vulkan, Metal).
                    // See shadowMapping.glsllib. This is complemented there by reversing T as well.
                    if (outFace == 2)
                        outFace = 3;
                    else if (outFace == 3)
                        outFace = 2;
                }
                QRhiTextureRenderTarget *rt = pEntry->m_rhiRenderTargets[outFace];
                cb->beginPass(rt, Qt::white, { 1.0f, 0 });
                rhiRenderOneShadowMap(rhiCtx, inData, pEntry, &ps, sortedOpaqueObjects, false, face);
                cb->endPass();
            }

            rhiBlurShadowMap(rhiCtx, pEntry, renderer, globalLights[i]->m_shadowFilter, globalLights[i]->m_shadowMapFar, false);
        }
    }
}

static bool rhiPrepareAoTexture(QSSGRhiContext *rhiCtx, const QSize &size, QSSGRhiRenderableTexture *renderableTex)
{
    QRhi *rhi = rhiCtx->rhi();
    bool needsBuild = false;

    if (!renderableTex->texture) {
        // the ambient occlusion texture is always non-msaa, even if multisampling is used in the main pass
        renderableTex->texture = rhiCtx->rhi()->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget);
        needsBuild = true;
    } else if (renderableTex->texture->pixelSize() != size) {
        renderableTex->texture->setPixelSize(size);
        needsBuild = true;
    }

    if (needsBuild) {
        if (!renderableTex->texture->create()) {
            qWarning("Failed to build ambient occlusion texture (size %dx%d)", size.width(), size.height());
            renderableTex->reset();
            return false;
        }

        delete renderableTex->rt;
        delete renderableTex->rpDesc;
        renderableTex->rt = rhi->newTextureRenderTarget({ renderableTex->texture });
        renderableTex->rpDesc = renderableTex->rt->newCompatibleRenderPassDescriptor();
        renderableTex->rt->setRenderPassDescriptor(renderableTex->rpDesc);
        if (!renderableTex->rt->create()) {
            qWarning("Failed to build render target for ambient occlusion texture");
            renderableTex->reset();
            return false;
        }
    }

    return true;
}

static void rhiRenderAoTexture(QSSGRhiContext *rhiCtx,
                               const QSSGRhiGraphicsPipelineState &basePipelineState,
                               const QSSGLayerRenderData &inData,
                               const QSSGRenderCamera &camera)
{
    // no texelFetch in GLSL <= 120 and GLSL ES 100
    if (!rhiCtx->rhi()->isFeatureSupported(QRhi::TexelFetch)) {
        QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
        // just clear and stop there
        cb->beginPass(inData.m_rhiAoTexture.rt, Qt::white, { 1.0f, 0 });
        cb->endPass();
        return;
    }

    QSSGRef<QSSGRhiShaderStagesWithResources> shaderPipeline = inData.renderer->getRhiSsaoShader();
    if (!shaderPipeline)
        return;

    QSSGRhiGraphicsPipelineState ps = basePipelineState;
    ps.shaderStages = shaderPipeline->stages();

    const float R2 = inData.layer.aoDistance * inData.layer.aoDistance * 0.16f;
    const QSize textureSize = inData.m_rhiAoTexture.texture->pixelSize();
    const float rw = float(textureSize.width());
    const float rh = float(textureSize.height());
    const float fov = camera.verticalFov(rw / rh);
    const float tanHalfFovY = tanf(0.5f * fov * (rh / rw));
    const float invFocalLenX = tanHalfFovY * (rw / rh);

    const QVector4D aoProps(inData.layer.aoStrength * 0.01f, inData.layer.aoDistance * 0.4f, inData.layer.aoSoftness * 0.02f, inData.layer.aoBias);
    const QVector4D aoProps2(float(inData.layer.aoSamplerate), (inData.layer.aoDither) ? 1.0f : 0.0f, 0.0f, 0.0f);
    const QVector4D aoScreenConst(1.0f / R2, rh / (2.0f * tanHalfFovY), 1.0f / rw, 1.0f / rh);
    const QVector4D uvToEyeConst(2.0f * invFocalLenX, -2.0f * tanHalfFovY, -invFocalLenX, tanHalfFovY);
    const QVector2D cameraProps(camera.clipNear, camera.clipFar);

//    layout(std140, binding = 0) uniform buf {
//        vec4 aoProperties;
//        vec4 aoProperties2;
//        vec4 aoScreenConst;
//        vec4 uvToEyeConst;
//        vec2 cameraProperties;

    const int UBUF_SIZE = 72;
    const QSSGRhiUniformBufferSetKey ubufKey = { &inData.layer, nullptr, nullptr, QSSGRhiUniformBufferSetKey::AoTexture };
    QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet(ubufKey));
    QRhiBuffer *&ubuf = uniformBuffers.ubuf;
    if (!ubuf) {
        ubuf = rhiCtx->rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, UBUF_SIZE);
        ubuf->create();
    } else if (ubuf->size() < UBUF_SIZE) {
        ubuf->setSize(UBUF_SIZE);
        ubuf->create();
    }

    QRhiResourceUpdateBatch *rub = rhiCtx->rhi()->nextResourceUpdateBatch();
    rub->updateDynamicBuffer(ubuf, 0, 16, &aoProps);
    rub->updateDynamicBuffer(ubuf, 16, 16, &aoProps2);
    rub->updateDynamicBuffer(ubuf, 32, 16, &aoScreenConst);
    rub->updateDynamicBuffer(ubuf, 48, 16, &uvToEyeConst);
    rub->updateDynamicBuffer(ubuf, 64, 8, &cameraProps);

    QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                             QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
    QSSGRhiContext::ShaderResourceBindingList bindings = {
        QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, inData.m_rhiDepthTexture.texture, sampler)
    };
    QRhiShaderResourceBindings *srb = rhiCtx->srb(bindings);

    inData.renderer->rhiQuadRenderer()->prepareQuad(rhiCtx, rub);
    inData.renderer->rhiQuadRenderer()->recordRenderQuadPass(rhiCtx, &ps, srb, inData.m_rhiAoTexture.rt, {});
}

static inline void offsetProjectionMatrix(QMatrix4x4 &inProjectionMatrix,
                                          const QVector2D &inVertexOffsets)
{
    inProjectionMatrix(0, 3) += inProjectionMatrix(3, 3) * inVertexOffsets.x();
    inProjectionMatrix(1, 3) += inProjectionMatrix(3, 3) * inVertexOffsets.y();
}

// These are meant to be pixel offsets, so you need to divide them by the width/height
// of the layer respectively.
static const QVector2D s_ProgressiveAAVertexOffsets[QSSGLayerRenderPreparationData::MAX_AA_LEVELS] = {
    QVector2D(-0.170840f, -0.553840f), // 1x
    QVector2D(0.162960f, -0.319340f), // 2x
    QVector2D(0.360260f, -0.245840f), // 3x
    QVector2D(-0.561340f, -0.149540f), // 4x
    QVector2D(0.249460f, 0.453460f), // 5x
    QVector2D(-0.336340f, 0.378260f), // 6x
    QVector2D(0.340000f, 0.166260f), // 7x
    QVector2D(0.235760f, 0.527760f), // 8x
};

static inline bool isValidItem2D(QSSGRenderItem2D *item2D)
{
    return item2D->combinedOpacity >= QSSG_RENDER_MINIMUM_RENDER_OPACITY
            && item2D->qsgTexture
            && QSGTexturePrivate::get(item2D->qsgTexture)->rhiTexture();
}

// Phase 1: prepare. Called when the renderpass is not yet started on the command buffer.
void QSSGLayerRenderData::rhiPrepare()
{
    QSSGRhiContext *rhiCtx = renderer->contextInterface()->rhiContext().data();
    Q_ASSERT(rhiCtx->isValid());

    setShaderFeature(QSSGShaderDefines::asString(QSSGShaderDefines::Rhi), true);

    QSSGRhiGraphicsPipelineState *ps = rhiCtx->resetGraphicsPipelineState(this);

    const QRectF vp = layerPrepResult->viewport();
    ps->viewport = { float(vp.x()), float(vp.y()), float(vp.width()), float(vp.height()), 0.0f, 1.0f };
    ps->scissorEnable = true;
    const QRect sc = layerPrepResult->scissor().toRect();
    ps->scissor = { sc.x(), sc.y(), sc.width(), sc.height() };


    const bool animating = layerPrepResult->flags.wasLayerDataDirty();
    if (animating)
        layer.progAAPassIndex = 0;

    const bool progressiveAA = layer.antialiasingMode == QSSGRenderLayer::AAMode::ProgressiveAA && !animating;
    layer.progressiveAAIsActive = progressiveAA;

    const bool temporalAA = layer.temporalAAEnabled && !progressiveAA &&  layer.antialiasingMode != QSSGRenderLayer::AAMode::MSAA;

    layer.temporalAAIsActive = temporalAA;

    QVector2D vertexOffsetsAA;

    if (progressiveAA && layer.progAAPassIndex > 0) {
        int idx = layer.progAAPassIndex - 1;
        vertexOffsetsAA = s_ProgressiveAAVertexOffsets[idx] / QVector2D{ float(vp.width()/2.0), float(vp.height()/2.0) };
    }

    if (temporalAA) {
        const int t = 1 - 2 * (layer.tempAAPassIndex % 2);
        const float f = t * layer.temporalAAStrength;
        vertexOffsetsAA = { f / float(vp.width()/2.0), f / float(vp.height()/2.0) };
    }

    if (temporalAA || progressiveAA /*&& !vertexOffsetsAA.isNull()*/) {
        // TODO - optimize this exact matrix operation.
        for (qint32 idx = 0, end = modelContexts.size(); idx < end; ++idx) {
            QMatrix4x4 &originalProjection(modelContexts[idx]->modelViewProjection);
            offsetProjectionMatrix(originalProjection, vertexOffsetsAA);  //????? do these get reset per frame, or does it accumulate???
        }
    }

    QSSGStackPerfTimer ___timer(renderer->contextInterface()->performanceTimer(), Q_FUNC_INFO);
    if (camera) {
        renderer->beginLayerRender(*this);

        QSSGRhiContext *rhiCtx = renderer->contextInterface()->rhiContext().data();
        Q_ASSERT(rhiCtx->rhi()->isRecordingFrame());
        QRhiCommandBuffer *cb = rhiCtx->commandBuffer();

        const QVector2D theCameraProps = QVector2D(camera->clipNear, camera->clipFar);
        const auto &sortedOpaqueObjects = getOpaqueRenderableObjects(true); // front to back
        const auto &sortedTransparentObjects = getTransparentRenderableObjects(); // back to front
        const auto &item2Ds = getRenderableItem2Ds();

        // If needed, generate a depth texture, containing both opaque and alpha objects.
        if (layerPrepResult->flags.requiresDepthTexture() && m_progressiveAAPassIndex == 0) {
            cb->debugMarkBegin(QByteArrayLiteral("Quick3D depth texture"));

            if (rhiPrepareDepthTexture(rhiCtx, layerPrepResult->textureDimensions(), &m_rhiDepthTexture)) {
                Q_ASSERT(m_rhiDepthTexture.isValid());
                if (rhiPrepareDepthPass(rhiCtx, *ps, m_rhiDepthTexture.rpDesc, *this,
                                        sortedOpaqueObjects, sortedTransparentObjects, item2Ds,
                                        QSSGRhiUniformBufferSetKey::DepthTexture,
                                        1))
                {
                    bool needsSetVieport = true;
                    cb->beginPass(m_rhiDepthTexture.rt, Qt::transparent, { 1.0f, 0 });
                    // NB! We do not pass sortedTransparentObjects in the 4th
                    // argument to stay compatible with the 5.15 code base,
                    // which also does not include semi-transparent objects in
                    // the depth texture.
                    rhiRenderDepthPass(rhiCtx, *this, sortedOpaqueObjects, {}, item2Ds, &needsSetVieport);
                    cb->endPass();
                } else {
                    m_rhiDepthTexture.reset();
                }
            }

            cb->debugMarkEnd();
        } else {
            // Do not keep it around when no longer needed. Internally QRhi
            // takes care of keeping the native texture resource around as long
            // as it is in use by an in-flight frame we do not have to worry
            // about that here.
            m_rhiDepthTexture.reset();
        }

        // Screen space ambient occlusion. Relies on the depth texture and generates an AO map.
        if (layerPrepResult->flags.requiresSsaoPass() && m_progressiveAAPassIndex == 0 && camera) {
           cb->debugMarkBegin(QByteArrayLiteral("Quick3D SSAO map"));

           if (rhiPrepareAoTexture(rhiCtx, layerPrepResult->textureDimensions(), &m_rhiAoTexture)) {
               Q_ASSERT(m_rhiAoTexture.isValid());
               rhiRenderAoTexture(rhiCtx, *ps, *this, *camera);
           }

           cb->debugMarkEnd();
        } else {
            m_rhiAoTexture.reset();
        }

        // Shadows. Generates a 2D or cube shadow map. (opaque objects only)
        if (layerPrepResult->flags.requiresShadowMapPass() && m_progressiveAAPassIndex == 0) {
            if (!shadowMapManager)
                createShadowMapManager();

            if (!opaqueObjects.isEmpty() || !globalLights.isEmpty()) {
                cb->debugMarkBegin(QByteArrayLiteral("Quick3D shadow map"));

                rhiRenderShadowMap(rhiCtx, *this, shadowMapManager, layerPrepResult->viewport(), *camera,
                                   globalLights, // scoped lights are not relevant here
                                   sortedOpaqueObjects,
                                   renderer);

                cb->debugMarkEnd();
            }
        }

        // Z (depth) pre-pass, if enabled, is part of the main render pass. (opaque objects only)
        // Prepare the data for it.
        bool zPrePass = layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthPrePass)
                && layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthTest)
                && (!sortedOpaqueObjects.isEmpty() || !item2Ds.isEmpty());
        if (zPrePass) {
            cb->debugMarkBegin(QByteArrayLiteral("Quick3D prepare Z prepass"));
            if (!rhiPrepareDepthPass(rhiCtx, *ps, rhiCtx->mainRenderPassDescriptor(), *this,
                                     sortedOpaqueObjects, {}, item2Ds,
                                     QSSGRhiUniformBufferSetKey::ZPrePass,
                                     rhiCtx->mainPassSampleCount()))
            {
                // alas, no Z prepass for you
                m_zPrePassPossible = false;
            }
            cb->debugMarkEnd();
        }

        // Now onto preparing the data for the main pass.

        // make the buffer copies and other stuff we put on the command buffer in
        // here show up within a named section in tools like RenderDoc when running
        // with QSG_RHI_PROFILE=1 (which enables debug markers)
        cb->debugMarkBegin(QByteArrayLiteral("Quick3D prepare renderables"));

        QSSGRhiGraphicsPipelineState *ps = rhiCtx->graphicsPipelineState(this);

        ps->depthFunc = QRhiGraphicsPipeline::LessOrEqual;
        ps->blendEnable = false;

        if (layer.background == QSSGRenderLayer::Background::SkyBox) {
            QRhi *rhi = rhiCtx->rhi();
            QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();

            QRhiTexture *texture = layer.lightProbe->m_textureData.m_rhiTexture;
            QSSGRhiContext::ShaderResourceBindingList bindings;

            QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear, //We have mipmaps
                                                     QRhiSampler::Repeat, QRhiSampler::ClampToEdge });
            int samplerBinding = 1; //the shader code is hand-written, so we don't need to look that up
            const int ubufSize = 2 * 4 * 4 * sizeof(float) + sizeof(float); // 2x mat4 + 1 float
            bindings.append(QRhiShaderResourceBinding::sampledTexture(samplerBinding,
                                                                      QRhiShaderResourceBinding::FragmentStage,
                                                                      texture, sampler));

            const QSSGRhiUniformBufferSetKey ubufKey = { nullptr, nullptr, nullptr, QSSGRhiUniformBufferSetKey::SkyBox };
            QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet(ubufKey));

            QRhiBuffer *&ubuf = uniformBuffers.ubuf;
            if (!ubuf) {
                ubuf = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubufSize);
                ubuf->create();
            }

            const QMatrix4x4 &projection = camera->projection;
            const QMatrix4x4 &viewMatrix = camera->globalTransform;
            float adjustY = rhi->isYUpInNDC() ? 1.0f : -1.0f;

            constexpr int matrixSize = 4 * 4 * sizeof(float);
            rub->updateDynamicBuffer(ubuf, 0, matrixSize, viewMatrix.constData());
            rub->updateDynamicBuffer(ubuf, matrixSize, matrixSize, projection.constData());
            rub->updateDynamicBuffer(ubuf, 2*matrixSize, sizeof(float), &adjustY);

            bindings.append(QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf));

            layer.skyBoxSrb = rhiCtx->srb(bindings);

            renderer->rhiQuadRenderer()->prepareQuad(rhiCtx, rub);
        }

        if (layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthTest) && !sortedOpaqueObjects.isEmpty()) {
            ps->depthTestEnable = true;
            // enable depth write for opaque objects when there was no Z prepass
            ps->depthWriteEnable = !layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthPrePass) || !m_zPrePassPossible;
        } else {
            ps->depthTestEnable = false;
            ps->depthWriteEnable = false;
        }

        // opaque objects (or, this list is empty when LayerEnableDepthTest is disabled)
        for (const auto &handle : sortedOpaqueObjects) {
            QSSGRenderableObject *theObject = handle.obj;
            QSSGScopedLightsListScope lightsScope(globalLights, lightDirections, sourceLightDirections, theObject->scopedLights);
            setShaderFeature(QSSGShaderDefines::asString(QSSGShaderDefines::CgLighting), !globalLights.empty());
            rhiPrepareRenderable(rhiCtx, *this, *theObject, theCameraProps, 0, *camera);
        }

        // textured quads for QQuickItem trees parented to QQuick3DObjects.
        layer.item2DSrbs.resize(item2Ds.count());
        if (!item2Ds.isEmpty()) {
            QRhi *rhi = rhiCtx->rhi();
            int validItem2DCount = 0;
            for (int i = 0, count = item2Ds.count(); i < count; ++i) {
                layer.item2DSrbs[i] = nullptr;
                if (isValidItem2D(static_cast<QSSGRenderItem2D *>(item2Ds[i].node)))
                    validItem2DCount += 1;
            }
            if (validItem2DCount) {
                // Now that we know the number of valid textures, have a single
                // uniform buffer containing the transforms and stuff for all.
                const int plainUbufSize = 64 + 8 + 4;
                const int oneUbufSize = rhi->ubufAligned(plainUbufSize);
                // We only use one buffer for all Item2Ds in a View3D (good!),
                // but there can be multiple View3D instances in a scene. So
                // still needs something in the key to differentiate: use the
                // layer (which is per-View3D), just like the main pass
                // rendering the 3D objects would do.
                const QSSGRhiUniformBufferSetKey ubufKey = { &layer, nullptr, nullptr, QSSGRhiUniformBufferSetKey::Item2D };
                QSSGRhiUniformBufferSet &uniformBuffers(rhiCtx->uniformBufferSet(ubufKey));
                QRhiBuffer *&ubuf = uniformBuffers.ubuf;
                const int ubufSize = validItem2DCount * oneUbufSize;
                if (!ubuf) {
                    ubuf = rhiCtx->rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubufSize);
                    ubuf->create();
                } else if (ubuf->size() < ubufSize) {
                    ubuf->setSize(ubufSize);
                    ubuf->create();
                }
                QRhiSampler *sampler = rhiCtx->sampler({ QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                                         QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge });
                QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();
                int actualIndex = 0;
                for (int i = 0, count = item2Ds.count(); i < count; ++i) {
                    QSSGRenderItem2D *item2D = static_cast<QSSGRenderItem2D *>(item2Ds[i].node);
                    if (!isValidItem2D(item2D))
                        continue;
                    QRhiTexture *texture = QSGTexturePrivate::get(item2D->qsgTexture)->rhiTexture();
                    const QVector2D dimensions = QVector2D(item2D->qsgTexture->textureSize().width(),
                                                           item2D->qsgTexture->textureSize().height());
                    const float opacity = item2D->combinedOpacity;
                    const int ubufOffset = actualIndex * oneUbufSize;
                    rub->updateDynamicBuffer(ubuf, ubufOffset, 64, item2D->MVP.constData());
                    rub->updateDynamicBuffer(ubuf, ubufOffset + 64, 8, &dimensions);
                    rub->updateDynamicBuffer(ubuf, ubufOffset + 72, 4, &opacity);
                    QSSGRhiContext::ShaderResourceBindingList bindings;
                    bindings.append(QRhiShaderResourceBinding::uniformBuffer(0, VISIBILITY_ALL, ubuf, ubufOffset, plainUbufSize));
                    bindings.append(QRhiShaderResourceBinding::sampledTexture(1,
                                                                              QRhiShaderResourceBinding::FragmentStage,
                                                                              texture,
                                                                              sampler));
                    layer.item2DSrbs[i] = rhiCtx->srb(bindings);
                    actualIndex += 1;
                }
                renderer->rhiQuadRenderer()->prepareQuad(rhiCtx, rub);
            }
        }

        // transparent objects (or, without LayerEnableDepthTest, all objects)
        ps->blendEnable = true;
        ps->depthWriteEnable = false;

        for (const auto &handle : sortedTransparentObjects) {
            QSSGRenderableObject *theObject = handle.obj;
            if (!(theObject->renderableFlags.isCompletelyTransparent())) {
                QSSGScopedLightsListScope lightsScope(globalLights, lightDirections, sourceLightDirections, theObject->scopedLights);
                setShaderFeature(QSSGShaderDefines::asString(QSSGShaderDefines::CgLighting), !globalLights.empty());
                rhiPrepareRenderable(rhiCtx, *this, *theObject, theCameraProps, 0, *camera);
            }
        }

        cb->debugMarkEnd();

        renderer->endLayerRender();
    }
}

static void rhiRenderRenderable(QSSGRhiContext *rhiCtx,
                                QSSGLayerRenderData &inData,
                                QSSGRenderableObject &object,
                                bool *needsSetViewport)
{
    if (object.renderableFlags.isDefaultMaterialMeshSubset()) {
        QSSGSubsetRenderable &subsetRenderable(static_cast<QSSGSubsetRenderable &>(object));

        QRhiGraphicsPipeline *ps = subsetRenderable.rhiRenderData.mainPass.pipeline;
        QRhiShaderResourceBindings *srb = subsetRenderable.rhiRenderData.mainPass.srb;
        if (!ps || !srb)
            return;

        QRhiBuffer *vertexBuffer = subsetRenderable.subset.rhi.ia.vertexBuffer->buffer();
        QRhiBuffer *indexBuffer = subsetRenderable.subset.rhi.ia.indexBuffer ? subsetRenderable.subset.rhi.ia.indexBuffer->buffer() : nullptr;

        QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
        // QRhi optimizes out unnecessary binding of the same pipline
        cb->setGraphicsPipeline(ps);
        cb->setShaderResources(srb);

        if (*needsSetViewport) {
            cb->setViewport(rhiCtx->graphicsPipelineState(&inData)->viewport);
            *needsSetViewport = false;
        }

        QRhiCommandBuffer::VertexInput vb(vertexBuffer, 0);
        if (indexBuffer) {
            cb->setVertexInput(0, 1, &vb, indexBuffer, 0, subsetRenderable.subset.rhi.ia.indexBuffer->indexFormat());
            cb->drawIndexed(subsetRenderable.subset.count, 1, subsetRenderable.subset.offset);
        } else {
            cb->setVertexInput(0, 1, &vb);
            cb->draw(subsetRenderable.subset.count, 1, subsetRenderable.subset.offset);
        }
    } else if (object.renderableFlags.isCustomMaterialMeshSubset()) {
        QSSGCustomMaterialRenderable &renderable(static_cast<QSSGCustomMaterialRenderable &>(object));
        QSSGMaterialSystem &customMaterialSystem(*renderable.generator->contextInterface()->customMaterialSystem().data());
        customMaterialSystem.renderRhiSubset(rhiCtx, renderable, inData, needsSetViewport);
    } else {
        Q_ASSERT(false);
    }
}

// Phase 2: render. Called within an active renderpass on the command buffer.
void QSSGLayerRenderData::rhiRender()
{
    QSSGRhiContext *rhiCtx = renderer->contextInterface()->rhiContext().data();

    QSSGStackPerfTimer ___timer(renderer->contextInterface()->performanceTimer(), Q_FUNC_INFO);
    if (camera) {
        renderer->beginLayerRender(*this);

        QRhiCommandBuffer *cb = rhiCtx->commandBuffer();
        const auto &theOpaqueObjects = getOpaqueRenderableObjects(true);
        const auto &item2Ds = getRenderableItem2Ds();
        bool needsSetViewport = true;

        bool zPrePass = layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthPrePass)
                && layer.flags.testFlag(QSSGRenderLayer::Flag::LayerEnableDepthTest)
                && (!theOpaqueObjects.isEmpty() || !item2Ds.isEmpty());
        if (zPrePass && m_zPrePassPossible) {
            cb->debugMarkBegin(QByteArrayLiteral("Quick3D render Z prepass"));
            rhiRenderDepthPass(rhiCtx, *this, theOpaqueObjects, {}, item2Ds, &needsSetViewport);
            cb->debugMarkEnd();
        }

        if (layer.background == QSSGRenderLayer::Background::SkyBox
                && rhiCtx->rhi()->isFeatureSupported(QRhi::TexelFetch))
        {
            auto shaderPipeline = renderer->getRhiSkyBoxShader();
            Q_ASSERT(shaderPipeline);
            QSSGRhiGraphicsPipelineState *ps = rhiCtx->graphicsPipelineState(this);
            ps->shaderStages = shaderPipeline->stages();
            QRhiShaderResourceBindings *srb = layer.skyBoxSrb;
            QRhiRenderPassDescriptor *rpDesc = rhiCtx->mainRenderPassDescriptor();
            renderer->rhiQuadRenderer()->recordRenderQuad(rhiCtx, ps, srb, rpDesc, {});
        }

        cb->debugMarkBegin(QByteArrayLiteral("Quick3D render opaque"));
        for (const auto &handle : theOpaqueObjects) {
            QSSGRenderableObject *theObject = handle.obj;
            rhiRenderRenderable(rhiCtx, *this, *theObject, &needsSetViewport);
        }
        cb->debugMarkEnd();

        if (!item2Ds.isEmpty()) {
            cb->debugMarkBegin(QByteArrayLiteral("Quick3D render 2D item quads"));
            auto shaderPipeline = renderer->getRhiTexturedQuadShader();
            Q_ASSERT(shaderPipeline);
            QSSGRhiGraphicsPipelineState *ps = rhiCtx->graphicsPipelineState(this);
            ps->shaderStages = shaderPipeline->stages();
            QRhiRenderPassDescriptor *rpDesc = rhiCtx->mainRenderPassDescriptor();
            for (int i = 0, count = item2Ds.count(); i < count; ++i) {
                QRhiShaderResourceBindings *srb = layer.item2DSrbs[i];
                if (srb) {
                    // ### These quads may have transparency (from the 2D item
                    // opacity) so need blending but are treated as opaque, so
                    // depth write must be enabled (see also the Z prepass)
                    // This is incorrect but matches the 5.15 code. To be
                    // investigated.
                    const QSSGRhiQuadRenderer::Flags quadFlags =
                            QSSGRhiQuadRenderer::UvCoords
                            | QSSGRhiQuadRenderer::DepthTest | QSSGRhiQuadRenderer::DepthWrite
                            | QSSGRhiQuadRenderer::PremulBlend;
                    renderer->rhiQuadRenderer()->recordRenderQuad(rhiCtx, ps, srb, rpDesc, quadFlags);
                }
            }
            cb->debugMarkEnd();
        }

        cb->debugMarkBegin(QByteArrayLiteral("Quick3D render alpha"));
        const auto &theTransparentObjects = getTransparentRenderableObjects();
        for (const auto &handle : theTransparentObjects) {
            QSSGRenderableObject *theObject = handle.obj;
            if (!theObject->renderableFlags.isCompletelyTransparent())
                rhiRenderRenderable(rhiCtx, *this, *theObject, &needsSetViewport);
        }
        cb->debugMarkEnd();

        renderer->endLayerRender();
    }
}

QT_END_NAMESPACE
