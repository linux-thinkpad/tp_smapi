%define module tp_smapi
%define version 0.42

Name:           %{module}
Version:        %{version}
Release:        1%{?dist}
Summary:        IBM ThinkPad hardware functions driver - DKMS version
Group:          Kernel/Drivers
License:        GPLv2
Source0:        %{module}-%{version}.tgz

Requires:       dkms >= 1.00
Requires:       kernel-headers
Requires:       kernel-devel

BuildArch:      noarch


%description 
The package contains kernel driver for ThinkPad SMAPI (System
Management Application Program Interface). The driver is built using
DKMS.

%prep
%setup -q


%install
if [ "$RPM_BUILD_ROOT" != "/" ]; then
	rm -rf $RPM_BUILD_ROOT
fi
mkdir -p $RPM_BUILD_ROOT/usr/src/%{module}-%{version}/
cp -rf * $RPM_BUILD_ROOT/usr/src/%{module}-%{version}

%clean
if [ "$RPM_BUILD_ROOT" != "/" ]; then
	rm -rf $RPM_BUILD_ROOT
fi

%files
%defattr(-,root,root)
%doc README TODO
%{_usrsrc}/%{module}-%{version}/

%doc

%post
dkms add -m %{module} -v %{version} --rpm_safe_upgrade
dkms build -m %{module} -v %{version}
dkms install -m %{module} -v %{version}

%preun
dkms remove -m %{module} -v %{version} --all --rpm_safe_upgrade

%changelog
