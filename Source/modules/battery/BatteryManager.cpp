// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "config.h"
#include "modules/battery/BatteryManager.h"

#include "core/dom/Document.h"
#include "core/events/Event.h"
#include "modules/battery/BatteryDispatcher.h"
#include "modules/battery/BatteryStatus.h"
#include "platform/RuntimeEnabledFeatures.h"

namespace blink {

BatteryManager* BatteryManager::create(ExecutionContext* context)
{
    BatteryManager* batteryManager = new BatteryManager(context);
    batteryManager->suspendIfNeeded();
    return batteryManager;
}

BatteryManager::~BatteryManager()
{
#if !ENABLE(OILPAN)
    stopUpdating();
#endif
}

BatteryManager::BatteryManager(ExecutionContext* context)
    : ActiveDOMObject(context)
    , PlatformEventController(toDocument(context)->page())
    , m_batteryStatus(BatteryStatus::create())
    , m_state(NotStarted)
{
}

ScriptPromise BatteryManager::startRequest(ScriptState* scriptState)
{
    if (m_state == Pending)
        return m_resolver->promise();

    m_resolver = ScriptPromiseResolver::create(scriptState);
    ScriptPromise promise = m_resolver->promise();

    ASSERT(executionContext());
    // If the context is in a stopped state already, do not start updating.
    if (m_state == Resolved || executionContext()->activeDOMObjectsAreStopped()) {
        // FIXME: Consider returning the same promise in this case. See crbug.com/385025.
        m_state = Resolved;
        m_resolver->resolve(this);
    } else if (m_state == NotStarted) {
        m_state = Pending;
        m_hasEventListener = true;
        startUpdating();
    }

    return promise;
}

bool BatteryManager::charging()
{
    return m_batteryStatus->charging();
}

double BatteryManager::chargingTime()
{
    return m_batteryStatus->chargingTime();
}

double BatteryManager::dischargingTime()
{
    return m_batteryStatus->dischargingTime();
}

double BatteryManager::level()
{
    return m_batteryStatus->level();
}

void BatteryManager::didUpdateData()
{
    ASSERT(RuntimeEnabledFeatures::batteryStatusEnabled());
    ASSERT(m_state != NotStarted);

    BatteryStatus* oldStatus = m_batteryStatus;
    m_batteryStatus = BatteryDispatcher::instance().latestData();

    if (m_state == Pending) {
        ASSERT(m_resolver);
        m_state = Resolved;
        m_resolver->resolve(this);
        return;
    }

    Document* document = toDocument(executionContext());
    ASSERT(document);
    if (document->activeDOMObjectsAreSuspended() || document->activeDOMObjectsAreStopped())
        return;

    ASSERT(oldStatus);

    if (m_batteryStatus->charging() != oldStatus->charging())
        dispatchEvent(Event::create(EventTypeNames::chargingchange));
    if (m_batteryStatus->chargingTime() != oldStatus->chargingTime())
        dispatchEvent(Event::create(EventTypeNames::chargingtimechange));
    if (m_batteryStatus->dischargingTime() != oldStatus->dischargingTime())
        dispatchEvent(Event::create(EventTypeNames::dischargingtimechange));
    if (m_batteryStatus->level() != oldStatus->level())
        dispatchEvent(Event::create(EventTypeNames::levelchange));
}

void BatteryManager::registerWithDispatcher()
{
    BatteryDispatcher::instance().addController(this);
}

void BatteryManager::unregisterWithDispatcher()
{
    BatteryDispatcher::instance().removeController(this);
}

bool BatteryManager::hasLastData()
{
    return BatteryDispatcher::instance().latestData();
}

void BatteryManager::suspend()
{
    m_hasEventListener = false;
    stopUpdating();
}

void BatteryManager::resume()
{
    m_hasEventListener = true;
    startUpdating();
}

void BatteryManager::stop()
{
    m_hasEventListener = false;
    m_state = NotStarted;
    stopUpdating();
}

bool BatteryManager::hasPendingActivity() const
{
    // Prevent V8 from garbage collecting the wrapper object if there are
    // event listeners attached to it.
    return m_state == Resolved && hasEventListeners();
}

void BatteryManager::trace(Visitor* visitor)
{
    visitor->trace(m_resolver);
    visitor->trace(m_batteryStatus);
    PlatformEventController::trace(visitor);
    RefCountedGarbageCollectedEventTargetWithInlineData<BatteryManager>::trace(visitor);
    ActiveDOMObject::trace(visitor);
}

} // namespace blink
