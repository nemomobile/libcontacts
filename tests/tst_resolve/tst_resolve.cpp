/*
 * Copyright (C) 2014 Jolla Ltd.
 * Contact: Richard Braakman <richard.braakman@jollamobile.com>
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

#include <QObject>
#include <QtTest>
#include <QtDebug>

#include <QContact>
#include <QContactEmailAddress>
#include <QContactName>
#include <QContactOnlineAccount>
#include <QContactPhoneNumber>

#include "seasidecache.h"

static const QString AccountPath(QString::fromLatin1("/example/jabber/0"));

class tst_Resolve : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();

private:
    // only firstname and lastname are mandatory
    bool makeContact(QString firstname, QString lastname, QString phone, QString email, QString account);
    void makeContacts();

    QList<QContactId> m_createdContacts;

private slots:
    void resolveByPhone();
    void resolveByPhoneNotFound();
    void resolveByEmail();
    void resolveByEmailNotFound();
    void resolveByAccount();
    void resolveByAccountNotFound();

    void resolveDuringContactLink();

    void resolutionChanged();
};

namespace {
struct TestResolveListener : public SeasideCache::ResolveListener {
    TestResolveListener()
        : m_resolved(false), m_item(0)
        { }

    void resolvePhoneNumber(const QString &number, bool requireComplete = true)
        {
            m_item = SeasideCache::resolvePhoneNumber(this, number, requireComplete);
            m_resolved = m_item != 0;
        }

    virtual void addressResolved(const QString &, const QString &, SeasideCache::CacheItem *item)
        { m_resolved = true; m_item = item; }

    bool m_resolved;
    SeasideCache::CacheItem *m_item;
};

}; // anonymous

void tst_Resolve::initTestCase()
{
    makeContacts();
    SeasideCache::registerUser(this);
}

void tst_Resolve::cleanupTestCase()
{
    SeasideCache::unregisterUser(this);
    QVERIFY(SeasideCache::manager()->removeContacts(m_createdContacts));
    m_createdContacts.clear();
}

bool tst_Resolve::makeContact(QString firstname, QString lastname, QString phone, QString email, QString account)
{
    QContact contact;

    QContactName name;
    name.setFirstName(firstname);
    name.setLastName(lastname);
    if (!contact.saveDetail(&name))
        return false;

    if (!phone.isEmpty()) {
        QContactPhoneNumber pn;
        pn.setNumber(phone);
        if (!contact.saveDetail(&pn))
            return false;
    }

    if (!email.isEmpty()) {
        QContactEmailAddress e;
        e.setEmailAddress(email);
        if (!contact.saveDetail(&e))
            return false;
    }

    if (!account.isEmpty()) {
        QContactOnlineAccount acc;
        acc.setAccountUri(account);
        acc.setValue(QContactOnlineAccount__FieldAccountPath, AccountPath);
        if (!contact.saveDetail(&acc))
            return false;
    }

    if (!SeasideCache::manager()->saveContact(&contact))
        return false;

    m_createdContacts << contact.id();
    return true;
}

void tst_Resolve::makeContacts()
{
    QVERIFY(makeContact("Alfred", "Alfredson", "+358474005000", "alfred@alfred.com", ""));
    QVERIFY(makeContact("Berta", "Berenstain", "", "berta.b@geemail.com", "berta.b@geemail.com"));
    QVERIFY(makeContact("Carlo", "Rizzi", "+358471112222", "", ""));
    QVERIFY(makeContact("Daffy", "Duck", "+358470009955", "daffyd@example.com", ""));
    QVERIFY(makeContact("Dafferd", "Duck", "", "daffy.d@example.com", ""));
    QVERIFY(makeContact("Ernest", "Everest", "+358477758885", "", ""));
}

void tst_Resolve::resolveByPhone()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString number("+358470009955");

    item = SeasideCache::resolvePhoneNumber(&listener, number, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QContactName name = item->contact.detail<QContactName>();
    QCOMPARE(name.firstName(), QString::fromLatin1("Daffy"));
}

void tst_Resolve::resolveByPhoneNotFound()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString number("+358470000000");

    item = SeasideCache::resolvePhoneNumber(&listener, number, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QCOMPARE(item, (SeasideCache::CacheItem *)0);
}

void tst_Resolve::resolveByEmail()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString address("berta.b@geemail.com");

    item = SeasideCache::resolveEmailAddress(&listener, address, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QContactName name = item->contact.detail<QContactName>();
    QCOMPARE(name.firstName(), QString::fromLatin1("Berta"));
}

void tst_Resolve::resolveByEmailNotFound()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString address("example@example.com");

    item = SeasideCache::resolveEmailAddress(&listener, address, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QCOMPARE(item, (SeasideCache::CacheItem *)0);
}

void tst_Resolve::resolveByAccount()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString remoteUid("berta.b@geemail.com");

    item = SeasideCache::resolveOnlineAccount(&listener, AccountPath, remoteUid, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QContactName name = item->contact.detail<QContactName>();
    QCOMPARE(name.firstName(), QString::fromLatin1("Berta"));
}

void tst_Resolve::resolveByAccountNotFound()
{
    SeasideCache::CacheItem *item;
    TestResolveListener listener;
    QString remoteUid("example@example.com");

    item = SeasideCache::resolveOnlineAccount(&listener, AccountPath, remoteUid, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    QCOMPARE(item, (SeasideCache::CacheItem *)0);
}

struct ItemWatcher : public SeasideCache::ItemData {
    QList<int> m_constituents;
    bool m_aggregationComplete;

    ItemWatcher() : m_aggregationComplete(false)
    { }

    virtual void displayLabelOrderChanged(SeasideCache::DisplayLabelOrder)
    { }
    virtual void updateContact(const QtContacts::QContact&, QtContacts::QContact*, SeasideCache::ContactState)
    { }
    virtual void mergeCandidatesFetched(const QList<int> &)
    { }

    virtual void aggregationOperationCompleted()
    { m_aggregationComplete = true; }

    virtual void constituentsFetched(const QList<int> &ids)
    { m_constituents = ids; }

    virtual QList<int> constituents() const
    { return m_constituents; }
};

// Test that address resolutions don't interfere with contact linking
void tst_Resolve::resolveDuringContactLink()
{
    SeasideCache::CacheItem *item1;
    TestResolveListener listener1;
    QString address1("daffyd@example.com");
    SeasideCache::CacheItem *item2;
    TestResolveListener listener2;
    QString address2("daffy.d@example.com");

    item1 = SeasideCache::resolveEmailAddress(&listener1, address1, true);
    item2 = SeasideCache::resolveEmailAddress(&listener2, address2, true);
    if (!item1) {
        QTRY_VERIFY(listener1.m_resolved);
        item1 = listener1.m_item;
    }
    QVERIFY(item1);
    QCOMPARE(item1->displayLabel, QString::fromLatin1("Daffy Duck"));
    if (!item2) {
        QTRY_VERIFY(listener2.m_resolved);
        item2 = listener2.m_item;
    }
    QVERIFY(item2);
    QCOMPARE(item2->displayLabel, QString::fromLatin1("Dafferd Duck"));

    int iid = item1->iid;
    item1->itemData = new ItemWatcher;
    item2->itemData = new ItemWatcher;
    SeasideCache::aggregateContacts(item1->contact, item2->contact);

    // Fire off an address resolution simultaneously
    SeasideCache::CacheItem *item;
    QString number("+358477758885");
    TestResolveListener listener;

    item = SeasideCache::resolvePhoneNumber(&listener, number, true);
    if (!item) {
        QTRY_VERIFY(listener.m_resolved);
        item = listener.m_item;
    }

    // Did the address resolution go ok?
    QCOMPARE(item->displayLabel, QString::fromLatin1("Ernest Everest"));

    // wait for the aggregation
    item1 = SeasideCache::existingItem(iid);
    QVERIFY(item1);
    QVERIFY(item1->itemData);
    QTRY_VERIFY(((ItemWatcher *) item1->itemData)->m_aggregationComplete);

    // the aggregate's constituents are not updated in the cache, so they
    // have to be reloaded before comparing. Is that a bug?
    SeasideCache::fetchConstituents(item1->contact);
    QTRY_COMPARE(item1->itemData->constituents().count(), 2);

    // Check that the expected two contacts are the constituents.
    int iid1 = item1->itemData->constituents()[0];
    int iid2 = item1->itemData->constituents()[1];
    item1 = SeasideCache::itemById(iid1);
    item2 = SeasideCache::itemById(iid2);
    QTRY_COMPARE(item1->contactState, SeasideCache::ContactComplete);
    QTRY_COMPARE(item2->contactState, SeasideCache::ContactComplete);

    QStringList names, expected;
    names << item1->displayLabel << item2->displayLabel;
    qSort(names);
    expected << QString::fromLatin1("Dafferd Duck") << QString::fromLatin1("Daffy Duck");
    QCOMPARE(names, expected);
}

void tst_Resolve::resolutionChanged()
{
    // Attach a model so that the cache will process async events
    struct DummyModel : public SeasideCache::ListModel
    {
    public:
        DummyModel() {
            SeasideCache::registerModel(this, SeasideCache::FilterFavorites);
        }
        ~DummyModel() {
            SeasideCache::unregisterModel(this);
        }

        virtual void sourceAboutToRemoveItems(int, int) {}
        virtual void sourceItemsRemoved() {}

        virtual void sourceAboutToInsertItems(int, int) {}
        virtual void sourceItemsInserted(int, int) {}

        virtual void sourceDataChanged(int, int) {}

        virtual void sourceItemsChanged() {}

        virtual void makePopulated() {}
        virtual void updateDisplayLabelOrder() {}
        virtual void updateSortProperty() {}
        virtual void updateGroupProperty() {}

        virtual int rowCount(const QModelIndex&) const { return 0; }
        virtual QVariant data(const QModelIndex&, int) const { return QVariant(); }
    };

    // Use a listener to observe resolution changes
    struct ChangeListener : public SeasideCache::ChangeListener {
        ChangeListener() {
            SeasideCache::registerChangeListener(this);
        }
        ~ChangeListener() {
            SeasideCache::unregisterChangeListener(this);
        }

        void itemUpdated(SeasideCache::CacheItem *) {}
        void itemAboutToBeRemoved(SeasideCache::CacheItem *) {}

        void addressResolutionsChanged(const QSet<QPair<QString, QString> > &changeSet) { addresses |= changeSet; }

        QSet<QPair<QString, QString> > addresses;
    };

    DummyModel model;
    ChangeListener changeListener;
    TestResolveListener listener;

    // Check that our test numbers do not resolve
    listener.resolvePhoneNumber("988889999");
    QTRY_VERIFY(listener.m_resolved);
    QVERIFY(listener.m_item == 0);

    listener.resolvePhoneNumber("188889999");
    QTRY_VERIFY(listener.m_resolved);
    QVERIFY(listener.m_item == 0);

    // Create a contact whose phone number does not conflict with any other
    QVERIFY(makeContact("Freddy", "Fugazi", "61188889999", "", ""));

    // Wait until the addition is processed, which incurs a delay for coalescing events
    QTest::qWait(1000);
    QVERIFY(changeListener.addresses.isEmpty());

    listener.resolvePhoneNumber("188889999");
    QTRY_VERIFY(listener.m_resolved);
    QCOMPARE(listener.m_item->displayLabel, QString::fromLatin1("Freddy Fugazi"));

    QVERIFY(changeListener.addresses.isEmpty());

    // Create a contact whose number conflicts with an existing one
    QVERIFY(makeContact("Graeme", "Garden", "99988889999", "", ""));

    // We should have been notified of a resolution change
    QTRY_COMPARE(changeListener.addresses, (QSet<QPair<QString, QString> >() << qMakePair(QString(), SeasideCache::minimizePhoneNumber("188889999"))));
    changeListener.addresses.clear();

    // Check that resolution still works correctly
    listener.resolvePhoneNumber("988889999");
    QTRY_VERIFY(listener.m_resolved);
    QCOMPARE(listener.m_item->displayLabel, QString::fromLatin1("Graeme Garden"));

    listener.resolvePhoneNumber("188889999");
    QTRY_VERIFY(listener.m_resolved);
    QCOMPARE(listener.m_item->displayLabel, QString::fromLatin1("Freddy Fugazi"));

    // Remove the last added contact
    QVERIFY(changeListener.addresses.isEmpty());
    QVERIFY(SeasideCache::manager()->removeContact(m_createdContacts.takeLast()));

    QTRY_COMPARE(changeListener.addresses, (QSet<QPair<QString, QString> >() << qMakePair(QString(), SeasideCache::minimizePhoneNumber("188889999"))));
    changeListener.addresses.clear();

    // This number now resolves to the alternate match
    listener.resolvePhoneNumber("988889999");
    QTRY_VERIFY(listener.m_resolved);
    QCOMPARE(listener.m_item->displayLabel, QString::fromLatin1("Freddy Fugazi"));

    listener.resolvePhoneNumber("188889999");
    QTRY_VERIFY(listener.m_resolved);
    QCOMPARE(listener.m_item->displayLabel, QString::fromLatin1("Freddy Fugazi"));

    // Remove the last added contact
    QVERIFY(changeListener.addresses.isEmpty());
    QVERIFY(SeasideCache::manager()->removeContact(m_createdContacts.takeLast()));
    QTest::qWait(1000);
    QVERIFY(changeListener.addresses.isEmpty());

    listener.resolvePhoneNumber("988889999");
    QTRY_VERIFY(listener.m_resolved);
    QVERIFY(listener.m_item == 0);

    listener.resolvePhoneNumber("188889999");
    QTRY_VERIFY(listener.m_resolved);
    QVERIFY(listener.m_item == 0);
}

#include "tst_resolve.moc"
QTEST_GUILESS_MAIN(tst_Resolve)
