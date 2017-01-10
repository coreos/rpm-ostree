Summary: Test failure to install due rofiles violatiin
Name: test-posttrunc-fail
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
cat > scriptpkg1 << EOF
#!/bin/sh
echo "Hello!"
EOF
chmod a+x scriptpkg1

%post
echo 'should fail' >> /usr/share/licenses/glibc/COPYING

%install
mkdir -p %{buildroot}/usr/bin
install scriptpkg1 %{buildroot}/usr/bin

%clean
rm -rf %{buildroot}

%files
/usr/bin/scriptpkg1
