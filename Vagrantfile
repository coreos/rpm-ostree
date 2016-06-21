# vi: set ft=ruby :

# See `HACKING.md` for more information on this.

Vagrant.configure(2) do |config|
    config.vm.box = "centos/atomic-host"
    config.vm.hostname = "centosah-dev"
    config.vm.define "vmcheck" do |vmcheck|
    end

    config.vm.provider "libvirt" do |libvirt, override|
      libvirt.cpus = 2
      libvirt.memory = 2048
      libvirt.driver = 'kvm'
    end

    config.vm.provision "ansible" do |ansible|
      ansible.playbook = "vagrant/setup.yml"
      ansible.host_key_checking = false
      ansible.extra_vars = { ansible_ssh_user: 'vagrant' }
      ansible.raw_ssh_args = ['-o ControlMaster=no']
    end
end
