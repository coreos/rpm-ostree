Summary: Awesome utility that allows convenient barbing
Name: bar
Version: 1.0
Release: 1
License: GPL+
Group: Development/Tools
URL: http://bar.bar.com
BuildArch: x86_64

# LONG LIVE BARBING!
Conflicts: foo

%description
%{summary}

%prep

%build
cat > bar << EOF
#!/bin/sh
echo "Happy barbing!"
EOF
chmod a+x bar

%install
mkdir -p %{buildroot}/usr/bin
install bar %{buildroot}/usr/bin

%clean
rm -rf %{buildroot}

%files
/usr/bin/bar

%changelog
* Tue Jun 21 2016 Jonathan Lebon <jlebon@redhat.com> 1.0-1
- First Build
