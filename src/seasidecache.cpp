/*
 * Copyright (C) 2013 Jolla Mobile <andrew.den.exter@jollamobile.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "seasidecache.h"

#include "synchronizelists.h"

#include "qtcontacts-extensions_impl.h"
#include "qcontactstatusflags_impl.h"

#include <QCoreApplication>
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QStandardPaths>
#else
#include <QDesktopServices>
#endif
#include <QDir>
#include <QEvent>
#include <QFile>

#include <QContactAvatar>
#include <QContactDetailFilter>
#include <QContactDisplayLabel>
#include <QContactEmailAddress>
#include <QContactFavorite>
#include <QContactGender>
#include <QContactName>
#include <QContactNickname>
#include <QContactOnlineAccount>
#include <QContactOrganization>
#include <QContactPhoneNumber>
#include <QContactGlobalPresence>
#include <QContactSyncTarget>

#include <QVersitContactExporter>
#include <QVersitContactImporter>
#include <QVersitReader>
#include <QVersitWriter>

#include <QtDebug>

#include <mlocale.h>

#ifdef USING_QTPIM
QTVERSIT_USE_NAMESPACE
#endif

namespace {

ML10N::MLocale mLocale;

const QString aggregateRelationshipType =
#ifdef USING_QTPIM
    QContactRelationship::Aggregates();
#else
    QContactRelationship::Aggregates;
#endif

const QString syncTargetLocal = QLatin1String("local");
const QString syncTargetWasLocal = QLatin1String("was_local");

QStringList getAllContactNameGroups()
{
    QStringList groups(mLocale.exemplarCharactersIndex());
    groups.append(QString::fromLatin1("#"));
    return groups;
}

QString managerName()
{
#ifdef USING_QTPIM
    // Temporary override until qtpim supports QTCONTACTS_MANAGER_OVERRIDE
    return QStringLiteral("org.nemomobile.contacts.sqlite");
#endif
    QByteArray environmentManager = qgetenv("NEMO_CONTACT_MANAGER");
    return !environmentManager.isEmpty()
            ? QString::fromLatin1(environmentManager, environmentManager.length())
            : QString();
}

typedef QList<DetailTypeId> DetailList;

template<typename T>
DetailTypeId detailType()
{
#ifdef USING_QTPIM
    return T::Type;
#else
    return T::DefinitionName;
#endif
}

DetailTypeId detailType(const QContactDetail &detail)
{
#ifdef USING_QTPIM
    return detail.type();
#else
    return detail.definitionName();
#endif
}

template<typename T, typename Filter, typename Field>
void setDetailType(Filter &filter, Field field)
{
#ifdef USING_QTPIM
    filter.setDetailType(T::Type, field);
#else
    filter.setDetailDefinitionName(T::DefinitionName, field);
#endif
}

DetailList detailTypesHint(const QContactFetchHint &hint)
{
#ifdef USING_QTPIM
    return hint.detailTypesHint();
#else
    return hint.detailDefinitionsHint();
#endif
}

void setDetailTypesHint(QContactFetchHint &hint, const DetailList &types)
{
#ifdef USING_QTPIM
    hint.setDetailTypesHint(types);
#else
    hint.setDetailDefinitionsHint(types);
#endif
}

QContactFetchHint basicFetchHint()
{
    QContactFetchHint fetchHint;

    // We generally have no use for these things:
    fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships |
                                   QContactFetchHint::NoActionPreferences |
                                   QContactFetchHint::NoBinaryBlobs);

    return fetchHint;
}

QContactFetchHint metadataFetchHint(quint32 fetchTypes = 0)
{
    QContactFetchHint fetchHint(basicFetchHint());

    // Include all detail types which come from the main contacts table
    DetailList types;
    types << detailType<QContactSyncTarget>() <<
             detailType<QContactName>() <<
             detailType<QContactDisplayLabel>() <<
             detailType<QContactFavorite>() <<
             detailType<QContactGender>() <<
             detailType<QContactStatusFlags>();

    if (fetchTypes & SeasideCache::FetchAccountUri) {
        types << detailType<QContactOnlineAccount>();
    }
    if (fetchTypes & SeasideCache::FetchPhoneNumber) {
        types << detailType<QContactPhoneNumber>();
    }
    if (fetchTypes & SeasideCache::FetchEmailAddress) {
        types << detailType<QContactEmailAddress>();
    }

    setDetailTypesHint(fetchHint, types);
    return fetchHint;
}

QContactFetchHint onlineFetchHint(quint32 fetchTypes = 0)
{
    QContactFetchHint fetchHint(metadataFetchHint(fetchTypes));

    // We also need global presence state
    setDetailTypesHint(fetchHint, detailTypesHint(fetchHint) << detailType<QContactGlobalPresence>());
    return fetchHint;
}

QContactFetchHint favoriteFetchHint(quint32 fetchTypes = 0)
{
    QContactFetchHint fetchHint(onlineFetchHint(fetchTypes));

    // We also need avatar info
    setDetailTypesHint(fetchHint, detailTypesHint(fetchHint) << detailType<QContactAvatar>());
    return fetchHint;
}

QContactFilter allFilter()
{
    return QContactFilter();
}

QContactFilter favoriteFilter()
{
    return QContactFavorite::match();
}

QContactFilter nonfavoriteFilter()
{
    QContactDetailFilter filter;
    setDetailType<QContactFavorite>(filter, QContactFavorite::FieldFavorite);
    filter.setMatchFlags(QContactFilter::MatchExactly);
    filter.setValue(false);

    return filter;
}

QContactFilter onlineFilter()
{
    return QContactStatusFlags::matchFlag(QContactStatusFlags::IsOnline);
}

QContactFilter aggregateFilter()
{
    static const QString aggregate(QString::fromLatin1("aggregate"));

    QContactDetailFilter filter;
    setDetailType<QContactSyncTarget>(filter, QContactSyncTarget::FieldSyncTarget);
    filter.setValue(aggregate);

    return filter;
}

typedef QPair<QString, QString> StringPair;

StringPair addressPair(const QContactPhoneNumber &phoneNumber)
{
    return qMakePair(QString(), SeasideCache::normalizePhoneNumber(phoneNumber.number()));
}

StringPair addressPair(const QContactEmailAddress &emailAddress)
{
    return qMakePair(emailAddress.emailAddress().toLower(), QString());
}

StringPair addressPair(const QContactOnlineAccount &account)
{
    return qMakePair(account.value<QString>(QContactOnlineAccount__FieldAccountPath), account.accountUri().toLower());
}

bool ignoreContactForNameGroups(const QContact &contact)
{
    static const QString aggregate(QString::fromLatin1("aggregate"));

    // Don't include the self contact in name groups
    if (SeasideCache::apiId(contact) == SeasideCache::selfContactId()) {
        return true;
    }

    // Also ignore non-aggregate contacts
    QContactSyncTarget syncTarget = contact.detail<QContactSyncTarget>();
    return (syncTarget.syncTarget() != aggregate);
}

QList<quint32> internalIds(const QList<SeasideCache::ContactIdType> &ids)
{
    QList<quint32> rv;
    rv.reserve(ids.count());

    foreach (const SeasideCache::ContactIdType &id, ids) {
        rv.append(SeasideCache::internalId(id));
    }

    return rv;
}

}

SeasideCache *SeasideCache::instancePtr = 0;
QStringList SeasideCache::allContactNameGroups = getAllContactNameGroups();

SeasideCache* SeasideCache::instance()
{
    return instancePtr;
}

SeasideCache::ContactIdType SeasideCache::apiId(const QContact &contact)
{
#ifdef USING_QTPIM
    return contact.id();
#else
    return contact.id().localId();
#endif
}

SeasideCache::ContactIdType SeasideCache::apiId(quint32 iid)
{
    return QtContactsSqliteExtensions::apiContactId(iid);
}

bool SeasideCache::validId(const ContactIdType &id)
{
#ifdef USING_QTPIM
    return !id.isNull();
#else
    return (id != 0);
#endif
}

#ifndef USING_QTPIM
bool SeasideCache::validId(const QContactId &id)
{
    return (id.localId() != 0);
}
#endif

quint32 SeasideCache::internalId(const QContact &contact)
{
    return internalId(contact.id());
}

quint32 SeasideCache::internalId(const QContactId &id)
{
    return QtContactsSqliteExtensions::internalContactId(id);
}

#ifndef USING_QTPIM
quint32 SeasideCache::internalId(QContactLocalId id)
{
    return QtContactsSqliteExtensions::internalContactId(id);
}
#endif

SeasideCache::SeasideCache()
    : m_manager(managerName())
#ifdef HAS_MLITE
    , m_displayLabelOrderConf(QLatin1String("/org/nemomobile/contacts/display_label_order"))
    , m_sortPropertyConf(QLatin1String("/org/nemomobile/contacts/sort_property"))
    , m_groupPropertyConf(QLatin1String("/org/nemomobile/contacts/group_property"))
#endif
    , m_resultsRead(0)
    , m_populated(0)
    , m_cacheIndex(0)
    , m_queryIndex(0)
    , m_fetchProcessedCount(0)
    , m_fetchByIdProcessedCount(0)
    , m_syncFilter(FilterNone)
    , m_displayLabelOrder(FirstNameFirst)
    , m_sortProperty(QString::fromLatin1("firstName"))
    , m_groupProperty(QString::fromLatin1("firstName"))
    , m_keepPopulated(false)
    , m_populateProgress(Unpopulated)
    , m_fetchTypes(0)
    , m_fetchTypesChanged(false)
    , m_updatesPending(false)
    , m_refreshRequired(false)
    , m_contactsUpdated(false)
    , m_activeResolve(0)
{
    Q_ASSERT(!instancePtr);
    instancePtr = this;

    m_timer.start();
    m_fetchPostponed.invalidate();

#ifdef HAS_MLITE
    connect(&m_displayLabelOrderConf, SIGNAL(valueChanged()), this, SLOT(displayLabelOrderChanged()));
    QVariant displayLabelOrder = m_displayLabelOrderConf.value();
    if (displayLabelOrder.isValid())
        m_displayLabelOrder = static_cast<DisplayLabelOrder>(displayLabelOrder.toInt());

    connect(&m_sortPropertyConf, SIGNAL(valueChanged()), this, SLOT(sortPropertyChanged()));
    QVariant sortPropertyConf = m_sortPropertyConf.value();
    if (sortPropertyConf.isValid())
        m_sortProperty = sortPropertyConf.toString();

    connect(&m_groupPropertyConf, SIGNAL(valueChanged()), this, SLOT(groupPropertyChanged()));
    QVariant groupPropertyConf = m_groupPropertyConf.value();
    if (groupPropertyConf.isValid())
        m_groupProperty = groupPropertyConf.toString();
#endif

#ifdef USING_QTPIM
    connect(&m_manager, SIGNAL(dataChanged()), this, SLOT(updateContacts()));
    connect(&m_manager, SIGNAL(contactsAdded(QList<QContactId>)),
            this, SLOT(contactsAdded(QList<QContactId>)));
    connect(&m_manager, SIGNAL(contactsChanged(QList<QContactId>)),
            this, SLOT(contactsChanged(QList<QContactId>)));
    connect(&m_manager, SIGNAL(contactsRemoved(QList<QContactId>)),
            this, SLOT(contactsRemoved(QList<QContactId>)));
#else
    connect(&m_manager, SIGNAL(dataChanged()), this, SLOT(updateContacts()));
    connect(&m_manager, SIGNAL(contactsAdded(QList<QContactLocalId>)),
            this, SLOT(contactsAdded(QList<QContactLocalId>)));
    connect(&m_manager, SIGNAL(contactsChanged(QList<QContactLocalId>)),
            this, SLOT(contactsChanged(QList<QContactLocalId>)));
    connect(&m_manager, SIGNAL(contactsRemoved(QList<QContactLocalId>)),
            this, SLOT(contactsRemoved(QList<QContactLocalId>)));
#endif

    connect(&m_fetchRequest, SIGNAL(resultsAvailable()), this, SLOT(contactsAvailable()));
    connect(&m_fetchByIdRequest, SIGNAL(resultsAvailable()), this, SLOT(contactsAvailable()));
    connect(&m_contactIdRequest, SIGNAL(resultsAvailable()), this, SLOT(contactIdsAvailable()));
    connect(&m_relationshipsFetchRequest, SIGNAL(resultsAvailable()), this, SLOT(relationshipsAvailable()));

    connect(&m_fetchRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_fetchByIdRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_contactIdRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_relationshipsFetchRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_removeRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_saveRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_relationshipSaveRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_relationshipRemoveRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));

    m_fetchRequest.setManager(&m_manager);
    m_fetchByIdRequest.setManager(&m_manager);
    m_contactIdRequest.setManager(&m_manager);
    m_relationshipsFetchRequest.setManager(&m_manager);
    m_removeRequest.setManager(&m_manager);
    m_saveRequest.setManager(&m_manager);
    m_relationshipSaveRequest.setManager(&m_manager);
    m_relationshipRemoveRequest.setManager(&m_manager);

    setSortOrder(m_sortProperty);
}

SeasideCache::~SeasideCache()
{
    if (instancePtr == this)
        instancePtr = 0;
}

void SeasideCache::checkForExpiry()
{
    if (instancePtr->m_users.isEmpty()) {
        bool unused = true;
        for (int i = 0; i < FilterTypesCount; ++i) {
            unused &= instancePtr->m_models[i].isEmpty();
        }
        if (unused) {
            instancePtr->m_expiryTimer.start(30000, instancePtr);
        }
    }
}

void SeasideCache::registerModel(ListModel *model, FilterType type, FetchDataType fetchTypes)
{
    if (!instancePtr) {
        new SeasideCache;
    } else {
        instancePtr->m_expiryTimer.stop();
        for (int i = 0; i < FilterTypesCount; ++i)
            instancePtr->m_models[i].removeAll(model);
    }

    instancePtr->m_models[type].append(model);
    instancePtr->keepPopulated(fetchTypes);
}

void SeasideCache::unregisterModel(ListModel *model)
{
    for (int i = 0; i < FilterTypesCount; ++i)
        instancePtr->m_models[i].removeAll(model);

    checkForExpiry();
}

void SeasideCache::registerUser(QObject *user)
{
    if (!instancePtr) {
        new SeasideCache;
    } else {
        instancePtr->m_expiryTimer.stop();
    }
    instancePtr->m_users.insert(user);
}

void SeasideCache::unregisterUser(QObject *user)
{
    instancePtr->m_users.remove(user);

    checkForExpiry();
}

void SeasideCache::registerNameGroupChangeListener(SeasideNameGroupChangeListener *listener)
{
    if (!instancePtr)
        new SeasideCache;
    instancePtr->m_nameGroupChangeListeners.append(listener);
}

void SeasideCache::unregisterNameGroupChangeListener(SeasideNameGroupChangeListener *listener)
{
    if (!instancePtr)
        return;
    instancePtr->m_nameGroupChangeListeners.removeAll(listener);
}

void SeasideCache::registerChangeListener(ChangeListener *listener)
{
    if (!instancePtr)
        new SeasideCache;
    instancePtr->m_changeListeners.append(listener);
}

void SeasideCache::unregisterChangeListener(ChangeListener *listener)
{
    if (!instancePtr)
        return;
    instancePtr->m_changeListeners.removeAll(listener);
}

void SeasideCache::unregisterResolveListener(ResolveListener *listener)
{
    if (!instancePtr)
        return;

    // We might have outstanding resolve requests for this listener
    if (instancePtr->m_activeResolve && (instancePtr->m_activeResolve->listener == listener)) {
        instancePtr->m_activeResolve = 0;
    }

    QList<ResolveData>::iterator it = instancePtr->m_resolveAddresses.begin();
    while (it != instancePtr->m_resolveAddresses.end()) {
        if (it->listener == listener) {
            it = instancePtr->m_resolveAddresses.erase(it);
        } else {
            ++it;
        }
    }

    it = instancePtr->m_unknownAddresses.begin();
    while (it != instancePtr->m_unknownAddresses.end()) {
        if (it->listener == listener) {
            it = instancePtr->m_unknownAddresses.erase(it);
        } else {
            ++it;
        }
    }
}

void SeasideCache::setNameGrouper(SeasideNameGrouper *grouper)
{
    if (!instancePtr)
        new SeasideCache;
    instancePtr->m_nameGrouper.reset(grouper);

    allContactNameGroups = getAllContactNameGroups();
    QStringList groups = instancePtr->m_nameGrouper->allNameGroups();
    for (int i = groups.count() - 1; i > -1; --i) {
        const QString &group = groups.at(i);
        if (!allContactNameGroups.contains(group))
            allContactNameGroups.prepend(group);
    }
}

QString SeasideCache::nameGroup(const CacheItem *cacheItem)
{
    if (!cacheItem)
        return QString();

    return cacheItem->nameGroup;
}

QString SeasideCache::determineNameGroup(const CacheItem *cacheItem)
{
    if (!cacheItem)
        return QString();

    if (!instancePtr->m_nameGrouper.isNull()) {
        QString group = instancePtr->m_nameGrouper->nameGroupForContact(cacheItem->contact, instancePtr->m_groupProperty);
        if (!group.isNull()) {
            return group;
        }
    }

    const QContactName name(cacheItem->contact.detail<QContactName>());
    const QString nameProperty(instancePtr->m_groupProperty == QString::fromLatin1("firstName") ? name.firstName() : name.lastName());

    QString group;
    if (!nameProperty.isEmpty()) {
        group = mLocale.indexBucket(nameProperty);
    } else  if (!cacheItem->displayLabel.isEmpty()) {
        group = mLocale.indexBucket(cacheItem->displayLabel);
    }

    if (group.isNull() || !allContactNameGroups.contains(group)) {
        QString displayLabel = generateDisplayLabelFromNonNameDetails(cacheItem->contact);
        if (!displayLabel.isEmpty())
            group = mLocale.indexBucket(displayLabel);
    }
    if (group.isNull() || !allContactNameGroups.contains(group)) {
        group = QString::fromLatin1("#");   // 'other' group
    }
    return group;
}

QStringList SeasideCache::allNameGroups()
{
    if (!instancePtr)
        new SeasideCache;
    return allContactNameGroups;
}

QHash<QString, QSet<quint32> > SeasideCache::nameGroupMembers()
{
    if (instancePtr)
        return instancePtr->m_contactNameGroups;
    return QHash<QString, QSet<quint32> >();
}

SeasideCache::DisplayLabelOrder SeasideCache::displayLabelOrder()
{
    return instancePtr->m_displayLabelOrder;
}

QString SeasideCache::sortProperty()
{
    return instancePtr->m_sortProperty;
}

QString SeasideCache::groupProperty()
{
    return instancePtr->m_groupProperty;
}

int SeasideCache::contactId(const QContact &contact)
{
    quint32 internal = internalId(contact);
    return static_cast<int>(internal);
}

SeasideCache::CacheItem *SeasideCache::itemById(const ContactIdType &id, bool requireComplete)
{
    if (!validId(id))
        return 0;

    quint32 iid = internalId(id);

    CacheItem *item = 0;

    QHash<quint32, CacheItem>::iterator it = instancePtr->m_people.find(iid);
    if (it != instancePtr->m_people.end()) {
        item = &(*it);
    } else {
        // Insert a new item into the cache if the one doesn't exist.
        item = &(instancePtr->m_people[iid]);
#ifdef USING_QTPIM
        item->contact.setId(id);
#else
        QContactId contactId;
        contactId.setLocalId(id);
        item->contact.setId(contactId);
#endif
    }

    if (requireComplete) {
        ensureCompletion(item);
    }
    return item;
}

#ifdef USING_QTPIM
SeasideCache::CacheItem *SeasideCache::itemById(int id, bool requireComplete)
{
    if (id != 0) {
        QContactId contactId(apiId(static_cast<quint32>(id)));
        if (!contactId.isNull()) {
            return itemById(contactId, requireComplete);
        }
    }

    return 0;
}
#endif

SeasideCache::CacheItem *SeasideCache::existingItem(const ContactIdType &id)
{
#ifdef USING_QTPIM
    return existingItem(internalId(id));
#else
    QHash<quint32, CacheItem>::iterator it = instancePtr->m_people.find(id);
    return it != instancePtr->m_people.end()
            ? &(*it)
            : 0;
#endif
}

#ifdef USING_QTPIM
SeasideCache::CacheItem *SeasideCache::existingItem(quint32 iid)
{
    QHash<quint32, CacheItem>::iterator it = instancePtr->m_people.find(iid);
    return it != instancePtr->m_people.end()
            ? &(*it)
            : 0;
}
#endif

QContact SeasideCache::contactById(const ContactIdType &id)
{
    quint32 iid = internalId(id);
    return instancePtr->m_people.value(iid, CacheItem()).contact;
}

void SeasideCache::ensureCompletion(CacheItem *cacheItem)
{
    if (cacheItem->contactState < ContactRequested) {
        refreshContact(cacheItem);
    }
}

void SeasideCache::refreshContact(CacheItem *cacheItem)
{
    cacheItem->contactState = ContactRequested;
    instancePtr->m_changedContacts.append(cacheItem->apiId());
    instancePtr->fetchContacts();
}

SeasideCache::CacheItem *SeasideCache::itemByPhoneNumber(const QString &number, bool requireComplete)
{
    QString normalizedNumber = normalizePhoneNumber(number);
    QHash<QString, quint32>::const_iterator it = instancePtr->m_phoneNumberIds.find(normalizedNumber);
    if (it != instancePtr->m_phoneNumberIds.end())
        return itemById(*it, requireComplete);

    return 0;
}

SeasideCache::CacheItem *SeasideCache::itemByEmailAddress(const QString &email, bool requireComplete)
{
    QHash<QString, quint32>::const_iterator it = instancePtr->m_emailAddressIds.find(email.toLower());
    if (it != instancePtr->m_emailAddressIds.end())
        return itemById(*it, requireComplete);

    return 0;
}

SeasideCache::CacheItem *SeasideCache::itemByOnlineAccount(const QString &localUid, const QString &remoteUid, bool requireComplete)
{
    QPair<QString, QString> address = qMakePair(localUid, remoteUid.toLower());

    QHash<QPair<QString, QString>, quint32>::const_iterator it = instancePtr->m_onlineAccountIds.find(address);
    if (it != instancePtr->m_onlineAccountIds.end())
        return itemById(*it, requireComplete);

    return 0;
}

SeasideCache::CacheItem *SeasideCache::resolvePhoneNumber(ResolveListener *listener, const QString &number, bool requireComplete)
{
    CacheItem *item = itemByPhoneNumber(number, requireComplete);
    if (!item) {
        instancePtr->resolveAddress(listener, QString(), number, requireComplete);
    } else if (requireComplete) {
        ensureCompletion(item);
    }

    return item;
}

SeasideCache::CacheItem *SeasideCache::resolveEmailAddress(ResolveListener *listener, const QString &address, bool requireComplete)
{
    CacheItem *item = itemByEmailAddress(address, requireComplete);
    if (!item) {
        instancePtr->resolveAddress(listener, address, QString(), requireComplete);
    } else if (requireComplete) {
        ensureCompletion(item);
    }
    return item;
}

SeasideCache::CacheItem *SeasideCache::resolveOnlineAccount(ResolveListener *listener, const QString &localUid, const QString &remoteUid, bool requireComplete)
{
    CacheItem *item = itemByOnlineAccount(localUid, remoteUid, requireComplete);
    if (!item) {
        instancePtr->resolveAddress(listener, localUid, remoteUid, requireComplete);
    } else if (requireComplete) {
        ensureCompletion(item);
    }
    return item;
}

SeasideCache::ContactIdType SeasideCache::selfContactId()
{
    return instancePtr->m_manager.selfContactId();
}

void SeasideCache::requestUpdate()
{
    if (!m_updatesPending)
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    m_updatesPending = true;
}

bool SeasideCache::saveContact(const QContact &contact)
{
    ContactIdType id = apiId(contact);
    if (validId(id)) {
        instancePtr->m_contactsToSave[id] = contact;
        instancePtr->contactDataChanged(internalId(id));
    } else {
        instancePtr->m_contactsToCreate.append(contact);
    }

    instancePtr->requestUpdate();

    return true;
}

void SeasideCache::contactDataChanged(quint32 iid)
{
    instancePtr->contactDataChanged(iid, FilterFavorites);
    instancePtr->contactDataChanged(iid, FilterOnline);
    instancePtr->contactDataChanged(iid, FilterAll);
}

void SeasideCache::contactDataChanged(quint32 iid, FilterType filter)
{
    int row = contactIndex(iid, filter);
    if (row != -1) {
        QList<ListModel *> &models = m_models[filter];
        for (int i = 0; i < models.count(); ++i) {
            models.at(i)->sourceDataChanged(row, row);
        }
    }
}

bool SeasideCache::removeContact(const QContact &contact)
{
    ContactIdType id = apiId(contact);
    if (!validId(id))
        return false;

    instancePtr->m_contactsToRemove.append(id);

    quint32 iid = internalId(id);
    instancePtr->removeContactData(iid, FilterFavorites);
    instancePtr->removeContactData(iid, FilterOnline);
    instancePtr->removeContactData(iid, FilterAll);

    instancePtr->requestUpdate();
    return true;
}

void SeasideCache::removeContactData(quint32 iid, FilterType filter)
{
    int row = contactIndex(iid, filter);
    if (row == -1)
        return;

    QList<ListModel *> &models = m_models[filter];
    for (int i = 0; i < models.count(); ++i)
        models.at(i)->sourceAboutToRemoveItems(row, row);

    m_contactIndices[filter].remove(m_contacts[filter].at(row));
    m_contacts[filter].removeAt(row);

    if (filter == FilterAll) {
        const QString group(nameGroup(existingItem(iid)));
        if (!group.isNull()) {
            QSet<QString> modifiedNameGroups;
            removeFromContactNameGroup(iid, group, &modifiedNameGroups);
            notifyNameGroupsChanged(modifiedNameGroups);
        }
    }

    for (int i = 0; i < models.count(); ++i)
        models.at(i)->sourceItemsRemoved();
}

bool SeasideCache::fetchConstituents(const QContact &contact)
{
    QContactId personId(contact.id());
    if (!validId(personId))
        return false;

    if (!instancePtr->m_contactsToFetchConstituents.contains(personId)) {
        instancePtr->m_contactsToFetchConstituents.append(personId);
        instancePtr->requestUpdate();
    }
    return true;
}

bool SeasideCache::fetchMergeCandidates(const QContact &contact)
{
    QContactId personId(contact.id());
    if (!validId(personId))
        return false;

    if (!instancePtr->m_contactsToFetchCandidates.contains(personId)) {
        instancePtr->m_contactsToFetchCandidates.append(personId);
        instancePtr->requestUpdate();
    }
    return true;
}

const QList<quint32> *SeasideCache::contacts(FilterType type)
{
    return &instancePtr->m_contacts[type];
}

bool SeasideCache::isPopulated(FilterType filterType)
{
    return instancePtr->m_populated & (1 << filterType);
}

// small helper to avoid inconvenience
QString SeasideCache::generateDisplayLabel(const QContact &contact, DisplayLabelOrder order)
{
    QContactName name = contact.detail<QContactName>();

    QString displayLabel;

    QString nameStr1;
    QString nameStr2;
    if (order == LastNameFirst) {
        nameStr1 = name.lastName();
        nameStr2 = name.firstName();
    } else {
        nameStr1 = name.firstName();
        nameStr2 = name.lastName();
    }

    if (!nameStr1.isEmpty())
        displayLabel.append(nameStr1);

    if (!nameStr2.isEmpty()) {
        if (!displayLabel.isEmpty())
            displayLabel.append(" ");
        displayLabel.append(nameStr2);
    }

    if (!displayLabel.isEmpty()) {
        return displayLabel;
    }

    // Try to generate a label from the contact details, in our preferred order
    displayLabel = generateDisplayLabelFromNonNameDetails(contact);
    if (!displayLabel.isEmpty()) {
        return displayLabel;
    }

    // If no label was determined from any of the available detail, fallback to the backend's label
    displayLabel = contact.detail<QContactDisplayLabel>().label();
    if (!displayLabel.isEmpty()) {
        return displayLabel;
    }

    return "(Unnamed)"; // TODO: localisation
}

QString SeasideCache::generateDisplayLabelFromNonNameDetails(const QContact &contact)
{
    foreach (const QContactNickname& nickname, contact.details<QContactNickname>()) {
        if (!nickname.nickname().isEmpty()) {
            return nickname.nickname();
        }
    }

    foreach (const QContactGlobalPresence& gp, contact.details<QContactGlobalPresence>()) {
        // should only be one of these, but qtct is strange, and doesn't list it as a unique detail in the schema...
        if (!gp.nickname().isEmpty()) {
            return gp.nickname();
        }
    }

    foreach (const QContactPresence& presence, contact.details<QContactPresence>()) {
        if (!presence.nickname().isEmpty()) {
            return presence.nickname();
        }
    }

    foreach (const QContactOnlineAccount& account, contact.details<QContactOnlineAccount>()) {
        if (!account.accountUri().isEmpty()) {
            return account.accountUri();
        }
    }

    foreach (const QContactEmailAddress& email, contact.details<QContactEmailAddress>()) {
        if (!email.emailAddress().isEmpty()) {
            return email.emailAddress();
        }
    }

    QContactOrganization company = contact.detail<QContactOrganization>();
    if (!company.name().isEmpty()) {
        return company.name();
    }

    foreach (const QContactPhoneNumber& phone, contact.details<QContactPhoneNumber>()) {
        if (!phone.number().isEmpty())
            return phone.number();
    }

    return QString();
}

static QUrl avatarUrlWithMetadata(const QContact &c, const QString &metadataFragment = QString())
{
    QList<int> matchingIndices;
    QList<QContactAvatar> avatarDetails = c.details<QContactAvatar>();
    for (int i = 0; i < avatarDetails.size(); ++i) {
        const QContactAvatar &av(avatarDetails[i]);
        QUrl avatarImageUrl = av.imageUrl();
        if (avatarImageUrl.isEmpty()) {
            // ignore empty avatars.
            continue;
        } else if (metadataFragment.isEmpty() && av.value(QContactAvatar__FieldAvatarMetadata).toString() == QString(QLatin1String("local"))) {
            // prefer "local" avatars if no metadata filter is specified.
            return avatarImageUrl;
        } else if (metadataFragment.isEmpty() || av.value(QContactAvatar__FieldAvatarMetadata).toString().startsWith(metadataFragment)) {
            // prefer file system images if possible
            if (!avatarImageUrl.scheme().isEmpty() && avatarImageUrl.scheme() != "file") {
                // queue it as fallback.
                matchingIndices.append(i);
            } else {
                // return this local file path image.
                return avatarImageUrl;
            }
        } else {
            // this avatar doesn't match the metadata requirement.  ignore it.
            continue;
        }
    }

    // no local file path avatar image; use the first fallback.
    if (matchingIndices.size()) {
        return avatarDetails[matchingIndices[0]].imageUrl();
    }

    // no matching avatar image.
    return QUrl();
}

QUrl SeasideCache::filteredAvatarUrl(const QContact &contact, const QStringList &metadataFragments)
{
    if (metadataFragments.isEmpty()) {
        return avatarUrlWithMetadata(contact);
    }

    foreach (const QString &metadataFragment, metadataFragments) {
        QUrl matchingUrl = avatarUrlWithMetadata(contact, metadataFragment);
        if (!matchingUrl.isEmpty()) {
            return matchingUrl;
        }
    }

    return QUrl();
}

QString SeasideCache::normalizePhoneNumber(const QString &input)
{
    // TODO: use a configuration variable to make this configurable
    static const int maxCharacters = 7;

    return QtContactsSqliteExtensions::minimizePhoneNumber(input, maxCharacters);
}

static QContactFilter filterForMergeCandidates(const QContact &contact)
{
    // Find any contacts that we might merge with the supplied contact
    QContactFilter rv;

    QContactName name(contact.detail<QContactName>());
    QString firstName(name.firstName());
    QString lastName(name.lastName());

    if (firstName.isEmpty() && lastName.isEmpty()) {
        // Use the displayLabel to match with
        QString label(contact.detail<QContactDisplayLabel>().label());

        // Partial match to first name
        QContactDetailFilter firstNameFilter;
        setDetailType<QContactName>(firstNameFilter, QContactName::FieldFirstName);
        firstNameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
        firstNameFilter.setValue(label);
        rv = rv | firstNameFilter;

        // Partial match to last name
        QContactDetailFilter lastNameFilter;
        setDetailType<QContactName>(lastNameFilter, QContactName::FieldLastName);
        lastNameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
        lastNameFilter.setValue(label);
        rv = rv | lastNameFilter;

        // Partial match to nickname
        QContactDetailFilter nicknameFilter;
        setDetailType<QContactNickname>(nicknameFilter, QContactNickname::FieldNickname);
        nicknameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
        nicknameFilter.setValue(label);
        rv = rv | nicknameFilter;
    } else {
        if (!firstName.isEmpty()) {
            // Partial match to first name
            QContactDetailFilter nameFilter;
            setDetailType<QContactName>(nameFilter, QContactName::FieldFirstName);
            nameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
            nameFilter.setValue(firstName);
            rv = rv | nameFilter;

            // Partial match to first name in the nickname
            QContactDetailFilter nicknameFilter;
            setDetailType<QContactNickname>(nicknameFilter, QContactNickname::FieldNickname);
            nicknameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
            nicknameFilter.setValue(firstName);
            rv = rv | nicknameFilter;
        }
        if (!lastName.isEmpty()) {
            // Partial match to last name
            QContactDetailFilter nameFilter;
            setDetailType<QContactName>(nameFilter, QContactName::FieldLastName);
            nameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
            nameFilter.setValue(lastName);
            rv = rv | nameFilter;

            // Partial match to last name in the nickname
            QContactDetailFilter nicknameFilter;
            setDetailType<QContactNickname>(nicknameFilter, QContactNickname::FieldNickname);
            nicknameFilter.setMatchFlags(QContactFilter::MatchContains | QContactFilter::MatchFixedString);
            nicknameFilter.setValue(lastName);
            rv = rv | nicknameFilter;
        }
    }

    // Phone number match
    foreach (const QContactPhoneNumber &number, contact.details<QContactPhoneNumber>()) {
        rv = rv | QContactPhoneNumber::match(number.number());
    }

    // Email address match
    foreach (const QContactEmailAddress &emailAddress, contact.details<QContactEmailAddress>()) {
        QString address(emailAddress.emailAddress());
        int index = address.indexOf(QChar::fromLatin1('@'));
        if (index > 0) {
            // Match any address that is the same up to the @ symbol
            address = address.left(index);
        }

        QContactDetailFilter filter;
        setDetailType<QContactEmailAddress>(filter, QContactEmailAddress::FieldEmailAddress);
        filter.setMatchFlags((index > 0 ? QContactFilter::MatchStartsWith : QContactFilter::MatchExactly) | QContactFilter::MatchFixedString);
        filter.setValue(address);
        rv = rv | filter;
    }

    // Account URI match
    foreach (const QContactOnlineAccount &account, contact.details<QContactOnlineAccount>()) {
        QString uri(account.accountUri());
        int index = uri.indexOf(QChar::fromLatin1('@'));
        if (index > 0) {
            // Match any account URI that is the same up to the @ symbol
            uri = uri.left(index);
        }

        QContactDetailFilter filter;
        setDetailType<QContactOnlineAccount>(filter, QContactOnlineAccount::FieldAccountUri);
        filter.setMatchFlags((index > 0 ? QContactFilter::MatchStartsWith : QContactFilter::MatchExactly) | QContactFilter::MatchFixedString);
        filter.setValue(uri);
        rv = rv | filter;
    }

    // Only return aggregate contact IDs
    return rv & aggregateFilter();
}

bool SeasideCache::event(QEvent *event)
{
    if (event->type() != QEvent::UpdateRequest)
        return QObject::event(event);

    // Test these conditions in priority order
    if ((!m_relationshipsToSave.isEmpty() && !m_relationshipSaveRequest.isActive()) ||
        (!m_relationshipsToRemove.isEmpty() && !m_relationshipRemoveRequest.isActive())) {
        // this has to be before contact saves are processed so that the disaggregation flow
        // works properly
        if (!m_relationshipsToSave.isEmpty()) {
            m_relationshipSaveRequest.setRelationships(m_relationshipsToSave);
            m_relationshipSaveRequest.start();
            m_relationshipsToSave.clear();
        }
        if (!m_relationshipsToRemove.isEmpty()) {
            m_relationshipRemoveRequest.setRelationships(m_relationshipsToRemove);
            m_relationshipRemoveRequest.start();
            m_relationshipsToRemove.clear();
        }

    } else if (!m_contactsToRemove.isEmpty() && !m_removeRequest.isActive()) {
        m_removeRequest.setContactIds(m_contactsToRemove);
        m_removeRequest.start();

        m_contactsToRemove.clear();
    } else if ((!m_contactsToCreate.isEmpty() || !m_contactsToSave.isEmpty()) && !m_saveRequest.isActive()) {
        m_contactsToCreate.reserve(m_contactsToCreate.count() + m_contactsToSave.count());

        typedef QHash<ContactIdType, QContact>::iterator iterator;
        for (iterator it = m_contactsToSave.begin(); it != m_contactsToSave.end(); ++it) {
            m_contactsToCreate.append(*it);
        }

        m_saveRequest.setContacts(m_contactsToCreate);
        m_saveRequest.start();

        m_contactsToCreate.clear();
        m_contactsToSave.clear();
    } else if (!m_constituentIds.isEmpty() && !m_fetchByIdRequest.isActive()) {
        // Fetch the constituent information (even if they're already in the
        // cache, because we don't update non-aggregates on change notifications)
#ifdef USING_QTPIM
        m_fetchByIdRequest.setIds(m_constituentIds.toList());
#else
        m_fetchByIdRequest.setLocalIds(m_constituentIds.toList());
#endif
        m_fetchByIdRequest.start();

        m_fetchByIdProcessedCount = 0;
    } else if (!m_contactsToFetchConstituents.isEmpty() && !m_relationshipsFetchRequest.isActive()) {
        QContactId aggregateId = m_contactsToFetchConstituents.first();

        // Find the constituents of this contact
#ifdef USING_QTPIM
        QContact first;
        first.setId(aggregateId);
        m_relationshipsFetchRequest.setFirst(first);
        m_relationshipsFetchRequest.setRelationshipType(QContactRelationship::Aggregates());
#else
        m_relationshipsFetchRequest.setFirst(aggregateId);
        m_relationshipsFetchRequest.setRelationshipType(QContactRelationship::Aggregates);
#endif

        m_relationshipsFetchRequest.start();
    } else if (!m_contactsToFetchCandidates.isEmpty() && !m_contactIdRequest.isActive()) {
#ifdef USING_QTPIM
        ContactIdType contactId(m_contactsToFetchCandidates.first());
#else
        ContactIdType contactId(m_contactsToFetchCandidates.first().localId());
#endif
        const QContact contact(contactById(contactId));

        // Find candidates to merge with this contact
        m_contactIdRequest.setFilter(filterForMergeCandidates(contact));
        m_contactIdRequest.setSorting(m_sortOrder);
        m_contactIdRequest.start();
    } else if ((m_populateProgress == Unpopulated) && m_keepPopulated && !m_fetchRequest.isActive()) {
        // Start a query to fully populate the cache, starting with favorites
        m_fetchRequest.setFilter(favoriteFilter());
        m_fetchRequest.setFetchHint(favoriteFetchHint(m_fetchTypes));
        m_fetchRequest.setSorting(m_sortOrder);
        m_fetchRequest.start();

        m_fetchProcessedCount = 0;
        m_populateProgress = FetchFavorites;
    } else if ((m_populateProgress == Populated) && m_fetchTypesChanged && !m_fetchRequest.isActive()) {
        // We need to refetch the metadata for all contacts (because the required data changed)
        m_fetchRequest.setFilter(favoriteFilter());
        m_fetchRequest.setFetchHint(favoriteFetchHint(m_fetchTypes));
        m_fetchRequest.setSorting(m_sortOrder);
        m_fetchRequest.start();

        m_fetchProcessedCount = 0;
        m_fetchTypesChanged = false;
        m_populateProgress = RefetchFavorites;
    } else if (!m_changedContacts.isEmpty() && !m_fetchRequest.isActive()) {
        m_resultsRead = 0;

#ifdef USING_QTPIM
        QContactIdFilter filter;
#else
        QContactLocalIdFilter filter;
#endif
        filter.setIds(m_changedContacts);
        m_changedContacts.clear();

        // A local ID filter will fetch all contacts, rather than just aggregates;
        // we only want to retrieve aggregate contacts that have changed
        m_fetchRequest.setFilter(filter & aggregateFilter());
        m_fetchRequest.setFetchHint(basicFetchHint());
        m_fetchRequest.setSorting(m_sortOrder);
        m_fetchRequest.start();

        m_fetchProcessedCount = 0;
    } else if (!m_resolveAddresses.isEmpty() && !m_fetchRequest.isActive()) {
        const ResolveData &resolve = m_resolveAddresses.first();

        if (resolve.first.isEmpty()) {
            // Search for phone number
            m_fetchRequest.setFilter(QContactPhoneNumber::match(resolve.second));
        } else if (resolve.second.isEmpty()) {
            // Search for email address
            QContactDetailFilter detailFilter;
            setDetailType<QContactEmailAddress>(detailFilter, QContactEmailAddress::FieldEmailAddress);
            detailFilter.setMatchFlags(QContactFilter::MatchExactly | QContactFilter::MatchFixedString); // allow case insensitive
            detailFilter.setValue(resolve.first);

            m_fetchRequest.setFilter(detailFilter);
        } else {
            // Search for online account
            QContactDetailFilter localFilter;
            setDetailType<QContactOnlineAccount>(localFilter, QContactOnlineAccount__FieldAccountPath);
            localFilter.setValue(resolve.first);

            QContactDetailFilter remoteFilter;
            setDetailType<QContactOnlineAccount>(remoteFilter, QContactOnlineAccount::FieldAccountUri);
            remoteFilter.setMatchFlags(QContactFilter::MatchExactly | QContactFilter::MatchFixedString); // allow case insensitive
            remoteFilter.setValue(resolve.second);

            m_fetchRequest.setFilter(localFilter & remoteFilter);
        }

        // If completion is not required, we need to at least retrieve as much detail
        // as the favorites store, so we don't update any favorite with a smaller data subset
        m_activeResolve = &resolve;
        m_fetchRequest.setFetchHint(resolve.requireComplete ? basicFetchHint() : favoriteFetchHint(m_fetchTypes));
        m_fetchRequest.setSorting(m_sortOrder);
        m_fetchRequest.start();

        m_fetchProcessedCount = 0;
    } else if (m_refreshRequired && !m_contactIdRequest.isActive()) {
        m_refreshRequired = false;

        m_resultsRead = 0;
        m_syncFilter = FilterFavorites;
        m_contactIdRequest.setFilter(favoriteFilter());
        m_contactIdRequest.setSorting(m_sortOrder);
        m_contactIdRequest.start();
    } else {
        m_updatesPending = false;

        const QHash<ContactIdType,int> expiredContacts = m_expiredContacts;
        m_expiredContacts.clear();

        typedef QHash<ContactIdType,int>::const_iterator iterator;
        for (iterator it = expiredContacts.begin(); it != expiredContacts.end(); ++it) {
            if (*it >= 0)
                continue;

            quint32 iid = internalId(it.key());
            QHash<quint32, CacheItem>::iterator cacheItem = m_people.find(iid);
            if (cacheItem != m_people.end()) {
                delete cacheItem->itemData;
                m_people.erase(cacheItem);
            }
        }
    }
    return true;
}

void SeasideCache::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_fetchTimer.timerId()) {
        fetchContacts();
    }

    if (event->timerId() == m_expiryTimer.timerId()) {
        m_expiryTimer.stop();
        instancePtr = 0;
        deleteLater();
    }
}

void SeasideCache::contactsAdded(const QList<ContactIdType> &ids)
{
    if (m_keepPopulated) {
        updateContacts(ids);
    }
}

void SeasideCache::contactsChanged(const QList<ContactIdType> &ids)
{
    if (m_keepPopulated) {
        updateContacts(ids);
    } else {
        // Update these contacts if they're already in the cache
        QList<ContactIdType> presentIds;
        foreach (const ContactIdType &id, ids) {
            if (existingItem(id)) {
                presentIds.append(id);
            }
        }
        updateContacts(presentIds);
    }
}

void SeasideCache::contactsRemoved(const QList<ContactIdType> &ids)
{
    QList<ContactIdType> presentIds;

    foreach (const ContactIdType &id, ids) {
        if (CacheItem *item = existingItem(id)) {
            // Report this item is about to be removed
            foreach (ChangeListener *listener, m_changeListeners) {
                listener->itemAboutToBeRemoved(item);
            }

            ItemListener *listener = item->listeners;
            while (listener) {
                ItemListener *next = listener->next;
                listener->itemAboutToBeRemoved(item);
                listener = next;
            }
            item->listeners = 0;

            if (!m_keepPopulated) {
                presentIds.append(id);
            }
        }
    }

    if (m_keepPopulated) {
        m_refreshRequired = true;
    } else {
        // Remove these contacts if they're already in the cache; they won't be removed by syncing
        foreach (const ContactIdType &id, presentIds) {
            m_expiredContacts[id] += -1;
        }
    }

    requestUpdate();
}

void SeasideCache::updateContacts()
{
    QList<ContactIdType> contactIds;

    typedef QHash<quint32, CacheItem>::iterator iterator;
    for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
        if (it->contactState != ContactAbsent)
            contactIds.append(it->apiId());
    }

    updateContacts(contactIds);
}

void SeasideCache::fetchContacts()
{
    static const int WaitIntervalMs = 250;

    if (m_fetchRequest.isActive()) {
        // The current fetch is still active - we may as well continue to accumulate
        m_fetchTimer.start(WaitIntervalMs, this);
    } else {
        m_fetchTimer.stop();
        m_fetchPostponed.invalidate();

        // Fetch any changed contacts immediately
        if (m_contactsUpdated) {
            m_contactsUpdated = false;
            if (m_keepPopulated) {
                // Refresh our contact sets in case sorting has changed
                m_refreshRequired = true;
            }
        }
        requestUpdate();
    }
}

void SeasideCache::updateContacts(const QList<ContactIdType> &contactIds)
{
    // Wait for new changes to be reported
    static const int PostponementIntervalMs = 500;

    // Maximum wait until we fetch all changes previously reported
    static const int MaxPostponementMs = 5000;

    if (!contactIds.isEmpty()) {
        m_contactsUpdated = true;
        m_changedContacts.append(contactIds);

        if (m_fetchPostponed.isValid()) {
            // We are waiting to accumulate further changes
            int remainder = MaxPostponementMs - m_fetchPostponed.elapsed();
            if (remainder > 0) {
                // We can postpone further
                m_fetchTimer.start(std::min(remainder, PostponementIntervalMs), this);
            }
        } else {
            // Wait for further changes before we query for the ones we have now
            m_fetchPostponed.restart();
            m_fetchTimer.start(PostponementIntervalMs, this);
        }
    }
}

void SeasideCache::updateCache(CacheItem *item, const QContact &contact, bool partialFetch)
{
    if (item->contactState < ContactRequested) {
        item->contactState = partialFetch ? ContactPartial : ContactComplete;
    } else if (!partialFetch) {
        // Don't set a complete contact back after a partial update
        item->contactState = ContactComplete;
    }

    item->statusFlags = contact.detail<QContactStatusFlags>().flagsValue();

    if (item->itemData) {
        item->itemData->updateContact(contact, &item->contact, item->contactState);
    } else {
        item->contact = contact;
    }

    item->displayLabel = generateDisplayLabel(item->contact, m_displayLabelOrder);
    item->nameGroup = determineNameGroup(item);

    reportItemUpdated(item);
}

void SeasideCache::reportItemUpdated(CacheItem *item)
{
    // Report the change to this contact
    ItemListener *listener = item->listeners;
    while (listener) {
        listener->itemUpdated(item);
        listener = listener->next;
    }

    foreach (ChangeListener *listener, m_changeListeners) {
        listener->itemUpdated(item);
    }
}

void SeasideCache::resolveUnknownAddresses(const QString &first, const QString &second, CacheItem *item)
{
    QList<ResolveData>::iterator it = instancePtr->m_unknownAddresses.begin();
    while (it != instancePtr->m_unknownAddresses.end()) {
        bool resolved = false;

        if (first == QString()) {
            // This is a phone number - test in normalized form
            resolved = (it->first == QString()) && (it->compare == second);
        } else if (second == QString()) {
            // Email address - compare in lowercased form
            resolved = (it->compare == first) && (it->second == QString());
        } else {
            // Online account - compare URI in lowercased form
            resolved = (it->first == first) && (it->compare == second);
        }

        if (resolved) {
            // Inform the listener of resolution
            it->listener->addressResolved(it->first, it->second, item);

            // Do we need to request completion as well?
            if (it->requireComplete) {
                ensureCompletion(item);
            }

            it = instancePtr->m_unknownAddresses.erase(it);
        } else {
            ++it;
        }
    }
}

bool SeasideCache::updateContactIndexing(const QContact &oldContact, const QContact &contact, quint32 iid, const QSet<DetailTypeId> &queryDetailTypes, CacheItem *item)
{
    bool modified = false;

    QSet<StringPair> oldAddresses;

    if (queryDetailTypes.isEmpty() || queryDetailTypes.contains(detailType<QContactPhoneNumber>())) {
        // Addresses which are no longer in the contact should be de-indexed
        foreach (const QContactPhoneNumber &phoneNumber, oldContact.details<QContactPhoneNumber>()) {
            oldAddresses.insert(addressPair(phoneNumber));
        }

        // Update our address indexes for any address details in this contact
        foreach (const QContactPhoneNumber &phoneNumber, contact.details<QContactPhoneNumber>()) {
            const StringPair address(addressPair(phoneNumber));

            if (!oldAddresses.remove(address)) {
                // This address was not previously recorded
                modified = true;
                resolveUnknownAddresses(address.first, address.second, item);
            }

            m_phoneNumberIds[address.second] = iid;
        }

        // Remove any addresses no longer available for this contact
        if (!oldAddresses.isEmpty()) {
            modified = true;
            foreach (const StringPair &address, oldAddresses) {
                m_phoneNumberIds.remove(address.second);
            }
            oldAddresses.clear();
        }
    }

    if (queryDetailTypes.isEmpty() || queryDetailTypes.contains(detailType<QContactEmailAddress>())) {
        foreach (const QContactEmailAddress &emailAddress, oldContact.details<QContactEmailAddress>()) {
            oldAddresses.insert(addressPair(emailAddress));
        }

        foreach (const QContactEmailAddress &emailAddress, contact.details<QContactEmailAddress>()) {
            const StringPair address(addressPair(emailAddress));

            if (!oldAddresses.remove(address)) {
                modified = true;
                resolveUnknownAddresses(address.first, address.second, item);
            }

            m_emailAddressIds[address.first] = iid;
        }

        if (!oldAddresses.isEmpty()) {
            modified = true;
            foreach (const StringPair &address, oldAddresses) {
                m_emailAddressIds.remove(address.first);
            }
            oldAddresses.clear();
        }
    }

    if (queryDetailTypes.isEmpty() || queryDetailTypes.contains(detailType<QContactOnlineAccount>())) {
        foreach (const QContactOnlineAccount &account, oldContact.details<QContactOnlineAccount>()) {
            oldAddresses.insert(addressPair(account));
        }

        foreach (const QContactOnlineAccount &account, contact.details<QContactOnlineAccount>()) {
            const StringPair address(addressPair(account));

            if (!oldAddresses.remove(address)) {
                modified = true;
                resolveUnknownAddresses(address.first, address.second, item);
            }

            m_onlineAccountIds[address] = iid;
        }

        if (!oldAddresses.isEmpty()) {
            modified = true;
            foreach (const StringPair &address, oldAddresses) {
                m_onlineAccountIds.remove(address);
            }
            oldAddresses.clear();
        }
    }

    return modified;
}

void SeasideCache::contactsAvailable()
{
    QContactAbstractRequest *request = static_cast<QContactAbstractRequest *>(sender());

    QList<QContact> contacts;
    QContactFetchHint fetchHint;
    if (request == &m_fetchByIdRequest) {
        contacts = m_fetchByIdRequest.contacts();
        if (m_fetchByIdProcessedCount) {
            contacts = contacts.mid(m_fetchByIdProcessedCount);
        }
        m_fetchByIdProcessedCount += contacts.count();
        fetchHint = m_fetchByIdRequest.fetchHint();
    } else {
        contacts = m_fetchRequest.contacts();
        if (m_fetchProcessedCount) {
            contacts = contacts.mid(m_fetchProcessedCount);
        }
        m_fetchProcessedCount += contacts.count();
        fetchHint = m_fetchRequest.fetchHint();
    }

    QSet<DetailTypeId> queryDetailTypes;
    foreach (const DetailTypeId &typeId, detailTypesHint(fetchHint)) {
        queryDetailTypes.insert(typeId);
    }
    const bool partialFetch = !queryDetailTypes.isEmpty();

    if (m_populateProgress > Unpopulated && m_populateProgress < Populated) {
        // We are populating the cache
        FilterType type(m_populateProgress == FetchFavorites ? FilterFavorites
                                                             : (m_populateProgress == FetchMetadata ? FilterAll
                                                                                                    : FilterOnline));
        appendContacts(contacts, type, partialFetch);
    } else {
        // An update.
        QSet<QString> modifiedGroups;

        for (int i = m_resultsRead; i < contacts.count(); ++i) {
            QContact contact = contacts.at(i);
            quint32 iid = internalId(contact);

            CacheItem *item = existingItem(iid);

            const bool preexisting = (item != 0);
            if (!item) {
                // We haven't seen this contact before
                item = &(m_people[iid]);
                item->iid = iid;
            }

            QString oldNameGroup;
            QString oldDisplayLabel;

            if (preexisting) {
                oldNameGroup = item->nameGroup;
                oldDisplayLabel = item->displayLabel;

                if (partialFetch) {
                    // Copy any existing detail types that are in the current record to the new instance
                    foreach (const QContactDetail &existing, item->contact.details()) {
                        if (!queryDetailTypes.contains(detailType(existing))) {
                            QContactDetail copy(existing);
                            contact.saveDetail(&copy);
                        }
                    }
                }
            }

            bool roleDataChanged = false;

            // This is a simplification of reality, should we test more changes?
            if (!partialFetch || queryDetailTypes.contains(detailType<QContactAvatar>())) {
                roleDataChanged |= (contact.details<QContactAvatar>() != item->contact.details<QContactAvatar>());
            }
            if (!partialFetch || queryDetailTypes.contains(detailType<QContactGlobalPresence>())) {
                roleDataChanged |= (contact.detail<QContactGlobalPresence>() != item->contact.detail<QContactGlobalPresence>());
            }

            roleDataChanged |= updateContactIndexing(item->contact, contact, iid, queryDetailTypes, item);

            updateCache(item, contact, partialFetch);
            roleDataChanged |= (item->displayLabel != oldDisplayLabel);

            // do this even if !roleDataChanged as name groups are affected by other display label changes
            if (item->nameGroup != oldNameGroup) {
                if (!ignoreContactForNameGroups(item->contact)) {
                    addToContactNameGroup(item->iid, item->nameGroup, &modifiedGroups);
                    if (!oldNameGroup.isNull()) {
                        removeFromContactNameGroup(item->iid, oldNameGroup, &modifiedGroups);
                    }
                }
            }

            if (roleDataChanged) {
                instancePtr->contactDataChanged(item->iid);
            }
        }
        m_resultsRead = contacts.count();
        notifyNameGroupsChanged(modifiedGroups);
    }
}

void SeasideCache::addToContactNameGroup(quint32 iid, const QString &group, QSet<QString> *modifiedGroups)
{
    if (!group.isNull()) {
        QSet<quint32> &set(m_contactNameGroups[group]);
        if (!set.contains(iid)) {
            set.insert(iid);
            if (modifiedGroups && !m_nameGroupChangeListeners.isEmpty()) {
                modifiedGroups->insert(group);
            }
        }
    }
}

void SeasideCache::removeFromContactNameGroup(quint32 iid, const QString &group, QSet<QString> *modifiedGroups)
{
    if (!group.isNull()) {
        QSet<quint32> &set(m_contactNameGroups[group]);
        if (set.remove(iid)) {
            if (modifiedGroups && !m_nameGroupChangeListeners.isEmpty()) {
                modifiedGroups->insert(group);
            }
        }
    }
}

void SeasideCache::notifyNameGroupsChanged(const QSet<QString> &groups)
{
    if (groups.isEmpty() || m_nameGroupChangeListeners.isEmpty())
        return;

    QHash<QString, QSet<quint32> > updates;
    foreach (const QString &group, groups)
        updates.insert(group, m_contactNameGroups[group]);

    for (int i = 0; i < m_nameGroupChangeListeners.count(); ++i)
        m_nameGroupChangeListeners[i]->nameGroupsUpdated(updates);
}

void SeasideCache::contactIdsAvailable()
{
    if (!m_contactsToFetchCandidates.isEmpty()) {
        foreach (const ContactIdType &id, m_contactIdRequest.ids()) {
            m_candidateIds.insert(id);
        }
        return;
    }

    if (m_syncFilter != FilterNone) {
        synchronizeList(this, m_contacts[m_syncFilter], m_cacheIndex, internalIds(m_contactIdRequest.ids()), m_queryIndex);
    }
}

void SeasideCache::relationshipsAvailable()
{
#ifdef USING_QTPIM
    static const QString aggregatesRelationship = QContactRelationship::Aggregates();
#else
    static const QString aggregatesRelationship = QContactRelationship::Aggregates;
#endif

    foreach (const QContactRelationship &rel, m_relationshipsFetchRequest.relationships()) {
        if (rel.relationshipType() == aggregatesRelationship) {
#ifdef USING_QTPIM
            m_constituentIds.insert(apiId(rel.second()));
#else
            m_constituentIds.insert(rel.second().localId());
#endif
        }
    }
}

void SeasideCache::removeRange(FilterType filter, int index, int count)
{
    QList<quint32> &cacheIds = m_contacts[filter];
    QList<ListModel *> &models = m_models[filter];

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceAboutToRemoveItems(index, index + count - 1);

    for (int i = 0; i < count; ++i) {
        if (filter == FilterAll) {
            const quint32 iid = cacheIds.at(index);
            m_expiredContacts[apiId(iid)] -= 1;
        }

        m_contactIndices[filter].remove(cacheIds.at(index));
        cacheIds.removeAt(index);
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsRemoved();
}

int SeasideCache::insertRange(FilterType filter, int index, int count, const QList<quint32> &queryIds, int queryIndex)
{
    QList<quint32> &cacheIds = m_contacts[filter];
    QList<ListModel *> &models = m_models[filter];

    const quint32 selfId = internalId(m_manager.selfContactId());

    int end = index + count - 1;
    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceAboutToInsertItems(index, end);

    for (int i = 0; i < count; ++i) {
        quint32 iid = queryIds.at(queryIndex + i);
        if (iid == selfId)
            continue;

        if (filter == FilterAll) {
            const ContactIdType apiId = SeasideCache::apiId(iid);
            m_expiredContacts[apiId] += 1;
        }

        cacheIds.insert(index + i, iid);
        m_contactIndices[filter].insert(iid, index + i);
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsInserted(index, end);

    return end - index + 1;
}

void SeasideCache::appendContacts(const QList<QContact> &contacts, FilterType filterType, bool partialFetch)
{
    if (!contacts.isEmpty()) {
        QList<quint32> &cacheIds = m_contacts[filterType];
        QList<ListModel *> &models = m_models[filterType];

        cacheIds.reserve(contacts.count());

        const int begin = cacheIds.count();
        int end = cacheIds.count() + contacts.count() - 1;

        if (begin <= end) {
            QSet<QString> modifiedGroups;

            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceAboutToInsertItems(begin, end);

            foreach (QContact contact, contacts) {
                quint32 iid = internalId(contact);

                cacheIds.append(iid);
                m_contactIndices[filterType].insert(iid, cacheIds.count() - 1);

                CacheItem &cacheItem = m_people[iid];

                // If we have already requested this contact as a favorite, don't update with fewer details
                if ((cacheItem.iid == 0) ||
                    (cacheItem.contact.detail<QContactFavorite>().isFavorite() == false)) {
                    cacheItem.iid = iid;
                    updateContactIndexing(QContact(), contact, iid, QSet<DetailTypeId>(), &cacheItem);
                    updateCache(&cacheItem, contact, partialFetch);
                }

                if (filterType == FilterAll) {
                    addToContactNameGroup(iid, nameGroup(&cacheItem), &modifiedGroups);
                }
            }

            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceItemsInserted(begin, end);

            notifyNameGroupsChanged(modifiedGroups);
        }
    }
}

void SeasideCache::requestStateChanged(QContactAbstractRequest::State state)
{
    if (state != QContactAbstractRequest::FinishedState)
        return;

    QContactAbstractRequest *request = static_cast<QContactAbstractRequest *>(sender());

    bool activityCompleted = true;

    if (request == &m_relationshipsFetchRequest) {
        if (!m_contactsToFetchConstituents.isEmpty()) {
            QContactId aggregateId = m_contactsToFetchConstituents.takeFirst();
            if (!m_constituentIds.isEmpty()) {
                m_contactsToLinkTo.append(aggregateId);
            } else {
                // We didn't find any constituents - report the empty list
#ifdef USING_QTPIM
                CacheItem *cacheItem = itemById(aggregateId);
#else
                CacheItem *cacheItem = itemById(aggregateId.localId());
#endif
                if (cacheItem->itemData) {
                    cacheItem->itemData->constituentsFetched(QList<int>());
                }

                updateConstituentAggregations(cacheItem->apiId());
            }
        }
    } else if (request == &m_fetchByIdRequest) {
        if (!m_contactsToLinkTo.isEmpty()) {
            // Report these results
            QContactId aggregateId = m_contactsToLinkTo.takeFirst();
#ifdef USING_QTPIM
            CacheItem *cacheItem = itemById(aggregateId);
#else
            CacheItem *cacheItem = itemById(aggregateId.localId());
#endif

            QList<int> constituentIds;
            foreach (const ContactIdType &id, m_constituentIds) {
                constituentIds.append(internalId(id));
            }
            m_constituentIds.clear();

            if (cacheItem->itemData) {
                cacheItem->itemData->constituentsFetched(constituentIds);
            }

            updateConstituentAggregations(cacheItem->apiId());
        }
    } else if (request == &m_contactIdRequest) {
        if (!m_contactsToFetchCandidates.isEmpty()) {
            // Report these results
            QContactId contactId = m_contactsToFetchCandidates.takeFirst();
#ifdef USING_QTPIM
            CacheItem *cacheItem = itemById(contactId);
#else
            CacheItem *cacheItem = itemById(contactId.localId());
#endif

            const quint32 contactIid = internalId(contactId);

            QList<int> candidateIds;
            foreach (const ContactIdType &id, m_candidateIds) {
                // Exclude the original source contact
                const quint32 iid = internalId(id);
                if (iid != contactIid) {
                    candidateIds.append(iid);
                }
            }
            m_candidateIds.clear();

            if (cacheItem->itemData) {
                cacheItem->itemData->mergeCandidatesFetched(candidateIds);
            }
        } else if (m_syncFilter != FilterNone) {
            // We have completed fetching this filter set
            completeSynchronizeList(this, m_contacts[m_syncFilter], m_cacheIndex, internalIds(m_contactIdRequest.ids()), m_queryIndex);

            // Notify models of completed updates
            QList<ListModel *> &models = m_models[m_syncFilter];
            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceItemsChanged();

            if (m_syncFilter == FilterFavorites) {
                // Next, query for all contacts (including favorites)
                m_syncFilter = FilterAll;
                m_contactIdRequest.setFilter(allFilter());
                m_contactIdRequest.setSorting(m_sortOrder);
                m_contactIdRequest.start();

                activityCompleted = false;
            } else if (m_syncFilter == FilterAll) {
                // Next, query for online contacts
                m_syncFilter = FilterOnline;
                m_contactIdRequest.setFilter(onlineFilter());
                m_contactIdRequest.setSorting(m_onlineSortOrder);
                m_contactIdRequest.start();

                activityCompleted = false;
            }
        } else {
            qWarning() << "ID fetch completed with no filter?";
        }
    } else if (request == &m_relationshipSaveRequest || request == &m_relationshipRemoveRequest) {
        QSet<ContactIdType> contactIds;
        foreach (const QContactRelationship &relationship, m_relationshipSaveRequest.relationships() +
                                                           m_relationshipRemoveRequest.relationships()) {
#ifdef USING_QTPIM
            contactIds.insert(SeasideCache::apiId(relationship.first()));
#else
            contactIds.insert(relationship.first().localId());
#endif
        }

        foreach (const ContactIdType &contactId, contactIds) {
            CacheItem *cacheItem = itemById(contactId);
            if (cacheItem && cacheItem->itemData)
                cacheItem->itemData->aggregationOperationCompleted();
        }
    } else if (request == &m_fetchRequest) {
        if (m_populateProgress == Unpopulated && m_keepPopulated) {
            // Start a query to fully populate the cache, starting with favorites
            m_fetchRequest.setFilter(favoriteFilter());
            m_fetchRequest.setFetchHint(favoriteFetchHint(m_fetchTypes));
            m_fetchRequest.setSorting(m_sortOrder);
            m_fetchRequest.start();
            m_fetchProcessedCount = 0;

            m_populateProgress = FetchFavorites;
            activityCompleted = false;
        } else if (m_populateProgress == FetchFavorites) {
            makePopulated(FilterFavorites);
            qDebug() << "Favorites queried in" << m_timer.elapsed() << "ms";

            // Next, query for all contacts (except favorites)
            // Request the metadata of all contacts (only data from the primary table)
            m_fetchRequest.setFilter(allFilter());
            m_fetchRequest.setFetchHint(metadataFetchHint(m_fetchTypes));
            m_fetchRequest.setSorting(m_sortOrder);
            m_fetchRequest.start();
            m_fetchProcessedCount = 0;

            m_fetchTypesChanged = false;
            m_populateProgress = FetchMetadata;
            activityCompleted = false;
        } else if (m_populateProgress == FetchMetadata) {
            makePopulated(FilterNone);
            makePopulated(FilterAll);
            qDebug() << "All queried in" << m_timer.elapsed() << "ms";

            // Now query for online contacts
            m_fetchRequest.setFilter(onlineFilter());
            m_fetchRequest.setFetchHint(onlineFetchHint(m_fetchTypes));
            m_fetchRequest.setSorting(m_onlineSortOrder);
            m_fetchRequest.start();
            m_fetchProcessedCount = 0;

            m_populateProgress = FetchOnline;
            activityCompleted = false;
        } else if (m_populateProgress == FetchOnline) {
            makePopulated(FilterOnline);
            qDebug() << "Online queried in" << m_timer.elapsed() << "ms";

            m_populateProgress = Populated;
        } else if (m_populateProgress == RefetchFavorites) {
            // Re-fetch the non-favorites
            m_fetchRequest.setFilter(nonfavoriteFilter());
            m_fetchRequest.setFetchHint(onlineFetchHint(m_fetchTypes));
            m_fetchRequest.setSorting(m_sortOrder);
            m_fetchRequest.start();
            m_fetchProcessedCount = 0;

            m_fetchProcessedCount = 0;
            m_populateProgress = RefetchOthers;
        } else if (m_populateProgress == RefetchOthers) {
            // We're up to date again
            m_populateProgress = Populated;
        } else {
            // Result of a specific query
            if (m_activeResolve) {
                CacheItem *item = 0;
                if (!m_fetchRequest.contacts().isEmpty()) {
                    item = itemById(apiId(m_fetchRequest.contacts().first()), false);
                } else {
                    // This address is unknown - keep it for later resolution
                    ResolveData data(*m_activeResolve);
                    if (data.first == QString()) {
                        // Compare this phone number in normalized form
                        data.compare = normalizePhoneNumber(data.second);
                    } else if (data.second == QString()) {
                        // Compare this email address in lowercased form
                        data.compare = data.first.toLower();
                    } else {
                        // Compare this account URI in lowercased form
                        data.compare = data.second.toLower();
                    }

                    m_unknownAddresses.append(data);
                }
                m_activeResolve->listener->addressResolved(m_activeResolve->first, m_activeResolve->second, item);

                m_activeResolve = 0;
                m_resolveAddresses.takeFirst();
            }
        }
    }

    if (activityCompleted) {
        // See if there are any more requests to dispatch
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    }
}

void SeasideCache::makePopulated(FilterType filter)
{
    m_populated |= (1 << filter);

    QList<ListModel *> &models = m_models[filter];
    for (int i = 0; i < models.count(); ++i)
        models.at(i)->makePopulated();
}

void SeasideCache::setSortOrder(const QString &property)
{
    bool firstNameFirst = (property == QString::fromLatin1("firstName"));

    QContactSortOrder firstNameOrder;
    setDetailType<QContactName>(firstNameOrder, QContactName::FieldFirstName);
    firstNameOrder.setCaseSensitivity(Qt::CaseInsensitive);
    firstNameOrder.setDirection(Qt::AscendingOrder);
    firstNameOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

    QContactSortOrder lastNameOrder;
    setDetailType<QContactName>(lastNameOrder, QContactName::FieldLastName);
    lastNameOrder.setCaseSensitivity(Qt::CaseInsensitive);
    lastNameOrder.setDirection(Qt::AscendingOrder);
    lastNameOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

    m_sortOrder = firstNameFirst ? (QList<QContactSortOrder>() << firstNameOrder << lastNameOrder)
                                 : (QList<QContactSortOrder>() << lastNameOrder << firstNameOrder);

    m_onlineSortOrder = m_sortOrder;

    QContactSortOrder onlineOrder;
    setDetailType<QContactGlobalPresence>(onlineOrder, QContactGlobalPresence::FieldPresenceState);
    onlineOrder.setDirection(Qt::AscendingOrder);

    m_onlineSortOrder.prepend(onlineOrder);
}

void SeasideCache::displayLabelOrderChanged()
{
#ifdef HAS_MLITE
    QVariant displayLabelOrder = m_displayLabelOrderConf.value();
    if (displayLabelOrder.isValid() && displayLabelOrder.toInt() != m_displayLabelOrder) {
        m_displayLabelOrder = static_cast<DisplayLabelOrder>(displayLabelOrder.toInt());

        QSet<QString> modifiedGroups;

        // Update the display labels
        typedef QHash<quint32, CacheItem>::iterator iterator;
        for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
            // Regenerate the display label
            QString newLabel = generateDisplayLabel(it->contact, m_displayLabelOrder);
            if (newLabel != it->displayLabel) {
                it->displayLabel = newLabel;

                contactDataChanged(it->iid);
                reportItemUpdated(&*it);
            }

            if (it->itemData) {
                it->itemData->displayLabelOrderChanged(m_displayLabelOrder);
            }

            // If the contact's name group is derived from display label, it may have changed
            const QString group(determineNameGroup(&*it));
            if (group != it->nameGroup) {
                if (!ignoreContactForNameGroups(it->contact)) {
                    if (!it->nameGroup.isNull()) {
                        removeFromContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);
                    }

                    it->nameGroup = group;
                    addToContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);
                }
            }
        }

        notifyNameGroupsChanged(modifiedGroups);

        for (int i = 0; i < FilterTypesCount; ++i) {
            const QList<ListModel *> &models = m_models[i];
            for (int j = 0; j < models.count(); ++j) {
                ListModel *model = models.at(j);
                model->updateDisplayLabelOrder();
                model->sourceItemsChanged();
            }
        }
    }
#endif
}

void SeasideCache::sortPropertyChanged()
{
#ifdef HAS_MLITE
    QVariant sortProperty = m_sortPropertyConf.value();
    if (sortProperty.isValid() && sortProperty.toString() != m_sortProperty) {
        const QString newProperty(sortProperty.toString());
        if ((newProperty != QString::fromLatin1("firstName")) &&
            (newProperty != QString::fromLatin1("lastName"))) {
            qWarning() << "Invalid sort property configuration:" << newProperty;
            return;
        }

        m_sortProperty = newProperty;
        setSortOrder(m_sortProperty);

        for (int i = 0; i < FilterTypesCount; ++i) {
            const QList<ListModel *> &models = m_models[i];
            for (int j = 0; j < models.count(); ++j) {
                models.at(j)->updateSortProperty();
                // No need for sourceItemsChanged, as the sorted list update will cause that
            }
        }

        // Update the sorted list order
        m_refreshRequired = true;
        requestUpdate();
    }
#endif
}

void SeasideCache::groupPropertyChanged()
{
#ifdef HAS_MLITE
    QVariant groupProperty = m_groupPropertyConf.value();
    if (groupProperty.isValid() && groupProperty.toString() != m_groupProperty) {
        const QString newProperty(groupProperty.toString());
        if ((newProperty != QString::fromLatin1("firstName")) &&
            (newProperty != QString::fromLatin1("lastName"))) {
            qWarning() << "Invalid group property configuration:" << newProperty;
            return;
        }

        m_groupProperty = newProperty;

        // Update the name groups
        QSet<QString> modifiedGroups;

        typedef QHash<quint32, CacheItem>::iterator iterator;
        for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
            // Update the nameGroup for this contact
            const QString group(determineNameGroup(&*it));
            if (group != it->nameGroup) {
                if (!ignoreContactForNameGroups(it->contact)) {
                    if (!it->nameGroup.isNull()) {
                        removeFromContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);
                    }

                    it->nameGroup = group;
                    addToContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);
                }
            }
        }

        notifyNameGroupsChanged(modifiedGroups);

        for (int i = 0; i < FilterTypesCount; ++i) {
            const QList<ListModel *> &models = m_models[i];
            for (int j = 0; j < models.count(); ++j) {
                ListModel *model = models.at(j);
                model->updateGroupProperty();
                model->sourceItemsChanged();
            }
        }
    }
#endif
}

int SeasideCache::importContacts(const QString &path)
{
    QFile vcf(path);
    if (!vcf.open(QIODevice::ReadOnly)) {
        qWarning() << Q_FUNC_INFO << "Cannot open " << path;
        return 0;
    }

    // TODO: thread
    QVersitReader reader(&vcf);
    reader.startReading();
    reader.waitForFinished();

    QVersitContactImporter importer;
    importer.importDocuments(reader.results());

    QList<QContact> newContacts = importer.contacts();

    instancePtr->m_contactsToCreate += newContacts;
    instancePtr->requestUpdate();

    return newContacts.count();
}

QString SeasideCache::exportContacts()
{
    QVersitContactExporter exporter;

    QList<QContact> contacts;
    contacts.reserve(instancePtr->m_people.count());

    QList<ContactIdType> contactsToFetch;
    contactsToFetch.reserve(instancePtr->m_people.count());

    const quint32 selfId = internalId(instancePtr->m_manager.selfContactId());

    typedef QHash<quint32, CacheItem>::iterator iterator;
    for (iterator it = instancePtr->m_people.begin(); it != instancePtr->m_people.end(); ++it) {
        if (it.key() == selfId) {
            continue;
        } else if (it->contactState == ContactComplete) {
            contacts.append(it->contact);
        } else {
            contactsToFetch.append(apiId(it.key()));
        }
    }

    if (!contactsToFetch.isEmpty()) {
        QList<QContact> fetchedContacts = instancePtr->m_manager.contacts(contactsToFetch);
        contacts.append(fetchedContacts);
    }

    if (!exporter.exportContacts(contacts)) {
        qWarning() << Q_FUNC_INFO << "Failed to export contacts: " << exporter.errorMap();
        return QString();
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    QString baseDir;
    foreach (const QString &loc, QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation)) {
        baseDir = loc;
        break;
    }
#else
    const QString baseDir = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
#endif
    QFile vcard(baseDir
              + QDir::separator()
              + QDateTime::currentDateTime().toString("ss_mm_hh_dd_mm_yyyy")
              + ".vcf");

    if (!vcard.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot open " << vcard.fileName();
        return QString();
    }

    QVersitWriter writer(&vcard);
    if (!writer.startWriting(exporter.documents())) {
        qWarning() << Q_FUNC_INFO << "Can't start writing vcards " << writer.error();
        return QString();
    }

    // TODO: thread
    writer.waitForFinished();
    return vcard.fileName();
}

void SeasideCache::keepPopulated(quint32 fetchTypes)
{
    if ((m_fetchTypes & fetchTypes) != fetchTypes) {
        m_fetchTypes |= fetchTypes;
        m_fetchTypesChanged = true;
        requestUpdate();
    }

    if (!m_keepPopulated) {
        m_keepPopulated = true;
        requestUpdate();
    }
}

// Aggregates contact2 into contact1. Aggregate relationships will be created between the first
// contact and the constituents of the second contact.
void SeasideCache::aggregateContacts(const QContact &contact1, const QContact &contact2)
{
    instancePtr->m_contactPairsToLink.append(qMakePair(
              ContactLinkRequest(apiId(contact1)),
              ContactLinkRequest(apiId(contact2))));
    instancePtr->fetchConstituents(contact1);
    instancePtr->fetchConstituents(contact2);
}

// Disaggregates contact2 (a non-aggregate constituent) from contact1 (an aggregate).  This removes
// the existing aggregate relationships between the two contacts.
void SeasideCache::disaggregateContacts(const QContact &contact1, const QContact &contact2)
{
    instancePtr->m_relationshipsToRemove.append(makeRelationship(aggregateRelationshipType, contact1, contact2));
    instancePtr->m_relationshipsToSave.append(makeRelationship(QLatin1String("IsNot"), contact1, contact2));

    if (contact2.detail<QContactSyncTarget>().syncTarget() == syncTargetWasLocal) {
        // restore the local sync target that was changed in a previous link creation operation
        QContact c = contact2;
        QContactSyncTarget syncTarget = c.detail<QContactSyncTarget>();
        syncTarget.setSyncTarget(syncTargetLocal);
        c.saveDetail(&syncTarget);
        saveContact(c);
    }

    QCoreApplication::postEvent(instancePtr, new QEvent(QEvent::UpdateRequest));
}

void SeasideCache::updateConstituentAggregations(const ContactIdType &contactId)
{
    typedef QList<QPair<ContactLinkRequest, ContactLinkRequest> >::iterator iterator;
    for (iterator it = m_contactPairsToLink.begin(); it != m_contactPairsToLink.end(); ) {
        QPair<ContactLinkRequest, ContactLinkRequest> &pair = *it;
        if (pair.first.contactId == contactId)
            pair.first.constituentsFetched = true;
        if (pair.second.contactId == contactId)
            pair.second.constituentsFetched = true;
        if (pair.first.constituentsFetched && pair.second.constituentsFetched) {
            completeContactAggregation(pair.first.contactId, pair.second.contactId);
            it = m_contactPairsToLink.erase(it);
        } else {
            ++it;
        }
    }
}

// Called once constituents have been fetched for both persons.
void SeasideCache::completeContactAggregation(const ContactIdType &contact1Id, const ContactIdType &contact2Id)
{
    CacheItem *cacheItem1 = itemById(contact1Id);
    CacheItem *cacheItem2 = itemById(contact2Id);
    if (!cacheItem1 || !cacheItem2 || !cacheItem1->itemData || !cacheItem2->itemData)
        return;

    // Contact1 needs to be linked to each of person2's constituents. However, a local constituent
    // cannot be linked to two aggregate contacts. So, if both contacts have local constituents,
    // change contact2's local constitent's syncTarget to "was_local" and don't aggregate it with
    // contact1.
    const QList<int> &constituents1 = cacheItem1->itemData->constituents();
    const QList<int> &constituents2 = cacheItem2->itemData->constituents();
    QContact contact2Local;
    bool bothHaveLocals = false;
    foreach (int id, constituents1) {
        QContact c = contactById(apiId(id));
        if (c.detail<QContactSyncTarget>().syncTarget() == syncTargetLocal) {
            foreach (int id, constituents2) {
                QContact c = contactById(apiId(id));
                if (c.detail<QContactSyncTarget>().syncTarget() == syncTargetLocal) {
                    contact2Local = c;
                    bothHaveLocals = true;
                    break;
                }
            }
            break;
        }
    }
    if (bothHaveLocals) {
        QContactSyncTarget syncTarget = contact2Local.detail<QContactSyncTarget>();
        syncTarget.setSyncTarget(syncTargetWasLocal);
        contact2Local.saveDetail(&syncTarget);
        saveContact(contact2Local);
    }

    // For each constituent of contact2, add a relationship between it and contact1, and remove the
    // relationship between it and contact2.
    foreach (int id, constituents2) {
        QContact c = contactById(apiId(id));
        m_relationshipsToSave.append(makeRelationship(aggregateRelationshipType, contactById(contact1Id), c));
        m_relationshipsToRemove.append(makeRelationship(aggregateRelationshipType, contactById(contact2Id), c));
    }

    if (!m_relationshipsToSave.isEmpty() || !m_relationshipsToRemove.isEmpty())
        requestUpdate();
}

void SeasideCache::resolveAddress(ResolveListener *listener, const QString &first, const QString &second, bool requireComplete)
{
    ResolveData data;
    data.first = first;
    data.second = second;
    data.requireComplete = requireComplete;
    data.listener = listener;

    m_resolveAddresses.append(data);
    requestUpdate();
}

int SeasideCache::contactIndex(quint32 iid, FilterType filterType)
{
    QMap<quint32, int> &indices(m_contactIndices[filterType]);

    QMap<quint32, int>::iterator it = indices.find(iid);
    if (it != indices.end()) {
        int index = *it;

        const QList<quint32> &cacheIds(m_contacts[filterType]);
        quint32 contactIid = cacheIds.at(index);
        if (iid != contactIid) {
            // This index is no longer correct - we need to update it
            index = cacheIds.indexOf(iid);
            if (index == -1) {
                indices.erase(it);
            } else {
                *it = index;
            }
        }
        return index;
    }

    return -1;
}

QContactRelationship SeasideCache::makeRelationship(const QString &type, const QContact &contact1, const QContact &contact2)
{
    QContactRelationship relationship;
    relationship.setRelationshipType(type);
#ifdef USING_QTPIM
    relationship.setFirst(contact1);
    relationship.setSecond(contact2);
#else
    relationship.setFirst(contact1.id());
    relationship.setSecond(contact2.id());
#endif
    return relationship;
}

// Instantiate the contact ID functions for qtcontacts-sqlite
#include <qtcontacts-extensions_impl.h>
