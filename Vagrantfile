# vi: set ft=ruby :

# See `HACKING.md` for more information on this.

Vagrant.configure(2) do |config|
    config.vm.box = "centos/atomic-host"
    config.vm.hostname = "centosah-dev"
    config.vm.define "vmcheck" do |vmcheck|
    end

    # It's just easier to have ssh set up as root from the start so that tests
    # don't need to sudo, which can sometimes cause issues. If we need to test
    # any unprivileged things, we can still just sudo back into the vagrant
    # user.
    config.ssh.username = 'root'
    config.ssh.password = 'vagrant'
    config.ssh.insert_key = 'true'

    # turn off the default rsync in the vagrant box and replace by one that
    # places it directly to root (and also skip sync'ing .git and any cached
    # containers)
    config.vm.synced_folder ".", "/home/vagrant/sync", disabled: true
    config.vm.synced_folder ".", "/root/sync", type: "rsync",
      rsync__exclude: [".git/", "vagrant/*.tar.gz"],

      # override the default args so that
      #  (1) we don't use --delete (otherwise we will have to regen each time)
      #  (2) we can tell rsync to skip ignored files
      rsync__args: ["--verbose", "--archive", "-z", "--filter", ":- .gitignore"]

    config.vm.provider "libvirt" do |libvirt, override|
      libvirt.cpus = 2
      libvirt.memory = 2048
      libvirt.driver = 'kvm'
    end

    config.vm.provision "ansible" do |ansible|
      ansible.playbook = "vagrant/setup.yml"
      ansible.host_key_checking = false
      ansible.extra_vars = { ansible_ssh_user: 'root' }
      ansible.raw_ssh_args = ['-o ControlMaster=no']
      # for debugging the ansible playbook
      #ansible.raw_arguments = ['-v']
    end
end
