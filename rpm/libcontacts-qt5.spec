Name:       libcontacts-qt5
Summary:    Nemo contact cache library
Version:    0.0.0
Release:    1
Group:      System/Libraries
License:    BSD
URL:        https://github.com/nemomobile/libcontacts
Source0:    %{name}-%{version}.tar.bz2
Requires:   qtcontacts-sqlite-qt5
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Test)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(Qt5Versit)
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions) >= 0.1.1

%description
%{summary}.

%package tests
Summary:    Nemo contact cache library tests
Group:      System/Libraries
Requires:   %{name} = %{version}-%{release}

%description tests
%{summary}.

%package devel
Summary:    Nemo contact cache library headers
Group:      System/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
%{summary}.

%prep
%setup -q -n %{name}-%{version}

%build

%qmake5 

make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%qmake_install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/libcontactcache-qt5.so*

%files tests
%defattr(-,root,root,-)
/opt/tests/contactcache-qt5/*

%files devel
%defattr(-,root,root,-)
%{_includedir}/contactcache-qt5/*
%{_libdir}/pkgconfig/contactcache-qt5.pc
