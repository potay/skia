/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkGpuBlurUtils.h"

#include "SkRect.h"

#if SK_SUPPORT_GPU
#include "effects/GrConvolutionEffect.h"
#include "GrContext.h"
#endif

namespace SkGpuBlurUtils {

#if SK_SUPPORT_GPU

#define MAX_BLUR_SIGMA 4.0f

static void scale_rect(SkRect* rect, float xScale, float yScale) {
    rect->fLeft   = SkScalarMul(rect->fLeft,   SkFloatToScalar(xScale));
    rect->fTop    = SkScalarMul(rect->fTop,    SkFloatToScalar(yScale));
    rect->fRight  = SkScalarMul(rect->fRight,  SkFloatToScalar(xScale));
    rect->fBottom = SkScalarMul(rect->fBottom, SkFloatToScalar(yScale));
}

static float adjust_sigma(float sigma, int *scaleFactor, int *radius) {
    *scaleFactor = 1;
    while (sigma > MAX_BLUR_SIGMA) {
        *scaleFactor *= 2;
        sigma *= 0.5f;
    }
    *radius = static_cast<int>(ceilf(sigma * 3.0f));
    GrAssert(*radius <= GrConvolutionEffect::kMaxKernelRadius);
    return sigma;
}

static void convolve_gaussian(GrContext* context,
                              GrTexture* texture,
                              const SkRect& rect,
                              float sigma,
                              int radius,
                              Gr1DKernelEffect::Direction direction) {
    GrPaint paint;

    SkAutoTUnref<GrEffectRef> conv(GrConvolutionEffect::CreateGaussian(texture,
                                                                       direction,
                                                                       radius,
                                                                       sigma));
    paint.addColorEffect(conv);
    context->drawRect(paint, rect);
}

GrTexture* GaussianBlur(GrContext* context,
                        GrTexture* srcTexture,
                        bool canClobberSrc,
                        const SkRect& rect,
                        float sigmaX,
                        float sigmaY) {
    GrAssert(NULL != context);

    GrContext::AutoRenderTarget art(context);

    GrContext::AutoMatrix am;
    am.setIdentity(context);

    SkIRect clearRect;
    int scaleFactorX, radiusX;
    int scaleFactorY, radiusY;
    sigmaX = adjust_sigma(sigmaX, &scaleFactorX, &radiusX);
    sigmaY = adjust_sigma(sigmaY, &scaleFactorY, &radiusY);

    SkRect srcRect(rect);
    scale_rect(&srcRect, 1.0f / scaleFactorX, 1.0f / scaleFactorY);
    srcRect.roundOut();
    scale_rect(&srcRect, static_cast<float>(scaleFactorX),
                         static_cast<float>(scaleFactorY));

    GrContext::AutoClip acs(context, srcRect);

    GrAssert(kBGRA_8888_GrPixelConfig == srcTexture->config() ||
             kRGBA_8888_GrPixelConfig == srcTexture->config() ||
             kAlpha_8_GrPixelConfig == srcTexture->config());

    GrTextureDesc desc;
    desc.fFlags = kRenderTarget_GrTextureFlagBit | kNoStencil_GrTextureFlagBit;
    desc.fWidth = SkScalarFloorToInt(srcRect.width());
    desc.fHeight = SkScalarFloorToInt(srcRect.height());
    desc.fConfig = srcTexture->config();

    GrAutoScratchTexture temp1, temp2;
    GrTexture* dstTexture = temp1.set(context, desc);
    GrTexture* tempTexture = canClobberSrc ? srcTexture : temp2.set(context, desc);
    if (NULL == dstTexture || NULL == tempTexture) {
        return NULL;
    }

    for (int i = 1; i < scaleFactorX || i < scaleFactorY; i *= 2) {
        GrPaint paint;
        SkMatrix matrix;
        matrix.setIDiv(srcTexture->width(), srcTexture->height());
        context->setRenderTarget(dstTexture->asRenderTarget());
        SkRect dstRect(srcRect);
        scale_rect(&dstRect, i < scaleFactorX ? 0.5f : 1.0f,
                             i < scaleFactorY ? 0.5f : 1.0f);
        GrTextureParams params(SkShader::kClamp_TileMode, true);
        paint.addColorTextureEffect(srcTexture, matrix, params);
        context->drawRectToRect(paint, dstRect, srcRect);
        srcRect = dstRect;
        srcTexture = dstTexture;
        SkTSwap(dstTexture, tempTexture);
    }

    SkIRect srcIRect;
    srcRect.roundOut(&srcIRect);

    if (sigmaX > 0.0f) {
        if (scaleFactorX > 1) {
            // Clear out a radius to the right of the srcRect to prevent the
            // X convolution from reading garbage.
            clearRect = SkIRect::MakeXYWH(srcIRect.fRight, srcIRect.fTop,
                                          radiusX, srcIRect.height());
            context->clear(&clearRect, 0x0);
        }
        context->setRenderTarget(dstTexture->asRenderTarget());
        convolve_gaussian(context, srcTexture, srcRect, sigmaX, radiusX,
                          Gr1DKernelEffect::kX_Direction);
        srcTexture = dstTexture;
        SkTSwap(dstTexture, tempTexture);
    }

    if (sigmaY > 0.0f) {
        if (scaleFactorY > 1 || sigmaX > 0.0f) {
            // Clear out a radius below the srcRect to prevent the Y
            // convolution from reading garbage.
            clearRect = SkIRect::MakeXYWH(srcIRect.fLeft, srcIRect.fBottom,
                                          srcIRect.width(), radiusY);
            context->clear(&clearRect, 0x0);
        }

        context->setRenderTarget(dstTexture->asRenderTarget());
        convolve_gaussian(context, srcTexture, srcRect, sigmaY, radiusY,
                          Gr1DKernelEffect::kY_Direction);
        srcTexture = dstTexture;
        SkTSwap(dstTexture, tempTexture);
    }

    if (scaleFactorX > 1 || scaleFactorY > 1) {
        // Clear one pixel to the right and below, to accommodate bilinear
        // upsampling.
        clearRect = SkIRect::MakeXYWH(srcIRect.fLeft, srcIRect.fBottom,
                                      srcIRect.width() + 1, 1);
        context->clear(&clearRect, 0x0);
        clearRect = SkIRect::MakeXYWH(srcIRect.fRight, srcIRect.fTop,
                                      1, srcIRect.height());
        context->clear(&clearRect, 0x0);
        SkMatrix matrix;
        matrix.setIDiv(srcTexture->width(), srcTexture->height());
        context->setRenderTarget(dstTexture->asRenderTarget());

        GrPaint paint;
        // FIXME:  this should be mitchell, not bilinear.
        GrTextureParams params(SkShader::kClamp_TileMode, true);
        paint.addColorTextureEffect(srcTexture, matrix, params);

        SkRect dstRect(srcRect);
        scale_rect(&dstRect, (float) scaleFactorX, (float) scaleFactorY);
        context->drawRectToRect(paint, dstRect, srcRect);
        srcRect = dstRect;
        srcTexture = dstTexture;
        SkTSwap(dstTexture, tempTexture);
    }
    if (srcTexture == temp1.texture()) {
        return temp1.detach();
    } else if (srcTexture == temp2.texture()) {
        return temp2.detach();
    } else {
        srcTexture->ref();
        return srcTexture;
    }
}
#endif

}