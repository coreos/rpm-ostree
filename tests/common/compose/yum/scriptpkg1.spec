Summary: An app that uses useradd in its %pre
Name: scriptpkg1
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
cat > scriptpkg1 << EOF
#!/bin/sh
echo "Hello!"
EOF
chmod a+x scriptpkg1

%pre
# Test our /etc/passwd handling
groupadd -r scriptpkg1

%posttrans
# Firewalld; https://github.com/projectatomic/rpm-ostree/issues/638
. /etc/os-release || :

%install
mkdir -p %{buildroot}/usr/bin
install scriptpkg1 %{buildroot}/usr/bin

%clean
rm -rf %{buildroot}

%files
/usr/bin/scriptpkg1

%changelog
* Wed Aug 17 2016 Jonathan Lebon <jlebon@redhat.com> 1.0-1
- First Build
