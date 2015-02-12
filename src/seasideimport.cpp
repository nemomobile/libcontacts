/*
 * Copyright (C) 2013 Jolla Mobile <matthew.vogt@jollamobile.com>
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

#include "seasideimport.h"
#include "seasidecache.h"

#include <QContactDetailFilter>
#include <QContactFetchHint>
#include <QContactManager>
#include <QContactSortOrder>
#include <QContactSyncTarget>

#include <QContactAddress>
#include <QContactAnniversary>
#include <QContactAvatar>
#include <QContactBirthday>
#include <QContactEmailAddress>
#include <QContactFamily>
#include <QContactGeoLocation>
#include <QContactGuid>
#include <QContactHobby>
#include <QContactName>
#include <QContactNickname>
#include <QContactNote>
#include <QContactOnlineAccount>
#include <QContactOrganization>
#include <QContactPhoneNumber>
#include <QContactRingtone>
#include <QContactTag>
#include <QContactUrl>

#include <QContactIdFilter>
#include <QContactExtendedDetail>

#include <QVersitContactExporter>
#include <QVersitContactImporter>
#include <QVersitReader>
#include <QVersitWriter>

#include <QHash>
#include <QString>

namespace {

QContactFetchHint basicFetchHint()
{
    QContactFetchHint fetchHint;

    fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships |
                                   QContactFetchHint::NoActionPreferences |
                                   QContactFetchHint::NoBinaryBlobs);

    return fetchHint;
}

bool nameIsEmpty(const QContactName &name)
{
    if (name.isEmpty())
        return true;

    return (name.prefix().isEmpty() &&
            name.firstName().isEmpty() &&
            name.middleName().isEmpty() &&
            name.lastName().isEmpty() &&
            name.suffix().isEmpty());
}

QString contactNameString(const QContact &contact)
{
    QStringList details;
    QContactName name(contact.detail<QContactName>());
    if (nameIsEmpty(name))
        return QString();

    details.append(name.prefix());
    details.append(name.firstName());
    details.append(name.middleName());
    details.append(name.lastName());
    details.append(name.suffix());
    return details.join(QChar::fromLatin1('|'));
}

bool allCharactersMatchScript(const QString &s, QChar::Script script)
{
    for (int i=0; i<s.length(); i++) {
        if (s[i].script() != script) {
            return false;
        }
    }
    return true;
}

bool applyNameFixes(QContactName *nameDetail)
{
    // Chinese names shouldn't have a middle name, so if it is present in a Han-script-only
    // name, it is probably wrong and it should be prepended to the first name instead.
    QString middleName = nameDetail->middleName();
    if (middleName.isEmpty()) {
        return false;
    }
    QString firstName = nameDetail->firstName();
    QString lastName = nameDetail->lastName();
    if (!allCharactersMatchScript(middleName, QChar::Script_Han)
            || (!firstName.isEmpty() && !allCharactersMatchScript(firstName, QChar::Script_Han))
            || (!lastName.isEmpty() && !allCharactersMatchScript(lastName, QChar::Script_Han))) {
        return false;
    }
    nameDetail->setFirstName(middleName + firstName);
    nameDetail->setMiddleName(QString());
    return true;
}

void setNickname(QContact &contact, const QString &text)
{
    foreach (const QContactNickname &nick, contact.details<QContactNickname>()) {
        if (nick.nickname() == text) {
            return;
        }
    }

    QContactNickname nick;
    nick.setNickname(text);
    contact.saveDetail(&nick);
}


template<typename T, typename F>
QVariant detailValue(const T &detail, F field)
{
    return detail.value(field);
}

typedef QMap<int, QVariant> DetailMap;

DetailMap detailValues(const QContactDetail &detail)
{
    DetailMap rv(detail.values());
    return rv;
}

static bool variantEqual(const QVariant &lhs, const QVariant &rhs)
{
    // Work around incorrect result from QVariant::operator== when variants contain QList<int>
    static const int QListIntType = QMetaType::type("QList<int>");

    const int lhsType = lhs.userType();
    if (lhsType != rhs.userType()) {
        return false;
    }

    if (lhsType == QListIntType) {
        return (lhs.value<QList<int> >() == rhs.value<QList<int> >());
    }
    return (lhs == rhs);
}

static bool detailValuesSuperset(const QContactDetail &lhs, const QContactDetail &rhs)
{
    // True if all values in rhs are present in lhs
    const DetailMap lhsValues(detailValues(lhs));
    const DetailMap rhsValues(detailValues(rhs));

    if (lhsValues.count() < rhsValues.count()) {
        return false;
    }

    foreach (const DetailMap::key_type &key, rhsValues.keys()) {
        if (!variantEqual(lhsValues[key], rhsValues[key])) {
            return false;
        }
    }

    return true;
}

static void fixupDetail(QContactDetail &)
{
}

// Fixup QContactUrl because importer produces incorrectly typed URL field
static void fixupDetail(QContactUrl &url)
{
    QVariant urlField = url.value(QContactUrl::FieldUrl);
    if (!urlField.isNull()) {
        QString urlString = urlField.toString();
        if (!urlString.isEmpty()) {
            url.setValue(QContactUrl::FieldUrl, QUrl(urlString));
        } else {
            url.setValue(QContactUrl::FieldUrl, QVariant());
        }
    }
}

// Fixup QContactOrganization because importer produces invalid department
static void fixupDetail(QContactOrganization &org)
{
    QVariant deptField = org.value(QContactOrganization::FieldDepartment);
    if (!deptField.isNull()) {
        QStringList deptList = deptField.toStringList();

        // Remove any empty elements from the list
        QStringList::iterator it = deptList.begin();
        while (it != deptList.end()) {
            if ((*it).isEmpty()) {
                it = deptList.erase(it);
            } else {
                ++it;
            }
        }

        if (!deptList.isEmpty()) {
            org.setValue(QContactOrganization::FieldDepartment, deptList);
        } else {
            org.setValue(QContactOrganization::FieldDepartment, QVariant());
        }
    }
}

template<typename T>
bool updateExistingDetails(QContact *updateContact, const QContact &importedContact, bool singular = false)
{
    bool rv = false;

    QList<T> existingDetails(updateContact->details<T>());
    if (singular && !existingDetails.isEmpty())
        return rv;

    foreach (T detail, importedContact.details<T>()) {
        // Make any corrections to the input
        fixupDetail(detail);

        // See if the contact already has a detail which is a superset of this one
        bool found = false;
        foreach (const T &existing, existingDetails) {
            if (detailValuesSuperset(existing, detail)) {
                found = true;
                break;
            }
        }
        if (!found) {
            updateContact->saveDetail(&detail);
            rv = true;
        }
    }

    return rv;
}

bool mergeIntoExistingContact(QContact *updateContact, const QContact &importedContact)
{
    bool rv = false;

    // Update the existing contact with any details in the new import
    rv |= updateExistingDetails<QContactAddress>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactAnniversary>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactAvatar>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactBirthday>(updateContact, importedContact, true);
    rv |= updateExistingDetails<QContactEmailAddress>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactFamily>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactGeoLocation>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactGuid>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactHobby>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactNickname>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactNote>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactOnlineAccount>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactOrganization>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactPhoneNumber>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactRingtone>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactTag>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactUrl>(updateContact, importedContact);
    rv |= updateExistingDetails<QContactExtendedDetail>(updateContact, importedContact);

    return rv;
}

bool mergeContacts(QContact *mergeInto, const QContact &contact)
{
    // this function basically puts details from contact into mergeInto.
    QContact temp(*mergeInto);
    *mergeInto = contact;
    return mergeIntoExistingContact(mergeInto, temp);
}

}

QContactFilter SeasideImport::localContactFilter()
{
    // Contacts that are local to the device have sync target 'local' or 'was_local' or 'bluetooth'
    QContactDetailFilter filterLocal, filterWasLocal, filterBluetooth;
    filterLocal.setDetailType(QContactSyncTarget::Type, QContactSyncTarget::FieldSyncTarget);
    filterWasLocal.setDetailType(QContactSyncTarget::Type, QContactSyncTarget::FieldSyncTarget);
    filterBluetooth.setDetailType(QContactSyncTarget::Type, QContactSyncTarget::FieldSyncTarget);
    filterLocal.setValue(QString::fromLatin1("local"));
    filterWasLocal.setValue(QString::fromLatin1("was_local"));
    filterBluetooth.setValue(QString::fromLatin1("bluetooth"));

    return filterLocal | filterWasLocal | filterBluetooth;
}

QList<QContact> SeasideImport::buildMergeImportContacts(const QList<QVersitDocument> &details,
                                                int *newCount,
                                                int *updatedCount)
{
    return buildImportContacts(details, newCount, updatedCount);
}

QList<QContact> SeasideImport::buildImportContacts(const QList<QVersitDocument> &details,
                                                   int *newCount,
                                                   int *updatedCount,
                                                   const QSet<QContactDetail::DetailType> &unimportableDetailTypes,
                                                   const QStringList &importableSyncTargets,
                                                   const QContactFilter &mergeMatchFilter,
                                                   QContactManager *manager,
                                                   QVersitContactHandler *propertyHandler,
                                                   bool mergeImportListDuplicates,
                                                   bool mergeDatabaseDuplicates,
                                                   QMap<int, int> *importDuplicateIndexes,
                                                   QMap<int, QContactId> *databaseDuplicateIndexes)
{
    if (newCount)
        *newCount = 0;
    if (updatedCount)
        *updatedCount = 0;

    // For tracking dropped documents due to de-duplication and merging
    QMap<int, int> originalIndexToMergeIndex;
    QMap<int, QContactId> originalIndexToMergeId;

    // Read the contacts from the import details
    QVersitContactHandler *contactHandler = (propertyHandler ? propertyHandler : new SeasidePropertyHandler());
    QVersitContactImporter importer;
    importer.setPropertyHandler(contactHandler);
    importer.importDocuments(details);
    QList<QContact> importedContacts(importer.contacts());
    if (!propertyHandler) {
        delete contactHandler;
    }

    // preprocess imported contacts prior to save.
    QHash<int, QString> importedContactLabels;
    for (int i = 0; i < importedContacts.size(); ++i) {
        // Fix up name (field ordering) if required
        QContact &contact(importedContacts[i]);
        QContactName nameDetail = contact.detail<QContactName>();
        if (applyNameFixes(&nameDetail)) {
            contact.saveDetail(&nameDetail);
        }

        // Remove any details that our backend can't store, or which
        // the client wishes stripped from the imported contacts.
        foreach (QContactDetail detail, contact.details()) {
            if (unimportableDetailTypes.contains(detail.type())) {
                qDebug() << "  Removing unimportable detail:" << detail;
                contact.removeDetail(&detail);
            } else if (detail.type() == QContactSyncTarget::Type) {
                // We allow some syncTarget values
                const QString syncTarget(detail.value<QString>(QContactSyncTarget::FieldSyncTarget));
                if (!importableSyncTargets.contains(syncTarget)) {
                    qDebug() << "  Removing unimportable syncTarget:" << syncTarget;
                    contact.removeDetail(&detail);
                }
            }
        }

        // Set nickname by default if the name is empty
        if (contactNameString(contact).isEmpty()) {
            QContactName nameDetail = contact.detail<QContactName>();
            contact.removeDetail(&nameDetail);
            if (contact.details<QContactNickname>().isEmpty()) {
                QString label = contact.detail<QContactDisplayLabel>().label();
                if (label.isEmpty()) {
                    label = SeasideCache::generateDisplayLabelFromNonNameDetails(contact);
                }
                setNickname(contact, label);
                importedContactLabels.insert(i, label);
            }
        }
    }

    // if necessary, attempt to remove duplicates from the import list
    if (mergeImportListDuplicates) {
        QHash<QString, int> importGuids;
        QHash<QString, int> importNames;
        QHash<QString, int> importLabels;

        for (int i = 0; i < importedContacts.size(); ++i) {
            QContact &contact(importedContacts[i]);

            // select some information from the contact from which we will determine duplicateness
            const QString guid = contact.detail<QContactGuid>().guid();
            const QString name = contactNameString(contact);
            QString label;
            if (importedContactLabels.contains(i)) {
                label = importedContactLabels.value(i);
            } else if (!contact.detail<QContactDisplayLabel>().label().isEmpty()) {
                label = contact.detail<QContactDisplayLabel>().label();
            } else {
                label = SeasideCache::generateDisplayLabelFromNonNameDetails(contact);
            }

            // check guid for duplicates
            int matchingPreviousIndex = importGuids.value(guid, -1);
            if (matchingPreviousIndex != -1) {
                // already seen this guid.
                const QContact &previous(importedContacts[matchingPreviousIndex]);
                const QString previousName = contactNameString(previous);
                QString previousLabel;
                if (importedContactLabels.contains(matchingPreviousIndex)) {
                    previousLabel = importedContactLabels.value(matchingPreviousIndex);
                } else if (!previous.detail<QContactDisplayLabel>().label().isEmpty()) {
                    previousLabel = contact.detail<QContactDisplayLabel>().label();
                } else {
                    previousLabel = SeasideCache::generateDisplayLabelFromNonNameDetails(previous);
                }

                // if the names or labels are the same, treat as duplicate.
                if (name.isEmpty() && previousName.isEmpty() && label == previousLabel) {
                    // matching label and guid, leave matchingPreviousIndex.
                } else if (!name.isEmpty() && !previousName.isEmpty() && name == previousName) {
                    // matching name and guid, leave matchingPreviousIndex.
                } else {
                    // non-match.  Remove the guid to avoid conflicts.
                    QContactGuid guidDetail = contact.detail<QContactGuid>();
                    contact.removeDetail(&guidDetail);
                    matchingPreviousIndex = -1; // not a real match.
                }
            }

            // check name or label for duplicates
            if (matchingPreviousIndex == -1) {
                if (name.isEmpty()) {
                    // check for contact with duplicate label in list
                    matchingPreviousIndex = importLabels.value(label, -1);
                } else {
                    // check for contact with duplicate name in list
                    matchingPreviousIndex = importNames.value(name, -1);
                }
            }

            // now either save new contact, or merge into previous match.
            if (matchingPreviousIndex == -1) {
                // new contact.  add the search data to the hashes.
                if (!guid.isEmpty()) {
                    importGuids.insert(guid, i);
                }
                if (!name.isEmpty()) {
                    importNames.insert(name, i);
                }
                if (!label.isEmpty()) {
                    importLabels.insert(label, i);
                }
            } else {
                // duplicate, must merge.
                QContact &previous(importedContacts[matchingPreviousIndex]);
                mergeIntoExistingContact(&previous, contact);
                originalIndexToMergeIndex.insert(i, matchingPreviousIndex);
            }
        }
    }

    // Find all names and GUIDs for local contacts that might match these contacts
    QContactFetchHint fetchHint(basicFetchHint());
    fetchHint.setDetailTypesHint(QList<QContactDetail::DetailType>() << QContactName::Type << QContactNickname::Type << QContactGuid::Type);

    QHash<QString, QContactId> existingGuids;
    QHash<QString, QContactId> existingNames;
    QMap<QContactId, QString> existingContactNames;
    QHash<QString, QContactId> existingNicknames;

    QContactManager *mgr(manager ? manager : SeasideCache::manager());
    foreach (const QContact &contact, mgr->contacts(mergeMatchFilter, QList<QContactSortOrder>(), fetchHint)) {
        const QString guid = contact.detail<QContactGuid>().guid();
        const QString name = contactNameString(contact);

        if (!guid.isEmpty()) {
            existingGuids.insert(guid, contact.id());
        }
        if (!name.isEmpty()) {
            existingNames.insert(name, contact.id());
            existingContactNames.insert(contact.id(), name);
        }
        foreach (const QContactNickname &nick, contact.details<QContactNickname>()) {
            existingNicknames.insert(nick.nickname(), contact.id());
        }
    }

    // Find any imported contacts that match contacts we already have.
    // This allows us to determine which are new, and which are updates.
    QMap<int, QContactId> existingIds;
    for (int i = 0; i < importedContacts.size(); ++i) {
        if (originalIndexToMergeIndex.contains(i)) {
            // we can ignore this contact, it was merged into another, previously.
            continue;
        }

        QContact &contact(importedContacts[i]);
        const QString& guid = contact.detail<QContactGuid>().guid();
        const QString name = contactNameString(contact);

        QContactId existingMatchId;
        if (existingGuids.contains(guid)) {
            // have found a GUID match.  Check to see if the names match also.
            QContactId guidMatchId = existingGuids.value(guid);
            if (!name.isEmpty() && existingContactNames[existingMatchId] != name) {
                // the names don't match, this is a different contact.
                // remove the guid to avoid conflict.
                QContactGuid guidDetail = contact.detail<QContactGuid>();
                contact.removeDetail(&guidDetail);
            } else {
                // found a match.
                existingMatchId = guidMatchId;
                existingIds.insert(i, existingMatchId); // this will be an update to existing database contact.
                contact.setId(existingMatchId);
            }
        }

        if (existingMatchId.isNull() && !name.isEmpty() && existingNames.contains(name)) {
            // found a matching name.
            existingMatchId = existingNames.value(name);
            existingIds.insert(i, existingMatchId); // this will be an update to existing database contact.
            contact.setId(existingMatchId);
        }
    }

    // Now build a list of contacts to store to the database.
    QList<QContact> retn;
    if (mergeDatabaseDuplicates) {
        // If the database versions of the matching contacts are identical, merge them if required.
        // This codepath should only be used for "import" style syncs, where the sync is non-destructive
        // of data already on the device.

        // Retrieve all the contacts that we have matches for.
        QContactIdFilter idFilter;
        idFilter.setIds(existingIds.values());
        QList<QContact> existingContacts = mgr->contacts(idFilter & mergeMatchFilter, QList<QContactSortOrder>(), basicFetchHint());
        for (int i = 0; i < importedContacts.size(); ++i) {
            if (originalIndexToMergeIndex.contains(i)) {
                // can skip this one.
                continue;
            }

            QContact &importContact(importedContacts[i]);
            if (existingIds.contains(i)) {
                // this import contact must have a match in the existingContacts list.
                Q_FOREACH (const QContact &existing, existingContacts) {
                    if (existing.id() == importContact.id()) {
                        bool modified = mergeContacts(&importContact, existing);
                        if (!modified) {
                            // the import contact does not differ from the database contact.
                            // we drop it from the returned list as we don't want to save it.
                            // first, set this index as "dropped" in favour of the existing contact's id.
                            originalIndexToMergeId.insert(i, existing.id());
                            // second, set any indexes who were dropped in favour of us, to be dropped in favour of that id too.
                            QList<int> originalIndexes = originalIndexToMergeIndex.keys();
                            Q_FOREACH (int originalIdx, originalIndexes) {
                                if (originalIndexToMergeIndex.value(originalIdx) == i) {
                                    originalIndexToMergeId.insert(originalIdx, existing.id());
                                    originalIndexToMergeIndex.remove(originalIdx); // moved to the other map.
                                }
                            }
                        } else {
                            // the import contact does differ from the database contact.
                            retn.append(importContact);
                            if (updatedCount) *updatedCount += 1;
                        }
                        break; // found the match.
                    }
                }
            } else {
                // this is a new contact.
                retn.append(importContact);
                if (newCount) *newCount += 1;
            }
        }
    } else {
        // This is the "proper sync" codepath
        // In this codepath, we clobber whatever exists in the database
        // with the data in this newly imported contact.
        for (int i = 0; i < importedContacts.size(); ++i) {
            // ignore any duplicates which we were instructed to ignore.
            if (!originalIndexToMergeIndex.contains(i)) {
                retn.append(importedContacts[i]);
                if (existingIds.contains(i)) {
                    if (updatedCount) *updatedCount += 1;
                } else {
                    if (newCount) *newCount += 1;
                }
            }
        }
    }

    if (importDuplicateIndexes) {
        // keys are the index of every entry in the input documents list
        // which was dropped due to being a duplicate of another entry
        // in the input documents list.
        // values are the index of the "other entry".
        *importDuplicateIndexes = originalIndexToMergeIndex;
    }

    if (databaseDuplicateIndexes) {
        // keys are the index of every entry in the input docuemnts list
        // which was dropped due to being a duplicate of an existing
        // contact in the database with no changes.
        // values are the contactId of that pre-existing contact.
        *databaseDuplicateIndexes = originalIndexToMergeId;
    }

    return retn;
}

