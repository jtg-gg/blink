/*
 * Copyright (C) 2012 Apple Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "core/rendering/RenderMultiColumnSet.h"

#include "core/paint/MultiColumnSetPainter.h"
#include "core/rendering/RenderMultiColumnFlowThread.h"

namespace blink {

RenderMultiColumnSet::RenderMultiColumnSet(RenderFlowThread* flowThread)
    : RenderRegion(0, flowThread)
    , m_columnHeight(0)
    , m_maxColumnHeight(RenderFlowThread::maxLogicalHeight())
    , m_minSpaceShortage(RenderFlowThread::maxLogicalHeight())
    , m_minimumColumnHeight(0)
{
}

RenderMultiColumnSet* RenderMultiColumnSet::createAnonymous(RenderFlowThread* flowThread, RenderStyle* parentStyle)
{
    Document& document = flowThread->document();
    RenderMultiColumnSet* renderer = new RenderMultiColumnSet(flowThread);
    renderer->setDocumentForAnonymous(&document);
    renderer->setStyle(RenderStyle::createAnonymousStyleWithDisplay(parentStyle, BLOCK));
    return renderer;
}

RenderMultiColumnSet* RenderMultiColumnSet::nextSiblingMultiColumnSet() const
{
    for (RenderObject* sibling = nextSibling(); sibling; sibling = sibling->nextSibling()) {
        if (sibling->isRenderMultiColumnSet())
            return toRenderMultiColumnSet(sibling);
    }
    return 0;
}

RenderMultiColumnSet* RenderMultiColumnSet::previousSiblingMultiColumnSet() const
{
    for (RenderObject* sibling = previousSibling(); sibling; sibling = sibling->previousSibling()) {
        if (sibling->isRenderMultiColumnSet())
            return toRenderMultiColumnSet(sibling);
    }
    return 0;
}

LayoutSize RenderMultiColumnSet::flowThreadTranslationAtOffset(LayoutUnit blockOffset) const
{
    unsigned columnIndex = columnIndexAtOffset(blockOffset);
    LayoutRect portionRect(flowThreadPortionRectAt(columnIndex));
    flipForWritingMode(portionRect);
    LayoutRect columnRect(columnRectAt(columnIndex));
    flipForWritingMode(columnRect);
    return toLayoutPoint(contentBoxOffset()) + columnRect.location() - portionRect.location();
}

LayoutUnit RenderMultiColumnSet::heightAdjustedForSetOffset(LayoutUnit height) const
{
    // Adjust for the top offset within the content box of the multicol container (containing
    // block), unless this is the first set. We know that the top offset for the first set will be
    // zero, but if the multicol container has non-zero top border or padding, the set's top offset
    // (initially being 0 and relative to the border box) will be negative until it has been laid
    // out. Had we used this bogus offset, we would calculate the wrong height, and risk performing
    // a wasted layout iteration. Of course all other sets (if any) have this problem in the first
    // layout pass too, but there's really nothing we can do there until the flow thread has been
    // laid out anyway.
    if (previousSiblingMultiColumnSet()) {
        RenderBlockFlow* multicolBlock = multiColumnBlockFlow();
        LayoutUnit contentLogicalTop = logicalTop() - multicolBlock->borderAndPaddingBefore();
        height -= contentLogicalTop;
    }
    return max(height, LayoutUnit(1)); // Let's avoid zero height, as that would probably cause an infinite amount of columns to be created.
}

LayoutUnit RenderMultiColumnSet::pageLogicalTopForOffset(LayoutUnit offset) const
{
    unsigned columnIndex = columnIndexAtOffset(offset, AssumeNewColumns);
    return logicalTopInFlowThread() + columnIndex * pageLogicalHeight();
}

void RenderMultiColumnSet::setAndConstrainColumnHeight(LayoutUnit newHeight)
{
    m_columnHeight = newHeight;
    if (m_columnHeight > m_maxColumnHeight)
        m_columnHeight = m_maxColumnHeight;
    // FIXME: the height may also be affected by the enclosing pagination context, if any.
}

unsigned RenderMultiColumnSet::findRunWithTallestColumns() const
{
    unsigned indexWithLargestHeight = 0;
    LayoutUnit largestHeight;
    LayoutUnit previousOffset = logicalTopInFlowThread();
    size_t runCount = m_contentRuns.size();
    ASSERT(runCount);
    for (size_t i = 0; i < runCount; i++) {
        const ContentRun& run = m_contentRuns[i];
        LayoutUnit height = run.columnLogicalHeight(previousOffset);
        if (largestHeight < height) {
            largestHeight = height;
            indexWithLargestHeight = i;
        }
        previousOffset = run.breakOffset();
    }
    return indexWithLargestHeight;
}

void RenderMultiColumnSet::distributeImplicitBreaks()
{
#if ENABLE(ASSERT)
    // There should be no implicit breaks assumed at this point.
    for (unsigned i = 0; i < m_contentRuns.size(); i++)
        ASSERT(!m_contentRuns[i].assumedImplicitBreaks());
#endif // ENABLE(ASSERT)

    // Insert a final content run to encompass all content. This will include overflow if this is
    // the last set.
    addContentRun(logicalBottomInFlowThread());
    unsigned columnCount = m_contentRuns.size();

    // If there is room for more breaks (to reach the used value of column-count), imagine that we
    // insert implicit breaks at suitable locations. At any given time, the content run with the
    // currently tallest columns will get another implicit break "inserted", which will increase its
    // column count by one and shrink its columns' height. Repeat until we have the desired total
    // number of breaks. The largest column height among the runs will then be the initial column
    // height for the balancer to use.
    while (columnCount < usedColumnCount()) {
        unsigned index = findRunWithTallestColumns();
        m_contentRuns[index].assumeAnotherImplicitBreak();
        columnCount++;
    }
}

LayoutUnit RenderMultiColumnSet::calculateColumnHeight(BalancedHeightCalculation calculationMode) const
{
    if (calculationMode == GuessFromFlowThreadPortion) {
        // Initial balancing. Start with the lowest imaginable column height. We use the tallest
        // content run (after having "inserted" implicit breaks), and find its start offset (by
        // looking at the previous run's end offset, or, if there's no previous run, the set's start
        // offset in the flow thread).
        unsigned index = findRunWithTallestColumns();
        LayoutUnit startOffset = index > 0 ? m_contentRuns[index - 1].breakOffset() : logicalTopInFlowThread();
        return std::max<LayoutUnit>(m_contentRuns[index].columnLogicalHeight(startOffset), m_minimumColumnHeight);
    }

    if (actualColumnCount() <= usedColumnCount()) {
        // With the current column height, the content fits without creating overflowing columns. We're done.
        return m_columnHeight;
    }

    if (m_contentRuns.size() >= usedColumnCount()) {
        // Too many forced breaks to allow any implicit breaks. Initial balancing should already
        // have set a good height. There's nothing more we should do.
        return m_columnHeight;
    }

    // If the initial guessed column height wasn't enough, stretch it now. Stretch by the lowest
    // amount of space shortage found during layout.

    ASSERT(m_minSpaceShortage > 0); // We should never _shrink_ the height!
    ASSERT(m_minSpaceShortage != RenderFlowThread::maxLogicalHeight()); // If this happens, we probably have a bug.
    if (m_minSpaceShortage == RenderFlowThread::maxLogicalHeight())
        return m_columnHeight; // So bail out rather than looping infinitely.

    return m_columnHeight + m_minSpaceShortage;
}

void RenderMultiColumnSet::addContentRun(LayoutUnit endOffsetFromFirstPage)
{
    if (!multiColumnFlowThread()->heightIsAuto())
        return;
    if (!m_contentRuns.isEmpty() && endOffsetFromFirstPage <= m_contentRuns.last().breakOffset())
        return;
    // Append another item as long as we haven't exceeded used column count. What ends up in the
    // overflow area shouldn't affect column balancing.
    if (m_contentRuns.size() < usedColumnCount())
        m_contentRuns.append(ContentRun(endOffsetFromFirstPage));
}

bool RenderMultiColumnSet::recalculateColumnHeight(BalancedHeightCalculation calculationMode)
{
    if (previousSiblingMultiColumnSet()) {
        // FIXME: column spanner layout is not yet implemented. Until it's in place, we only operate
        // on the first set during layout. We need to ignore the others here, or assertions will
        // fail.
        return false;
    }
    ASSERT(multiColumnFlowThread()->heightIsAuto());

    LayoutUnit oldColumnHeight = m_columnHeight;
    if (calculationMode == GuessFromFlowThreadPortion) {
        // Post-process the content runs and find out where the implicit breaks will occur.
        distributeImplicitBreaks();
    }
    LayoutUnit newColumnHeight = calculateColumnHeight(calculationMode);
    setAndConstrainColumnHeight(newColumnHeight);

    // After having calculated an initial column height, the multicol container typically needs at
    // least one more layout pass with a new column height, but if a height was specified, we only
    // need to do this if we think that we need less space than specified. Conversely, if we
    // determined that the columns need to be as tall as the specified height of the container, we
    // have already laid it out correctly, and there's no need for another pass.

    // We can get rid of the content runs now, if we haven't already done so. They are only needed
    // to calculate the initial balanced column height. In fact, we have to get rid of them before
    // the next layout pass, since each pass will rebuild this.
    m_contentRuns.clear();

    if (m_columnHeight == oldColumnHeight)
        return false; // No change. We're done.

    m_minSpaceShortage = RenderFlowThread::maxLogicalHeight();
    return true; // Need another pass.
}

void RenderMultiColumnSet::recordSpaceShortage(LayoutUnit spaceShortage)
{
    if (spaceShortage >= m_minSpaceShortage)
        return;

    // The space shortage is what we use as our stretch amount. We need a positive number here in
    // order to get anywhere.
    ASSERT(spaceShortage > 0);

    m_minSpaceShortage = spaceShortage;
}

void RenderMultiColumnSet::resetColumnHeight()
{
    // Nuke previously stored minimum column height. Contents may have changed for all we know.
    m_minimumColumnHeight = 0;

    m_maxColumnHeight = calculateMaxColumnHeight();

    LayoutUnit oldColumnHeight = pageLogicalHeight();

    if (multiColumnFlowThread()->heightIsAuto())
        m_columnHeight = 0;
    else
        setAndConstrainColumnHeight(heightAdjustedForSetOffset(multiColumnFlowThread()->columnHeightAvailable()));

    if (pageLogicalHeight() != oldColumnHeight)
        setChildNeedsLayout(MarkOnlyThis);

    // Content runs are only needed in the initial layout pass, in order to find an initial column
    // height, and should have been deleted afterwards. We're about to rebuild the content runs, so
    // the list needs to be empty.
    ASSERT(m_contentRuns.isEmpty());
}

void RenderMultiColumnSet::expandToEncompassFlowThreadContentsIfNeeded()
{
    ASSERT(multiColumnFlowThread()->lastMultiColumnSet() == this);
    LayoutRect rect(flowThreadPortionRect());

    // Get the offset within the flow thread in its block progression direction. Then get the
    // flow thread's remaining logical height including its overflow and expand our rect
    // to encompass that remaining height and overflow. The idea is that we will generate
    // additional columns and pages to hold that overflow, since people do write bad
    // content like <body style="height:0px"> in multi-column layouts.
    bool isHorizontal = flowThread()->isHorizontalWritingMode();
    LayoutUnit logicalTopOffset = isHorizontal ? rect.y() : rect.x();
    LayoutRect layoutRect = flowThread()->layoutOverflowRect();
    LayoutUnit logicalHeightWithOverflow = (isHorizontal ? layoutRect.maxY() : layoutRect.maxX()) - logicalTopOffset;
    setFlowThreadPortionRect(LayoutRect(rect.x(), rect.y(), isHorizontal ? rect.width() : logicalHeightWithOverflow, isHorizontal ? logicalHeightWithOverflow : rect.height()));
}

void RenderMultiColumnSet::computeLogicalHeight(LayoutUnit, LayoutUnit logicalTop, LogicalExtentComputedValues& computedValues) const
{
    computedValues.m_extent = m_columnHeight;
    computedValues.m_position = logicalTop;
}

LayoutUnit RenderMultiColumnSet::calculateMaxColumnHeight() const
{
    RenderBlockFlow* multicolBlock = multiColumnBlockFlow();
    RenderStyle* multicolStyle = multicolBlock->style();
    LayoutUnit availableHeight = multiColumnFlowThread()->columnHeightAvailable();
    LayoutUnit maxColumnHeight = availableHeight ? availableHeight : RenderFlowThread::maxLogicalHeight();
    if (!multicolStyle->logicalMaxHeight().isMaxSizeNone()) {
        LayoutUnit logicalMaxHeight = multicolBlock->computeContentLogicalHeight(multicolStyle->logicalMaxHeight(), -1);
        if (logicalMaxHeight != -1 && maxColumnHeight > logicalMaxHeight)
            maxColumnHeight = logicalMaxHeight;
    }
    return heightAdjustedForSetOffset(maxColumnHeight);
}

LayoutUnit RenderMultiColumnSet::columnGap() const
{
    RenderBlockFlow* parentBlock = multiColumnBlockFlow();
    if (parentBlock->style()->hasNormalColumnGap())
        return parentBlock->style()->fontDescription().computedPixelSize(); // "1em" is recommended as the normal gap setting. Matches <p> margins.
    return parentBlock->style()->columnGap();
}

unsigned RenderMultiColumnSet::actualColumnCount() const
{
    // We must always return a value of 1 or greater. Column count = 0 is a meaningless situation,
    // and will confuse and cause problems in other parts of the code.
    if (!pageLogicalHeight())
        return 1;

    // Our portion rect determines our column count. We have as many columns as needed to fit all the content.
    LayoutUnit logicalHeightInColumns = flowThread()->isHorizontalWritingMode() ? flowThreadPortionRect().height() : flowThreadPortionRect().width();
    if (!logicalHeightInColumns)
        return 1;

    unsigned count = ceil(logicalHeightInColumns.toFloat() / pageLogicalHeight().toFloat());
    ASSERT(count >= 1);
    return count;
}

LayoutRect RenderMultiColumnSet::columnRectAt(unsigned index) const
{
    LayoutUnit colLogicalWidth = pageLogicalWidth();
    LayoutUnit colLogicalHeight = pageLogicalHeight();
    LayoutUnit colLogicalTop = borderBefore() + paddingBefore();
    LayoutUnit colLogicalLeft = borderAndPaddingLogicalLeft();
    LayoutUnit colGap = columnGap();

    if (multiColumnFlowThread()->progressionIsInline()) {
        if (style()->isLeftToRightDirection())
            colLogicalLeft += index * (colLogicalWidth + colGap);
        else
            colLogicalLeft += contentLogicalWidth() - colLogicalWidth - index * (colLogicalWidth + colGap);
    } else {
        colLogicalTop += index * (colLogicalHeight + colGap);
    }

    if (isHorizontalWritingMode())
        return LayoutRect(colLogicalLeft, colLogicalTop, colLogicalWidth, colLogicalHeight);
    return LayoutRect(colLogicalTop, colLogicalLeft, colLogicalHeight, colLogicalWidth);
}

unsigned RenderMultiColumnSet::columnIndexAtOffset(LayoutUnit offset, ColumnIndexCalculationMode mode) const
{
    LayoutRect portionRect(flowThreadPortionRect());

    // Handle the offset being out of range.
    LayoutUnit flowThreadLogicalTop = isHorizontalWritingMode() ? portionRect.y() : portionRect.x();
    if (offset < flowThreadLogicalTop)
        return 0;
    // If we're laying out right now, we cannot constrain against some logical bottom, since it
    // isn't known yet. Otherwise, just return the last column if we're past the logical bottom.
    if (mode == ClampToExistingColumns) {
        LayoutUnit flowThreadLogicalBottom = isHorizontalWritingMode() ? portionRect.maxY() : portionRect.maxX();
        if (offset >= flowThreadLogicalBottom)
            return actualColumnCount() - 1;
    }

    if (LayoutUnit pageLogicalHeight = this->pageLogicalHeight())
        return (offset - flowThreadLogicalTop).toFloat() / pageLogicalHeight.toFloat();

    return 0;
}

LayoutRect RenderMultiColumnSet::flowThreadPortionRectAt(unsigned index) const
{
    LayoutRect portionRect = flowThreadPortionRect();
    if (isHorizontalWritingMode())
        portionRect = LayoutRect(portionRect.x(), portionRect.y() + index * pageLogicalHeight(), portionRect.width(), pageLogicalHeight());
    else
        portionRect = LayoutRect(portionRect.x() + index * pageLogicalHeight(), portionRect.y(), pageLogicalHeight(), portionRect.height());
    return portionRect;
}

LayoutRect RenderMultiColumnSet::flowThreadPortionOverflowRect(const LayoutRect& portionRect, unsigned index, unsigned colCount, LayoutUnit colGap) const
{
    // This function determines the portion of the flow thread that paints for the column. Along the inline axis, columns are
    // unclipped at outside edges (i.e., the first and last column in the set), and they clip to half the column
    // gap along interior edges.
    //
    // In the block direction, we will not clip overflow out of the top of the first column, or out of the bottom of
    // the last column. This applies only to the true first column and last column across all column sets.
    //
    // FIXME: Eventually we will know overflow on a per-column basis, but we can't do this until we have a painting
    // mode that understands not to paint contents from a previous column in the overflow area of a following column.
    // This problem applies to regions and pages as well and is not unique to columns.
    bool isFirstColumn = !index;
    bool isLastColumn = index == colCount - 1;
    bool isLeftmostColumn = style()->isLeftToRightDirection() ? isFirstColumn : isLastColumn;
    bool isRightmostColumn = style()->isLeftToRightDirection() ? isLastColumn : isFirstColumn;

    // Calculate the overflow rectangle, based on the flow thread's, clipped at column logical
    // top/bottom unless it's the first/last column.
    LayoutRect overflowRect = overflowRectForFlowThreadPortion(portionRect, isFirstColumn && isFirstRegion(), isLastColumn && isLastRegion());

    // Avoid overflowing into neighboring columns, by clipping in the middle of adjacent column
    // gaps. Also make sure that we avoid rounding errors.
    if (isHorizontalWritingMode()) {
        if (!isLeftmostColumn)
            overflowRect.shiftXEdgeTo(portionRect.x() - colGap / 2);
        if (!isRightmostColumn)
            overflowRect.shiftMaxXEdgeTo(portionRect.maxX() + colGap - colGap / 2);
    } else {
        if (!isLeftmostColumn)
            overflowRect.shiftYEdgeTo(portionRect.y() - colGap / 2);
        if (!isRightmostColumn)
            overflowRect.shiftMaxYEdgeTo(portionRect.maxY() + colGap - colGap / 2);
    }
    return overflowRect;
}

void RenderMultiColumnSet::paintObject(const PaintInfo& paintInfo, const LayoutPoint& paintOffset)
{
    MultiColumnSetPainter(*this).paintObject(paintInfo, paintOffset);
}

void RenderMultiColumnSet::collectLayerFragments(LayerFragments& fragments, const LayoutRect& layerBoundingBox, const LayoutRect& dirtyRect)
{
    // |layerBoundingBox| is in the flow thread coordinate space, relative to the top/left edge of
    // the flow thread, but note that it has been converted with respect to writing mode (so that
    // it's visual/physical in that sense).
    //
    // |dirtyRect| is visual, relative to the multicol container.
    //
    // Then there's the output from this method - the stuff we put into the list of fragments. The
    // fragment.paginationOffset point is the actual visual translation required to get from a
    // location in the flow thread to a location in a given column. The fragment.paginationClip
    // rectangle, on the other hand, is in flow thread coordinates.
    //
    // All other rectangles in this method are sized physically, and the inline direction coordinate
    // is physical too, but the block direction coordinate is "logical top". This is the same as
    // e.g. RenderBox::frameRect(). These rectangles also pretend that there's only one long column,
    // i.e. they are for the flow thread.

    // Put the layer bounds into flow thread-local coordinates by flipping it first. Since we're in
    // a renderer, most rectangles are represented this way.
    LayoutRect layerBoundsInFlowThread(layerBoundingBox);
    flowThread()->flipForWritingMode(layerBoundsInFlowThread);

    // Now we can compare with the flow thread portions owned by each column. First let's
    // see if the rect intersects our flow thread portion at all.
    LayoutRect clippedRect(layerBoundsInFlowThread);
    clippedRect.intersect(RenderRegion::flowThreadPortionOverflowRect());
    if (clippedRect.isEmpty())
        return;

    // Now we know we intersect at least one column. Let's figure out the logical top and logical
    // bottom of the area we're checking.
    LayoutUnit layerLogicalTop = isHorizontalWritingMode() ? layerBoundsInFlowThread.y() : layerBoundsInFlowThread.x();
    LayoutUnit layerLogicalBottom = (isHorizontalWritingMode() ? layerBoundsInFlowThread.maxY() : layerBoundsInFlowThread.maxX()) - 1;

    // Figure out the start and end columns and only check within that range so that we don't walk the
    // entire column set.
    unsigned startColumn = columnIndexAtOffset(layerLogicalTop);
    unsigned endColumn = columnIndexAtOffset(layerLogicalBottom);

    LayoutUnit colLogicalWidth = pageLogicalWidth();
    LayoutUnit colGap = columnGap();
    unsigned colCount = actualColumnCount();

    RenderMultiColumnFlowThread* flowThread = multiColumnFlowThread();
    bool progressionIsInline = flowThread->progressionIsInline();
    bool leftToRight = style()->isLeftToRightDirection();

    LayoutUnit initialBlockOffset = logicalTop() - flowThread->logicalTop();

    for (unsigned i = startColumn; i <= endColumn; i++) {
        // Get the portion of the flow thread that corresponds to this column.
        LayoutRect flowThreadPortion = flowThreadPortionRectAt(i);

        // Now get the overflow rect that corresponds to the column.
        LayoutRect flowThreadOverflowPortion = flowThreadPortionOverflowRect(flowThreadPortion, i, colCount, colGap);

        // In order to create a fragment we must intersect the portion painted by this column.
        LayoutRect clippedRect(layerBoundsInFlowThread);
        clippedRect.intersect(flowThreadOverflowPortion);
        if (clippedRect.isEmpty())
            continue;

        // We also need to intersect the dirty rect. We have to apply a translation and shift based off
        // our column index.
        LayoutPoint translationOffset;
        LayoutUnit inlineOffset = progressionIsInline ? i * (colLogicalWidth + colGap) : LayoutUnit();
        if (!leftToRight)
            inlineOffset = -inlineOffset;
        translationOffset.setX(inlineOffset);
        LayoutUnit blockOffset;
        if (progressionIsInline) {
            blockOffset = initialBlockOffset + (isHorizontalWritingMode() ? -flowThreadPortion.y() : -flowThreadPortion.x());
        } else {
            // Column gap can apply in the block direction for page fragmentainers.
            // There is currently no spec which calls for column-gap to apply
            // for page fragmentainers at all, but it's applied here for compatibility
            // with the old multicolumn implementation.
            blockOffset = i * colGap;
        }
        if (isFlippedBlocksWritingMode(style()->writingMode()))
            blockOffset = -blockOffset;
        translationOffset.setY(blockOffset);
        if (!isHorizontalWritingMode())
            translationOffset = translationOffset.transposedPoint();
        // FIXME: The translation needs to include the multicolumn set's content offset within the
        // multicolumn block as well. This won't be an issue until we start creating multiple multicolumn sets.

        // Shift the dirty rect to be in flow thread coordinates with this translation applied.
        LayoutRect translatedDirtyRect(dirtyRect);
        translatedDirtyRect.moveBy(-translationOffset);

        // See if we intersect the dirty rect.
        clippedRect = layerBoundingBox;
        clippedRect.intersect(translatedDirtyRect);
        if (clippedRect.isEmpty())
            continue;

        // Something does need to paint in this column. Make a fragment now and supply the physical translation
        // offset and the clip rect for the column with that offset applied.
        LayerFragment fragment;
        fragment.paginationOffset = translationOffset;

        LayoutRect flippedFlowThreadOverflowPortion(flowThreadOverflowPortion);
        // Flip it into more a physical (RenderLayer-style) rectangle.
        flowThread->flipForWritingMode(flippedFlowThreadOverflowPortion);
        fragment.paginationClip = flippedFlowThreadOverflowPortion;
        fragments.append(fragment);
    }
}

void RenderMultiColumnSet::addOverflowFromChildren()
{
    unsigned colCount = actualColumnCount();
    if (!colCount)
        return;

    LayoutRect lastRect = columnRectAt(colCount - 1);
    addLayoutOverflow(lastRect);
    if (!hasOverflowClip())
        addVisualOverflow(lastRect);
}

const char* RenderMultiColumnSet::renderName() const
{
    return "RenderMultiColumnSet";
}

void RenderMultiColumnSet::insertedIntoTree()
{
    RenderRegion::insertedIntoTree();

    attachRegion();
}

void RenderMultiColumnSet::willBeRemovedFromTree()
{
    RenderRegion::willBeRemovedFromTree();

    detachRegion();
}

void RenderMultiColumnSet::attachRegion()
{
    if (documentBeingDestroyed())
        return;

    // A region starts off invalid.
    setIsValid(false);

    if (!m_flowThread)
        return;

    // Only after adding the region to the thread, the region is marked to be valid.
    m_flowThread->addRegionToThread(this);
}

void RenderMultiColumnSet::detachRegion()
{
    if (m_flowThread) {
        m_flowThread->removeRegionFromThread(this);
        m_flowThread = 0;
    }
}

}
