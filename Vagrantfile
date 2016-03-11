# vi: set ft=ruby :

# In the future, this might do something more interesting
# like contain code to sync git from the local dir into
# the system and do builds/installs there.  But for
# now at least this exists as a starting point, and
# once `ostree admin unlock` exists in the next version,
# things will be a bit simpler.

Vagrant.configure(2) do |config|
    config.vm.box = "fedora/23-atomic-host"
    config.vm.hostname = "fedoraah-dev"
end
