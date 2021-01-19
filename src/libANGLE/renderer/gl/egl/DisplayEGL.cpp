//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// DisplayEGL.cpp: Common across EGL parts of platform specific egl::Display implementations

#include "libANGLE/renderer/gl/egl/DisplayEGL.h"

#include "common/debug.h"
#include "libANGLE/Context.h"
#include "libANGLE/Display.h"
#include "libANGLE/Surface.h"
#include "libANGLE/renderer/gl/ContextGL.h"
#include "libANGLE/renderer/gl/RendererGL.h"
#include "libANGLE/renderer/gl/egl/ContextEGL.h"
#include "libANGLE/renderer/gl/egl/DmaBufImageSiblingEGL.h"
#include "libANGLE/renderer/gl/egl/FunctionsEGLDL.h"
#include "libANGLE/renderer/gl/egl/ImageEGL.h"
#include "libANGLE/renderer/gl/egl/PbufferSurfaceEGL.h"
#include "libANGLE/renderer/gl/egl/RendererEGL.h"
#include "libANGLE/renderer/gl/egl/SyncEGL.h"
#include "libANGLE/renderer/gl/egl/WindowSurfaceEGL.h"
#include "libANGLE/renderer/gl/renderergl_utils.h"

namespace
{

rx::RobustnessVideoMemoryPurgeStatus GetRobustnessVideoMemoryPurge(const egl::AttributeMap &attribs)
{
    return static_cast<rx::RobustnessVideoMemoryPurgeStatus>(
        attribs.get(EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV, GL_FALSE));
}

std::vector<EGLint> RenderableTypesFromPlatformAttrib(const rx::FunctionsEGL *egl,
                                                      const EGLAttrib platformAttrib)
{
    std::vector<EGLint> renderableTypes;
    switch (platformAttrib)
    {
        case EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE:
            renderableTypes.push_back(EGL_OPENGL_BIT);
            break;

        case EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE:
        {
            static_assert(EGL_OPENGL_ES3_BIT == EGL_OPENGL_ES3_BIT_KHR,
                          "Extension define must match core");

            gl::Version eglVersion(egl->majorVersion, egl->minorVersion);
            if (eglVersion >= gl::Version(1, 5) || egl->hasExtension("EGL_KHR_create_context"))
            {
                renderableTypes.push_back(EGL_OPENGL_ES3_BIT);
            }
            renderableTypes.push_back(EGL_OPENGL_ES2_BIT);
        }
        break;

        default:
            break;
    }
    return renderableTypes;
}

class WorkerContextEGL final : public rx::WorkerContext
{
  public:
    WorkerContextEGL(EGLContext context, rx::FunctionsEGL *functions, EGLSurface pbuffer);
    ~WorkerContextEGL() override;

    bool makeCurrent() override;
    void unmakeCurrent() override;

  private:
    EGLContext mContext;
    rx::FunctionsEGL *mFunctions;
    EGLSurface mPbuffer;
};

WorkerContextEGL::WorkerContextEGL(EGLContext context,
                                   rx::FunctionsEGL *functions,
                                   EGLSurface pbuffer)
    : mContext(context), mFunctions(functions), mPbuffer(pbuffer)
{}

WorkerContextEGL::~WorkerContextEGL()
{
    mFunctions->destroyContext(mContext);
}

bool WorkerContextEGL::makeCurrent()
{
    if (mFunctions->makeCurrent(mPbuffer, mContext) == EGL_FALSE)
    {
        ERR() << "Unable to make the EGL context current.";
        return false;
    }
    return true;
}

void WorkerContextEGL::unmakeCurrent()
{
    mFunctions->makeCurrent(EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

}  // namespace

namespace rx
{

static constexpr bool kDefaultEGLVirtualizedContexts = true;

DisplayEGL::DisplayEGL(const egl::DisplayState &state)
    : DisplayGL(state), mVirtualizedContexts(kDefaultEGLVirtualizedContexts)
{}

DisplayEGL::~DisplayEGL() {}

ImageImpl *DisplayEGL::createImage(const egl::ImageState &state,
                                   const gl::Context *context,
                                   EGLenum target,
                                   const egl::AttributeMap &attribs)
{
    return new ImageEGL(state, context, target, attribs, mEGL);
}

EGLSyncImpl *DisplayEGL::createSync(const egl::AttributeMap &attribs)
{
    return new SyncEGL(attribs, mEGL);
}

const char *DisplayEGL::getEGLPath() const
{
#if defined(ANGLE_PLATFORM_ANDROID)
#    if defined(__LP64__)
    return "/system/lib64/libEGL.so";
#    else
    return "/system/lib/libEGL.so";
#    endif
#else
    return "libEGL.so.1";
#endif
}

egl::Error DisplayEGL::initializeContext(EGLContext shareContext,
                                         const egl::AttributeMap &eglAttributes,
                                         EGLContext *outContext,
                                         native_egl::AttributeVector *outAttribs) const
{
    gl::Version eglVersion(mEGL->majorVersion, mEGL->minorVersion);

    EGLint requestedMajor =
        eglAttributes.getAsInt(EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, EGL_DONT_CARE);
    EGLint requestedMinor =
        eglAttributes.getAsInt(EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, EGL_DONT_CARE);
    bool initializeRequested = requestedMajor != EGL_DONT_CARE && requestedMinor != EGL_DONT_CARE;

    static_assert(EGL_CONTEXT_MAJOR_VERSION == EGL_CONTEXT_MAJOR_VERSION_KHR,
                  "Major Version define should match");
    static_assert(EGL_CONTEXT_MINOR_VERSION == EGL_CONTEXT_MINOR_VERSION_KHR,
                  "Minor Version define should match");

    std::vector<egl::AttributeMap> contextAttribLists;
    if (eglVersion >= gl::Version(1, 5) || mEGL->hasExtension("EGL_KHR_create_context"))
    {
        if (initializeRequested)
        {
            egl::AttributeMap requestedVersionAttribs;
            requestedVersionAttribs.insert(EGL_CONTEXT_MAJOR_VERSION, requestedMajor);
            requestedVersionAttribs.insert(EGL_CONTEXT_MINOR_VERSION, requestedMinor);

            contextAttribLists.push_back(std::move(requestedVersionAttribs));
        }
        else
        {
            // clang-format off
            const gl::Version esVersionsFrom2_0[] = {
                gl::Version(3, 2),
                gl::Version(3, 1),
                gl::Version(3, 0),
                gl::Version(2, 0),
            };
            // clang-format on

            for (const auto &version : esVersionsFrom2_0)
            {
                egl::AttributeMap versionAttribs;
                versionAttribs.insert(EGL_CONTEXT_MAJOR_VERSION,
                                      static_cast<EGLint>(version.major));
                versionAttribs.insert(EGL_CONTEXT_MINOR_VERSION,
                                      static_cast<EGLint>(version.minor));

                contextAttribLists.push_back(std::move(versionAttribs));
            }
        }
    }
    else
    {
        if (initializeRequested && (requestedMajor != 2 || requestedMinor != 0))
        {
            return egl::EglBadAttribute() << "Unsupported requested context version";
        }

        egl::AttributeMap fallbackAttribs;
        fallbackAttribs.insert(EGL_CONTEXT_CLIENT_VERSION, 2);

        contextAttribLists.push_back(std::move(fallbackAttribs));
    }

    for (const egl::AttributeMap &attribs : contextAttribLists)
    {
        // If robustness is supported, try to create a context with robustness enabled. If it fails,
        // fall back to creating a context without the robustness parameters. We've seen devices
        // that expose the robustness extensions but fail to create robust contexts.
        if (mHasEXTCreateContextRobustness)
        {
            egl::AttributeMap attribsWithRobustness(attribs);

            attribsWithRobustness.insert(EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY,
                                         EGL_LOSE_CONTEXT_ON_RESET);
            if (mHasNVRobustnessVideoMemoryPurge)
            {
                attribsWithRobustness.insert(EGL_GENERATE_RESET_ON_VIDEO_MEMORY_PURGE_NV, GL_TRUE);
            }

            native_egl::AttributeVector attribVector = attribsWithRobustness.toIntVector();
            EGLContext context = mEGL->createContext(mConfig, shareContext, attribVector.data());
            if (context != EGL_NO_CONTEXT)
            {
                *outContext = context;
                *outAttribs = std::move(attribVector);
                return egl::NoError();
            }

            INFO() << "EGL_EXT_create_context_robustness available but robust context creation "
                      "failed.";
        }

        native_egl::AttributeVector attribVector = attribs.toIntVector();
        EGLContext context = mEGL->createContext(mConfig, shareContext, attribVector.data());
        if (context != EGL_NO_CONTEXT)
        {
            *outContext = context;
            *outAttribs = std::move(attribVector);
            return egl::NoError();
        }
    }

    return egl::Error(mEGL->getError(), "eglCreateContext failed");
}

egl::Error DisplayEGL::initialize(egl::Display *display)
{
    mDisplayAttributes = display->getAttributeMap();
    mVirtualizedContexts =
        ShouldUseVirtualizedContexts(mDisplayAttributes, kDefaultEGLVirtualizedContexts);
    mEGL = new FunctionsEGLDL();

    void *eglHandle =
        reinterpret_cast<void *>(mDisplayAttributes.get(EGL_PLATFORM_ANGLE_EGL_HANDLE_ANGLE, 0));
    ANGLE_TRY(mEGL->initialize(display->getNativeDisplayId(), getEGLPath(), eglHandle));

    gl::Version eglVersion(mEGL->majorVersion, mEGL->minorVersion);
    if (eglVersion < gl::Version(1, 4))
    {
        return egl::EglNotInitialized() << "EGL >= 1.4 is required";
    }

    mHasEXTCreateContextRobustness   = mEGL->hasExtension("EGL_EXT_create_context_robustness");
    mHasNVRobustnessVideoMemoryPurge = mEGL->hasExtension("EGL_NV_robustness_video_memory_purge");

    const EGLAttrib platformAttrib      = mDisplayAttributes.get(EGL_PLATFORM_ANGLE_TYPE_ANGLE, 0);
    std::vector<EGLint> renderableTypes = RenderableTypesFromPlatformAttrib(mEGL, platformAttrib);
    if (renderableTypes.empty())
    {
        return egl::EglNotInitialized() << "No available renderable types.";
    }

    egl::AttributeMap baseConfigAttribs;
    baseConfigAttribs.insert(EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER);
    baseConfigAttribs.insert(EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT);

    egl::AttributeMap configAttribsWithFormat(baseConfigAttribs);
    // Choose RGBA8888
    configAttribsWithFormat.insert(EGL_RED_SIZE, 8);
    configAttribsWithFormat.insert(EGL_GREEN_SIZE, 8);
    configAttribsWithFormat.insert(EGL_BLUE_SIZE, 8);
    configAttribsWithFormat.insert(EGL_ALPHA_SIZE, 8);

    // Choose D24S8
    // EGL1.5 spec Section 2.2 says that depth, multisample and stencil buffer depths
    // must match for contexts to be compatible.
    configAttribsWithFormat.insert(EGL_DEPTH_SIZE, 24);
    configAttribsWithFormat.insert(EGL_STENCIL_SIZE, 8);

    EGLConfig configWithFormat = EGL_NO_CONFIG_KHR;
    for (EGLint renderableType : renderableTypes)
    {
        baseConfigAttribs.insert(EGL_RENDERABLE_TYPE, renderableType);
        configAttribsWithFormat.insert(EGL_RENDERABLE_TYPE, renderableType);

        std::vector<EGLint> attribVector = configAttribsWithFormat.toIntVector();

        EGLint numConfig = 0;
        if (mEGL->chooseConfig(attribVector.data(), &configWithFormat, 1, &numConfig) == EGL_TRUE)
        {
            break;
        }
    }

    if (configWithFormat == EGL_NO_CONFIG_KHR)
    {
        return egl::EglNotInitialized()
               << "eglChooseConfig failed with " << egl::Error(mEGL->getError());
    }

    // A mock pbuffer is only needed if surfaceless contexts are not supported.
    mSupportsSurfaceless = mEGL->hasExtension("EGL_KHR_surfaceless_context");
    if (!mSupportsSurfaceless)
    {
        int mockPbufferAttribs[] = {
            EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE,
        };
        mMockPbuffer = mEGL->createPbufferSurface(configWithFormat, mockPbufferAttribs);
        if (mMockPbuffer == EGL_NO_SURFACE)
        {
            return egl::EglNotInitialized()
                   << "eglCreatePbufferSurface failed with " << egl::Error(mEGL->getError());
        }
    }

    // Create mMockPbuffer with a normal config, but create a no_config mContext, if possible
    if (mEGL->hasExtension("EGL_KHR_no_config_context"))
    {
        mConfigAttribList = baseConfigAttribs.toIntVector();
        mConfig           = EGL_NO_CONFIG_KHR;
    }
    else
    {
        mConfigAttribList = configAttribsWithFormat.toIntVector();
        mConfig           = configWithFormat;
    }

    ANGLE_TRY(createRenderer(EGL_NO_CONTEXT, true, false, &mRenderer));

    const gl::Version &maxVersion = mRenderer->getMaxSupportedESVersion();
    if (maxVersion < gl::Version(2, 0))
    {
        return egl::EglNotInitialized() << "OpenGL ES 2.0 is not supportable.";
    }

    ANGLE_TRY(DisplayGL::initialize(display));

    INFO() << "ANGLE DisplayEGL initialized: " << getRendererDescription();

    return egl::NoError();
}

void DisplayEGL::terminate()
{
    DisplayGL::terminate();

    EGLBoolean success = mEGL->makeCurrent(EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (success == EGL_FALSE)
    {
        ERR() << "eglMakeCurrent error " << egl::Error(mEGL->getError());
    }

    if (mMockPbuffer != EGL_NO_SURFACE)
    {
        success      = mEGL->destroySurface(mMockPbuffer);
        mMockPbuffer = EGL_NO_SURFACE;
        if (success == EGL_FALSE)
        {
            ERR() << "eglDestroySurface error " << egl::Error(mEGL->getError());
        }
    }

    mRenderer.reset();

    mCurrentNativeContexts.clear();

    egl::Error result = mEGL->terminate();
    if (result.isError())
    {
        ERR() << "eglTerminate error " << result;
    }

    SafeDelete(mEGL);
}

SurfaceImpl *DisplayEGL::createWindowSurface(const egl::SurfaceState &state,
                                             EGLNativeWindowType window,
                                             const egl::AttributeMap &attribs)
{
    EGLConfig config;
    EGLint numConfig;
    EGLBoolean success;

    const EGLint configAttribList[] = {EGL_CONFIG_ID, mConfigIds[state.config->configID], EGL_NONE};
    success                         = mEGL->chooseConfig(configAttribList, &config, 1, &numConfig);
    ASSERT(success && numConfig == 1);

    return new WindowSurfaceEGL(state, mEGL, config, window);
}

SurfaceImpl *DisplayEGL::createPbufferSurface(const egl::SurfaceState &state,
                                              const egl::AttributeMap &attribs)
{
    EGLConfig config;
    EGLint numConfig;
    EGLBoolean success;

    const EGLint configAttribList[] = {EGL_CONFIG_ID, mConfigIds[state.config->configID], EGL_NONE};
    success                         = mEGL->chooseConfig(configAttribList, &config, 1, &numConfig);
    ASSERT(success && numConfig == 1);

    return new PbufferSurfaceEGL(state, mEGL, config);
}

class ExternalSurfaceEGL : public SurfaceEGL
{
  public:
    ExternalSurfaceEGL(const egl::SurfaceState &state,
                       const FunctionsEGL *egl,
                       EGLConfig config,
                       EGLint width,
                       EGLint height)
        : SurfaceEGL(state, egl, config), mWidth(width), mHeight(height)
    {}
    ~ExternalSurfaceEGL() override = default;

    egl::Error initialize(const egl::Display *display) override { return egl::NoError(); }
    EGLint getSwapBehavior() const override { return EGL_BUFFER_DESTROYED; }
    EGLint getWidth() const override { return mWidth; }
    EGLint getHeight() const override { return mHeight; }
    bool isExternal() const override { return true; }

  private:
    const EGLint mWidth;
    const EGLint mHeight;
};

SurfaceImpl *DisplayEGL::createPbufferFromClientBuffer(const egl::SurfaceState &state,
                                                       EGLenum buftype,
                                                       EGLClientBuffer clientBuffer,
                                                       const egl::AttributeMap &attribs)
{
    switch (buftype)
    {
        case EGL_EXTERNAL_SURFACE_ANGLE:
            return new ExternalSurfaceEGL(state, mEGL, EGL_NO_CONFIG_KHR,
                                          attribs.getAsInt(EGL_WIDTH, 0),
                                          attribs.getAsInt(EGL_HEIGHT, 0));

        default:
            return DisplayGL::createPbufferFromClientBuffer(state, buftype, clientBuffer, attribs);
    }
}

SurfaceImpl *DisplayEGL::createPixmapSurface(const egl::SurfaceState &state,
                                             NativePixmapType nativePixmap,
                                             const egl::AttributeMap &attribs)
{
    UNIMPLEMENTED();
    return nullptr;
}

ContextImpl *DisplayEGL::createContext(const gl::State &state,
                                       gl::ErrorSet *errorSet,
                                       const egl::Config *configuration,
                                       const gl::Context *shareContext,
                                       const egl::AttributeMap &attribs)
{
    std::shared_ptr<RendererEGL> renderer;
    bool usingExternalContext = attribs.get(EGL_EXTERNAL_CONTEXT_ANGLE, EGL_FALSE) == EGL_TRUE;
    if (mVirtualizedContexts && !usingExternalContext)
    {
        renderer = mRenderer;
    }
    else
    {
        EGLContext nativeShareContext = EGL_NO_CONTEXT;
        if (usingExternalContext)
        {
            ASSERT(!shareContext);
        }
        else if (shareContext)
        {
            ContextEGL *shareContextEGL = GetImplAs<ContextEGL>(shareContext);
            nativeShareContext          = shareContextEGL->getContext();
        }

        // Create a new renderer for this context.  It only needs to share with the user's requested
        // share context because there are no internal resources in DisplayEGL that are shared
        // at the GL level.
        egl::Error error =
            createRenderer(nativeShareContext, false, usingExternalContext, &renderer);
        if (error.isError())
        {
            ERR() << "Failed to create a shared renderer: " << error.getMessage();
            return nullptr;
        }
    }

    RobustnessVideoMemoryPurgeStatus robustnessVideoMemoryPurgeStatus =
        GetRobustnessVideoMemoryPurge(attribs);
    return new ContextEGL(state, errorSet, renderer, robustnessVideoMemoryPurgeStatus);
}

template <typename T>
void DisplayEGL::getConfigAttrib(EGLConfig config, EGLint attribute, T *value) const
{
    EGLint tmp;
    EGLBoolean success = mEGL->getConfigAttrib(config, attribute, &tmp);
    ASSERT(success == EGL_TRUE);
    *value = tmp;
}

template <typename T, typename U>
void DisplayEGL::getConfigAttribIfExtension(EGLConfig config,
                                            EGLint attribute,
                                            T *value,
                                            const char *extension,
                                            const U &defaultValue) const
{
    if (mEGL->hasExtension(extension))
    {
        getConfigAttrib(config, attribute, value);
    }
    else
    {
        *value = static_cast<T>(defaultValue);
    }
}

egl::ConfigSet DisplayEGL::generateConfigs()
{
    egl::ConfigSet configSet;
    mConfigIds.clear();

    EGLint numConfigs;
    EGLBoolean success = mEGL->chooseConfig(mConfigAttribList.data(), nullptr, 0, &numConfigs);
    ASSERT(success == EGL_TRUE && numConfigs > 0);

    std::vector<EGLConfig> configs(numConfigs);
    EGLint numConfigs2;
    success =
        mEGL->chooseConfig(mConfigAttribList.data(), configs.data(), numConfigs, &numConfigs2);
    ASSERT(success == EGL_TRUE && numConfigs2 == numConfigs);

    for (int i = 0; i < numConfigs; i++)
    {
        egl::Config config;

        getConfigAttrib(configs[i], EGL_BUFFER_SIZE, &config.bufferSize);
        getConfigAttrib(configs[i], EGL_RED_SIZE, &config.redSize);
        getConfigAttrib(configs[i], EGL_GREEN_SIZE, &config.greenSize);
        getConfigAttrib(configs[i], EGL_BLUE_SIZE, &config.blueSize);
        getConfigAttrib(configs[i], EGL_LUMINANCE_SIZE, &config.luminanceSize);
        getConfigAttrib(configs[i], EGL_ALPHA_SIZE, &config.alphaSize);
        getConfigAttrib(configs[i], EGL_ALPHA_MASK_SIZE, &config.alphaMaskSize);
        getConfigAttrib(configs[i], EGL_BIND_TO_TEXTURE_RGB, &config.bindToTextureRGB);
        getConfigAttrib(configs[i], EGL_BIND_TO_TEXTURE_RGBA, &config.bindToTextureRGBA);
        getConfigAttrib(configs[i], EGL_COLOR_BUFFER_TYPE, &config.colorBufferType);
        getConfigAttrib(configs[i], EGL_CONFIG_CAVEAT, &config.configCaveat);
        getConfigAttrib(configs[i], EGL_CONFIG_ID, &config.configID);
        getConfigAttrib(configs[i], EGL_CONFORMANT, &config.conformant);
        getConfigAttrib(configs[i], EGL_DEPTH_SIZE, &config.depthSize);
        getConfigAttrib(configs[i], EGL_LEVEL, &config.level);
        getConfigAttrib(configs[i], EGL_MAX_PBUFFER_WIDTH, &config.maxPBufferWidth);
        getConfigAttrib(configs[i], EGL_MAX_PBUFFER_HEIGHT, &config.maxPBufferHeight);
        getConfigAttrib(configs[i], EGL_MAX_PBUFFER_PIXELS, &config.maxPBufferPixels);
        getConfigAttrib(configs[i], EGL_MAX_SWAP_INTERVAL, &config.maxSwapInterval);
        getConfigAttrib(configs[i], EGL_MIN_SWAP_INTERVAL, &config.minSwapInterval);
        getConfigAttrib(configs[i], EGL_NATIVE_RENDERABLE, &config.nativeRenderable);
        getConfigAttrib(configs[i], EGL_NATIVE_VISUAL_ID, &config.nativeVisualID);
        getConfigAttrib(configs[i], EGL_NATIVE_VISUAL_TYPE, &config.nativeVisualType);
        getConfigAttrib(configs[i], EGL_RENDERABLE_TYPE, &config.renderableType);
        getConfigAttrib(configs[i], EGL_SAMPLE_BUFFERS, &config.sampleBuffers);
        getConfigAttrib(configs[i], EGL_SAMPLES, &config.samples);
        getConfigAttrib(configs[i], EGL_STENCIL_SIZE, &config.stencilSize);
        getConfigAttrib(configs[i], EGL_SURFACE_TYPE, &config.surfaceType);
        getConfigAttrib(configs[i], EGL_TRANSPARENT_TYPE, &config.transparentType);
        getConfigAttrib(configs[i], EGL_TRANSPARENT_RED_VALUE, &config.transparentRedValue);
        getConfigAttrib(configs[i], EGL_TRANSPARENT_GREEN_VALUE, &config.transparentGreenValue);
        getConfigAttrib(configs[i], EGL_TRANSPARENT_BLUE_VALUE, &config.transparentBlueValue);
        getConfigAttribIfExtension(configs[i], EGL_COLOR_COMPONENT_TYPE_EXT,
                                   &config.colorComponentType, "EGL_EXT_pixel_format_float",
                                   EGL_COLOR_COMPONENT_TYPE_FIXED_EXT);

        // Pixmaps are not supported on EGL, make sure the config doesn't expose them.
        config.surfaceType &= ~EGL_PIXMAP_BIT;

        if (config.colorBufferType == EGL_RGB_BUFFER)
        {
            ASSERT(config.colorComponentType == EGL_COLOR_COMPONENT_TYPE_FIXED_EXT);
            if (config.redSize == 8 && config.greenSize == 8 && config.blueSize == 8 &&
                config.alphaSize == 8)
            {
                config.renderTargetFormat = GL_RGBA8;
            }
            else if (config.redSize == 8 && config.greenSize == 8 && config.blueSize == 8 &&
                     config.alphaSize == 0)
            {
                config.renderTargetFormat = GL_RGB8;
            }
            else if (config.redSize == 5 && config.greenSize == 6 && config.blueSize == 5 &&
                     config.alphaSize == 0)
            {
                config.renderTargetFormat = GL_RGB565;
            }
            else if (config.redSize == 5 && config.greenSize == 5 && config.blueSize == 5 &&
                     config.alphaSize == 1)
            {
                config.renderTargetFormat = GL_RGB5_A1;
            }
            else if (config.redSize == 4 && config.greenSize == 4 && config.blueSize == 4 &&
                     config.alphaSize == 4)
            {
                config.renderTargetFormat = GL_RGBA4;
            }
            else if (config.redSize == 10 && config.greenSize == 10 && config.blueSize == 10 &&
                     config.alphaSize == 2)
            {
                config.renderTargetFormat = GL_RGB10_A2;
            }
            else
            {
                ERR() << "RGBA(" << config.redSize << "," << config.greenSize << ","
                      << config.blueSize << "," << config.alphaSize << ") not handled";
                continue;
            }
        }
        else
        {
            continue;
        }

        if (config.depthSize == 0 && config.stencilSize == 0)
        {
            config.depthStencilFormat = GL_ZERO;
        }
        else if (config.depthSize == 16 && config.stencilSize == 0)
        {
            config.depthStencilFormat = GL_DEPTH_COMPONENT16;
        }
        else if (config.depthSize == 24 && config.stencilSize == 0)
        {
            config.depthStencilFormat = GL_DEPTH_COMPONENT24;
        }
        else if (config.depthSize == 24 && config.stencilSize == 8)
        {
            config.depthStencilFormat = GL_DEPTH24_STENCIL8;
        }
        else if (config.depthSize == 0 && config.stencilSize == 8)
        {
            config.depthStencilFormat = GL_STENCIL_INDEX8;
        }
        else
        {
            continue;
        }

        config.matchNativePixmap  = EGL_NONE;
        config.optimalOrientation = 0;

        int internalId         = configSet.add(config);
        mConfigIds[internalId] = config.configID;
    }

    return configSet;
}

bool DisplayEGL::testDeviceLost()
{
    return false;
}

egl::Error DisplayEGL::restoreLostDevice(const egl::Display *display)
{
    UNIMPLEMENTED();
    return egl::NoError();
}

bool DisplayEGL::isValidNativeWindow(EGLNativeWindowType window) const
{
    return true;
}

egl::Error DisplayEGL::validateClientBuffer(const egl::Config *configuration,
                                            EGLenum buftype,
                                            EGLClientBuffer clientBuffer,
                                            const egl::AttributeMap &attribs) const
{
    switch (buftype)
    {
        case EGL_EXTERNAL_SURFACE_ANGLE:
            ASSERT(clientBuffer == nullptr);
            return egl::NoError();

        default:
            return DisplayGL::validateClientBuffer(configuration, buftype, clientBuffer, attribs);
    }
}

egl::Error DisplayEGL::waitClient(const gl::Context *context)
{
    UNIMPLEMENTED();
    return egl::NoError();
}

egl::Error DisplayEGL::waitNative(const gl::Context *context, EGLint engine)
{
    UNIMPLEMENTED();
    return egl::NoError();
}

egl::Error DisplayEGL::makeCurrent(egl::Display *display,
                                   egl::Surface *drawSurface,
                                   egl::Surface *readSurface,
                                   gl::Context *context)
{
    CurrentNativeContext &currentContext = mCurrentNativeContexts[std::this_thread::get_id()];

    EGLSurface newSurface = EGL_NO_SURFACE;
    if (drawSurface)
    {
        SurfaceEGL *drawSurfaceEGL = GetImplAs<SurfaceEGL>(drawSurface);
        newSurface                 = drawSurfaceEGL->getSurface();
    }

    EGLContext newContext = EGL_NO_CONTEXT;
    if (context)
    {
        ContextEGL *contextEGL = GetImplAs<ContextEGL>(context);
        newContext             = contextEGL->getContext();
    }

    if (currentContext.isExternalContext || (context && context->isExternal()))
    {
        ASSERT(currentContext.surface == EGL_NO_SURFACE);
        if (!currentContext.isExternalContext)
        {
            // Switch to an ANGLE external context.
            ASSERT(context);
            ASSERT(currentContext.context == EGL_NO_CONTEXT);
            currentContext.context           = newContext;
            currentContext.isExternalContext = true;

            // We only support using external surface with external context.
            ASSERT(GetImplAs<SurfaceEGL>(drawSurface)->isExternal());
            ASSERT(GetImplAs<SurfaceEGL>(drawSurface)->getSurface() == EGL_NO_SURFACE);
        }
        else if (context)
        {
            // Switch surface but not context.
            ASSERT(currentContext.context == newContext);
            ASSERT(newSurface == EGL_NO_SURFACE);
            ASSERT(newContext != EGL_NO_CONTEXT);
            // We only support using external surface with external context.
            ASSERT(GetImplAs<SurfaceEGL>(drawSurface)->isExternal());
            ASSERT(GetImplAs<SurfaceEGL>(drawSurface)->getSurface() == EGL_NO_SURFACE);
        }
        else
        {
            // Release the ANGLE external context.
            ASSERT(newSurface == EGL_NO_SURFACE);
            ASSERT(newContext == EGL_NO_CONTEXT);
            ASSERT(currentContext.context != EGL_NO_CONTEXT);
            currentContext.context           = EGL_NO_CONTEXT;
            currentContext.isExternalContext = false;
        }

        // Do not need to call eglMakeCurrent(), since we don't support switching EGLSurface for
        // external context.
        return DisplayGL::makeCurrent(display, drawSurface, readSurface, context);
    }

    // The context should never change when context virtualization is being used unless binding a
    // null context.
    if (mVirtualizedContexts && newContext != EGL_NO_CONTEXT)
    {
        ASSERT(currentContext.context == EGL_NO_CONTEXT || newContext == currentContext.context);

        newContext = mRenderer->getContext();

        // If we know that we're only running on one thread (mVirtualizedContexts == true) and
        // EGL_NO_SURFACE is going to be bound, we can optimize this case by not changing the
        // surface binding and emulate the surfaceless extension in the frontend.
        if (newSurface == EGL_NO_SURFACE)
        {
            newSurface = currentContext.surface;
        }

        // It's possible that no surface has been created yet and the driver doesn't support
        // surfaceless, bind the mock pbuffer.
        if (newSurface == EGL_NO_SURFACE && !mSupportsSurfaceless)
        {
            newSurface = mMockPbuffer;
            ASSERT(newSurface != EGL_NO_SURFACE);
        }
    }

    if (newSurface != currentContext.surface || newContext != currentContext.context)
    {
        if (mEGL->makeCurrent(newSurface, newContext) == EGL_FALSE)
        {
            return egl::Error(mEGL->getError(), "eglMakeCurrent failed");
        }
        currentContext.surface = newSurface;
        currentContext.context = newContext;
    }

    return DisplayGL::makeCurrent(display, drawSurface, readSurface, context);
}

gl::Version DisplayEGL::getMaxSupportedESVersion() const
{
    return mRenderer->getMaxSupportedESVersion();
}

void DisplayEGL::destroyNativeContext(EGLContext context)
{
    // If this context is current, remove it from the tracking of current contexts to make sure we
    // don't try to make it current again.
    for (auto &currentContext : mCurrentNativeContexts)
    {
        if (currentContext.second.context == context)
        {
            currentContext.second.surface = EGL_NO_SURFACE;
            currentContext.second.context = EGL_NO_CONTEXT;
        }
    }

    mEGL->destroyContext(context);
}

void DisplayEGL::generateExtensions(egl::DisplayExtensions *outExtensions) const
{
    gl::Version eglVersion(mEGL->majorVersion, mEGL->minorVersion);

    outExtensions->createContextRobustness =
        mEGL->hasExtension("EGL_EXT_create_context_robustness");

    outExtensions->postSubBuffer    = false;  // Since SurfaceEGL::postSubBuffer is not implemented
    outExtensions->presentationTime = mEGL->hasExtension("EGL_ANDROID_presentation_time");

    // Contexts are virtualized so textures and semaphores can be shared globally
    outExtensions->displayTextureShareGroup   = true;
    outExtensions->displaySemaphoreShareGroup = true;

    // We will fallback to regular swap if swapBuffersWithDamage isn't
    // supported, so indicate support here to keep validation happy.
    outExtensions->swapBuffersWithDamage = true;

    outExtensions->image     = mEGL->hasExtension("EGL_KHR_image");
    outExtensions->imageBase = mEGL->hasExtension("EGL_KHR_image_base");
    // Pixmaps are not supported in ANGLE's EGL implementation.
    // outExtensions->imagePixmap = mEGL->hasExtension("EGL_KHR_image_pixmap");
    outExtensions->glTexture2DImage      = mEGL->hasExtension("EGL_KHR_gl_texture_2D_image");
    outExtensions->glTextureCubemapImage = mEGL->hasExtension("EGL_KHR_gl_texture_cubemap_image");
    outExtensions->glTexture3DImage      = mEGL->hasExtension("EGL_KHR_gl_texture_3D_image");
    outExtensions->glRenderbufferImage   = mEGL->hasExtension("EGL_KHR_gl_renderbuffer_image");
    outExtensions->pixelFormatFloat      = mEGL->hasExtension("EGL_EXT_pixel_format_float");

    outExtensions->glColorspace = mEGL->hasExtension("EGL_KHR_gl_colorspace");
    if (outExtensions->glColorspace)
    {
        outExtensions->glColorspaceDisplayP3Linear =
            mEGL->hasExtension("EGL_EXT_gl_colorspace_display_p3_linear");
        outExtensions->glColorspaceDisplayP3 =
            mEGL->hasExtension("EGL_EXT_gl_colorspace_display_p3");
        outExtensions->glColorspaceScrgb = mEGL->hasExtension("EGL_EXT_gl_colorspace_scrgb");
        outExtensions->glColorspaceScrgbLinear =
            mEGL->hasExtension("EGL_EXT_gl_colorspace_scrgb_linear");
        outExtensions->glColorspaceDisplayP3Passthrough =
            mEGL->hasExtension("EGL_EXT_gl_colorspace_display_p3_passthrough");
        outExtensions->imageGlColorspace = mEGL->hasExtension("EGL_EXT_image_gl_colorspace");
    }

    outExtensions->imageNativeBuffer = mEGL->hasExtension("EGL_ANDROID_image_native_buffer");

    outExtensions->getFrameTimestamps = mEGL->hasExtension("EGL_ANDROID_get_frame_timestamps");

    outExtensions->fenceSync =
        eglVersion >= gl::Version(1, 5) || mEGL->hasExtension("EGL_KHR_fence_sync");
    outExtensions->waitSync =
        eglVersion >= gl::Version(1, 5) || mEGL->hasExtension("EGL_KHR_wait_sync");

    outExtensions->getNativeClientBufferANDROID =
        mEGL->hasExtension("EGL_ANDROID_get_native_client_buffer");

    outExtensions->createNativeClientBufferANDROID =
        mEGL->hasExtension("EGL_ANDROID_create_native_client_buffer");

    outExtensions->nativeFenceSyncANDROID = mEGL->hasExtension("EGL_ANDROID_native_fence_sync");

    outExtensions->noConfigContext = mEGL->hasExtension("EGL_KHR_no_config_context");

    outExtensions->surfacelessContext = mEGL->hasExtension("EGL_KHR_surfaceless_context");

    outExtensions->framebufferTargetANDROID = mEGL->hasExtension("EGL_ANDROID_framebuffer_target");

    outExtensions->imageDmaBufImportEXT = mEGL->hasExtension("EGL_EXT_image_dma_buf_import");

    outExtensions->imageDmaBufImportModifiersEXT =
        mEGL->hasExtension("EGL_EXT_image_dma_buf_import_modifiers");

    outExtensions->robustnessVideoMemoryPurgeNV = mHasNVRobustnessVideoMemoryPurge;

    // Surfaceless can be support if the native driver supports it or we know that we are running on
    // a single thread (mVirtualizedContexts == true)
    outExtensions->surfacelessContext = mSupportsSurfaceless || mVirtualizedContexts;

    outExtensions->externalContextAndSurface = true;

    DisplayGL::generateExtensions(outExtensions);
}

void DisplayEGL::generateCaps(egl::Caps *outCaps) const
{
    outCaps->textureNPOT = true;  // Since we request GLES >= 2
}

void DisplayEGL::setBlobCacheFuncs(EGLSetBlobFuncANDROID set, EGLGetBlobFuncANDROID get)
{
    if (mEGL->hasExtension("EGL_ANDROID_blob_cache"))
    {
        mEGL->setBlobCacheFuncsANDROID(set, get);
    }
}

egl::Error DisplayEGL::makeCurrentSurfaceless(gl::Context *context)
{
    // Nothing to do because EGL always uses the same context and the previous surface can be left
    // current.
    return egl::NoError();
}

egl::Error DisplayEGL::createRenderer(EGLContext shareContext,
                                      bool makeNewContextCurrent,
                                      bool isExternalContext,
                                      std::shared_ptr<RendererEGL> *outRenderer)
{
    EGLContext context = EGL_NO_CONTEXT;
    native_egl::AttributeVector attribs;

    // If isExternalContext is true, the external context is current, so we don't need to make the
    // mMockPbuffer current.
    if (isExternalContext)
    {
        ASSERT(shareContext == EGL_NO_CONTEXT);
        ASSERT(!makeNewContextCurrent);
        // TODO(penghuang): Should we consider creating a share context to avoid querying and
        // restoring GL context state? http://anglebug.com/5509
        context = mEGL->getCurrentContext();
        ASSERT(context != EGL_NO_CONTEXT);
        // TODO(penghuang): get the version from the current context. http://anglebug.com/5509
        attribs = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    }
    else
    {
        ANGLE_TRY(initializeContext(shareContext, mDisplayAttributes, &context, &attribs));
        if (mEGL->makeCurrent(mMockPbuffer, context) == EGL_FALSE)
        {
            return egl::EglNotInitialized()
                   << "eglMakeCurrent failed with " << egl::Error(mEGL->getError());
        }
    }

    std::unique_ptr<FunctionsGL> functionsGL(mEGL->makeFunctionsGL());
    functionsGL->initialize(mDisplayAttributes);

    outRenderer->reset(new RendererEGL(std::move(functionsGL), mDisplayAttributes, this, context,
                                       attribs, isExternalContext));

    CurrentNativeContext &currentContext = mCurrentNativeContexts[std::this_thread::get_id()];
    if (makeNewContextCurrent)
    {
        currentContext.surface = mMockPbuffer;
        currentContext.context = context;
    }
    else if (!isExternalContext)
    {
        // Reset the current context back to the previous state
        if (mEGL->makeCurrent(currentContext.surface, currentContext.context) == EGL_FALSE)
        {
            return egl::EglNotInitialized()
                   << "eglMakeCurrent failed with " << egl::Error(mEGL->getError());
        }
    }

    return egl::NoError();
}

WorkerContext *DisplayEGL::createWorkerContext(std::string *infoLog,
                                               EGLContext sharedContext,
                                               const native_egl::AttributeVector workerAttribs)
{
    EGLContext context = mEGL->createContext(mConfig, sharedContext, workerAttribs.data());
    if (context == EGL_NO_CONTEXT)
    {
        *infoLog += "Unable to create the EGL context.";
        return nullptr;
    }
    return new WorkerContextEGL(context, mEGL, EGL_NO_SURFACE);
}

void DisplayEGL::initializeFrontendFeatures(angle::FrontendFeatures *features) const
{
    mRenderer->initializeFrontendFeatures(features);
}

void DisplayEGL::populateFeatureList(angle::FeatureList *features)
{
    mRenderer->getFeatures().populateFeatureList(features);
}

RendererGL *DisplayEGL::getRenderer() const
{
    return mRenderer.get();
}

egl::Error DisplayEGL::validateImageClientBuffer(const gl::Context *context,
                                                 EGLenum target,
                                                 EGLClientBuffer clientBuffer,
                                                 const egl::AttributeMap &attribs) const
{
    switch (target)
    {
        case EGL_LINUX_DMA_BUF_EXT:
            return egl::NoError();

        default:
            return DisplayGL::validateImageClientBuffer(context, target, clientBuffer, attribs);
    }
}

ExternalImageSiblingImpl *DisplayEGL::createExternalImageSibling(const gl::Context *context,
                                                                 EGLenum target,
                                                                 EGLClientBuffer buffer,
                                                                 const egl::AttributeMap &attribs)
{
    switch (target)
    {
        case EGL_LINUX_DMA_BUF_EXT:
            ASSERT(context == nullptr);
            ASSERT(buffer == nullptr);
            return new DmaBufImageSiblingEGL(attribs);

        default:
            return DisplayGL::createExternalImageSibling(context, target, buffer, attribs);
    }
}

}  // namespace rx
