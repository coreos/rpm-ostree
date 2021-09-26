#!/bin/bash
# Inject ideally temporary overrides into our cosa build
# skopeo for containers https://github.com/containers/skopeo/pull/1476
cd overrides/rpm
curl -L --remote-name-all \
  https://kojipkgs.fedoraproject.org//packages/skopeo/1.5.1/1.fc35/x86_64/skopeo-1.5.1-1.fc35.x86_64.rpm
