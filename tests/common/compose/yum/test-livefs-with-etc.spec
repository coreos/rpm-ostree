Name: test-livefs-with-etc
Summary: %{name}
Version: 1.0
Release: 1
License: GPL+
Group: Development/Tools
URL: http://foo.bar.com
BuildArch: x86_64

%description
%{summary}

%prep

%build
cat > %{name} << EOF
#!/bin/sh
echo "livefs-with-etc"
EOF
chmod a+x %{name}
cat > %{name}.conf <<EOF
A config file for %{name}
EOF

%install
mkdir -p %{buildroot}/usr/bin
install %{name} %{buildroot}/usr/bin
mkdir -p %{buildroot}/etc
install %{name}.conf %{buildroot}/etc

%files
/usr/bin/%{name}
/etc/%{name}.conf
