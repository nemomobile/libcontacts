include(../package.pri)

SRCDIR = ../../src/
INCLUDEPATH += $$SRCDIR
DEPENDPATH = $$INCLUDEPATH

QT -= gui
QT += testlib
TEMPLATE = app
CONFIG -= app_bundle

target.path = /opt/tests/$${PACKAGENAME}
INSTALLS += target
