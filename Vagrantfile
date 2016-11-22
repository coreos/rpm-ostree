# vi: set ft=ruby :

# See `HACKING.md` for more information on this.

Vagrant.configure(2) do |config|

    config.vm.box = "centosah/7alpha"
    config.vm.box_url = 'https://ci.centos.org/artifacts/sig-atomic/centos-continuous/images-alpha/cloud/latest/images/centos-atomic-host-7-vagrant-libvirt.box'

    config.vm.hostname = "centosah-dev"

    config.vm.define "vmcheck" do |vmcheck|
    end

    if Vagrant.has_plugin?('vagrant-sshfs')
      config.vm.synced_folder ".", "/var/roothome/sync", type: 'sshfs'
      File.write('.vagrant/using_sshfs', '')
    end

    # turn off the default rsync in the vagrant box
    config.vm.synced_folder ".", "/home/vagrant/sync", disabled: true

    config.vm.provider "libvirt" do |libvirt, override|
      libvirt.cpus = 2
      libvirt.memory = 2048
      libvirt.driver = 'kvm'
    end

    config.vm.provision "ansible" do |ansible|
      ansible.playbook = "vagrant/setup.yml"
      ansible.host_key_checking = false
      ansible.raw_ssh_args = ['-o ControlMaster=no']
      # for debugging the ansible playbook
      #ansible.raw_arguments = ['-v']
    end
end
