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

#ifndef SEASIDECACHE_H
#define SEASIDECACHE_H

#include "contactcacheexport.h"
#include "seasidenamegrouper.h"

#include <qtcontacts-extensions.h>
#include <QContactStatusFlags>

#include <QContact>
#include <QContactManager>
#include <QContactFetchRequest>
#include <QContactFetchByIdRequest>
#include <QContactRemoveRequest>
#include <QContactSaveRequest>
#include <QContactRelationshipFetchRequest>
#include <QContactRelationshipSaveRequest>
#include <QContactRelationshipRemoveRequest>
#ifdef USING_QTPIM
#include <QContactIdFilter>
#include <QContactIdFetchRequest>
#else
#include <QContactLocalIdFilter>
#include <QContactLocalIdFetchRequest>
#endif

#include <QBasicTimer>
#include <QSet>

#include <QElapsedTimer>
#include <QAbstractListModel>

#ifdef HAS_MLITE
#include <mgconfitem.h>
#endif

#ifdef USING_QTPIM
QTCONTACTS_USE_NAMESPACE

typedef QContactDetail::DetailType DetailTypeId;
#else
QTM_USE_NAMESPACE

typedef QString DetailTypeId;
#endif

class CONTACTCACHE_EXPORT SeasideNameGroupChangeListener
{
public:
    SeasideNameGroupChangeListener() {}
    ~SeasideNameGroupChangeListener() {}

    virtual void nameGroupsUpdated(const QHash<QString, QSet<quint32> > &groups) = 0;
};

class CONTACTCACHE_EXPORT SeasideCache : public QObject
{
    Q_OBJECT
public:
    typedef QtContactsSqliteExtensions::ApiContactIdType ContactIdType;

    enum FilterType {
        FilterNone,
        FilterAll,
        FilterFavorites,
        FilterOnline,
        FilterTypesCount
    };

    enum FetchDataType {
        FetchNone = 0,
        FetchAccountUri = (1 << 0),
        FetchPhoneNumber = (1 << 1),
        FetchEmailAddress = (1 << 2)
    };

    enum DisplayLabelOrder {
        FirstNameFirst = 0,
        LastNameFirst
    };

    enum ContactState {
        ContactAbsent,
        ContactPartial,
        ContactRequested,
        ContactComplete
    };

    struct ItemData
    {
        virtual ~ItemData() {}

        virtual void displayLabelOrderChanged(DisplayLabelOrder order) = 0;

        virtual void updateContact(const QContact &newContact, QContact *oldContact, ContactState state) = 0;

        virtual void constituentsFetched(const QList<int> &ids) = 0;
        virtual void mergeCandidatesFetched(const QList<int> &ids) = 0;
        virtual void aggregationOperationCompleted() = 0;

        virtual QList<int> constituents() const = 0;
    };

    struct CacheItem;
    struct ItemListener
    {
        ItemListener() : next(0), key(0) {}
        virtual ~ItemListener() {}

        virtual void itemUpdated(CacheItem *item) = 0;
        virtual void itemAboutToBeRemoved(CacheItem *item) = 0;

        ItemListener *next;
        void *key;
    };

    struct CacheItem
    {
        CacheItem() : itemData(0), iid(0), contactState(ContactAbsent), listeners(0) {}
        CacheItem(const QContact &contact)
            : contact(contact), itemData(0), iid(internalId(contact)),
              statusFlags(contact.detail<QContactStatusFlags>().flagsValue()), contactState(ContactAbsent), listeners(0) {}

        ContactIdType apiId() const { return SeasideCache::apiId(contact); }

        ItemListener *appendListener(ItemListener *listener, void *key)
        {
            if (listeners) {
                ItemListener *existing(listeners);
                while (existing->next) {
                    existing = existing->next;
                }
                existing->next = listener;
            } else {
                listeners = listener;
            }

            listener->next = 0;
            listener->key = key;
            return listener;
        }

        bool removeListener(ItemListener *listener)
        {
            if (listeners) {
                ItemListener *existing(listeners);
                ItemListener **previous = &listeners;

                while (existing) {
                    if (existing == listener) {
                        *previous = listener->next;
                        return true;
                    }
                    previous = &existing->next;
                    existing = existing->next;
                }
            }

            return false;
        }

        ItemListener *listener(void *key)
        {
            ItemListener *existing(listeners);
            while (existing && (existing->key != key) && (existing->next)) {
                existing = existing->next;
            }
            return (existing && (existing->key == key)) ? existing : 0;
        }

        QContact contact;
        ItemData *itemData;
        quint32 iid;
        quint64 statusFlags;
        ContactState contactState;
        ItemListener *listeners;
        QString nameGroup;
        QString displayLabel;
    };

    struct ContactLinkRequest
    {
        ContactLinkRequest(const SeasideCache::ContactIdType &id) : contactId(id), constituentsFetched(false) {}
        ContactLinkRequest(const ContactLinkRequest &req) : contactId(req.contactId), constituentsFetched(req.constituentsFetched) {}

        SeasideCache::ContactIdType contactId;
        bool constituentsFetched;
    };

    class ListModel : public QAbstractListModel
    {
    public:
        ListModel(QObject *parent = 0) : QAbstractListModel(parent) {}
        virtual ~ListModel() {}

        virtual void sourceAboutToRemoveItems(int begin, int end) = 0;
        virtual void sourceItemsRemoved() = 0;

        virtual void sourceAboutToInsertItems(int begin, int end) = 0;
        virtual void sourceItemsInserted(int begin, int end) = 0;

        virtual void sourceDataChanged(int begin, int end) = 0;

        virtual void sourceItemsChanged() = 0;

        virtual void makePopulated() = 0;
        virtual void updateDisplayLabelOrder() = 0;
        virtual void updateSortProperty() = 0;
        virtual void updateGroupProperty() = 0;
    };

    struct ResolveListener
    {
        virtual ~ResolveListener() {}

        virtual void addressResolved(const QString &first, const QString &second, CacheItem *item) = 0;
    };

    struct ChangeListener
    {
        virtual ~ChangeListener() {}

        virtual void itemUpdated(CacheItem *item) = 0;
        virtual void itemAboutToBeRemoved(CacheItem *item) = 0;
    };

    static SeasideCache *instance();

    static ContactIdType apiId(const QContact &contact);
    static ContactIdType apiId(quint32 iid);

    static bool validId(const ContactIdType &id);
#ifndef USING_QTPIM
    static bool validId(const QContactId &id);
#endif

    static quint32 internalId(const QContact &contact);
    static quint32 internalId(const QContactId &id);
#ifndef USING_QTPIM
    static quint32 internalId(QContactLocalId id);
#endif

    static void registerModel(ListModel *model, FilterType type, FetchDataType extraData = FetchNone);
    static void unregisterModel(ListModel *model);

    static void registerUser(QObject *user);
    static void unregisterUser(QObject *user);

    static void registerNameGroupChangeListener(SeasideNameGroupChangeListener *listener);
    static void unregisterNameGroupChangeListener(SeasideNameGroupChangeListener *listener);

    static void registerChangeListener(ChangeListener *listener);
    static void unregisterChangeListener(ChangeListener *listener);

    static void unregisterResolveListener(ResolveListener *listener);

    static void setNameGrouper(SeasideNameGrouper *grouper);

    static DisplayLabelOrder displayLabelOrder();
    static QString sortProperty();
    static QString groupProperty();

    static int contactId(const QContact &contact);

    static CacheItem *existingItem(const ContactIdType &id);
#ifdef USING_QTPIM
    static CacheItem *existingItem(quint32 iid);
#endif
    static CacheItem *itemById(const ContactIdType &id, bool requireComplete = true);
#ifdef USING_QTPIM
    static CacheItem *itemById(int id, bool requireComplete = true);
#endif
    static ContactIdType selfContactId();
    static QContact contactById(const ContactIdType &id);

    static void ensureCompletion(CacheItem *cacheItem);
    static void refreshContact(CacheItem *cacheItem);

    static QString nameGroup(const CacheItem *cacheItem);
    static QString determineNameGroup(const CacheItem *cacheItem);

    static QStringList allNameGroups();
    static QHash<QString, QSet<quint32> > nameGroupMembers();

    static CacheItem *itemByPhoneNumber(const QString &number, bool requireComplete = true);
    static CacheItem *itemByEmailAddress(const QString &address, bool requireComplete = true);
    static CacheItem *itemByOnlineAccount(const QString &localUid, const QString &remoteUid, bool requireComplete = true);

    static CacheItem *resolvePhoneNumber(ResolveListener *listener, const QString &number, bool requireComplete = true);
    static CacheItem *resolveEmailAddress(ResolveListener *listener, const QString &address, bool requireComplete = true);
    static CacheItem *resolveOnlineAccount(ResolveListener *listener, const QString &localUid, const QString &remoteUid, bool requireComplete = true);

    static bool saveContact(const QContact &contact);
    static bool removeContact(const QContact &contact);

    static void aggregateContacts(const QContact &contact1, const QContact &contact2);
    static void disaggregateContacts(const QContact &contact1, const QContact &contact2);

    static bool fetchConstituents(const QContact &contact);
    static bool fetchMergeCandidates(const QContact &contact);

    static int importContacts(const QString &path);
    static QString exportContacts();

    static const QList<quint32> *contacts(FilterType filterType);
    static bool isPopulated(FilterType filterType);

    static QString generateDisplayLabel(const QContact &contact, DisplayLabelOrder order = FirstNameFirst);
    static QString generateDisplayLabelFromNonNameDetails(const QContact &contact);
    static QUrl filteredAvatarUrl(const QContact &contact, const QStringList &metadataFragments = QStringList());

    static QString normalizePhoneNumber(const QString &input);

    bool event(QEvent *event);

    // For synchronizeLists()
    int insertRange(int index, int count, const QList<quint32> &source, int sourceIndex) { return insertRange(m_syncFilter, index, count, source, sourceIndex); }
    int removeRange(int index, int count) { removeRange(m_syncFilter, index, count); return 0; }

protected:
    void timerEvent(QTimerEvent *event);
    void setSortOrder(const QString &property);

private slots:
    void contactsAvailable();
    void contactIdsAvailable();
    void relationshipsAvailable();
    void requestStateChanged(QContactAbstractRequest::State state);
    void updateContacts();
#ifdef USING_QTPIM
    void contactsAdded(const QList<QContactId> &contactIds);
    void contactsChanged(const QList<QContactId> &contactIds);
    void contactsRemoved(const QList<QContactId> &contactIds);
#else
    void contactsAdded(const QList<QContactLocalId> &contactIds);
    void contactsChanged(const QList<QContactLocalId> &contactIds);
    void contactsRemoved(const QList<QContactLocalId> &contactIds);
#endif
    void displayLabelOrderChanged();
    void sortPropertyChanged();
    void groupPropertyChanged();

private:
    enum PopulateProgress {
        Unpopulated,
        FetchFavorites,
        FetchMetadata,
        FetchOnline,
        Populated,
        RefetchFavorites,
        RefetchOthers
    };

    SeasideCache();
    ~SeasideCache();

    static void checkForExpiry();

    void keepPopulated(quint32 fetchTypes);

    void requestUpdate();
    void appendContacts(const QList<QContact> &contacts, FilterType filterType, bool partialFetch, const QSet<DetailTypeId> &queryDetailTypes);
    void fetchContacts();
    void updateContacts(const QList<ContactIdType> &contactIds);

    void resolveUnknownAddresses(const QString &first, const QString &second, CacheItem *item);
    bool updateContactIndexing(const QContact &oldContact, const QContact &contact, quint32 iid, const QSet<DetailTypeId> &queryDetailTypes, CacheItem *item);
    void updateCache(CacheItem *item, const QContact &contact, bool partialFetch);
    void reportItemUpdated(CacheItem *item);

    void removeRange(FilterType filter, int index, int count);
    int insertRange(FilterType filter, int index, int count, const QList<quint32> &queryIds, int queryIndex);

    void contactDataChanged(quint32 iid);
    void contactDataChanged(quint32 iid, FilterType filter);
    void removeContactData(quint32 iid, FilterType filter);
    void makePopulated(FilterType filter);

    void addToContactNameGroup(quint32 iid, const QString &group, QSet<QString> *modifiedGroups = 0);
    void removeFromContactNameGroup(quint32 iid, const QString &group, QSet<QString> *modifiedGroups = 0);
    void notifyNameGroupsChanged(const QSet<QString> &groups);

    void updateConstituentAggregations(const ContactIdType &contactId);
    void completeContactAggregation(const ContactIdType &contact1Id, const ContactIdType &contact2Id);

    void resolveAddress(ResolveListener *listener, const QString &first, const QString &second, bool requireComplete);

    int contactIndex(quint32 iid, FilterType filter);

    static QContactRelationship makeRelationship(const QString &type, const QContact &contact1, const QContact &contact2);

    QList<quint32> m_contacts[FilterTypesCount];

    QBasicTimer m_expiryTimer;
    QBasicTimer m_fetchTimer;
    QHash<quint32, CacheItem> m_people;
    QHash<QString, quint32> m_phoneNumberIds;
    QHash<QString, quint32> m_emailAddressIds;
    QHash<QPair<QString, QString>, quint32> m_onlineAccountIds;
    QHash<ContactIdType, QContact> m_contactsToSave;
    QHash<QString, QSet<quint32> > m_contactNameGroups;
    QList<QContact> m_contactsToCreate;
    QList<ContactIdType> m_contactsToRemove;
    QList<ContactIdType> m_changedContacts;
    QList<QContactId> m_contactsToFetchConstituents;
    QList<QContactId> m_contactsToFetchCandidates;
    QList<QContactId> m_contactsToLinkTo;
    QList<QPair<ContactLinkRequest, ContactLinkRequest> > m_contactPairsToLink;
    QList<QContactRelationship> m_relationshipsToSave;
    QList<QContactRelationship> m_relationshipsToRemove;
    QScopedPointer<SeasideNameGrouper> m_nameGrouper;
    QList<SeasideNameGroupChangeListener*> m_nameGroupChangeListeners;
    QList<ChangeListener*> m_changeListeners;
    QList<ListModel *> m_models[FilterTypesCount];
    QSet<QObject *> m_users;
    QHash<ContactIdType,int> m_expiredContacts;
    QContactManager m_manager;
    QContactFetchRequest m_fetchRequest;
    QContactFetchByIdRequest m_fetchByIdRequest;
#ifdef USING_QTPIM
    QContactIdFetchRequest m_contactIdRequest;
#else
    QContactLocalIdFetchRequest m_contactIdRequest;
#endif
    QContactRelationshipFetchRequest m_relationshipsFetchRequest;
    QContactRemoveRequest m_removeRequest;
    QContactSaveRequest m_saveRequest;
    QContactRelationshipSaveRequest m_relationshipSaveRequest;
    QContactRelationshipRemoveRequest m_relationshipRemoveRequest;
    QList<QContactSortOrder> m_sortOrder;
    QList<QContactSortOrder> m_onlineSortOrder;
#ifdef HAS_MLITE
    MGConfItem m_displayLabelOrderConf;
    MGConfItem m_sortPropertyConf;
    MGConfItem m_groupPropertyConf;
#endif
    int m_populated;
    int m_cacheIndex;
    int m_queryIndex;
    int m_fetchProcessedCount;
    int m_fetchByIdProcessedCount;
    FilterType m_syncFilter;
    DisplayLabelOrder m_displayLabelOrder;
    QString m_sortProperty;
    QString m_groupProperty;
    bool m_keepPopulated;
    PopulateProgress m_populateProgress;
    quint32 m_fetchTypes;
    bool m_fetchTypesChanged;
    bool m_updatesPending;
    bool m_refreshRequired;
    bool m_contactsUpdated;
    QSet<ContactIdType> m_constituentIds;
    QSet<ContactIdType> m_candidateIds;

    struct ResolveData {
        QString first;
        QString second;
        QString compare;
        bool requireComplete;
        ResolveListener *listener;
    };
    QList<ResolveData> m_resolveAddresses;
    QList<ResolveData> m_unknownAddresses;
    const ResolveData *m_activeResolve;

    QElapsedTimer m_timer;
    QElapsedTimer m_fetchPostponed;

    static SeasideCache *instancePtr;
    static QStringList allContactNameGroups;
};

#endif
