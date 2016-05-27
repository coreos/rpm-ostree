%define        __spec_install_post %{nil}
%define          debug_package %{nil}
%define        __os_install_post %{_dbpath}/brp-compress

Summary: A very (un)useful package
Name: empty
Version: 1.0
Release: 1
License: GPL+
Group: Development/Tools
URL: http://foo.bar.com
BuildArch: x86_64
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root

%description
%{summary}

%prep

%build

%install
mkdir -p %{buildroot}/boot
mkdir -p %{buildroot}/var/lib
mkdir -p %{buildroot}/var/share/
mkdir -p %{buildroot}/var/tmp/
mkdir -p %{buildroot}/usr/sbin
mkdir -p %{buildroot}/etc
mkdir -p %{buildroot}/usr/lib
mkdir -p %{buildroot}/usr/lib/tmpfiles.d

for i in foo bar hello world; do
    echo $i > %{buildroot}/var/share/$i
done

touch %{buildroot}/boot/vmlinuz-kernel
echo "nobody:x:99:99:Nobody:/:/sbin/nologin" > %{buildroot}/etc/passwd
touch %{buildroot}/etc/group
touch %{buildroot}/etc/nsswitch.conf

cp empty %{buildroot}/usr/sbin/depmod
cp empty %{buildroot}/usr/sbin/dracut

touch %{buildroot}/var/tmp/initramfs.img

%clean
rm -rf %{buildroot}

%files
/var/lib
/var/share/*
/boot/*
/usr/sbin/*
/usr/lib/*
/var/tmp/*
/etc/*

%changelog
* Tue Mar 17 2015  Giuseppe Scrivano <gscrivan@redhat.com> 1.0-1
- First Build

EOF
