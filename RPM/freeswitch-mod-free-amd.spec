%define version %{fs_version}
%define release %{fs_release}

%define user %{fs_user}
%define group %{fs_group}

%define modules_dir %{mod_dir}

%define mod_filename mod_free_amd.so

%define default_freeswitch_pkg_path /usr/share/freeswitch/pkgconfig

##############################################################################
# General
##############################################################################

Summary: Free Answering Machine Detection module
Name: freeswitch-mod-free-amd
Version: %{version}
Release: %{release}%{?dist}
License: MPL
Packager: Gustavo Almeida <galmeida@broadvoice.com>
Source0: %{name}.tar.gz
BuildRequires: freeswitch-devel
Requires: freeswitch >= %{version}-%{release}
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
Free Answering Machine Detection module
This package contains AMD module for FreeSWITCH.
Free Answering Machine Detection module for FreeSWITCH.

##############################################################################
# Prep
##############################################################################

%prep
%setup -n mod_free_amd

##############################################################################
# Build
##############################################################################

%build
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:%{default_freeswitch_pkg_path}
make MODDIR=%{modules_dir}

##############################################################################
# Install
##############################################################################

%install
[ "%{buildroot}" != '/' ] && rm -rf %{buildroot}
make install MODDIR=%{modules_dir} DESTDIR=%{buildroot}

##############################################################################
# Clean
##############################################################################

%clean
[ "%{buildroot}" != '/' ] && rm -rf %{buildroot}

##############################################################################
# Post
##############################################################################

%post

##############################################################################
# Postun
##############################################################################

%postun

##############################################################################
# Files
##############################################################################

%files
%attr(0755,%{user},%{group})    %{modules_dir}/%{mod_filename}

##############################################################################
# Changelog
##############################################################################

%changelog
* Mon Nov 24 2025 - Gustavo Almeida
- Build and packaging of freeswitch-mod-free-amd
