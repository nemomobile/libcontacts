include(../config.pri)

TEMPLATE = lib
CONFIG += qt hide_symbols
CONFIG += create_pc create_prl no_install_prl

isEmpty(PREFIX) {
 PREFIX = /usr
}

# 'contacts' is too generic for the target name - use 'contactcache'
TARGET = $${PACKAGENAME}
target.path = $$PREFIX/lib
INSTALLS += target

# set version for generated pkgconfig files
VERSION=0.0.12
QMAKE_PKGCONFIG_INCDIR = $$PREFIX/include/$${PACKAGENAME}
QMAKE_PKGCONFIG_LIBDIR = $$PREFIX/lib
QMAKE_PKGCONFIG_DESTDIR = pkgconfig

CONFIG += link_pkgconfig
equals(QT_MAJOR_VERSION, 4) {
    packagesExist(mlite) {
        PKGCONFIG += mlite
        DEFINES += HAS_MLITE
    } else {
        warning("mlite not available. Some functionality may not work as expected.")
    }
}
equals(QT_MAJOR_VERSION, 5) {
    packagesExist(mlite5) {
        PKGCONFIG += mlite5
        DEFINES += HAS_MLITE
    } else {
        warning("mlite not available. Some functionality may not work as expected.")
    }
}

DEFINES += CONTACTCACHE_BUILD

SOURCES += \
    $$PWD/normalization.cpp \
    $$PWD/seasidecache.cpp

HEADERS += \
    $$PWD/normalization_p.h \
    $$PWD/contactcacheexport.h \
    $$PWD/seasidecache.h \
    $$PWD/synchronizelists.h \
    $$PWD/seasidenamegrouper.h

headers.files = \
    $$PWD/contactcacheexport.h \
    $$PWD/seasidecache.h \
    $$PWD/synchronizelists.h \
    $$PWD/seasidenamegrouper.h
headers.path = $$PREFIX/include/$$TARGET
INSTALLS += headers
