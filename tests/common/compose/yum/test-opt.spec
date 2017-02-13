Summary: Test package which installs in /opt
Name: test-opt
Version: 1.0
Release: 1
License: GPLv2+
Group: Development/Tools
URL: http://example.com
BuildArch: x86_64

%description
%{summary}

%prep

%build

%install
mkdir -p %{buildroot}/opt/app/bin
touch %{buildroot}/opt/app/bin/foo

%files
/opt/app
