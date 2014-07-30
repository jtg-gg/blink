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

#ifndef BitmapFontData_h
#define BitmapFontData_h

#include "SkScalerContext.h"
#include "SkTypeface.h"
#include "platform/PlatformExport.h"
#include "platform/fonts/Glyph.h"
#include "wtf/PassRefPtr.h"
#include "wtf/RefCounted.h"
#include <map>

namespace WebCore {
  
class Font;

class PLATFORM_EXPORT BitmapFontData : public RefCounted<BitmapFontData> {
  
    SkRefPtr<SkTypeface>  m_typeface;
    BitmapFontData(UChar u16lead);
  
    typedef std::map< UChar, std::map< float, RefPtr<WebCore::SimpleFontData> > > UnicodeToFontDataMap;
    static  UnicodeToFontDataMap* ms_unicodeToFontData;
  
    typedef std::map<UChar32, bool> HasGlyphCache;
    static HasGlyphCache* ms_hasGlyphCache;
  
    static char ms_fontFolder[];
  
    static bool hasGlyph(UChar32 unicode);
  
    static PassRefPtr<BitmapFontData> create(UChar u16lead)
    {
        return adoptRef(new BitmapFontData(u16lead));
    }
  
public:
    virtual ~BitmapFontData();
    static PassRefPtr<SimpleFontData> get(const Font& font, UChar32 unicode);
    static bool clear(UChar, float);
    static const char* getFontFolder() { return ms_fontFolder; }
    static bool init(const char* folder);
  
    void initializeFontData(SimpleFontData*, float fontSize);
    float widthForGlyph(Glyph, float fontSize) const;
    bool fillGlyphPage(GlyphPage*, unsigned offset, unsigned length, UChar* buffer, unsigned bufferLength, const SimpleFontData*) const;
    bool applyGlyphSelection(WidthIterator&, GlyphData&, bool mirror, int currentCharacter, unsigned& advanceLength) const;
  
    SkTypeface* typeface() const { return m_typeface.get(); }
};

class BitmapScalerContext : public SkScalerContext {
    UChar getu16lead() const;
  
    virtual unsigned generateGlyphCount() SK_OVERRIDE;
    virtual uint16_t generateCharToGlyph(SkUnichar uni) SK_OVERRIDE;
    virtual SkUnichar generateGlyphToChar(uint16_t trail) SK_OVERRIDE;
    virtual void generateAdvance(SkGlyph* glyph) SK_OVERRIDE;
    virtual void generateMetrics(SkGlyph* glyph) SK_OVERRIDE;
    virtual void generateImage(const SkGlyph& glyph) SK_OVERRIDE;
    virtual void generatePath(const SkGlyph& glyph, SkPath* path) SK_OVERRIDE;
    virtual void generateFontMetrics(SkPaint::FontMetrics* mx,
                                   SkPaint::FontMetrics* my) SK_OVERRIDE;
  
public:
    BitmapScalerContext(SkTypeface* typeface, const SkDescriptor* desc);
    virtual ~BitmapScalerContext();
};

class BitmapTypeface : public SkTypeface {
    const UChar m_u16lead;
  
    virtual SkScalerContext* onCreateScalerContext(const SkDescriptor* desc) const SK_OVERRIDE;
    virtual void onFilterRec(SkScalerContextRec* rec) const SK_OVERRIDE;
  
    virtual SkAdvancedTypefaceMetrics* onGetAdvancedTypefaceMetrics(
        SkAdvancedTypefaceMetrics::PerGlyphInfo perGlyphInfo,
        const uint32_t* glyphIDs,
        uint32_t glyphIDsCount) const SK_OVERRIDE;
  
    virtual SkStream* onOpenStream(int* ttcIndex) const SK_OVERRIDE;
    virtual void onGetFontDescriptor(SkFontDescriptor*, bool* isLocal) const SK_OVERRIDE;
  
    virtual int onCharsToGlyphs(const void* chars, Encoding, uint16_t glyphs[],
        int glyphCount) const SK_OVERRIDE;
  
    virtual int onCountGlyphs() const SK_OVERRIDE;
    virtual int onGetUPEM() const SK_OVERRIDE;
    virtual LocalizedStrings* onCreateFamilyNameIterator() const SK_OVERRIDE;
    virtual int onGetTableTags(SkFontTableTag tags[]) const SK_OVERRIDE;
    virtual size_t onGetTableData(SkFontTableTag, size_t offset,
        size_t length, void* data) const SK_OVERRIDE;
  
    BitmapTypeface(UChar u16lead);
  
    typedef std::map<UChar, SkTypeface*> TypefaceMap;
    static  TypefaceMap* m_typefaceMap;
  
public:
  
    static SkTypeface* get(UChar u16lead, bool& isNew);
    virtual ~BitmapTypeface();
  
    UChar getu16lead() const { return m_u16lead; }
  
};
} //namespace WebCore
#endif //BitmapFontData_h
