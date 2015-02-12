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

#ifndef SEASIDEIMPORT_H
#define SEASIDEIMPORT_H

#include "contactcacheexport.h"
#include "seasidepropertyhandler.h"

#include <QStringList>
#include <QList>
#include <QPair>
#include <QSet>

#include <QContact>
#include <QContactDetail>
#include <QContactFilter>
#include <QVersitDocument>
#include <QVersitContactHandler>

QTCONTACTS_USE_NAMESPACE
QTVERSIT_USE_NAMESPACE

class CONTACTCACHE_EXPORT SeasideImport
{
    SeasideImport();
    ~SeasideImport();

public:
    static QContactFilter localContactFilter();
    static QList<QContact> buildMergeImportContacts(const QList<QVersitDocument> &details,
                                                    int *newCount = 0,
                                                    int *updatedCount = 0);
    static QList<QContact> buildImportContacts(const QList<QVersitDocument> &details,
                                               int *newCount = 0,
                                               int *updatedCount = 0,
                                               const QSet<QContactDetail::DetailType> &unimportableDetailTypes = (QSet<QContactDetail::DetailType>() << QContactDetail::TypeGlobalPresence << QContactDetail::TypeVersion),
                                               const QStringList &importableSyncTargets = (QStringList() << QLatin1String("was_local") << QLatin1String("bluetooth")),
                                               const QContactFilter &mergeMatchFilter = localContactFilter(),
                                               QContactManager *manager = 0,
                                               QVersitContactHandler *propertyHandler = 0,
                                               bool mergeImportListDuplicates = true,
                                               bool mergeDatabaseDuplicates = true,
                                               QMap<int, int> *importDuplicateIndexes = 0,
                                               QMap<int, QContactId> *databaseDuplicateIndexes = 0);
};

#endif
