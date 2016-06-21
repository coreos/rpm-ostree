Summary: Awesome utility that allows convenient foobing
Name: foo
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
cat > foo << EOF
#!/bin/sh
echo "Happy foobing!"
EOF
chmod a+x foo

%install
mkdir -p %{buildroot}/usr/bin
install foo %{buildroot}/usr/bin

%clean
rm -rf %{buildroot}

%files
/usr/bin/foo

%changelog
* Tue Jun 21 2016 Jonathan Lebon <jlebon@redhat.com> 1.0-1
- First Build
