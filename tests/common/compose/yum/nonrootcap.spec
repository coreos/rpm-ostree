Summary: An app that uses has non-root files and caps
Name: nonrootcap
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
cat > tmp << EOF
#!/bin/sh
echo "Hello!"
EOF

chmod a+x tmp
cp tmp nrc-none.sh
cp tmp nrc-user.sh
cp tmp nrc-group.sh
cp tmp nrc-caps.sh
cp tmp nrc-usergroup.sh
cp tmp nrc-usergroupcaps.sh
rm tmp

%pre
groupadd -r nrcgroup
useradd -r nrcuser -g nrcgroup -s /sbin/nologin

%install
mkdir -p %{buildroot}/usr/bin
install *.sh %{buildroot}/usr/bin
mkdir -p %{buildroot}/var/lib/nonrootcap
mkdir -p %{buildroot}/run/nonrootcap

%clean
rm -rf %{buildroot}

%files
/usr/bin/nrc-none.sh
%attr(-, nrcuser, -) /usr/bin/nrc-user.sh
%attr(-, -, nrcgroup) /usr/bin/nrc-group.sh
%caps(cap_net_bind_service=ep) /usr/bin/nrc-caps.sh
%attr(-, nrcuser, nrcgroup) /usr/bin/nrc-usergroup.sh
%attr(-, nrcuser, nrcgroup) %caps(cap_net_bind_service=ep) /usr/bin/nrc-usergroupcaps.sh
%attr(-, nrcuser, nrcgroup) /var/lib/nonrootcap
%attr(-, nrcuser, nrcgroup) /run/nonrootcap

%changelog
* Wed Jan 05 2017 Jonathan Lebon <jlebon@redhat.com> 1.0-1
- First Build
