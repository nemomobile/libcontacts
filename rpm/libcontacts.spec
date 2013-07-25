Name:       libcontacts
Summary:    Nemo contact cache library
Version:    0.0.0
Release:    1
Group:      System/Libraries
License:    BSD
URL:        https://github.com/nemomobile/libcontacts
Source0:    %{name}-%{version}.tar.bz2
Requires:   qtcontacts-sqlite
BuildRequires:  pkgconfig(QtCore)
BuildRequires:  pkgconfig(QtContacts)
BuildRequires:  pkgconfig(QtVersit)
BuildRequires:  pkgconfig(mlite)
BuildRequires:  pkgconfig(qtcontacts-sqlite-extensions)

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

%qmake 

make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%qmake_install

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/libcontactcache.so*

%files tests
%defattr(-,root,root,-)
/opt/tests/contactcache/*

%files devel
%defattr(-,root,root,-)
%{_includedir}/contactcache/*
%{_libdir}/pkgconfig/contactcache.pc
