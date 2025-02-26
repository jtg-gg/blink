/*
 * Copyright (C) 2012 Motorola Mobility Inc.
 * Copyright (C) 2013 Google Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "core/html/PublicURLManager.h"

#include "core/fetch/MemoryCache.h"
#include "core/html/URLRegistry.h"
#include "platform/weborigin/KURL.h"
#include "wtf/Vector.h"
#include "wtf/text/StringHash.h"

namespace blink {

PassOwnPtrWillBeRawPtr<PublicURLManager> PublicURLManager::create(ExecutionContext* context)
{
    OwnPtrWillBeRawPtr<PublicURLManager> publicURLManager = adoptPtrWillBeNoop(new PublicURLManager(context));
    publicURLManager->suspendIfNeeded();
    return publicURLManager.release();
}

PublicURLManager::PublicURLManager(ExecutionContext* context)
    : ActiveDOMObject(context)
    , m_isStopped(false)
{
}

void PublicURLManager::registerURL(SecurityOrigin* origin, const KURL& url, URLRegistrable* registrable, const String& uuid)
{
    if (m_isStopped)
        return;

    RegistryURLMap::ValueType* found = m_registryToURL.add(&registrable->registry(), URLMap()).storedValue;
    found->key->registerURL(origin, url, registrable);
    found->value.add(url.string(), uuid);
}

void PublicURLManager::revoke(const KURL& url)
{
    for (auto& registryUrl : m_registryToURL) {
        if (registryUrl.value.contains(url.string())) {
            registryUrl.key->unregisterURL(url);
            registryUrl.value.remove(url.string());
            break;
        }
    }
}

void PublicURLManager::revoke(const String& uuid)
{
    // A linear scan; revoking by UUID is assumed rare.
    Vector<String> urlsToRemove;
    for (auto& registryUrl : m_registryToURL) {
        URLRegistry* registry = registryUrl.key;
        URLMap& registeredURLs = registryUrl.value;
        for (auto& registeredUrl : registeredURLs) {
            if (uuid == registeredUrl.value) {
                KURL url(ParsedURLString, registeredUrl.key);
                MemoryCache::removeURLFromCache(executionContext(), url);
                registry->unregisterURL(url);
                urlsToRemove.append(registeredUrl.key);
            }
        }
        for (unsigned j = 0; j < urlsToRemove.size(); j++)
            registeredURLs.remove(urlsToRemove[j]);
        urlsToRemove.clear();
    }
}

void PublicURLManager::stop()
{
    if (m_isStopped)
        return;

    m_isStopped = true;
    for (auto& registryUrl : m_registryToURL) {
        for (auto& url : registryUrl.value)
            registryUrl.key->unregisterURL(KURL(ParsedURLString, url.key));
    }

    m_registryToURL.clear();
}

void PublicURLManager::trace(Visitor* visitor)
{
    ActiveDOMObject::trace(visitor);
}

}
