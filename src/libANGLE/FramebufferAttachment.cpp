//
// Copyright (c) 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// FramebufferAttachment.cpp: the gl::FramebufferAttachment class and its derived classes
// objects and related functionality. [OpenGL ES 2.0.24] section 4.4.3 page 108.

#include "libANGLE/FramebufferAttachment.h"

#include "common/utilities.h"
#include "libANGLE/Config.h"
#include "libANGLE/Renderbuffer.h"
#include "libANGLE/Surface.h"
#include "libANGLE/Texture.h"
#include "libANGLE/formatutils.h"
#include "libANGLE/renderer/FramebufferImpl.h"
#include "libANGLE/renderer/FramebufferAttachmentObjectImpl.h"

namespace gl
{

////// FramebufferAttachment::Target Implementation //////

FramebufferAttachment::Target::Target()
    : mBinding(GL_NONE),
      mTextureIndex(ImageIndex::MakeInvalid())
{
}

FramebufferAttachment::Target::Target(GLenum binding, const ImageIndex &imageIndex)
    : mBinding(binding),
      mTextureIndex(imageIndex)
{
}

FramebufferAttachment::Target::Target(const Target &other)
    : mBinding(other.mBinding),
      mTextureIndex(other.mTextureIndex)
{
}

FramebufferAttachment::Target &FramebufferAttachment::Target::operator=(const Target &other)
{
    this->mBinding = other.mBinding;
    this->mTextureIndex = other.mTextureIndex;
    return *this;
}

////// FramebufferAttachment Implementation //////

FramebufferAttachment::FramebufferAttachment()
    : mType(GL_NONE),
      mResource(nullptr),
      mNumViews(1),
      mMultiviewLayout(GL_NONE),
      mBaseViewIndex(0),
      mViewportOffsets(1u)
{
}

FramebufferAttachment::FramebufferAttachment(const Context *context,
                                             GLenum type,
                                             GLenum binding,
                                             const ImageIndex &textureIndex,
                                             FramebufferAttachmentObject *resource)
    : mResource(nullptr),
      mNumViews(1),
      mMultiviewLayout(GL_NONE),
      mBaseViewIndex(0),
      mViewportOffsets(1u)
{
    attach(context, type, binding, textureIndex, resource);
}

FramebufferAttachment::FramebufferAttachment(FramebufferAttachment &&other)
    : FramebufferAttachment()
{
    *this = std::move(other);
}

FramebufferAttachment &FramebufferAttachment::operator=(FramebufferAttachment &&other)
{
    std::swap(mType, other.mType);
    std::swap(mTarget, other.mTarget);
    std::swap(mResource, other.mResource);
    std::swap(mNumViews, other.mNumViews);
    std::swap(mMultiviewLayout, other.mMultiviewLayout);
    std::swap(mBaseViewIndex, other.mBaseViewIndex);
    std::swap(mViewportOffsets, other.mViewportOffsets);
    return *this;
}

FramebufferAttachment::~FramebufferAttachment()
{
    ASSERT(!isAttached());
}

void FramebufferAttachment::detach(const Context *context)
{
    mType = GL_NONE;
    if (mResource != nullptr)
    {
        mResource->onDetach(context);
        mResource = nullptr;
    }
    mNumViews        = 1;
    mMultiviewLayout = GL_NONE;
    mBaseViewIndex   = 0;
    mViewportOffsets.resize(1u);
    mViewportOffsets[0] = Offset();

    // not technically necessary, could omit for performance
    mTarget = Target();
}

void FramebufferAttachment::attach(const Context *context,
                                   GLenum type,
                                   GLenum binding,
                                   const ImageIndex &textureIndex,
                                   FramebufferAttachmentObject *resource)
{
    if (resource == nullptr)
    {
        detach(context);
        return;
    }

    mType = type;
    mTarget = Target(binding, textureIndex);
    resource->onAttach(context);

    if (mResource != nullptr)
    {
        mResource->onDetach(context);
    }

    mResource = resource;
}

GLuint FramebufferAttachment::getRedSize() const
{
    return getFormat().info->redBits;
}

GLuint FramebufferAttachment::getGreenSize() const
{
    return getFormat().info->greenBits;
}

GLuint FramebufferAttachment::getBlueSize() const
{
    return getFormat().info->blueBits;
}

GLuint FramebufferAttachment::getAlphaSize() const
{
    return getFormat().info->alphaBits;
}

GLuint FramebufferAttachment::getDepthSize() const
{
    return getFormat().info->depthBits;
}

GLuint FramebufferAttachment::getStencilSize() const
{
    return getFormat().info->stencilBits;
}

GLenum FramebufferAttachment::getComponentType() const
{
    return getFormat().info->componentType;
}

GLenum FramebufferAttachment::getColorEncoding() const
{
    return getFormat().info->colorEncoding;
}

GLuint FramebufferAttachment::id() const
{
    return mResource->getId();
}

const ImageIndex &FramebufferAttachment::getTextureImageIndex() const
{
    ASSERT(type() == GL_TEXTURE);
    return mTarget.textureIndex();
}

GLenum FramebufferAttachment::cubeMapFace() const
{
    ASSERT(mType == GL_TEXTURE);

    const auto &index = mTarget.textureIndex();
    return IsCubeMapTextureTarget(index.type) ? index.type : GL_NONE;
}

GLint FramebufferAttachment::mipLevel() const
{
    ASSERT(type() == GL_TEXTURE);
    return mTarget.textureIndex().mipIndex;
}

GLint FramebufferAttachment::layer() const
{
    ASSERT(mType == GL_TEXTURE);

    const auto &index = mTarget.textureIndex();

    if (index.type == GL_TEXTURE_2D_ARRAY || index.type == GL_TEXTURE_3D)
    {
        return index.layerIndex;
    }
    return 0;
}

GLint FramebufferAttachment::getNumViews() const
{
    return mNumViews;
}

GLenum FramebufferAttachment::getMultiviewLayout() const
{
    return mMultiviewLayout;
}

GLint FramebufferAttachment::getBaseViewIndex() const
{
    return mBaseViewIndex;
}

const std::vector<Offset> &FramebufferAttachment::getMultiviewViewportOffsets() const
{
    return mViewportOffsets;
}

Texture *FramebufferAttachment::getTexture() const
{
    return rx::GetAs<Texture>(mResource);
}

Renderbuffer *FramebufferAttachment::getRenderbuffer() const
{
    return rx::GetAs<Renderbuffer>(mResource);
}

const egl::Surface *FramebufferAttachment::getSurface() const
{
    return rx::GetAs<egl::Surface>(mResource);
}

FramebufferAttachmentObject *FramebufferAttachment::getResource() const
{
    return mResource;
}

bool FramebufferAttachment::operator==(const FramebufferAttachment &other) const
{
    if (mResource != other.mResource || mType != other.mType || mNumViews != other.mNumViews ||
        mMultiviewLayout != other.mMultiviewLayout || mBaseViewIndex != other.mBaseViewIndex ||
        mViewportOffsets != other.mViewportOffsets)
    {
        return false;
    }

    if (mType == GL_TEXTURE && getTextureImageIndex() != other.getTextureImageIndex())
    {
        return false;
    }

    return true;
}

bool FramebufferAttachment::operator!=(const FramebufferAttachment &other) const
{
    return !(*this == other);
}

Error FramebufferAttachmentObject::getAttachmentRenderTarget(
    const Context *context,
    GLenum binding,
    const ImageIndex &imageIndex,
    rx::FramebufferAttachmentRenderTarget **rtOut) const
{
    return getAttachmentImpl()->getAttachmentRenderTarget(context, binding, imageIndex, rtOut);
}

angle::BroadcastChannel<> *FramebufferAttachmentObject::getDirtyChannel()
{
    return &mDirtyChannel;
}

}  // namespace gl
