%define module tp_smapi

Name:           %{module}
Version:        0.44
Release:        1%{?dist}
Summary:        IBM ThinkPad hardware functions driver - DKMS version
License:        GPLv2
Source0:        %{module}-%{version}.tgz

Requires:       dkms >= 1.00

BuildArch:      noarch


%description
The package contains kernel driver for ThinkPad SMAPI (System
Management Application Program Interface). The driver is built using
DKMS.

%prep
%setup -q


%install
mkdir -p %{buildroot}%{_usrsrc}/%{module}-%{version}/
cp -rf * %{buildroot}%{_usrsrc}/%{module}-%{version}

%files
%doc README TODO
%{_usrsrc}/%{module}-%{version}/

%post
dkms add -m %{module} -v %{version} --rpm_safe_upgrade
dkms build -m %{module} -v %{version}
dkms install -m %{module} -v %{version}

%preun
dkms remove -m %{module} -v %{version} --all --rpm_safe_upgrade

%changelog
