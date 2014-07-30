/*
 * Copyright (C) 2014 Jefry Tedjokusumo (jtg_gg@yahoo.com.sg)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "platform/SharedBuffer.h"
#include "platform/fonts/SimpleFontData.h"
#include "platform/fonts/Font.h"
#include "platform/graphics/skia/NativeImageSkia.h"
#include "platform/graphics/BitmapImage.h"
#include "BitmapFontData.h"
#include "SkGlyph.h"
#include "SkPixelRef.h"
#include "SkTypefaceCache.h"
#include "skia/ext/image_operations.h"
#include <stdio.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

namespace WebCore {
  
void inline getFileName(SkUnichar unicode, char fname[])
{
    sprintf(fname, "%s/%x.png", BitmapFontData::getFontFolder(), unicode);
}
  
// ============================== BitmapScalerContext ==============================
  
UChar BitmapScalerContext::getu16lead() const
{
    const BitmapTypeface* typeface = static_cast<const BitmapTypeface*>(this->getTypeface());
    return typeface->getu16lead();
}

unsigned BitmapScalerContext::generateGlyphCount()
{
    return 0xFFffFFff;
}

uint16_t BitmapScalerContext::generateCharToGlyph(SkUnichar uni)
{
    return 0;
}

SkUnichar BitmapScalerContext::generateGlyphToChar(uint16_t trail)
{
    const uint16_t u16Lead = getu16lead();
    if (u16Lead)
        return U16_GET_SUPPLEMENTARY(u16Lead, trail);
    else
        return trail;
}

void BitmapScalerContext::generateAdvance(SkGlyph* glyph)
{
    generateMetrics(glyph);
}

void BitmapScalerContext::generateMetrics(SkGlyph* glyph)
{
    const float textWidth = fRec.fTextSize * fRec.fPost2x2[0][0];
    const float textHeight = fRec.fTextSize * fRec.fPost2x2[1][1];
  
    glyph->fTop = -textHeight*0.875f;
    glyph->fLeft = 0;
    glyph->fWidth = textWidth;
    glyph->fHeight = textHeight;
    glyph->fMaskFormat = SkMask::kARGB32_Format;
    glyph->fAdvanceX = SkIntToFixed(0);
    glyph->fAdvanceY = SkIntToFixed(0);
    glyph->fLsbDelta = 0;
    glyph->fRsbDelta = 0;
}

void generateImageFail(const SkGlyph& glyph, SkUnichar unicode)
{
    int* imageBuf = static_cast<int*>(glyph.fImage);
    for (int y = 0; y<glyph.fHeight; y++)
        for (int x = 0; x<glyph.fWidth; x++) {
            const int index = y*glyph.fWidth + x;
            // pixel format ARGB
            imageBuf[index] = 0xFF000000 | unicode;
        }
}

void BitmapScalerContext::generateImage(const SkGlyph& glyph)
{
    SkUnichar unicode = glyphIDToChar(glyph.getGlyphID());
    RefPtr<SharedBuffer> resource = SharedBuffer::create();
  
    char fname[PATH_MAX];
    getFileName(unicode, fname);
    FILE* f = fopen(fname, "rb");
  
    if (f == NULL) {
        generateImageFail(glyph, unicode);
        return;
    }
  
    char buffer[10240];
    size_t bufferRead;
    do {
        bufferRead = fread(&buffer, sizeof(char), 10240, f);
        resource->append(buffer, bufferRead);
    } while (bufferRead == 10240);
  
    if (f) fclose(f); f = NULL;
    RefPtr<BitmapImage> image = BitmapImage::create();
    image->setData(resource, true);
  
    const SkBitmap& bitmap = image->nativeImageForCurrentFrame()->bitmap();
    SkBitmap dest = skia::ImageOperations::Resize(bitmap, skia::ImageOperations::RESIZE_GOOD, glyph.fWidth, glyph.fHeight);
  
    if (glyph.computeImageSize() == dest.getSize())
        memcpy(glyph.fImage, dest.pixelRef()->pixels(), glyph.computeImageSize());
    else
        generateImageFail(glyph, 0);
  
}

void BitmapScalerContext::generatePath(const SkGlyph& glyph, SkPath* path)
{
}

void BitmapScalerContext::generateFontMetrics(SkPaint::FontMetrics* mx,
                                              SkPaint::FontMetrics* my)
{
  
    const float textWidth = fRec.fTextSize * fRec.fPost2x2[0][0];
    const float textHeight = fRec.fTextSize * fRec.fPost2x2[1][1];
  
    SkPaint::FontMetrics theMetrics;
  
    theMetrics.fTop = -textHeight;
    theMetrics.fAscent = 0;
    theMetrics.fDescent = 0;
    theMetrics.fBottom = 0;
    theMetrics.fLeading = 0;
    theMetrics.fAvgCharWidth = textWidth;
    theMetrics.fXMin = 0;
    theMetrics.fXMax = textWidth;
    theMetrics.fXHeight = textHeight;
  
    if (mx != NULL) {
        *mx = theMetrics;
    }
    if (my != NULL) {
        *my = theMetrics;
    }
}

BitmapScalerContext::BitmapScalerContext(SkTypeface* typeface, const SkDescriptor* desc) :
SkScalerContext(typeface, desc)
{
}

BitmapScalerContext::~BitmapScalerContext()
{
}

// ============================== BitmapTypeface ==============================

BitmapTypeface::TypefaceMap* BitmapTypeface::m_typefaceMap = NULL;

SkScalerContext* BitmapTypeface::onCreateScalerContext(const SkDescriptor* desc) const
{
    return new BitmapScalerContext(const_cast<BitmapTypeface*>(this), desc);
}

void BitmapTypeface::onFilterRec(SkScalerContextRec* rec) const
{
}

SkAdvancedTypefaceMetrics* BitmapTypeface::onGetAdvancedTypefaceMetrics(
    SkAdvancedTypefaceMetrics::PerGlyphInfo perGlyphInfo,
    const uint32_t* glyphIDs,
    uint32_t glyphIDsCount) const
{
    return 0;
}

SkStream* BitmapTypeface::onOpenStream(int* ttcIndex) const
{
    return 0;
}

void BitmapTypeface::onGetFontDescriptor(SkFontDescriptor*, bool* isLocal) const
{
}

int BitmapTypeface::onCharsToGlyphs(const void* chars, Encoding, uint16_t glyphs[],
    int glyphCount) const
{
    return 0;
}

int BitmapTypeface::onCountGlyphs() const
{
    return 0;
}

int BitmapTypeface::onGetUPEM() const
{
    return 0;
}

BitmapTypeface::LocalizedStrings* BitmapTypeface::onCreateFamilyNameIterator() const
{
    return 0;
}

int BitmapTypeface::onGetTableTags(SkFontTableTag tags[]) const
{
    return 0;
}

size_t BitmapTypeface::onGetTableData(SkFontTableTag, size_t offset,
    size_t length, void* data) const
{
    return 0;
}

BitmapTypeface::BitmapTypeface(UChar u16lead) :
    SkTypeface(kNormal, SkTypefaceCache::NewFontID()), m_u16lead(u16lead)
{
}

BitmapTypeface::~BitmapTypeface()
{
    m_typefaceMap->erase(m_u16lead);
}

SkTypeface* BitmapTypeface::get(UChar u16lead, bool& isNew)
{
    if (m_typefaceMap == NULL)
        m_typefaceMap = new BitmapTypeface::TypefaceMap();
  
    isNew = false;
    TypefaceMap::iterator i = m_typefaceMap->find(u16lead);
    if (i != m_typefaceMap->end())
      return i->second;
  
    SkTypeface* bitmapTypeface = new BitmapTypeface(u16lead);
    m_typefaceMap->operator[](u16lead) = bitmapTypeface;
    isNew = true;
    return bitmapTypeface;
}

// ============================== BitmapFontData ==============================

BitmapFontData::UnicodeToFontDataMap* BitmapFontData::ms_unicodeToFontData = NULL;
BitmapFontData::HasGlyphCache* BitmapFontData::ms_hasGlyphCache = NULL;
char BitmapFontData::ms_fontFolder[PATH_MAX] = {0};
  
bool BitmapFontData::init(const char* folder)
{
    if (ms_hasGlyphCache == NULL && folder) {
        ms_hasGlyphCache = new HasGlyphCache();
        memcpy(ms_fontFolder, folder, sizeof(ms_fontFolder));
        return true;
    }
    return false;
}
  
bool BitmapFontData::hasGlyph(UChar32 unicode)
{
    HasGlyphCache::iterator i = ms_hasGlyphCache->find(unicode);
    if (i != ms_hasGlyphCache->end())
        return i->second;
  
    char fname[PATH_MAX];
    getFileName(unicode, fname);
    FILE* f = fopen(fname, "rb");
  
    ms_hasGlyphCache->operator[](unicode) = f!=NULL;
  
    if (f) fclose(f);
    return ms_hasGlyphCache->operator[](unicode);
}

PassRefPtr<SimpleFontData> BitmapFontData::get(const Font& font, UChar32 unicode)
{
    // not initalize, return 0
    if(ms_fontFolder[0] == 0 || ms_hasGlyphCache == NULL)
        return nullptr;
  
    if (ms_unicodeToFontData == NULL)
        ms_unicodeToFontData = new UnicodeToFontDataMap();
  
    if (!hasGlyph(unicode)) return nullptr;
  
    const float fontSize = font.fontDescription().computedSize();

    const UChar u16lead = U_IS_BMP(unicode) ? 0 : U16_LEAD(unicode);
    UnicodeToFontDataMap::iterator i = ms_unicodeToFontData->find(u16lead);
    if (i == ms_unicodeToFontData->end()) {
        RefPtr<SimpleFontData> fontData = SimpleFontData::create(BitmapFontData::create(u16lead), fontSize, false, false);
        fontData->setMissingGlyphData(font.primaryFont()->missingGlyphData());
        (ms_unicodeToFontData->operator[](u16lead))[fontSize] = fontData;
        return fontData;
    }
  
    UnicodeToFontDataMap::mapped_type::iterator j = i->second.find(fontSize);
    if (j == i->second.end()) {
        RefPtr<SimpleFontData> fontData = SimpleFontData::create(BitmapFontData::create(u16lead), fontSize, false, false);
        fontData->setMissingGlyphData(font.primaryFont()->missingGlyphData());
        i->second[fontSize] = fontData;
        return fontData;
    }
  
    return j->second;
}
  
bool BitmapFontData::clear(UChar u16lead, float textSize)
{
    UnicodeToFontDataMap::iterator i = ms_unicodeToFontData->find(u16lead);
    if (i != ms_unicodeToFontData->end()) {
        i->second.erase(textSize);
      
        if (i->second.size() == 0) {
            ms_unicodeToFontData->erase(i);
        }
        return true;
    }
  
    return false;
}

BitmapFontData::BitmapFontData(UChar u16lead)
{
    bool isNew;
    m_typeface = BitmapTypeface::get(u16lead, isNew);
    if (isNew) m_typeface->deref();
}

BitmapFontData::~BitmapFontData()
{
}


void BitmapFontData::initializeFontData(SimpleFontData* fontData, float fontSize)
{
}

float BitmapFontData::widthForGlyph(Glyph glyph, float fontSize) const
{
    return fontSize;
}

bool BitmapFontData::fillGlyphPage(GlyphPage* pageToFill, unsigned offset, unsigned length, UChar* buffer, unsigned bufferLength, const SimpleFontData* fontData) const
{
    unsigned allGlyphs = 0; // track if any of the glyphIDs are non-zero
    const bool singleBuffer = length == bufferLength;
    for (unsigned i = 0; i < length; i++) {
        if (singleBuffer)
            pageToFill->setGlyphDataForIndex(offset + i, buffer[i], fontData);
        else
            pageToFill->setGlyphDataForIndex(offset + i, buffer[i * 2 + 1], fontData);
        allGlyphs |= buffer[i];
    }
  
    return allGlyphs != 0;
}

bool BitmapFontData::applyGlyphSelection(WidthIterator&, GlyphData&, bool mirror, int currentCharacter, unsigned& advanceLength) const
{
    return true;
}
  
} //namespace WebCore
