/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <utils/JenkinsHash.h>

#include "Caches.h"
#include "Debug.h"
#include "GradientCache.h"
#include "Properties.h"

#include <cutils/properties.h>

namespace android {
namespace uirenderer {

///////////////////////////////////////////////////////////////////////////////
// Functions
///////////////////////////////////////////////////////////////////////////////

template<typename T>
static inline T min(T a, T b) {
    return a < b ? a : b;
}

///////////////////////////////////////////////////////////////////////////////
// Cache entry
///////////////////////////////////////////////////////////////////////////////

hash_t GradientCacheEntry::hash() const {
    uint32_t hash = JenkinsHashMix(0, count);
    for (uint32_t i = 0; i < count; i++) {
        hash = JenkinsHashMix(hash, android::hash_type(colors[i]));
        hash = JenkinsHashMix(hash, android::hash_type(positions[i]));
    }
    return JenkinsHashWhiten(hash);
}

int GradientCacheEntry::compare(const GradientCacheEntry& lhs, const GradientCacheEntry& rhs) {
    int deltaInt = int(lhs.count) - int(rhs.count);
    if (deltaInt != 0) return deltaInt;

    deltaInt = memcmp(lhs.colors.get(), rhs.colors.get(), lhs.count * sizeof(uint32_t));
    if (deltaInt != 0) return deltaInt;

    return memcmp(lhs.positions.get(), rhs.positions.get(), lhs.count * sizeof(float));
}

///////////////////////////////////////////////////////////////////////////////
// Constructors/destructor
///////////////////////////////////////////////////////////////////////////////

GradientCache::GradientCache(Extensions& extensions)
        : mCache(LruCache<GradientCacheEntry, Texture*>::kUnlimitedCapacity)
        , mSize(0)
        , mMaxSize(Properties::gradientCacheSize)
        , mUseFloatTexture(extensions.hasFloatTextures())
        , mHasNpot(extensions.hasNPot()){
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mMaxTextureSize);

    mCache.setOnEntryRemovedListener(this);
}

GradientCache::~GradientCache() {
    mCache.clear();
}

///////////////////////////////////////////////////////////////////////////////
// Size management
///////////////////////////////////////////////////////////////////////////////

uint32_t GradientCache::getSize() {
    return mSize;
}

uint32_t GradientCache::getMaxSize() {
    return mMaxSize;
}

///////////////////////////////////////////////////////////////////////////////
// Callbacks
///////////////////////////////////////////////////////////////////////////////

void GradientCache::operator()(GradientCacheEntry&, Texture*& texture) {
    if (texture) {
        mSize -= texture->objectSize();
        texture->deleteTexture();
        delete texture;
    }
}

///////////////////////////////////////////////////////////////////////////////
// Caching
///////////////////////////////////////////////////////////////////////////////

Texture* GradientCache::get(uint32_t* colors, float* positions, int count) {
    GradientCacheEntry gradient(colors, positions, count);
    Texture* texture = mCache.get(gradient);

    if (!texture) {
        texture = addLinearGradient(gradient, colors, positions, count);
    }

    return texture;
}

void GradientCache::clear() {
    mCache.clear();
}

void GradientCache::getGradientInfo(const uint32_t* colors, const int count,
        GradientInfo& info) {
    uint32_t width = 256 * (count - 1);

    // If the npot extension is not supported we cannot use non-clamp
    // wrap modes. We therefore find the nearest largest power of 2
    // unless width is already a power of 2
    if (!mHasNpot && (width & (width - 1)) != 0) {
        width = 1 << (32 - __builtin_clz(width));
    }

    bool hasAlpha = false;
    for (int i = 0; i < count; i++) {
        if (((colors[i] >> 24) & 0xff) < 255) {
            hasAlpha = true;
            break;
        }
    }

    info.width = min(width, uint32_t(mMaxTextureSize));
    info.hasAlpha = hasAlpha;
}

Texture* GradientCache::addLinearGradient(GradientCacheEntry& gradient,
        uint32_t* colors, float* positions, int count) {

    GradientInfo info;
    getGradientInfo(colors, count, info);

    Texture* texture = new Texture(Caches::getInstance());
    texture->blend = info.hasAlpha;
    texture->generation = 1;

    // Assume the cache is always big enough
    const uint32_t size = info.width * 2 * bytesPerPixel();
    while (getSize() + size > mMaxSize) {
        LOG_ALWAYS_FATAL_IF(!mCache.removeOldest(),
                "Ran out of things to remove from the cache? getSize() = %" PRIu32
                ", size = %" PRIu32 ", mMaxSize = %" PRIu32 ", width = %" PRIu32,
                getSize(), size, mMaxSize, info.width);
    }

    generateTexture(colors, positions, info.width, 2, texture);

    mSize += size;
    mCache.put(gradient, texture);

    return texture;
}

size_t GradientCache::bytesPerPixel() const {
    // We use 4 channels (RGBA)
    return 4 * (mUseFloatTexture ? sizeof(float) : sizeof(uint8_t));
}

void GradientCache::splitToBytes(uint32_t inColor, GradientColor& outColor) const {
    outColor.r = (inColor >> 16) & 0xff;
    outColor.g = (inColor >>  8) & 0xff;
    outColor.b = (inColor >>  0) & 0xff;
    outColor.a = (inColor >> 24) & 0xff;
}

void GradientCache::splitToFloats(uint32_t inColor, GradientColor& outColor) const {
    outColor.r = ((inColor >> 16) & 0xff) / 255.0f;
    outColor.g = ((inColor >>  8) & 0xff) / 255.0f;
    outColor.b = ((inColor >>  0) & 0xff) / 255.0f;
    outColor.a = ((inColor >> 24) & 0xff) / 255.0f;
}

void GradientCache::mixBytes(GradientColor& start, GradientColor& end, float amount,
        uint8_t*& dst) const {
    float oppAmount = 1.0f - amount;
    const float alpha = start.a * oppAmount + end.a * amount;
    const float a = alpha / 255.0f;

    *dst++ = uint8_t(a * (start.r * oppAmount + end.r * amount));
    *dst++ = uint8_t(a * (start.g * oppAmount + end.g * amount));
    *dst++ = uint8_t(a * (start.b * oppAmount + end.b * amount));
    *dst++ = uint8_t(alpha);
}

void GradientCache::mixFloats(GradientColor& start, GradientColor& end, float amount,
        uint8_t*& dst) const {
    float oppAmount = 1.0f - amount;
    const float a = start.a * oppAmount + end.a * amount;

    float* d = (float*) dst;
    *d++ = a * (start.r * oppAmount + end.r * amount);
    *d++ = a * (start.g * oppAmount + end.g * amount);
    *d++ = a * (start.b * oppAmount + end.b * amount);
    *d++ = a;

    dst += 4 * sizeof(float);
}

void GradientCache::generateTexture(uint32_t* colors, float* positions,
        const uint32_t width, const uint32_t height, Texture* texture) {
    const GLsizei rowBytes = width * bytesPerPixel();
    uint8_t pixels[rowBytes * height];

    static ChannelSplitter gSplitters[] = {
            &android::uirenderer::GradientCache::splitToBytes,
            &android::uirenderer::GradientCache::splitToFloats,
    };
    ChannelSplitter split = gSplitters[mUseFloatTexture];

    static ChannelMixer gMixers[] = {
            &android::uirenderer::GradientCache::mixBytes,
            &android::uirenderer::GradientCache::mixFloats,
    };
    ChannelMixer mix = gMixers[mUseFloatTexture];

    GradientColor start;
    (this->*split)(colors[0], start);

    GradientColor end;
    (this->*split)(colors[1], end);

    int currentPos = 1;
    float startPos = positions[0];
    float distance = positions[1] - startPos;

    uint8_t* dst = pixels;
    for (uint32_t x = 0; x < width; x++) {
        float pos = x / float(width - 1);
        if (pos > positions[currentPos]) {
            start = end;
            startPos = positions[currentPos];

            currentPos++;

            (this->*split)(colors[currentPos], end);
            distance = positions[currentPos] - startPos;
        }

        float amount = (pos - startPos) / distance;
        (this->*mix)(start, end, amount, dst);
    }

    memcpy(pixels + rowBytes, pixels, rowBytes);

    if (mUseFloatTexture) {
        // We have to use GL_RGBA16F because GL_RGBA32F does not support filtering
        texture->upload(GL_RGBA16F, width, height, GL_RGBA, GL_FLOAT, pixels);
    } else {
        texture->upload(GL_RGBA, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    texture->setFilter(GL_LINEAR);
    texture->setWrap(GL_CLAMP_TO_EDGE);
}

}; // namespace uirenderer
}; // namespace android
