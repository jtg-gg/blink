/*
 * Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nuanti Ltd.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
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
#include "core/page/FocusController.h"

#include "core/HTMLNames.h"
#include "core/dom/AXObjectCache.h"
#include "core/dom/Document.h"
#include "core/dom/Element.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/NodeTraversal.h"
#include "core/dom/Range.h"
#include "core/dom/shadow/ElementShadow.h"
#include "core/dom/shadow/ShadowRoot.h"
#include "core/editing/Editor.h"
#include "core/editing/FrameSelection.h"
#include "core/editing/htmlediting.h" // For firstPositionInOrBeforeNode
#include "core/events/Event.h"
#include "core/frame/FrameView.h"
#include "core/frame/LocalDOMWindow.h"
#include "core/frame/LocalFrame.h"
#include "core/html/HTMLAreaElement.h"
#include "core/html/HTMLImageElement.h"
#include "core/html/HTMLPlugInElement.h"
#include "core/html/HTMLShadowElement.h"
#include "core/html/HTMLTextFormControlElement.h"
#include "core/page/Chrome.h"
#include "core/page/ChromeClient.h"
#include "core/page/EventHandler.h"
#include "core/page/FrameTree.h"
#include "core/page/Page.h"
#include "core/frame/Settings.h"
#include "core/page/SpatialNavigation.h"
#include "core/rendering/HitTestResult.h"
#include "core/rendering/RenderLayer.h"
#include <limits>

namespace blink {

using namespace HTMLNames;

static inline bool isShadowInsertionPointFocusScopeOwner(Node& node)
{
    return isActiveShadowInsertionPoint(node) && toHTMLShadowElement(node).olderShadowRoot();
}

class FocusNavigationScope {
    STACK_ALLOCATED();
public:
    Node* rootNode() const;
    Element* owner() const;
    static FocusNavigationScope focusNavigationScopeOf(Node&);
    static FocusNavigationScope ownedByNonFocusableFocusScopeOwner(Node&);
    static FocusNavigationScope ownedByShadowHost(Node&);
    static FocusNavigationScope ownedByShadowInsertionPoint(HTMLShadowElement&);
    static FocusNavigationScope ownedByIFrame(HTMLFrameOwnerElement&);

private:
    explicit FocusNavigationScope(TreeScope*);
    RawPtrWillBeMember<TreeScope> m_rootTreeScope;
};

// FIXME: Some of Node* return values and Node* arguments should be Element*.

FocusNavigationScope::FocusNavigationScope(TreeScope* treeScope)
    : m_rootTreeScope(treeScope)
{
    ASSERT(treeScope);
}

Node* FocusNavigationScope::rootNode() const
{
    return &m_rootTreeScope->rootNode();
}

Element* FocusNavigationScope::owner() const
{
    Node* root = rootNode();
    if (root->isShadowRoot()) {
        ShadowRoot* shadowRoot = toShadowRoot(root);
        return shadowRoot->isYoungest() ? shadowRoot->host() : shadowRoot->shadowInsertionPointOfYoungerShadowRoot();
    }
    // FIXME: Figure out the right thing for OOPI here.
    if (Frame* frame = root->document().frame())
        return frame->deprecatedLocalOwner();
    return nullptr;
}

FocusNavigationScope FocusNavigationScope::focusNavigationScopeOf(Node& node)
{
    Node* root = &node;
    for (Node* n = &node; n; n = n->parentNode())
        root = n;
    // The result is not always a ShadowRoot nor a DocumentNode since
    // a starting node is in an orphaned tree in composed shadow tree.
    return FocusNavigationScope(&root->treeScope());
}

FocusNavigationScope FocusNavigationScope::ownedByNonFocusableFocusScopeOwner(Node& node)
{
    if (isShadowHost(node))
        return FocusNavigationScope::ownedByShadowHost(node);
    ASSERT(isShadowInsertionPointFocusScopeOwner(node));
    return FocusNavigationScope::ownedByShadowInsertionPoint(toHTMLShadowElement(node));
}

FocusNavigationScope FocusNavigationScope::ownedByShadowHost(Node& node)
{
    ASSERT(isShadowHost(node));
    return FocusNavigationScope(toElement(node).shadow()->youngestShadowRoot());
}

FocusNavigationScope FocusNavigationScope::ownedByIFrame(HTMLFrameOwnerElement& frame)
{
    ASSERT(frame.contentFrame());
    ASSERT(frame.contentFrame()->isLocalFrame());
    return FocusNavigationScope(toLocalFrame(frame.contentFrame())->document());
}

FocusNavigationScope FocusNavigationScope::ownedByShadowInsertionPoint(HTMLShadowElement& shadowInsertionPoint)
{
    ASSERT(isShadowInsertionPointFocusScopeOwner(shadowInsertionPoint));
    return FocusNavigationScope(shadowInsertionPoint.olderShadowRoot());
}

static inline void dispatchEventsOnWindowAndFocusedNode(Document* document, bool focused)
{
    // If we have a focused node we should dispatch blur on it before we blur the window.
    // If we have a focused node we should dispatch focus on it after we focus the window.
    // https://bugs.webkit.org/show_bug.cgi?id=27105

    // Do not fire events while modal dialogs are up.  See https://bugs.webkit.org/show_bug.cgi?id=33962
    if (Page* page = document->page()) {
        if (page->defersLoading())
            return;
    }

    if (!focused && document->focusedElement()) {
        RefPtrWillBeRawPtr<Element> focusedElement(document->focusedElement());
        focusedElement->setFocus(false);
        focusedElement->dispatchBlurEvent(nullptr);
        if (focusedElement == document->focusedElement()) {
            focusedElement->dispatchFocusOutEvent(EventTypeNames::focusout, nullptr);
            if (focusedElement == document->focusedElement())
                focusedElement->dispatchFocusOutEvent(EventTypeNames::DOMFocusOut, nullptr);
        }
    }

    if (LocalDOMWindow* window = document->domWindow())
        window->dispatchEvent(Event::create(focused ? EventTypeNames::focus : EventTypeNames::blur));
    if (focused && document->focusedElement()) {
        RefPtrWillBeRawPtr<Element> focusedElement(document->focusedElement());
        focusedElement->setFocus(true);
        focusedElement->dispatchFocusEvent(0, FocusTypePage);
        if (focusedElement == document->focusedElement()) {
            document->focusedElement()->dispatchFocusInEvent(EventTypeNames::focusin, nullptr, FocusTypePage);
            if (focusedElement == document->focusedElement())
                document->focusedElement()->dispatchFocusInEvent(EventTypeNames::DOMFocusIn, nullptr, FocusTypePage);
        }
    }
}

static inline bool hasCustomFocusLogic(const Element& element)
{
    return element.isHTMLElement() && toHTMLElement(element).hasCustomFocusLogic();
}

#if ENABLE(ASSERT)
static inline bool isNonFocusableShadowHost(const Node& node)
{
    if (!node.isElementNode())
        return false;
    const Element& element = toElement(node);
    return !element.isFocusable() && isShadowHost(element) && !hasCustomFocusLogic(element);
}
#endif

static inline bool isNonKeyboardFocusableShadowHost(const Node& node)
{
    if (!node.isElementNode())
        return false;
    const Element& element = toElement(node);
    return !element.isKeyboardFocusable() && isShadowHost(element) && !hasCustomFocusLogic(element);
}

static inline bool isKeyboardFocusableShadowHost(const Node& node)
{
    if (!node.isElementNode())
        return false;
    const Element& element = toElement(node);
    return element.isKeyboardFocusable() && isShadowHost(element) && !hasCustomFocusLogic(element);
}

static inline bool isNonFocusableFocusScopeOwner(Node& node)
{
    return isNonKeyboardFocusableShadowHost(node) || isShadowInsertionPointFocusScopeOwner(node);
}

static inline int adjustedTabIndex(Node& node)
{
    return isNonFocusableFocusScopeOwner(node) ? 0 : node.tabIndex();
}

static inline bool shouldVisit(Node& node)
{
    return (node.isElementNode() && toElement(node).isKeyboardFocusable()) || isNonFocusableFocusScopeOwner(node);
}

FocusController::FocusController(Page* page)
    : m_page(page)
    , m_isActive(false)
    , m_isFocused(false)
    , m_isChangingFocusedFrame(false)
{
}

PassOwnPtrWillBeRawPtr<FocusController> FocusController::create(Page* page)
{
    return adoptPtrWillBeNoop(new FocusController(page));
}

void FocusController::setFocusedFrame(PassRefPtrWillBeRawPtr<Frame> frame)
{
    ASSERT(!frame || frame->page() == m_page);
    if (m_focusedFrame == frame || m_isChangingFocusedFrame)
        return;

    m_isChangingFocusedFrame = true;

    RefPtrWillBeRawPtr<LocalFrame> oldFrame = (m_focusedFrame && m_focusedFrame->isLocalFrame()) ? toLocalFrame(m_focusedFrame.get()) : nullptr;

    RefPtrWillBeRawPtr<LocalFrame> newFrame = (frame && frame->isLocalFrame()) ? toLocalFrame(frame.get()) : nullptr;

    m_focusedFrame = frame.get();

    // Now that the frame is updated, fire events and update the selection focused states of both frames.
    if (oldFrame && oldFrame->view()) {
        oldFrame->selection().setFocused(false);
        oldFrame->domWindow()->dispatchEvent(Event::create(EventTypeNames::blur));
    }

    if (newFrame && newFrame->view() && isFocused()) {
        newFrame->selection().setFocused(true);
        newFrame->domWindow()->dispatchEvent(Event::create(EventTypeNames::focus));
    }

    m_isChangingFocusedFrame = false;

    m_page->chrome().client().focusedFrameChanged(newFrame.get());
}

void FocusController::focusDocumentView(PassRefPtrWillBeRawPtr<Frame> frame)
{
    ASSERT(!frame || frame->page() == m_page);
    if (m_focusedFrame == frame)
        return;

    RefPtrWillBeRawPtr<LocalFrame> focusedFrame = (m_focusedFrame && m_focusedFrame->isLocalFrame()) ? toLocalFrame(m_focusedFrame.get()) : nullptr;
    if (focusedFrame && focusedFrame->view()) {
        RefPtrWillBeRawPtr<Document> document = focusedFrame->document();
        Element* focusedElement = document ? document->focusedElement() : nullptr;
        if (focusedElement) {
            focusedElement->dispatchBlurEvent(nullptr);
            if (focusedElement == document->focusedElement()) {
                focusedElement->dispatchFocusOutEvent(EventTypeNames::focusout, nullptr);
                if (focusedElement == document->focusedElement())
                    focusedElement->dispatchFocusOutEvent(EventTypeNames::DOMFocusOut, nullptr);
            }
        }
    }

    RefPtrWillBeRawPtr<LocalFrame> newFocusedFrame = (frame && frame->isLocalFrame()) ? toLocalFrame(frame.get()) : nullptr;
    if (newFocusedFrame && newFocusedFrame->view()) {
        RefPtrWillBeRawPtr<Document> document = newFocusedFrame->document();
        Element* focusedElement = document ? document->focusedElement() : nullptr;
        if (focusedElement) {
            focusedElement->dispatchFocusEvent(0, FocusTypePage);
            if (focusedElement == document->focusedElement()) {
                document->focusedElement()->dispatchFocusInEvent(EventTypeNames::focusin, nullptr, FocusTypePage);
                if (focusedElement == document->focusedElement())
                    document->focusedElement()->dispatchFocusInEvent(EventTypeNames::DOMFocusIn, nullptr, FocusTypePage);
            }
        }
    }

    setFocusedFrame(frame);
}

Frame* FocusController::focusedOrMainFrame() const
{
    if (Frame* frame = focusedFrame())
        return frame;

    // FIXME: This is a temporary hack to ensure that we return a LocalFrame, even when the mainFrame is remote.
    // FocusController needs to be refactored to deal with RemoteFrames cross-process focus transfers.
    for (Frame* frame = m_page->mainFrame()->tree().top(); frame; frame = frame->tree().traverseNext()) {
        if (frame->isLocalRoot())
            return frame;
    }

    return m_page->mainFrame();
}

void FocusController::setFocused(bool focused)
{
    if (isFocused() == focused)
        return;

    m_isFocused = focused;

    if (!m_isFocused && focusedOrMainFrame()->isLocalFrame())
        toLocalFrame(focusedOrMainFrame())->eventHandler().stopAutoscroll();

    if (!m_focusedFrame)
        setFocusedFrame(m_page->mainFrame());

    // setFocusedFrame above might reject to update m_focusedFrame, or
    // m_focusedFrame might be changed by blur/focus event handlers.
    if (m_focusedFrame && m_focusedFrame->isLocalFrame() && toLocalFrame(m_focusedFrame.get())->view()) {
        toLocalFrame(m_focusedFrame.get())->selection().setFocused(focused);
        dispatchEventsOnWindowAndFocusedNode(toLocalFrame(m_focusedFrame.get())->document(), focused);
    }
}

Node* FocusController::findFocusableNodeDecendingDownIntoFrameDocument(FocusType type, Node* node)
{
    // The node we found might be a HTMLFrameOwnerElement, so descend down the tree until we find either:
    // 1) a focusable node, or
    // 2) the deepest-nested HTMLFrameOwnerElement.
    while (node && node->isFrameOwnerElement()) {
        HTMLFrameOwnerElement& owner = toHTMLFrameOwnerElement(*node);
        if (!owner.contentFrame() || !owner.contentFrame()->isLocalFrame())
            break;
        toLocalFrame(owner.contentFrame())->document()->updateLayoutIgnorePendingStylesheets();
        Node* foundNode = findFocusableNode(type, FocusNavigationScope::ownedByIFrame(owner), nullptr);
        if (!foundNode)
            break;
        ASSERT(node != foundNode);
        node = foundNode;
    }
    return node;
}

bool FocusController::setInitialFocus(FocusType type)
{
    bool didAdvanceFocus = advanceFocus(type, true);

    // If focus is being set initially, accessibility needs to be informed that system focus has moved
    // into the web area again, even if focus did not change within WebCore. PostNotification is called instead
    // of handleFocusedUIElementChanged, because this will send the notification even if the element is the same.
    if (focusedOrMainFrame()->isLocalFrame()) {
        Document* document = toLocalFrame(focusedOrMainFrame())->document();
        if (AXObjectCache* cache = document->existingAXObjectCache())
            cache->handleInitialFocus();
    }

    return didAdvanceFocus;
}

bool FocusController::advanceFocus(FocusType type, bool initialFocus)
{
    switch (type) {
    case FocusTypeForward:
    case FocusTypeBackward:
        return advanceFocusInDocumentOrder(type, initialFocus);
    case FocusTypeLeft:
    case FocusTypeRight:
    case FocusTypeUp:
    case FocusTypeDown:
        return advanceFocusDirectionally(type);
    default:
        ASSERT_NOT_REACHED();
    }

    return false;
}

bool FocusController::advanceFocusInDocumentOrder(FocusType type, bool initialFocus)
{
    // FIXME: Focus advancement won't work with externally rendered frames until after
    // inter-frame focus control is moved out of Blink.
    if (!focusedOrMainFrame()->isLocalFrame())
        return false;
    LocalFrame* frame = toLocalFrame(focusedOrMainFrame());
    ASSERT(frame);
    Document* document = frame->document();

    Node* currentNode = document->focusedElement();
    // FIXME: Not quite correct when it comes to focus transitions leaving/entering the WebView itself
    bool caretBrowsing = frame->settings() && frame->settings()->caretBrowsingEnabled();

    if (caretBrowsing && !currentNode)
        currentNode = frame->selection().start().deprecatedNode();

    document->updateLayoutIgnorePendingStylesheets();

    RefPtrWillBeRawPtr<Node> node = findFocusableNodeAcrossFocusScope(type, FocusNavigationScope::focusNavigationScopeOf(currentNode ? *currentNode : *document), currentNode);

    if (!node) {
        // We didn't find a node to focus, so we should try to pass focus to Chrome.
        if (!initialFocus && m_page->chrome().canTakeFocus(type)) {
            document->setFocusedElement(nullptr);
            setFocusedFrame(nullptr);
            m_page->chrome().takeFocus(type);
            return true;
        }

        // Chrome doesn't want focus, so we should wrap focus.
        if (!m_page->mainFrame()->isLocalFrame())
            return false;
        node = findFocusableNodeRecursively(type, FocusNavigationScope::focusNavigationScopeOf(*m_page->deprecatedLocalMainFrame()->document()), nullptr);
        node = findFocusableNodeDecendingDownIntoFrameDocument(type, node.get());

        if (!node)
            return false;
    }

    ASSERT(node);

    if (node == document->focusedElement())
        // Focus wrapped around to the same node.
        return true;

    if (!node->isElementNode())
        // FIXME: May need a way to focus a document here.
        return false;

    Element* element = toElement(node);
    if (element->isFrameOwnerElement() && (!isHTMLPlugInElement(*element) || !element->isKeyboardFocusable())) {
        // We focus frames rather than frame owners.
        // FIXME: We should not focus frames that have no scrollbars, as focusing them isn't useful to the user.
        HTMLFrameOwnerElement* owner = toHTMLFrameOwnerElement(element);
        if (!owner->contentFrame())
            return false;

        document->setFocusedElement(nullptr);
        setFocusedFrame(owner->contentFrame());
        return true;
    }

    // FIXME: It would be nice to just be able to call setFocusedElement(node)
    // here, but we can't do that because some elements (e.g. HTMLInputElement
    // and HTMLTextAreaElement) do extra work in their focus() methods.
    Document& newDocument = element->document();

    if (&newDocument != document) {
        // Focus is going away from this document, so clear the focused node.
        document->setFocusedElement(nullptr);
    }

    setFocusedFrame(newDocument.frame());

    if (caretBrowsing) {
        Position position = firstPositionInOrBeforeNode(element);
        VisibleSelection newSelection(position, position, DOWNSTREAM);
        frame->selection().setSelection(newSelection);
    }

    element->focus(false, type);
    return true;
}

Node* FocusController::findFocusableNodeAcrossFocusScope(FocusType type, const FocusNavigationScope& scope, Node* currentNode)
{
    ASSERT(!currentNode || !isNonFocusableShadowHost(*currentNode));
    Node* found;
    if (currentNode && type == FocusTypeForward && isKeyboardFocusableShadowHost(*currentNode)) {
        Node* foundInInnerFocusScope = findFocusableNodeRecursively(type, FocusNavigationScope::ownedByShadowHost(*currentNode), nullptr);
        found = foundInInnerFocusScope ? foundInInnerFocusScope : findFocusableNodeRecursively(type, scope, currentNode);
    } else {
        found = findFocusableNodeRecursively(type, scope, currentNode);
    }

    // If there's no focusable node to advance to, move up the focus scopes until we find one.
    FocusNavigationScope currentScope = scope;
    while (!found) {
        Node* owner = currentScope.owner();
        if (!owner)
            break;
        currentScope = FocusNavigationScope::focusNavigationScopeOf(*owner);
        if (type == FocusTypeBackward && isKeyboardFocusableShadowHost(*owner)) {
            found = owner;
            break;
        }
        found = findFocusableNodeRecursively(type, currentScope, owner);
    }
    found = findFocusableNodeDecendingDownIntoFrameDocument(type, found);
    return found;
}

Node* FocusController::findFocusableNodeRecursively(FocusType type, const FocusNavigationScope& scope, Node* start)
{
    // Starting node is exclusive.
    Node* foundOrNull = findFocusableNode(type, scope, start);
    if (!foundOrNull)
        return nullptr;
    Node& found = *foundOrNull;
    if (type == FocusTypeForward) {
        if (!isNonFocusableFocusScopeOwner(found))
            return &found;
        Node* foundInInnerFocusScope = findFocusableNodeRecursively(type, FocusNavigationScope::ownedByNonFocusableFocusScopeOwner(found), nullptr);
        return foundInInnerFocusScope ? foundInInnerFocusScope : findFocusableNodeRecursively(type, scope, &found);
    }
    ASSERT(type == FocusTypeBackward);
    if (isKeyboardFocusableShadowHost(found)) {
        Node* foundInInnerFocusScope = findFocusableNodeRecursively(type, FocusNavigationScope::ownedByShadowHost(found), nullptr);
        return foundInInnerFocusScope ? foundInInnerFocusScope : &found;
    }
    if (isNonFocusableFocusScopeOwner(found)) {
        Node* foundInInnerFocusScope = findFocusableNodeRecursively(type, FocusNavigationScope::ownedByNonFocusableFocusScopeOwner(found), nullptr);
        return foundInInnerFocusScope ? foundInInnerFocusScope :findFocusableNodeRecursively(type, scope, &found);
    }
    return &found;
}

Node* FocusController::findFocusableNode(FocusType type, const FocusNavigationScope& scope, Node* node)
{
    return type == FocusTypeForward ? nextFocusableNode(scope, node) : previousFocusableNode(scope, node);
}

Node* FocusController::findNodeWithExactTabIndex(Node* start, int tabIndex, FocusType type)
{
    // Search is inclusive of start
    for (Node* node = start; node; node = type == FocusTypeForward ? NodeTraversal::next(*node) : NodeTraversal::previous(*node)) {
        if (shouldVisit(*node) && adjustedTabIndex(*node) == tabIndex)
            return node;
    }
    return nullptr;
}

static Node* nextNodeWithGreaterTabIndex(Node* start, int tabIndex)
{
    // Search is inclusive of start
    int winningTabIndex = std::numeric_limits<short>::max() + 1;
    Node* winner = nullptr;
    for (Node& node : NodeTraversal::startsAt(start)) {
        int currentTabIndex = adjustedTabIndex(node);
        if (shouldVisit(node) && currentTabIndex > tabIndex && currentTabIndex < winningTabIndex) {
            winner = &node;
            winningTabIndex = currentTabIndex;
        }
    }

    return winner;
}

static Node* previousNodeWithLowerTabIndex(Node* start, int tabIndex)
{
    // Search is inclusive of start
    int winningTabIndex = 0;
    Node* winner = nullptr;
    for (Node* node = start; node; node = NodeTraversal::previous(*node)) {
        int currentTabIndex = adjustedTabIndex(*node);
        if (shouldVisit(*node) && currentTabIndex < tabIndex && currentTabIndex > winningTabIndex) {
            winner = node;
            winningTabIndex = currentTabIndex;
        }
    }
    return winner;
}

Node* FocusController::nextFocusableNode(const FocusNavigationScope& scope, Node* start)
{
    if (start) {
        int tabIndex = adjustedTabIndex(*start);
        // If a node is excluded from the normal tabbing cycle, the next focusable node is determined by tree order
        if (tabIndex < 0) {
            for (Node& node : NodeTraversal::startsAfter(*start)) {
                if (shouldVisit(node) && adjustedTabIndex(node) >= 0)
                    return &node;
            }
        } else {
            // First try to find a node with the same tabindex as start that comes after start in the scope.
            if (Node* winner = findNodeWithExactTabIndex(NodeTraversal::next(*start), tabIndex, FocusTypeForward))
                return winner;
        }
        if (!tabIndex) {
            // We've reached the last node in the document with a tabindex of 0. This is the end of the tabbing order.
            return nullptr;
        }
    }

    // Look for the first node in the scope that:
    // 1) has the lowest tabindex that is higher than start's tabindex (or 0, if start is null), and
    // 2) comes first in the scope, if there's a tie.
    if (Node* winner = nextNodeWithGreaterTabIndex(scope.rootNode(), start ? adjustedTabIndex(*start) : 0))
        return winner;

    // There are no nodes with a tabindex greater than start's tabindex,
    // so find the first node with a tabindex of 0.
    return findNodeWithExactTabIndex(scope.rootNode(), 0, FocusTypeForward);
}

Node* FocusController::previousFocusableNode(const FocusNavigationScope& scope, Node* start)
{
    Node* last = nullptr;
    for (Node* node = scope.rootNode(); node; node = node->lastChild())
        last = node;
    ASSERT(last);

    // First try to find the last node in the scope that comes before start and has the same tabindex as start.
    // If start is null, find the last node in the scope with a tabindex of 0.
    Node* startingNode;
    int startingTabIndex;
    if (start) {
        startingNode = NodeTraversal::previous(*start);
        startingTabIndex = adjustedTabIndex(*start);
    } else {
        startingNode = last;
        startingTabIndex = 0;
    }

    // However, if a node is excluded from the normal tabbing cycle, the previous focusable node is determined by tree order
    if (startingTabIndex < 0) {
        for (Node* node = startingNode; node; node = NodeTraversal::previous(*node)) {
            if (shouldVisit(*node) && adjustedTabIndex(*node) >= 0)
                return node;
        }
    } else {
        if (Node* winner = findNodeWithExactTabIndex(startingNode, startingTabIndex, FocusTypeBackward))
            return winner;
    }

    // There are no nodes before start with the same tabindex as start, so look for a node that:
    // 1) has the highest non-zero tabindex (that is less than start's tabindex), and
    // 2) comes last in the scope, if there's a tie.
    startingTabIndex = (start && startingTabIndex) ? startingTabIndex : std::numeric_limits<short>::max();
    return previousNodeWithLowerTabIndex(last, startingTabIndex);
}

static bool relinquishesEditingFocus(const Element& element)
{
    ASSERT(element.hasEditableStyle());
    return element.document().frame() && element.rootEditableElement();
}

static void clearSelectionIfNeeded(LocalFrame* oldFocusedFrame, LocalFrame* newFocusedFrame, Node* newFocusedNode)
{
    if (!oldFocusedFrame || !newFocusedFrame)
        return;

    if (oldFocusedFrame->document() != newFocusedFrame->document())
        return;

    FrameSelection& selection = oldFocusedFrame->selection();
    if (selection.isNone())
        return;

    bool caretBrowsing = oldFocusedFrame->settings()->caretBrowsingEnabled();
    if (caretBrowsing)
        return;

    Node* selectionStartNode = selection.selection().start().deprecatedNode();
    if (selectionStartNode == newFocusedNode || selectionStartNode->isDescendantOf(newFocusedNode))
        return;

    if (!enclosingTextFormControl(selectionStartNode))
        return;

    if (selectionStartNode->isInShadowTree() && selectionStartNode->shadowHost() == newFocusedNode)
        return;

    selection.clear();
}

bool FocusController::setFocusedElement(Element* element, PassRefPtrWillBeRawPtr<Frame> newFocusedFrame, FocusType type)
{
    RefPtrWillBeRawPtr<LocalFrame> oldFocusedFrame = toLocalFrame(focusedFrame());
    RefPtrWillBeRawPtr<Document> oldDocument = oldFocusedFrame ? oldFocusedFrame->document() : nullptr;

    Element* oldFocusedElement = oldDocument ? oldDocument->focusedElement() : nullptr;
    if (element && oldFocusedElement == element)
        return true;

    // FIXME: Might want to disable this check for caretBrowsing
    if (oldFocusedElement && oldFocusedElement->isRootEditableElement() && !relinquishesEditingFocus(*oldFocusedElement))
        return false;

    m_page->chrome().client().willSetInputMethodState();

    RefPtrWillBeRawPtr<Document> newDocument = nullptr;
    if (element)
        newDocument = &element->document();
    else if (newFocusedFrame && newFocusedFrame->isLocalFrame())
        newDocument = toLocalFrame(newFocusedFrame.get())->document();

    if (newDocument && oldDocument == newDocument && newDocument->focusedElement() == element)
        return true;

    clearSelectionIfNeeded(oldFocusedFrame.get(), toLocalFrame(newFocusedFrame.get()), element);

    if (oldDocument && oldDocument != newDocument)
        oldDocument->setFocusedElement(nullptr);

    if (newFocusedFrame && !newFocusedFrame->page()) {
        setFocusedFrame(nullptr);
        return false;
    }
    setFocusedFrame(newFocusedFrame);

    // Setting the focused node can result in losing our last reft to node when JS event handlers fire.
    RefPtrWillBeRawPtr<Element> protect = element;
    ALLOW_UNUSED_LOCAL(protect);
    if (newDocument) {
        bool successfullyFocused = newDocument->setFocusedElement(element, type);
        if (!successfullyFocused)
            return false;
    }

    return true;
}

void FocusController::setActive(bool active)
{
    if (m_isActive == active)
        return;

    m_isActive = active;

    Frame* frame = focusedOrMainFrame();
    if (frame->isLocalFrame()) {
        // Invalidate all custom scrollbars because they support the CSS
        // window-active attribute. This should be applied to the entire page so
        // we invalidate from the root FrameView instead of just the focused.
        if (FrameView* view = toLocalFrame(frame)->localFrameRoot()->document()->view())
            view->invalidateAllCustomScrollbarsOnActiveChanged();
        toLocalFrame(frame)->selection().pageActivationChanged();
    }
}

static void updateFocusCandidateIfNeeded(FocusType type, const FocusCandidate& current, FocusCandidate& candidate, FocusCandidate& closest)
{
    ASSERT(candidate.visibleNode->isElementNode());
    ASSERT(candidate.visibleNode->renderer());

    // Ignore iframes that don't have a src attribute
    if (frameOwnerElement(candidate) && (!frameOwnerElement(candidate)->contentFrame() || candidate.rect.isEmpty()))
        return;

    // Ignore off screen child nodes of containers that do not scroll (overflow:hidden)
    if (candidate.isOffscreen && !canBeScrolledIntoView(type, candidate))
        return;

    distanceDataForNode(type, current, candidate);
    if (candidate.distance == maxDistance())
        return;

    if (candidate.isOffscreenAfterScrolling && candidate.alignment < Full)
        return;

    if (closest.isNull()) {
        closest = candidate;
        return;
    }

    LayoutRect intersectionRect = intersection(candidate.rect, closest.rect);
    if (!intersectionRect.isEmpty() && !areElementsOnSameLine(closest, candidate)
        && intersectionRect == candidate.rect) {
        // If 2 nodes are intersecting, do hit test to find which node in on top.
        LayoutUnit x = intersectionRect.x() + intersectionRect.width() / 2;
        LayoutUnit y = intersectionRect.y() + intersectionRect.height() / 2;
        if (!candidate.visibleNode->document().page()->mainFrame()->isLocalFrame())
            return;
        HitTestResult result = candidate.visibleNode->document().page()->deprecatedLocalMainFrame()->eventHandler().hitTestResultAtPoint(IntPoint(x, y), HitTestRequest::ReadOnly | HitTestRequest::Active | HitTestRequest::IgnoreClipping);
        if (candidate.visibleNode->contains(result.innerNode())) {
            closest = candidate;
            return;
        }
        if (closest.visibleNode->contains(result.innerNode()))
            return;
    }

    if (candidate.alignment == closest.alignment) {
        if (candidate.distance < closest.distance)
            closest = candidate;
        return;
    }

    if (candidate.alignment > closest.alignment)
        closest = candidate;
}

void FocusController::findFocusCandidateInContainer(Node& container, const LayoutRect& startingRect, FocusType type, FocusCandidate& closest)
{
    Element* focusedElement = (focusedFrame() && toLocalFrame(focusedFrame())->document()) ? toLocalFrame(focusedFrame())->document()->focusedElement() : nullptr;

    Element* element = ElementTraversal::firstWithin(container);
    FocusCandidate current;
    current.rect = startingRect;
    current.focusableNode = focusedElement;
    current.visibleNode = focusedElement;

    for (; element; element = (element->isFrameOwnerElement() || canScrollInDirection(element, type))
        ? ElementTraversal::nextSkippingChildren(*element, &container)
        : ElementTraversal::next(*element, &container)) {
        if (element == focusedElement)
            continue;

        if (!element->isKeyboardFocusable() && !element->isFrameOwnerElement() && !canScrollInDirection(element, type))
            continue;

        FocusCandidate candidate = FocusCandidate(element, type);
        if (candidate.isNull())
            continue;

        candidate.enclosingScrollableBox = &container;
        updateFocusCandidateIfNeeded(type, current, candidate, closest);
    }
}

bool FocusController::advanceFocusDirectionallyInContainer(Node* container, const LayoutRect& startingRect, FocusType type)
{
    if (!container)
        return false;

    LayoutRect newStartingRect = startingRect;

    if (startingRect.isEmpty())
        newStartingRect = virtualRectForDirection(type, nodeRectInAbsoluteCoordinates(container));

    // Find the closest node within current container in the direction of the navigation.
    FocusCandidate focusCandidate;
    findFocusCandidateInContainer(*container, newStartingRect, type, focusCandidate);

    if (focusCandidate.isNull()) {
        // Nothing to focus, scroll if possible.
        // NOTE: If no scrolling is performed (i.e. scrollInDirection returns false), the
        // spatial navigation algorithm will skip this container.
        return scrollInDirection(container, type);
    }

    HTMLFrameOwnerElement* frameElement = frameOwnerElement(focusCandidate);
    // If we have an iframe without the src attribute, it will not have a contentFrame().
    // We ASSERT here to make sure that
    // updateFocusCandidateIfNeeded() will never consider such an iframe as a candidate.
    ASSERT(!frameElement || frameElement->contentFrame());
    if (frameElement && frameElement->contentFrame()->isLocalFrame()) {
        if (focusCandidate.isOffscreenAfterScrolling) {
            scrollInDirection(&focusCandidate.visibleNode->document(), type);
            return true;
        }
        // Navigate into a new frame.
        LayoutRect rect;
        Element* focusedElement = toLocalFrame(focusedOrMainFrame())->document()->focusedElement();
        if (focusedElement && !hasOffscreenRect(focusedElement))
            rect = nodeRectInAbsoluteCoordinates(focusedElement, true /* ignore border */);
        toLocalFrame(frameElement->contentFrame())->document()->updateLayoutIgnorePendingStylesheets();
        if (!advanceFocusDirectionallyInContainer(toLocalFrame(frameElement->contentFrame())->document(), rect, type)) {
            // The new frame had nothing interesting, need to find another candidate.
            return advanceFocusDirectionallyInContainer(container, nodeRectInAbsoluteCoordinates(focusCandidate.visibleNode, true), type);
        }
        return true;
    }

    if (canScrollInDirection(focusCandidate.visibleNode, type)) {
        if (focusCandidate.isOffscreenAfterScrolling) {
            scrollInDirection(focusCandidate.visibleNode, type);
            return true;
        }
        // Navigate into a new scrollable container.
        LayoutRect startingRect;
        Element* focusedElement = toLocalFrame(focusedOrMainFrame())->document()->focusedElement();
        if (focusedElement && !hasOffscreenRect(focusedElement))
            startingRect = nodeRectInAbsoluteCoordinates(focusedElement, true);
        return advanceFocusDirectionallyInContainer(focusCandidate.visibleNode, startingRect, type);
    }
    if (focusCandidate.isOffscreenAfterScrolling) {
        Node* container = focusCandidate.enclosingScrollableBox;
        scrollInDirection(container, type);
        return true;
    }

    // We found a new focus node, navigate to it.
    Element* element = toElement(focusCandidate.focusableNode);
    ASSERT(element);

    element->focus(false, type);
    return true;
}

bool FocusController::advanceFocusDirectionally(FocusType type)
{
    // FIXME: Directional focus changes don't yet work with RemoteFrames.
    if (!focusedOrMainFrame()->isLocalFrame())
        return false;
    LocalFrame* curFrame = toLocalFrame(focusedOrMainFrame());
    ASSERT(curFrame);

    Document* focusedDocument = curFrame->document();
    if (!focusedDocument)
        return false;

    Element* focusedElement = focusedDocument->focusedElement();
    Node* container = focusedDocument;

    if (container->isDocumentNode())
        toDocument(container)->updateLayoutIgnorePendingStylesheets();

    // Figure out the starting rect.
    LayoutRect startingRect;
    if (focusedElement) {
        if (!hasOffscreenRect(focusedElement)) {
            container = scrollableEnclosingBoxOrParentFrameForNodeInDirection(type, focusedElement);
            startingRect = nodeRectInAbsoluteCoordinates(focusedElement, true /* ignore border */);
        } else if (isHTMLAreaElement(*focusedElement)) {
            HTMLAreaElement& area = toHTMLAreaElement(*focusedElement);
            container = scrollableEnclosingBoxOrParentFrameForNodeInDirection(type, area.imageElement());
            startingRect = virtualRectForAreaElementAndDirection(area, type);
        }
    }

    bool consumed = false;
    do {
        consumed = advanceFocusDirectionallyInContainer(container, startingRect, type);
        startingRect = nodeRectInAbsoluteCoordinates(container, true /* ignore border */);
        container = scrollableEnclosingBoxOrParentFrameForNodeInDirection(type, container);
        if (container && container->isDocumentNode())
            toDocument(container)->updateLayoutIgnorePendingStylesheets();
    } while (!consumed && container);

    return consumed;
}

void FocusController::trace(Visitor* visitor)
{
    visitor->trace(m_page);
    visitor->trace(m_focusedFrame);
}

} // namespace blink
