/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeckoMediaSourceBuffer.h"

#include "GeckoMediaMacros.h"
#include "SourceBuffer.h"
#include "RefPtr.h"

using mozilla::dom::SourceBuffer;

struct GeckoMediaSourceBuffer
{
  GeckoMediaSourceBuffer(size_t aId,
                         GeckoMediaSourceBufferImpl aImpl,
                         size_t aParentId,
                         const char* aMimeType)
    : mSourceBuffer(new SourceBuffer(aImpl, aParentId, aMimeType))
    , mId(aId)
  {
  }

  RefPtr<SourceBuffer> mSourceBuffer;
  const size_t mId;
};

DEF_GECKO_MEDIA_REFLECTOR_CONTAINER(GeckoMediaSourceBuffer)
IMPL_GECKO_MEDIA_REFLECTOR_GETTER(GeckoMediaSourceBuffer)
IMPL_GECKO_MEDIA_REFLECTOR_SHUTDOWN(GeckoMediaSourceBuffer, SourceBuffer)

void
GeckoMedia_SourceBuffer_Create(size_t aId,
                               GeckoMediaSourceBufferImpl aImpl,
                               size_t aParentId,
                               const char* aMimeType)
{
  GeckoMediaSourceBuffer* reflector =
    sReflectors.AppendElement(GeckoMediaSourceBuffer(aId, aImpl, aParentId, aMimeType));
  MOZ_ASSERT(GetReflector(aId) == reflector);
}

SourceBuffer*
GetSourceBuffer(const size_t aId)
{
  GeckoMediaSourceBuffer* reflector = GetReflector(aId);
  if (NS_WARN_IF(!reflector)) {
    return nullptr;
  }
  return reflector->mSourceBuffer;
}
