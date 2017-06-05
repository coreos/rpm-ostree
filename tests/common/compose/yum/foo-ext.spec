Summary: Awesome utility that requires foo
Name: foo-ext
Version: 1.0
Release: 1
License: GPL+
Group: Development/Tools
URL: http://foo.bar.com
BuildArch: x86_64

Requires: foo

%description
%{summary}

%prep

%build
cat > foo-ext << EOF
#!/bin/sh
echo "Happy ext foobing!"
EOF
chmod a+x foo-ext

%install
mkdir -p %{buildroot}/usr/bin
install foo-ext %{buildroot}/usr/bin

%clean
rm -rf %{buildroot}

%files
/usr/bin/foo-ext

%changelog
* Tue Jun 21 2016 Jonathan Lebon <jlebon@redhat.com> 1.0-1
- First Build
