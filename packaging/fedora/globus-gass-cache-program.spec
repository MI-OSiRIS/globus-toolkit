%ifarch alpha ia64 ppc64 s390x sparc64 x86_64
%global flavor gcc64
%else
%global flavor gcc32
%endif

%if "%{?rhel}" == "4" || "%{?rhel}" == "5"
%global docdiroption "with-docdir"
%else
%global docdiroption "docdir"
%endif

Name:		globus-gass-cache-program
%global _name %(tr - _ <<< %{name})
Version:	5.2
Release:	3%{?dist}
Summary:	Globus Toolkit - Tools to manipulate local and remote GASS caches

Group:		Applications/Internet
License:	ASL 2.0
URL:		http://www.globus.org/
Source:		http://www.globus.org/ftppub/gt5/5.2/testing/packages/src/%{_name}-%{version}.tar.gz
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

Requires:	globus-common%{?_isa} >= 14
Requires:	globus-gram-client%{?_isa} >= 12
Requires:	globus-gass-server-ez%{?_isa} >= 4
Requires:	globus-gass-copy%{?_isa} >= 8
Requires:	globus-gass-cache%{?_isa} >= 8

BuildRequires:	grid-packaging-tools >= 3.4
BuildRequires:	globus-common-devel >= 14
BuildRequires:	globus-gram-client-devel >= 12
BuildRequires:	globus-gass-server-ez-devel >= 4
BuildRequires:	globus-gass-copy-devel >= 8
BuildRequires:	globus-gass-cache-devel >= 8
BuildRequires:	globus-core >= 8

%description
The Globus Toolkit is an open source software toolkit used for building Grid
systems and applications. It is being developed by the Globus Alliance and
many others all over the world. A growing number of projects and companies are
using the Globus Toolkit to unlock the potential of grids for their cause.

The %{name} package contains:
Tools to manipulate local and remote GASS caches

%prep
%setup -q -n %{_name}-%{version}

%build
# Remove files that should be replaced during bootstrap
rm -f doxygen/Doxyfile*
rm -f doxygen/Makefile.am
rm -f pkgdata/Makefile.am
rm -f globus_automake*
rm -rf autom4te.cache

%{_datadir}/globus/globus-bootstrap.sh

%configure --with-flavor=%{flavor} \
           --%{docdiroption}=%{_docdir}/%{name}-%{version} \
           --disable-static

make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT

GLOBUSPACKAGEDIR=$RPM_BUILD_ROOT%{_datadir}/globus/packages

# Generate package filelists
cat $GLOBUSPACKAGEDIR/%{_name}/%{flavor}_pgm.filelist \
    $GLOBUSPACKAGEDIR/%{_name}/noflavor_doc.filelist \
    $GLOBUSPACKAGEDIR/%{_name}/noflavor_data.filelist \
  | sed s!^!%{_prefix}! > package.filelist

%clean
rm -rf $RPM_BUILD_ROOT

%files -f package.filelist
%defattr(-,root,root,-)
%dir %{_datadir}/globus/packages/%{_name}
%dir %{_docdir}/%{name}-%{version}

%changelog
* Wed Jun 26 2013 Globus Toolkit <support@globus.org> - 5.2-3
- GT-424: New Fedora Packaging Guideline - no %_isa in BuildRequires

* Mon Nov 26 2012 Globus Toolkit <support@globus.org> - 5.2-2
- 5.2.3

* Tue Jul 17 2012 Joseph Bester <bester@mcs.anl.gov> - 5.2-1
- GT-252: Missing dependency in gass cache program

* Mon Jul 16 2012 Joseph Bester <bester@mcs.anl.gov> - 5.1-4
- GT 5.2.2 final

* Fri Jun 29 2012 Joseph Bester <bester@mcs.anl.gov> - 5.1-3
- GT 5.2.2 Release

* Wed May 09 2012 Joseph Bester <bester@mcs.anl.gov> - 5.1-2
- RHEL 4 patches

* Tue Feb 14 2012 Joseph Bester <bester@mcs.anl.gov> - 5.1-1
- GRAM-311: Undefined variable defaults in shell scripts
- RIC-226: Some dependencies are missing in GPT metadata

* Mon Dec 05 2011 Joseph Bester <bester@mcs.anl.gov> - 5.0-5
- Update for 5.2.0 release

* Mon Dec 05 2011 Joseph Bester <bester@mcs.anl.gov> - 5.0-4
- Last sync prior to 5.2.0

* Tue Oct 11 2011 Joseph Bester <bester@mcs.anl.gov> - 5.0-3
- Add explicit dependencies on >= 5.2 libraries

* Thu Sep 01 2011 Joseph Bester <bester@mcs.anl.gov> - 5.0-2
- Update for 5.1.2 release

* Mon Apr 25 2011 Mattias Ellert <mattias.ellert@fysast.uu.se> - 2.8-3
- Add README file

* Tue Feb 08 2011 Fedora Release Engineering <rel-eng@lists.fedoraproject.org> - 2.8-2
- Rebuilt for https://fedoraproject.org/wiki/Fedora_15_Mass_Rebuild

* Sat Jan 23 2010 Mattias Ellert <mattias.ellert@fysast.uu.se> - 2.8-1
- Update to Globus Toolkit 5.0.0

* Thu Jul 30 2009 Mattias Ellert <mattias.ellert@fysast.uu.se> - 2.7-1
- Autogenerated
