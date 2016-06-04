# vi: set ft=ruby :

# In the future, this might do something more interesting
# like contain code to sync git from the local dir into
# the system and do builds/installs there.  But for
# now at least this exists as a starting point, and
# once `ostree admin unlock` exists in the next version,
# things will be a bit simpler.

Vagrant.configure(2) do |config|
    config.vm.box = "centos/atomic-host"
    config.vm.hostname = "centosah-dev"

    config.vm.provision "ansible" do |ansible|
      ansible.playbook = "tests/vmcheck/setup.yml"
      ansible.host_key_checking = false
      ansible.extra_vars = { ansible_ssh_user: 'vagrant' }
      ansible.raw_ssh_args = ['-o ControlMaster=no']
    end
end
