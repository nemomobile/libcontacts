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

#include <qtcontacts-extensions_impl.h>
#include <qcontactstatusflags_impl.h>
#include <contactmanagerengine.h>

#include <private/qcontactmanager_p.h>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDBusConnection>
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
#include <QContactTimestamp>

#include <QVersitContactExporter>
#include <QVersitContactImporter>
#include <QVersitReader>
#include <QVersitWriter>

#include <QtDebug>

#include <mlocale.h>

#include <mce/dbus-names.h>
#include <mce/mode-names.h>

QTVERSIT_USE_NAMESPACE

namespace {

ML10N::MLocale mLocale;

const QString aggregateRelationshipType = QContactRelationship::Aggregates();

const QString syncTargetLocal = QLatin1String("local");
const QString syncTargetWasLocal = QLatin1String("was_local");

int getContactNameGroupCount()
{
    return mLocale.exemplarCharactersIndex().count();
}

QStringList getAllContactNameGroups()
{
    QStringList groups(mLocale.exemplarCharactersIndex());
    groups.append(QString::fromLatin1("#"));
    return groups;
}

QString managerName()
{
    return QString::fromLatin1("org.nemomobile.contacts.sqlite");
}

QMap<QString, QString> managerParameters()
{
    QMap<QString, QString> rv;
    // Report presence changes independently from other contact changes
    rv.insert(QString::fromLatin1("mergePresenceChanges"), QString::fromLatin1("false"));
    return rv;
}

Q_GLOBAL_STATIC_WITH_ARGS(QContactManager, manager, (managerName(), managerParameters()))

typedef QList<QContactDetail::DetailType> DetailList;

template<typename T>
QContactDetail::DetailType detailType()
{
    return T::Type;
}

QContactDetail::DetailType detailType(const QContactDetail &detail)
{
    return detail.type();
}

template<typename T, typename Filter, typename Field>
void setDetailType(Filter &filter, Field field)
{
    filter.setDetailType(T::Type, field);
}

DetailList detailTypesHint(const QContactFetchHint &hint)
{
    return hint.detailTypesHint();
}

void setDetailTypesHint(QContactFetchHint &hint, const DetailList &types)
{
    hint.setDetailTypesHint(types);
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

QContactFetchHint presenceFetchHint()
{
    QContactFetchHint fetchHint(basicFetchHint());

    setDetailTypesHint(fetchHint, DetailList() << detailType<QContactPresence>()
                                               << detailType<QContactGlobalPresence>());

    return fetchHint;
}

DetailList contactsTableDetails()
{
    DetailList types;

    // These details are reported in every query
    types << detailType<QContactSyncTarget>() <<
             detailType<QContactName>() <<
             detailType<QContactDisplayLabel>() <<
             detailType<QContactFavorite>() <<
             detailType<QContactTimestamp>() <<
             detailType<QContactGender>() <<
             detailType<QContactStatusFlags>();

    return types;
}

QContactFetchHint metadataFetchHint(quint32 fetchTypes = 0)
{
    QContactFetchHint fetchHint(basicFetchHint());

    // Include all detail types which come from the main contacts table
    DetailList types(contactsTableDetails());

    // Include nickname, as some contacts have no other name
    types << detailType<QContactNickname>();

    if (fetchTypes & SeasideCache::FetchAccountUri) {
        types << detailType<QContactOnlineAccount>();
    }
    if (fetchTypes & SeasideCache::FetchPhoneNumber) {
        types << detailType<QContactPhoneNumber>();
    }
    if (fetchTypes & SeasideCache::FetchEmailAddress) {
        types << detailType<QContactEmailAddress>();
    }
    if (fetchTypes & SeasideCache::FetchOrganization) {
        types << detailType<QContactOrganization>();
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

QContactFetchHint extendedMetadataFetchHint(quint32 fetchTypes)
{
    QContactFetchHint fetchHint(basicFetchHint());

    DetailList types;

    // Only query for the specific types we need
    if (fetchTypes & SeasideCache::FetchAccountUri) {
        types << detailType<QContactOnlineAccount>();
    }
    if (fetchTypes & SeasideCache::FetchPhoneNumber) {
        types << detailType<QContactPhoneNumber>();
    }
    if (fetchTypes & SeasideCache::FetchEmailAddress) {
        types << detailType<QContactEmailAddress>();
    }
    if (fetchTypes & SeasideCache::FetchOrganization) {
        types << detailType<QContactOrganization>();
    }

    setDetailTypesHint(fetchHint, types);
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

QList<StringPair> addressPairs(const QContactPhoneNumber &phoneNumber)
{
    QList<StringPair> rv;

    const QString normalized(SeasideCache::normalizePhoneNumber(phoneNumber.number()));
    if (!normalized.isEmpty()) {
        const QChar plus(QChar::fromLatin1('+'));
        if (normalized.startsWith(plus)) {
            // Also index the complete form of this number
            rv.append(qMakePair(QString(), normalized));
        }

        // Always index the minimized form of the number
        const QString minimized(SeasideCache::minimizePhoneNumber(normalized));
        rv.append(qMakePair(QString(), minimized));
    }

    return rv;
}

StringPair addressPair(const QContactEmailAddress &emailAddress)
{
    return qMakePair(emailAddress.emailAddress().toLower(), QString());
}

StringPair addressPair(const QContactOnlineAccount &account)
{
    StringPair address = qMakePair(account.value<QString>(QContactOnlineAccount__FieldAccountPath), account.accountUri().toLower());
    return !address.first.isEmpty() && !address.second.isEmpty() ? address : StringPair();
}

bool validAddressPair(const StringPair &address)
{
    return (!address.first.isEmpty() || !address.second.isEmpty());
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

QList<quint32> internalIds(const QList<QContactId> &ids)
{
    QList<quint32> rv;
    rv.reserve(ids.count());

    foreach (const QContactId &id, ids) {
        rv.append(SeasideCache::internalId(id));
    }

    return rv;
}

QString::const_iterator firstDtmfChar(QString::const_iterator it, QString::const_iterator end)
{
    static const QString dtmfChars(QString::fromLatin1("pPwWxX#*"));

    for ( ; it != end; ++it) {
        if (dtmfChars.contains(*it))
            return it;
    }
    return end;
}

const int ExactMatch = 100;

int matchLength(const QString &lhs, const QString &rhs)
{
    if (lhs.isEmpty() || rhs.isEmpty())
        return 0;

    QString::const_iterator lbegin = lhs.constBegin(), lend = lhs.constEnd();
    QString::const_iterator rbegin = rhs.constBegin(), rend = rhs.constEnd();

    // Do these numbers contain DTMF elements?
    QString::const_iterator ldtmf = firstDtmfChar(lbegin, lend);
    QString::const_iterator rdtmf = firstDtmfChar(rbegin, rend);

    QString::const_iterator lit, rit;
    bool processDtmf = false;
    int matchLength = 0;

    if ((ldtmf != lbegin) && (rdtmf != rbegin)) {
        // Start match length calculation at the last non-DTMF digit
        lit = ldtmf - 1;
        rit = rdtmf - 1;

        while (*lit == *rit) {
            ++matchLength;

            --lit;
            --rit;
            if ((lit == lbegin) || (rit == rbegin)) {
                if (*lit == *rit) {
                    ++matchLength;

                    if ((lit == lbegin) && (rit == rbegin)) {
                        // We have a complete, exact match - this must be the best match
                        return ExactMatch;
                    } else {
                        // We matched all of one number - continue looking in the DTMF part
                        processDtmf = true;
                    }
                }
                break;
            }
        }
    } else {
        // Process the DTMF section for a match
        processDtmf = true;
    }

    // Have we got a match?
    if ((matchLength >= QtContactsSqliteExtensions::DefaultMaximumPhoneNumberCharacters) ||
        processDtmf) {
        // See if the match continues into the DTMF area
        QString::const_iterator lit = ldtmf;
        QString::const_iterator rit = rdtmf;
        for ( ; (lit != lend) && (rit != rend); ++lit, ++rit) {
            if ((*lit).toLower() != (*rit).toLower())
                break;
            ++matchLength;
        }
    }

    return matchLength;
}

int bestPhoneNumberMatchLength(const QContact &contact, const QString &match)
{
    int bestMatchLength = 0;

    foreach (const QContactPhoneNumber& phone, contact.details<QContactPhoneNumber>()) {
        bestMatchLength = qMax(bestMatchLength, matchLength(SeasideCache::normalizePhoneNumber(phone.number()), match));
        if (bestMatchLength == ExactMatch) {
            return ExactMatch;
        }
    }

    return bestMatchLength;
}

}

SeasideCache *SeasideCache::instancePtr = 0;
int SeasideCache::contactNameGroupCount = getContactNameGroupCount();
QStringList SeasideCache::allContactNameGroups = getAllContactNameGroups();

QContactManager* SeasideCache::manager()
{
    return ::manager();
}

SeasideCache* SeasideCache::instance()
{
    return instancePtr;
}

QContactId SeasideCache::apiId(const QContact &contact)
{
    return contact.id();
}

QContactId SeasideCache::apiId(quint32 iid)
{
    return QtContactsSqliteExtensions::apiContactId(iid);
}

bool SeasideCache::validId(const QContactId &id)
{
    return !id.isNull();
}

quint32 SeasideCache::internalId(const QContact &contact)
{
    return internalId(contact.id());
}

quint32 SeasideCache::internalId(const QContactId &id)
{
    return QtContactsSqliteExtensions::internalContactId(id);
}

SeasideCache::SeasideCache()
    : m_syncFilter(FilterNone)
#ifdef HAS_MLITE
    , m_displayLabelOrderConf(QLatin1String("/org/nemomobile/contacts/display_label_order"))
    , m_sortPropertyConf(QLatin1String("/org/nemomobile/contacts/sort_property"))
    , m_groupPropertyConf(QLatin1String("/org/nemomobile/contacts/group_property"))
#endif
    , m_populated(0)
    , m_cacheIndex(0)
    , m_queryIndex(0)
    , m_fetchProcessedCount(0)
    , m_fetchByIdProcessedCount(0)
    , m_displayLabelOrder(FirstNameFirst)
    , m_sortProperty(QString::fromLatin1("firstName"))
    , m_groupProperty(QString::fromLatin1("firstName"))
    , m_keepPopulated(false)
    , m_populateProgress(Unpopulated)
    , m_fetchTypes(0)
    , m_extraFetchTypes(0)
    , m_dataTypesFetched(0)
    , m_updatesPending(false)
    , m_refreshRequired(false)
    , m_contactsUpdated(false)
    , m_displayOff(false)
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

    if (!QDBusConnection::systemBus().connect(MCE_SERVICE, MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
                                              MCE_DISPLAY_SIG, this, SLOT(displayStatusChanged(QString)))) {
        qWarning() << "Unable to connect to MCE displayStatusChanged signal";
    }

    QContactManager *mgr(manager());

    // The contactsPresenceChanged signal is not exported by QContactManager, so we
    // need to find it from the manager's engine object
    typedef QtContactsSqliteExtensions::ContactManagerEngine EngineType;
    EngineType *cme = dynamic_cast<EngineType *>(QContactManagerData::managerData(mgr)->m_engine);

    connect(mgr, SIGNAL(dataChanged()), this, SLOT(updateContacts()));
    connect(mgr, SIGNAL(contactsAdded(QList<QContactId>)),
            this, SLOT(contactsAdded(QList<QContactId>)));
    connect(mgr, SIGNAL(contactsChanged(QList<QContactId>)),
            this, SLOT(contactsChanged(QList<QContactId>)));
    connect(cme, SIGNAL(contactsPresenceChanged(QList<QContactId>)),
            this, SLOT(contactsPresenceChanged(QList<QContactId>)));
    connect(mgr, SIGNAL(contactsRemoved(QList<QContactId>)),
            this, SLOT(contactsRemoved(QList<QContactId>)));

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

    m_fetchRequest.setManager(mgr);
    m_fetchByIdRequest.setManager(mgr);
    m_contactIdRequest.setManager(mgr);
    m_relationshipsFetchRequest.setManager(mgr);
    m_removeRequest.setManager(mgr);
    m_saveRequest.setManager(mgr);
    m_relationshipSaveRequest.setManager(mgr);
    m_relationshipRemoveRequest.setManager(mgr);

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

void SeasideCache::registerModel(ListModel *model, FilterType type, FetchDataType requiredTypes, FetchDataType extraTypes)
{
    if (!instancePtr) {
        new SeasideCache;
    } else {
        instancePtr->m_expiryTimer.stop();
        for (int i = 0; i < FilterTypesCount; ++i)
            instancePtr->m_models[i].removeAll(model);
    }

    instancePtr->m_models[type].append(model);

    instancePtr->keepPopulated(requiredTypes & SeasideCache::FetchTypesMask, extraTypes & SeasideCache::FetchTypesMask);
    if (requiredTypes & SeasideCache::FetchTypesMask) {
        // If we have filtered models, they will need a contact ID refresh after the cache is populated
        instancePtr->m_refreshRequired = true;
    }
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

    allContactNameGroups = instancePtr->m_nameGrouper->allNameGroups();
    contactNameGroupCount = allContactNameGroups.count();
    if (!allContactNameGroups.contains(QLatin1String("#")))
        allContactNameGroups << QLatin1String("#");
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
        if (!group.isNull() && allContactNameGroups.contains(group)) {
            return group;
        }
    }

    const QContactName name(cacheItem->contact.detail<QContactName>());
    const QString nameProperty(instancePtr->m_groupProperty == QString::fromLatin1("firstName") ? name.firstName() : name.lastName());

    QString group;
    if (!nameProperty.isEmpty()) {
        group = mLocale.indexBucket(nameProperty);
    } else if (!cacheItem->displayLabel.isEmpty()) {
        group = mLocale.indexBucket(cacheItem->displayLabel);
    }

    if (!group.isEmpty()) {
        if (!allContactNameGroups.contains(group)) {
            // If this group is some kind of digit, group under '#'
            if (mLocale.toLatinNumbers(group.mid(0, 1)).at(0).isDigit()) {
                group = QString();
            }
        }
    }

    if (group.isEmpty()) {
        group = QString::fromLatin1("#");
    } else if (!allContactNameGroups.contains(group)) {
        // Insert before the '#' group, which is always last, and after the pre-defined groups
        const int maxIndex = allContactNameGroups.count() - 1;
        int index = qMin(contactNameGroupCount, maxIndex);
        for ( ; index < maxIndex; ++index) {
            if (group < allContactNameGroups.at(index)) {
                break;
            }
        }

        allContactNameGroups.insert(index, group);
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

SeasideCache::CacheItem *SeasideCache::itemById(const QContactId &id, bool requireComplete)
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
        item->iid = iid;
        item->contactState = ContactAbsent;

        item->contact.setId(id);
    }

    if (requireComplete) {
        ensureCompletion(item);
    }
    return item;
}

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

SeasideCache::CacheItem *SeasideCache::existingItem(const QContactId &id)
{
    return existingItem(internalId(id));
}

SeasideCache::CacheItem *SeasideCache::existingItem(quint32 iid)
{
    QHash<quint32, CacheItem>::iterator it = instancePtr->m_people.find(iid);
    return it != instancePtr->m_people.end()
            ? &(*it)
            : 0;
}

QContact SeasideCache::contactById(const QContactId &id)
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
    const QString normalized(normalizePhoneNumber(number));
    if (normalized.isEmpty())
        return 0;

    const QChar plus(QChar::fromLatin1('+'));
    if (normalized.startsWith(plus)) {
        // See if there is a match for the complete form of this number
        if (CacheItem *item = instancePtr->itemMatchingPhoneNumber(normalized, normalized, requireComplete)) {
            return item;
        }
    }

    const QString minimized(minimizePhoneNumber(normalized));
    if (((instancePtr->m_fetchTypes & SeasideCache::FetchPhoneNumber) == 0) &&
        !instancePtr->m_resolvedPhoneNumbers.contains(minimized)) {
        // We haven't previously queried this number, so there may be more matches than any
        // that we already have cached; return 0 to force a query
        return 0;
    }

    return instancePtr->itemMatchingPhoneNumber(minimized, normalized, requireComplete);
}

SeasideCache::CacheItem *SeasideCache::itemByEmailAddress(const QString &email, bool requireComplete)
{
    if (email.trimmed().isEmpty())
        return 0;

    QHash<QString, quint32>::const_iterator it = instancePtr->m_emailAddressIds.find(email.toLower());
    if (it != instancePtr->m_emailAddressIds.end())
        return itemById(*it, requireComplete);

    return 0;
}

SeasideCache::CacheItem *SeasideCache::itemByOnlineAccount(const QString &localUid, const QString &remoteUid, bool requireComplete)
{
    if (localUid.trimmed().isEmpty() || remoteUid.trimmed().isEmpty())
        return 0;

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

QContactId SeasideCache::selfContactId()
{
    return manager()->selfContactId();
}

void SeasideCache::requestUpdate()
{
    if (!m_updatesPending) {
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        m_updatesPending = true;
    }
}

bool SeasideCache::saveContact(const QContact &contact)
{
    QContactId id = apiId(contact);
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
    QContactId id = apiId(contact);
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

    m_contacts[filter].removeAt(row);

    if (filter == FilterAll) {
        const QString group(nameGroup(existingItem(iid)));
        QSet<QString> modifiedNameGroups;
        removeFromContactNameGroup(iid, group, &modifiedNameGroups);
        notifyNameGroupsChanged(modifiedNameGroups);
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

    // If none of the detail fields provides a label, fallback to the backend's label string, in
    // preference to using any of the addressing details directly
    const QString displayLabel = contact.detail<QContactDisplayLabel>().label();
    if (!displayLabel.isEmpty()) {
        return displayLabel;
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

static bool avatarUrlWithMetadata(const QContact &contact, QUrl &matchingUrl, const QString &metadataFragment = QString())
{
    static const QString coverMetadata(QString::fromLatin1("cover"));
    static const QString localMetadata(QString::fromLatin1("local"));
    static const QString fileScheme(QString::fromLatin1("file"));

    int fallbackScore = 0;
    QUrl fallbackUrl;

    QList<QContactAvatar> avatarDetails = contact.details<QContactAvatar>();
    for (int i = 0; i < avatarDetails.size(); ++i) {
        const QContactAvatar &av(avatarDetails[i]);

        const QString metadata(av.value(QContactAvatar__FieldAvatarMetadata).toString());
        if (!metadataFragment.isEmpty() && !metadata.startsWith(metadataFragment)) {
            // this avatar doesn't match the metadata requirement.  ignore it.
            continue;
        }

        const QUrl avatarImageUrl = av.imageUrl();

        if (metadata == localMetadata) {
            // We have a local avatar record - use the image it specifies
            matchingUrl = avatarImageUrl;
            return true;
        } else {
            // queue it as fallback if its score is better than the best fallback seen so far.
            // prefer local file system images over remote urls, and prefer normal avatars
            // over "cover" (background image) type avatars.
            const bool remote(!avatarImageUrl.scheme().isEmpty() && avatarImageUrl.scheme() != fileScheme);
            int score = remote ? 3 : 4;
            if (metadata == coverMetadata) {
                score -= 2;
            }

            if (score > fallbackScore) {
                fallbackUrl = avatarImageUrl;
                fallbackScore = score;
            }
        }
    }

    if (!fallbackUrl.isEmpty()) {
        matchingUrl = fallbackUrl;
        return true;
    }

    // no matching avatar image.
    return false;
}

QUrl SeasideCache::filteredAvatarUrl(const QContact &contact, const QStringList &metadataFragments)
{
    QUrl matchingUrl;

    if (metadataFragments.isEmpty()) {
        if (avatarUrlWithMetadata(contact, matchingUrl)) {
            return matchingUrl;
        }
    }

    foreach (const QString &metadataFragment, metadataFragments) {
        if (avatarUrlWithMetadata(contact, matchingUrl, metadataFragment)) {
            return matchingUrl;
        }
    }

    return QUrl();
}

QString SeasideCache::normalizePhoneNumber(const QString &input, bool validate)
{
    QtContactsSqliteExtensions::NormalizePhoneNumberFlags normalizeFlags(QtContactsSqliteExtensions::KeepPhoneNumberDialString);
    if (validate) {
        // If the number if not valid, return empty
        normalizeFlags |= QtContactsSqliteExtensions::ValidatePhoneNumber;
    }

    const QString normalized(QtContactsSqliteExtensions::normalizePhoneNumber(input, normalizeFlags));

    if (!normalized.isEmpty()) {
        // The normalized version may contain only DTMF chars - we should
        // only treat it as a phone number if has an initial numeric component
        const int digitIndex(normalized.at(0) == QLatin1Char('+') ? 1 : 0);
        if (digitIndex >= normalized.length() || !normalized.at(digitIndex).isDigit()) {
            return QString();
        }
    }

    return normalized;
}

QString SeasideCache::minimizePhoneNumber(const QString &input, bool validate)
{
    // TODO: use a configuration variable to make this configurable
    const int maxCharacters = QtContactsSqliteExtensions::DefaultMaximumPhoneNumberCharacters;

    QString validated(normalizePhoneNumber(input, validate));
    if (validated.isEmpty())
        return validated;

    return QtContactsSqliteExtensions::minimizePhoneNumber(validated, maxCharacters);
}

static QContactFilter filterForMergeCandidates(const QContact &contact)
{
    // Find any contacts that we might merge with the supplied contact
    QContactFilter rv;

    QContactName name(contact.detail<QContactName>());
    const QString firstName(name.firstName());
    const QString lastName(name.lastName());

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

            if (firstName.length() > 3) {
                // Also look for shortened forms of this name, such as 'Timothy' => 'Tim'
                QContactDetailFilter shortFilter;
                setDetailType<QContactName>(shortFilter, QContactName::FieldFirstName);
                shortFilter.setMatchFlags(QContactFilter::MatchStartsWith | QContactFilter::MatchFixedString);
                shortFilter.setValue(firstName.left(3));
                rv = rv | shortFilter;
            }
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
    foreach (const QContactPhoneNumber &phoneNumber, contact.details<QContactPhoneNumber>()) {
        const QString number(phoneNumber.number());
        if (number.isEmpty())
            continue;

        rv = rv | QContactPhoneNumber::match(number);
    }

    // Email address match
    foreach (const QContactEmailAddress &emailAddress, contact.details<QContactEmailAddress>()) {
        QString address(emailAddress.emailAddress());
        int index = address.indexOf(QChar::fromLatin1('@'));
        if (index > 0) {
            // Match any address that is the same up to the @ symbol
            address = address.left(index);
        }

        if (address.isEmpty())
            continue;

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

        if (uri.isEmpty())
            continue;

        QContactDetailFilter filter;
        setDetailType<QContactOnlineAccount>(filter, QContactOnlineAccount::FieldAccountUri);
        filter.setMatchFlags((index > 0 ? QContactFilter::MatchStartsWith : QContactFilter::MatchExactly) | QContactFilter::MatchFixedString);
        filter.setValue(uri);
        rv = rv | filter;
    }

    // If we know the contact gender rule out mismatches
    QContactGender gender(contact.detail<QContactGender>());
    if (gender.gender() != QContactGender::GenderUnspecified) {
        QContactDetailFilter matchFilter;
        setDetailType<QContactGender>(matchFilter, QContactGender::FieldGender);
        matchFilter.setValue(gender.gender());

        QContactDetailFilter unknownFilter;
        setDetailType<QContactGender>(unknownFilter, QContactGender::FieldGender);
        unknownFilter.setValue(QContactGender::GenderUnspecified);

        rv = rv & (matchFilter | unknownFilter);
    }

    // Only return aggregate contact IDs
    return rv & aggregateFilter();
}

void SeasideCache::startRequest(bool *idleProcessing)
{
    bool requestPending = false;

    // Test these conditions in priority order

    if (m_keepPopulated && (m_populateProgress != Populated)) {
        // We must populate the cache before we can do anything else
        if (m_fetchRequest.isActive()) {
            requestPending = true;
        } else {
            if (m_populateProgress == Unpopulated) {
                // Start a query to fully populate the cache, starting with favorites
                m_fetchRequest.setFilter(favoriteFilter());
                m_fetchRequest.setFetchHint(favoriteFetchHint(m_fetchTypes));
                m_fetchRequest.setSorting(m_sortOrder);
                m_fetchRequest.start();

                m_fetchProcessedCount = 0;
                m_populateProgress = FetchFavorites;
                m_dataTypesFetched |= m_fetchTypes;
            } else if (m_populateProgress == FetchMetadata) {
                // Next, query for all contacts
                // Request the metadata of all contacts (only data from the primary table, and any
                // other details required to determine whether the contacts matches the filter)
                m_fetchRequest.setFilter(allFilter());
                m_fetchRequest.setFetchHint(metadataFetchHint(m_fetchTypes));
                m_fetchRequest.setSorting(m_sortOrder);
                m_fetchRequest.start();

                m_fetchProcessedCount = 0;
            } else if (m_populateProgress == FetchOnline) {
                // Now query for online contacts - fetch the account details, so we know if they're valid
                m_fetchRequest.setFilter(onlineFilter());
                m_fetchRequest.setFetchHint(onlineFetchHint(m_fetchTypes | SeasideCache::FetchAccountUri));
                m_fetchRequest.setSorting(m_onlineSortOrder);
                m_fetchRequest.start();

                m_fetchProcessedCount = 0;
            }
        }

        // Do nothing else until the cache is populated
        return;
    }

    if (m_refreshRequired) {
        // We can't refresh the IDs til all contacts have been appended
        if (m_contactsToAppend.isEmpty()) {
            if (m_contactIdRequest.isActive()) {
                requestPending = true;
            } else {
                m_refreshRequired = false;
                m_syncFilter = FilterFavorites;

                m_contactIdRequest.setFilter(favoriteFilter());
                m_contactIdRequest.setSorting(m_sortOrder);
                m_contactIdRequest.start();
            }
        }
    } else if (m_syncFilter == FilterAll || m_syncFilter == FilterOnline) {
        if (m_contactIdRequest.isActive()) {
            requestPending = true;
        } else {
            if (m_syncFilter == FilterAll) {
                m_contactIdRequest.setFilter(allFilter());
                m_contactIdRequest.setSorting(m_sortOrder);
            } else if (m_syncFilter == FilterOnline) {
                m_contactIdRequest.setFilter(onlineFilter());
                m_contactIdRequest.setSorting(m_onlineSortOrder);
            }

            m_contactIdRequest.start();
        }
    }

    if (!m_relationshipsToSave.isEmpty() || !m_relationshipsToRemove.isEmpty()) {
        // this has to be before contact saves are processed so that the disaggregation flow
        // works properly
        if (!m_relationshipsToSave.isEmpty()) {
            if (!m_relationshipSaveRequest.isActive()) {
                m_relationshipSaveRequest.setRelationships(m_relationshipsToSave);
                m_relationshipSaveRequest.start();

                m_relationshipsToSave.clear();
            }
        }
        if (!m_relationshipsToRemove.isEmpty()) {
            if (!m_relationshipRemoveRequest.isActive()) {
                m_relationshipRemoveRequest.setRelationships(m_relationshipsToRemove);
                m_relationshipRemoveRequest.start();

                m_relationshipsToRemove.clear();
            }
        }

        // do not proceed with other tasks, even if we couldn't start a new request
        return;
    }

    if (!m_contactsToRemove.isEmpty()) {
        if (m_removeRequest.isActive()) {
            requestPending = true;
        } else {
            m_removeRequest.setContactIds(m_contactsToRemove);
            m_removeRequest.start();

            m_contactsToRemove.clear();
        }
    }

    if (!m_contactsToCreate.isEmpty() || !m_contactsToSave.isEmpty()) {
        if (m_saveRequest.isActive()) {
            requestPending = true;
        } else {
            m_contactsToCreate.reserve(m_contactsToCreate.count() + m_contactsToSave.count());

            typedef QHash<QContactId, QContact>::iterator iterator;
            for (iterator it = m_contactsToSave.begin(); it != m_contactsToSave.end(); ++it) {
                m_contactsToCreate.append(*it);
            }

            m_saveRequest.setContacts(m_contactsToCreate);
            m_saveRequest.start();

            m_contactsToCreate.clear();
            m_contactsToSave.clear();
        }
    }

    if (!m_constituentIds.isEmpty()) {
        if (m_fetchByIdRequest.isActive()) {
            requestPending = true;
        } else {
            // Fetch the constituent information (even if they're already in the
            // cache, because we don't update non-aggregates on change notifications)
            m_fetchByIdRequest.setIds(m_constituentIds.toList());
            m_fetchByIdRequest.start();

            m_fetchByIdProcessedCount = 0;
        }
    }

    if (!m_contactsToFetchConstituents.isEmpty()) {
        if (m_relationshipsFetchRequest.isActive()) {
            requestPending = true;
        } else {
            QContactId aggregateId = m_contactsToFetchConstituents.first();

            // Find the constituents of this contact
            QContact first;
            first.setId(aggregateId);
            m_relationshipsFetchRequest.setFirst(first);
            m_relationshipsFetchRequest.setRelationshipType(QContactRelationship::Aggregates());
            m_relationshipsFetchRequest.start();
        }
    }

    if (!m_contactsToFetchCandidates.isEmpty()) {
        if (m_contactIdRequest.isActive()) {
            requestPending = true;
        } else {
            QContactId contactId(m_contactsToFetchCandidates.first());
            const QContact contact(contactById(contactId));

            // Find candidates to merge with this contact
            m_contactIdRequest.setFilter(filterForMergeCandidates(contact));
            m_contactIdRequest.setSorting(m_sortOrder);
            m_contactIdRequest.start();
        }
    }

    if (m_fetchTypes) {
        quint32 unfetchedTypes = m_fetchTypes & ~m_dataTypesFetched & SeasideCache::FetchTypesMask;
        if (unfetchedTypes) {
            if (m_fetchRequest.isActive()) {
                requestPending = true;
            } else {
                // Fetch the missing data types for whichever contacts need them
                if (unfetchedTypes == SeasideCache::FetchPhoneNumber) {
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasPhoneNumber, QContactFilter::MatchContains));
                } else if (unfetchedTypes == SeasideCache::FetchEmailAddress) {
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasEmailAddress, QContactFilter::MatchContains));
                } else if (unfetchedTypes == SeasideCache::FetchAccountUri) {
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasOnlineAccount, QContactFilter::MatchContains));
                } else {
                    m_fetchRequest.setFilter(allFilter());
                }

                m_fetchRequest.setFetchHint(extendedMetadataFetchHint(unfetchedTypes));
                m_fetchRequest.start();

                m_fetchProcessedCount = 0;
                m_dataTypesFetched |= unfetchedTypes;
            }
        }
    }

    if (!m_resolveAddresses.isEmpty()) {
        if (m_fetchRequest.isActive()) {
            requestPending = true;
        } else {
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
            m_fetchRequest.setFetchHint(resolve.requireComplete ? basicFetchHint() : favoriteFetchHint(m_fetchTypes | m_extraFetchTypes));
            m_fetchRequest.start();

            m_fetchProcessedCount = 0;
        }
    }

    if (!m_changedContacts.isEmpty()) {
        if (m_fetchRequest.isActive()) {
            requestPending = true;
        } else if (!m_displayOff) {
            // If we request too many IDs we will exceed the SQLite bound variables limit
            // The actual limit is over 800, but we should reduce further to increase interactivity
            const int maxRequestIds = 200;

            QContactIdFilter filter;
            if (m_changedContacts.count() > maxRequestIds) {
                filter.setIds(m_changedContacts.mid(0, maxRequestIds));
                m_changedContacts = m_changedContacts.mid(maxRequestIds);
            } else {
                filter.setIds(m_changedContacts);
                m_changedContacts.clear();
            }

            // A local ID filter will fetch all contacts, rather than just aggregates;
            // we only want to retrieve aggregate contacts that have changed
            m_fetchRequest.setFilter(filter & aggregateFilter());
            m_fetchRequest.setFetchHint(basicFetchHint());
            m_fetchRequest.start();

            m_fetchProcessedCount = 0;
        }
    }

    if (!m_presenceChangedContacts.isEmpty()) {
        if (m_fetchRequest.isActive()) {
            requestPending = true;
        } else if (!m_displayOff) {
            const int maxRequestIds = 200;

            QContactIdFilter filter;
            if (m_presenceChangedContacts.count() > maxRequestIds) {
                filter.setIds(m_presenceChangedContacts.mid(0, maxRequestIds));
                m_presenceChangedContacts = m_presenceChangedContacts.mid(maxRequestIds);
            } else {
                filter.setIds(m_presenceChangedContacts);
                m_presenceChangedContacts.clear();
            }

            m_fetchRequest.setFilter(filter & aggregateFilter());
            m_fetchRequest.setFetchHint(presenceFetchHint());
            m_fetchRequest.start();

            m_fetchProcessedCount = 0;
        }
    }

    if (requestPending) {
        // Don't proceed if we were unable to start one of the above requests
        return;
    }

    // No remaining work is pending - do we have any background task requests?
    if (m_extraFetchTypes) {
        quint32 unfetchedTypes = m_extraFetchTypes & ~m_dataTypesFetched & SeasideCache::FetchTypesMask;
        if (unfetchedTypes) {
            if (m_fetchRequest.isActive()) {
                requestPending = true;
            } else {
                quint32 fetchType = 0;

                // Load extra data items that we want to be able to search on, if not already fetched
                if (unfetchedTypes & SeasideCache::FetchOrganization) {
                    fetchType = SeasideCache::FetchOrganization;
                    m_fetchRequest.setFilter(allFilter());
                } else if (unfetchedTypes & SeasideCache::FetchPhoneNumber) {
                    fetchType = SeasideCache::FetchPhoneNumber;
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasPhoneNumber, QContactFilter::MatchContains));
                } else if (unfetchedTypes & SeasideCache::FetchEmailAddress) {
                    fetchType = SeasideCache::FetchEmailAddress;
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasEmailAddress, QContactFilter::MatchContains));
                } else {
                    fetchType = SeasideCache::FetchAccountUri;
                    m_fetchRequest.setFilter(QContactStatusFlags::matchFlag(QContactStatusFlags::HasOnlineAccount, QContactFilter::MatchContains));
                }

                m_fetchRequest.setFetchHint(extendedMetadataFetchHint(fetchType));
                m_fetchRequest.start();

                m_fetchProcessedCount = 0;
                m_dataTypesFetched |= fetchType;
            }
        }
    }

    if (!requestPending) {
        // Nothing to do - proceeed with idle processing
        *idleProcessing = true;
    }
}

bool SeasideCache::event(QEvent *event)
{
    if (event->type() != QEvent::UpdateRequest)
        return QObject::event(event);

    bool idleProcessing = false;
    startRequest(&idleProcessing);

    if (!m_unknownResolveAddresses.isEmpty()) {
        // Report any unknown addresses
        while (!m_unknownResolveAddresses.isEmpty()) {
            const ResolveData &resolve = m_unknownResolveAddresses.takeFirst();
            resolve.listener->addressResolved(resolve.first, resolve.second, 0);
        }
    }

    if (!m_contactsToAppend.isEmpty() || !m_contactsToUpdate.isEmpty()) {
        applyPendingContactUpdates();

        // Send another event to trigger further processing
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        return true;
    }

    if (idleProcessing) {
        // We have nothing pending to do
        m_updatesPending = false;

        // Remove expired contacts when all other activity has been processed
        if (!m_expiredContacts.isEmpty()) {
            QList<quint32> removeIds;

            QHash<QContactId, int>::const_iterator it = m_expiredContacts.constBegin(), end = m_expiredContacts.constEnd();
            for ( ; it != end; ++it) {
                if (it.value() < 0) {
                    quint32 iid = internalId(it.key());
                    removeIds.append(iid);
                }
            }
            m_expiredContacts.clear();

            QSet<QString> modifiedGroups;

            // Before removal, ensure none of these contacts are in name groups
            foreach (quint32 iid, removeIds) {
                if (CacheItem *item = existingItem(iid)) {
                    removeFromContactNameGroup(item->iid, item->nameGroup, &modifiedGroups);
                }
            }

            notifyNameGroupsChanged(modifiedGroups);

            // Remove the contacts from the cache
            foreach (quint32 iid, removeIds) {
                QHash<quint32, CacheItem>::iterator cacheItem = m_people.find(iid);
                if (cacheItem != m_people.end()) {
                    delete cacheItem->itemData;
                    m_people.erase(cacheItem);
                }
            }
        }
    }
    return true;
}

void SeasideCache::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_fetchTimer.timerId()) {
        // If the display is off, defer these fetches until they can be seen
        if (!m_displayOff) {
            fetchContacts();
        }
    }

    if (event->timerId() == m_expiryTimer.timerId()) {
        m_expiryTimer.stop();
        instancePtr = 0;
        deleteLater();
    }
}

void SeasideCache::contactsAdded(const QList<QContactId> &ids)
{
    if (m_keepPopulated) {
        updateContacts(ids, &m_changedContacts);
    }
}

void SeasideCache::contactsChanged(const QList<QContactId> &ids)
{
    if (m_keepPopulated) {
        updateContacts(ids, &m_changedContacts);
    } else {
        // Update these contacts if they're already in the cache
        QList<QContactId> presentIds;
        foreach (const QContactId &id, ids) {
            if (existingItem(id)) {
                presentIds.append(id);
            }
        }
        updateContacts(presentIds, &m_changedContacts);
    }
}

void SeasideCache::contactsPresenceChanged(const QList<QContactId> &ids)
{
    if (m_keepPopulated) {
        updateContacts(ids, &m_presenceChangedContacts);
    } else {
        // Update these contacts if they're already in the cache
        QList<QContactId> presentIds;
        foreach (const QContactId &id, ids) {
            if (existingItem(id)) {
                presentIds.append(id);
            }
        }
        updateContacts(presentIds, &m_presenceChangedContacts);
    }
}

void SeasideCache::contactsRemoved(const QList<QContactId> &ids)
{
    QList<QContactId> presentIds;

    foreach (const QContactId &id, ids) {
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

            // Remove the links to addressible details
            updateContactIndexing(item->contact, QContact(), item->iid, QSet<QContactDetail::DetailType>(), item);

            if (!m_keepPopulated) {
                presentIds.append(id);
            }
        }
    }

    if (m_keepPopulated) {
        m_refreshRequired = true;
    } else {
        // Remove these contacts if they're already in the cache; they won't be removed by syncing
        foreach (const QContactId &id, presentIds) {
            m_expiredContacts[id] += -1;
        }
    }

    requestUpdate();
}

void SeasideCache::updateContacts()
{
    QList<QContactId> contactIds;

    typedef QHash<quint32, CacheItem>::iterator iterator;
    for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
        if (it->contactState != ContactAbsent)
            contactIds.append(it->apiId());
    }

    updateContacts(contactIds, &m_changedContacts);
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

void SeasideCache::updateContacts(const QList<QContactId> &contactIds, QList<QContactId> *updateList)
{
    // Wait for new changes to be reported
    static const int PostponementIntervalMs = 500;

    // Maximum wait until we fetch all changes previously reported
    static const int MaxPostponementMs = 5000;

    if (!contactIds.isEmpty()) {
        m_contactsUpdated = true;
        updateList->append(contactIds);

        // If the display is off, defer fetching these changes
        if (!m_displayOff) {
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
}

void SeasideCache::updateCache(CacheItem *item, const QContact &contact, bool partialFetch, bool initialInsert)
{
    if (item->contactState < ContactRequested) {
        item->contactState = partialFetch ? ContactPartial : ContactComplete;
    } else if (!partialFetch) {
        // Don't set a complete contact back after a partial update
        item->contactState = ContactComplete;
    }

    // Preserve the value of HasValidOnlineAccount, which is held only in the cache
    const int hasValidFlagValue = item->statusFlags & HasValidOnlineAccount;
    item->statusFlags = contact.detail<QContactStatusFlags>().flagsValue() | hasValidFlagValue;

    if (item->itemData) {
        item->itemData->updateContact(contact, &item->contact, item->contactState);
    } else {
        item->contact = contact;
    }

    item->displayLabel = generateDisplayLabel(item->contact, m_displayLabelOrder);
    item->nameGroup = determineNameGroup(item);

    if (!initialInsert) {
        reportItemUpdated(item);
    }
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

bool SeasideCache::updateContactIndexing(const QContact &oldContact, const QContact &contact, quint32 iid, const QSet<QContactDetail::DetailType> &queryDetailTypes, CacheItem *item)
{
    bool modified = false;

    QSet<StringPair> oldAddresses;

    if (queryDetailTypes.isEmpty() || queryDetailTypes.contains(detailType<QContactPhoneNumber>())) {
        // Addresses which are no longer in the contact should be de-indexed
        foreach (const QContactPhoneNumber &phoneNumber, oldContact.details<QContactPhoneNumber>()) {
            foreach (const StringPair &address, addressPairs(phoneNumber)) {
                if (validAddressPair(address))
                    oldAddresses.insert(address);
            }
        }

        // Update our address indexes for any address details in this contact
        foreach (const QContactPhoneNumber &phoneNumber, contact.details<QContactPhoneNumber>()) {
            foreach (const StringPair &address, addressPairs(phoneNumber)) {
                if (!validAddressPair(address))
                    continue;

                if (!oldAddresses.remove(address)) {
                    // This address was not previously recorded
                    modified = true;
                    resolveUnknownAddresses(address.first, address.second, item);
                }

                m_phoneNumberIds.insert(address.second, iid);
            }
        }

        // Remove any addresses no longer available for this contact
        if (!oldAddresses.isEmpty()) {
            modified = true;
            foreach (const StringPair &address, oldAddresses) {
                m_phoneNumberIds.remove(address.second, iid);
            }
            oldAddresses.clear();
        }
    }

    if (queryDetailTypes.isEmpty() || queryDetailTypes.contains(detailType<QContactEmailAddress>())) {
        foreach (const QContactEmailAddress &emailAddress, oldContact.details<QContactEmailAddress>()) {
            const StringPair address(addressPair(emailAddress));
            if (validAddressPair(address))
                oldAddresses.insert(address);
        }

        foreach (const QContactEmailAddress &emailAddress, contact.details<QContactEmailAddress>()) {
            const StringPair address(addressPair(emailAddress));
            if (!validAddressPair(address))
                continue;

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
            const StringPair address(addressPair(account));
            if (validAddressPair(address))
                oldAddresses.insert(address);
        }

        // Keep track of whether this contact has any valid IM accounts
        bool hasValid = false;

        foreach (const QContactOnlineAccount &account, contact.details<QContactOnlineAccount>()) {
            const StringPair address(addressPair(account));
            if (!validAddressPair(address))
                continue;

            if (!oldAddresses.remove(address)) {
                modified = true;
                resolveUnknownAddresses(address.first, address.second, item);
            }

            m_onlineAccountIds[address] = iid;
            hasValid = true;
        }

        if (hasValid) {
            item->statusFlags |= HasValidOnlineAccount;
        } else {
            item->statusFlags &= ~HasValidOnlineAccount;
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

void updateDetailsFromCache(QContact &contact, SeasideCache::CacheItem *item, const QSet<QContactDetail::DetailType> &queryDetailTypes)
{
    // Copy any existing detail types that are in the current record to the new instance
    foreach (const QContactDetail &existing, item->contact.details()) {
        const QContactDetail::DetailType existingType(detailType(existing));

        static const DetailList contactsTableTypes(contactsTableDetails());

        // The queried contact already contains any types in the contacts table, and those
        // types explicitly fetched by the query
        if (!queryDetailTypes.contains(existingType) && !contactsTableTypes.contains(existingType)) {
            QContactDetail copy(existing);
            contact.saveDetail(&copy);
        }
    }
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
    if (contacts.isEmpty())
        return;

    QSet<QContactDetail::DetailType> queryDetailTypes;
    foreach (const QContactDetail::DetailType &typeId, detailTypesHint(fetchHint)) {
        queryDetailTypes.insert(typeId);
    }
    const bool partialFetch = !queryDetailTypes.isEmpty();

    if (m_populateProgress > Unpopulated && m_populateProgress < Populated) {
        // We are populating the cache
        FilterType type(m_populateProgress == FetchFavorites ? FilterFavorites
                                                             : (m_populateProgress == FetchMetadata ? FilterAll
                                                                                                    : FilterOnline));
        QHash<FilterType, QPair<QSet<QContactDetail::DetailType>, QList<QContact> > >::iterator it = m_contactsToAppend.find(type);
        if (it != m_contactsToAppend.end()) {
            // All populate queries have the same detail types, so we can append this list to the existing one
            it.value().second.append(contacts);
        } else {
            m_contactsToAppend.insert(type, qMakePair(queryDetailTypes, contacts));
        }
    } else {
        if (m_activeResolve || (request == &m_fetchByIdRequest)) {
            // Process these results immediately
            applyContactUpdates(contacts, partialFetch, queryDetailTypes);
        } else {
            // Add these contacts to the list to be progressively appended
            QList<QPair<QSet<QContactDetail::DetailType>, QList<QContact> > >::iterator it = m_contactsToUpdate.begin(), end = m_contactsToUpdate.end();
            for ( ; it != end; ++it) {
                if ((*it).first == queryDetailTypes) {
                    (*it).second.append(contacts);
                    break;
                }
            }
            if (it == end) {
                m_contactsToUpdate.append(qMakePair(queryDetailTypes, contacts));
            }

            requestUpdate();
        }
    }
}

void SeasideCache::applyPendingContactUpdates()
{
    if (!m_contactsToAppend.isEmpty()) {
        // Insert the contacts in the order they're requested
        QHash<FilterType, QPair<QSet<QContactDetail::DetailType>, QList<QContact> > >::iterator end = m_contactsToAppend.end(), it = end;
        if ((it = m_contactsToAppend.find(FilterFavorites)) != end) {
        } else if ((it = m_contactsToAppend.find(FilterAll)) != end) {
        } else {
            it = m_contactsToAppend.find(FilterOnline);
        }
        Q_ASSERT(it != end);

        FilterType type = it.key();
        QSet<QContactDetail::DetailType> &detailTypes((*it).first);
        const bool partialFetch = !detailTypes.isEmpty();

        QList<QContact> &appendedContacts((*it).second);

        const int maxBatchSize = 200;
        const int minBatchSize = 50;

        if (appendedContacts.count() < maxBatchSize) {
            // For a small number of contacts, append all at once
            appendContacts(appendedContacts, type, partialFetch, detailTypes);
            appendedContacts.clear();
        } else {
            // Append progressively in batches
            appendContacts(appendedContacts.mid(0, minBatchSize), type, partialFetch, detailTypes);
            appendedContacts = appendedContacts.mid(minBatchSize);
        }

        if (appendedContacts.isEmpty()) {
            m_contactsToAppend.erase(it);

            // This list has been processed - have we finished populating the group?
            if (type == FilterFavorites && (m_populateProgress != FetchFavorites)) {
                makePopulated(FilterFavorites);
                qDebug() << "Favorites queried in" << m_timer.elapsed() << "ms";
            } else if (type == FilterAll && (m_populateProgress != FetchMetadata)) {
                makePopulated(FilterNone);
                makePopulated(FilterAll);
                qDebug() << "All queried in" << m_timer.elapsed() << "ms";
            } else if (type == FilterOnline && (m_populateProgress != FetchOnline)) {
                makePopulated(FilterOnline);
                qDebug() << "Online queried in" << m_timer.elapsed() << "ms";
            }
        }
    } else {
        QList<QPair<QSet<QContactDetail::DetailType>, QList<QContact> > >::iterator it = m_contactsToUpdate.begin();

        QSet<QContactDetail::DetailType> &detailTypes((*it).first);
        const bool partialFetch = !detailTypes.isEmpty();

        // Update a single contact at a time; the update can cause numerous QML bindings
        // to be re-evaluated, so even a single contact update might be a slow operation
        QList<QContact> &updatedContacts((*it).second);
        applyContactUpdates(QList<QContact>() << updatedContacts.takeFirst(), partialFetch, detailTypes);

        if (updatedContacts.isEmpty()) {
            m_contactsToUpdate.erase(it);
        }
    }
}

void SeasideCache::applyContactUpdates(const QList<QContact> &contacts, bool partialFetch, const QSet<QContactDetail::DetailType> &queryDetailTypes)
{
    QSet<QString> modifiedGroups;

    foreach (QContact contact, contacts) {
        quint32 iid = internalId(contact);

        QString oldNameGroup;
        QString oldDisplayLabel;

        CacheItem *item = existingItem(iid);
        if (!item) {
            // We haven't seen this contact before
            item = &(m_people[iid]);
            item->iid = iid;
        } else {
            oldNameGroup = item->nameGroup;
            oldDisplayLabel = item->displayLabel;

            if (partialFetch) {
                // Update our new instance with any details not returned by the current query
                updateDetailsFromCache(contact, item, queryDetailTypes);
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

        updateCache(item, contact, partialFetch, false);
        roleDataChanged |= (item->displayLabel != oldDisplayLabel);

        // do this even if !roleDataChanged as name groups are affected by other display label changes
        if (item->nameGroup != oldNameGroup) {
            if (!ignoreContactForNameGroups(item->contact)) {
                addToContactNameGroup(item->iid, item->nameGroup, &modifiedGroups);
                removeFromContactNameGroup(item->iid, oldNameGroup, &modifiedGroups);
            }
        }

        if (roleDataChanged) {
            instancePtr->contactDataChanged(item->iid);
        }
    }

    notifyNameGroupsChanged(modifiedGroups);
}

void SeasideCache::addToContactNameGroup(quint32 iid, const QString &group, QSet<QString> *modifiedGroups)
{
    if (!group.isEmpty()) {
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
    if (!group.isEmpty()) {
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
        foreach (const QContactId &id, m_contactIdRequest.ids()) {
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
    static const QString aggregatesRelationship = QContactRelationship::Aggregates();

    foreach (const QContactRelationship &rel, m_relationshipsFetchRequest.relationships()) {
        if (rel.relationshipType() == aggregatesRelationship) {
            m_constituentIds.insert(apiId(rel.second()));
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

        cacheIds.removeAt(index);
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsRemoved();
}

int SeasideCache::insertRange(FilterType filter, int index, int count, const QList<quint32> &queryIds, int queryIndex)
{
    QList<quint32> &cacheIds = m_contacts[filter];
    QList<ListModel *> &models = m_models[filter];

    const quint32 selfId = internalId(manager()->selfContactId());

    int end = index + count - 1;
    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceAboutToInsertItems(index, end);

    for (int i = 0; i < count; ++i) {
        quint32 iid = queryIds.at(queryIndex + i);
        if (iid == selfId)
            continue;

        if (filter == FilterAll) {
            const QContactId apiId = SeasideCache::apiId(iid);
            m_expiredContacts[apiId] += 1;
        }

        cacheIds.insert(index + i, iid);
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsInserted(index, end);

    return end - index + 1;
}

void SeasideCache::appendContacts(const QList<QContact> &contacts, FilterType filterType, bool partialFetch, const QSet<QContactDetail::DetailType> &queryDetailTypes)
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

                CacheItem *item = existingItem(iid);
                if (!item) {
                    item = &(m_people[iid]);
                    item->iid = iid;
                } else {
                    if (partialFetch) {
                        // Update our new instance with any details not returned by the current query
                        updateDetailsFromCache(contact, item, queryDetailTypes);
                    }
                }

                updateContactIndexing(item->contact, contact, iid, queryDetailTypes, item);
                updateCache(item, contact, partialFetch, true);

                if (filterType == FilterAll) {
                    addToContactNameGroup(iid, nameGroup(item), &modifiedGroups);
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

    if (request == &m_relationshipsFetchRequest) {
        if (!m_contactsToFetchConstituents.isEmpty()) {
            QContactId aggregateId = m_contactsToFetchConstituents.takeFirst();
            if (!m_constituentIds.isEmpty()) {
                m_contactsToLinkTo.append(aggregateId);
            } else {
                // We didn't find any constituents - report the empty list
                CacheItem *cacheItem = itemById(aggregateId);
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
            CacheItem *cacheItem = itemById(aggregateId);

            QList<int> constituentIds;
            foreach (const QContactId &id, m_constituentIds) {
                constituentIds.append(internalId(id));
            }
            m_constituentIds.clear();

            if (cacheItem->itemData) {
                cacheItem->itemData->constituentsFetched(constituentIds);
            }

            updateConstituentAggregations(cacheItem->apiId());
        }
    } else if (request == &m_contactIdRequest) {
        if (m_syncFilter != FilterNone) {
            // We have completed fetching this filter set
            completeSynchronizeList(this, m_contacts[m_syncFilter], m_cacheIndex, internalIds(m_contactIdRequest.ids()), m_queryIndex);

            // Notify models of completed updates
            QList<ListModel *> &models = m_models[m_syncFilter];
            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceItemsChanged();

            if (m_syncFilter == FilterFavorites) {
                // Next, query for all contacts (including favorites)
                m_syncFilter = FilterAll;
            } else if (m_syncFilter == FilterAll) {
                // Next, query for online contacts
                m_syncFilter = FilterOnline;
            } else if (m_syncFilter == FilterOnline) {
                m_syncFilter = FilterNone;
            }
        } else if (!m_contactsToFetchCandidates.isEmpty()) {
            // Report these results
            QContactId contactId = m_contactsToFetchCandidates.takeFirst();
            CacheItem *cacheItem = itemById(contactId);

            const quint32 contactIid = internalId(contactId);

            QList<int> candidateIds;
            foreach (const QContactId &id, m_candidateIds) {
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
        } else {
            qWarning() << "ID fetch completed with no filter?";
        }
    } else if (request == &m_relationshipSaveRequest || request == &m_relationshipRemoveRequest) {
        bool completed = false;
        QList<QContactRelationship> relationships;
        if (request == &m_relationshipSaveRequest) {
            relationships = m_relationshipSaveRequest.relationships();
            completed = !m_relationshipRemoveRequest.isActive();
        } else {
            relationships = m_relationshipRemoveRequest.relationships();
            completed = !m_relationshipSaveRequest.isActive();
        }

        foreach (const QContactRelationship &relationship, relationships) {
            m_aggregatedContacts.insert(SeasideCache::apiId(relationship.first()));
        }

        if (completed) {
            foreach (const QContactId &contactId, m_aggregatedContacts) {
                CacheItem *cacheItem = itemById(contactId);
                if (cacheItem && cacheItem->itemData)
                    cacheItem->itemData->aggregationOperationCompleted();
            }

            // We need to update these modified contacts immediately
            foreach (const QContactId &id, m_aggregatedContacts)
                m_changedContacts.append(id);
            fetchContacts();

            m_aggregatedContacts.clear();
        }
    } else if (request == &m_fetchRequest) {
        if (m_populateProgress == FetchFavorites) {
            if (m_contactsToAppend.find(FilterFavorites) == m_contactsToAppend.end()) {
                // No pending contacts, the models are now populated
                makePopulated(FilterFavorites);
                qDebug() << "Favorites queried in" << m_timer.elapsed() << "ms";
            }

            m_populateProgress = FetchMetadata;
        } else if (m_populateProgress == FetchMetadata) {
            if (m_contactsToAppend.find(FilterAll) == m_contactsToAppend.end()) {
                makePopulated(FilterNone);
                makePopulated(FilterAll);
                qDebug() << "All queried in" << m_timer.elapsed() << "ms";
            }

            m_populateProgress = FetchOnline;
        } else if (m_populateProgress == FetchOnline) {
            if (m_contactsToAppend.find(FilterOnline) == m_contactsToAppend.end()) {
                makePopulated(FilterOnline);
                qDebug() << "Online queried in" << m_timer.elapsed() << "ms";
            }

            m_populateProgress = Populated;
        } else {
            // Result of a specific query
            if (m_activeResolve) {
                if (m_activeResolve->first == QString()) {
                    // We have now queried this phone number
                    m_resolvedPhoneNumbers.insert(minimizePhoneNumber(m_activeResolve->second));
                }

                CacheItem *item = 0;
                const QList<QContact> &resolvedContacts(m_fetchRequest.contacts());
                if (!resolvedContacts.isEmpty()) {
                    if (resolvedContacts.count() == 1) {
                        item = itemById(apiId(resolvedContacts.first()), false);
                    } else {
                        // Lookup the result in our updated indexes
                        ResolveData data(*m_activeResolve);
                        if (data.first == QString()) {
                            item = itemByPhoneNumber(data.second, false);
                        } else if (data.second == QString()) {
                            item = itemByEmailAddress(data.first, false);
                        } else {
                            item = itemByOnlineAccount(data.first, data.second, false);
                        }
                    }
                } else {
                    // This address is unknown - keep it for later resolution
                    ResolveData data(*m_activeResolve);
                    if (data.first == QString()) {
                        // Compare this phone number in minimized form
                        data.compare = minimizePhoneNumber(data.second);
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

    // See if there are any more requests to dispatch
    QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
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
                    removeFromContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);

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
                    removeFromContactNameGroup(it->iid, it->nameGroup, &modifiedGroups);

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

void SeasideCache::displayStatusChanged(const QString &status)
{
    const bool off = (status == QLatin1String(MCE_DISPLAY_OFF_STRING));
    if (m_displayOff != off) {
        m_displayOff = off;

        if (!m_displayOff) {
            // The display has been enabled; check for pending fetches
            requestUpdate();
        }
    }
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

    QList<QContactId> contactsToFetch;
    contactsToFetch.reserve(instancePtr->m_people.count());

    const quint32 selfId = internalId(manager()->selfContactId());

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
        QList<QContact> fetchedContacts = manager()->contacts(contactsToFetch);
        contacts.append(fetchedContacts);
    }

    if (!exporter.exportContacts(contacts)) {
        qWarning() << Q_FUNC_INFO << "Failed to export contacts: " << exporter.errorMap();
        return QString();
    }

    QString baseDir;
    foreach (const QString &loc, QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation)) {
        baseDir = loc;
        break;
    }
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

void SeasideCache::keepPopulated(quint32 requiredTypes, quint32 extraTypes)
{
    bool updateRequired(false);

    // If these types are required, we will fetch them immediately
    quint32 unfetchedTypes = requiredTypes & ~m_fetchTypes & SeasideCache::FetchTypesMask;
    if (unfetchedTypes) {
        m_fetchTypes |= requiredTypes;
        updateRequired = true;
    }

    // Otherwise, we can fetch them when idle
    unfetchedTypes = extraTypes & ~m_extraFetchTypes & SeasideCache::FetchTypesMask;
    if (unfetchedTypes) {
        m_extraFetchTypes |= extraTypes;
        updateRequired = true;
    }

    if (((requiredTypes | extraTypes) & SeasideCache::FetchPhoneNumber) != 0) {
        // We won't need to check resolved numbers any further
        m_resolvedPhoneNumbers.clear();
    }

    if (!m_keepPopulated) {
        m_keepPopulated = true;
        updateRequired = true;
    }

    if (updateRequired) {
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

    instancePtr->requestUpdate();
}

void SeasideCache::updateConstituentAggregations(const QContactId &contactId)
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
void SeasideCache::completeContactAggregation(const QContactId &contact1Id, const QContactId &contact2Id)
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

    // Is this address a known-unknown?
    bool knownUnknown = false;
    QList<ResolveData>::const_iterator it = instancePtr->m_unknownAddresses.constBegin(), end = m_unknownAddresses.constEnd();
    for ( ; it != end; ++it) {
        if (it->first == first && it->second == second) {
            knownUnknown = true;
            break;
        }
    }

    if (knownUnknown) {
        m_unknownResolveAddresses.append(data);
    } else {
        m_resolveAddresses.append(data);
    }

    requestUpdate();
}

SeasideCache::CacheItem *SeasideCache::itemMatchingPhoneNumber(const QString &number, const QString &normalized, bool requireComplete)
{
    QMultiHash<QString, quint32>::const_iterator it = m_phoneNumberIds.find(number), end = m_phoneNumberIds.constEnd();
    if (it != end) {
        // How many matches are there for this number?
        int matchCount = 1;
        QMultiHash<QString, quint32>::const_iterator matchingIt = it + 1;
        while ((matchingIt != end) && (matchingIt.key() == number)) {
             ++matchCount;
             ++matchingIt;
        }
        if (matchCount == 1)
            return itemById(*it, requireComplete);

        // Choose the best match from these contacts
        int bestMatchLength = 0;
        CacheItem *matchItem = 0;
        for ( ; matchCount > 0; ++it, --matchCount) {
            if (CacheItem *item = existingItem(*it)) {
                int matchLength = bestPhoneNumberMatchLength(item->contact, normalized);
                if (matchLength > bestMatchLength) {
                    bestMatchLength = matchLength;
                    matchItem = item;
                    if (bestMatchLength == ExactMatch)
                        break;
                }
            }
        }

        if (matchItem != 0) {
            if (requireComplete) {
                ensureCompletion(matchItem);
            }
            return matchItem;
        }
    }

    return 0;
}

int SeasideCache::contactIndex(quint32 iid, FilterType filterType)
{
    const QList<quint32> &cacheIds(m_contacts[filterType]);
    return cacheIds.indexOf(iid);
}

QContactRelationship SeasideCache::makeRelationship(const QString &type, const QContact &contact1, const QContact &contact2)
{
    QContactRelationship relationship;
    relationship.setRelationshipType(type);
    relationship.setFirst(contact1);
    relationship.setSecond(contact2);
    return relationship;
}

// Instantiate the contact ID functions for qtcontacts-sqlite
#include <qtcontacts-extensions_impl.h>
