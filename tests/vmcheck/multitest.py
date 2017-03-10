#!/bin/env python

import os
import sys
import glob
import subprocess


def main():

    failed = False
    hosts = []
    for host in sys.argv[1:]:
        hosts.append(Host(host))

    for test in glob.iglob(os.path.join(sys.path[0], "test-*.sh")):
        host = wait_for_next_available_host(hosts)
        rc = host.flush()
        failed = failed or rc != 0
        host.dispatch(test)

    for host in hosts:
        rc = host.flush()
        failed = failed or rc != 0

    # fetch the journal from all the hosts which had a failure
    fetcher = os.path.join(sys.path[0], "fetch-journal.sh")
    for host in hosts:
        if host.saw_fail:
            fetcher_env = dict(os.environ)
            fetcher_env.update({'VM': host.hostname,
                                'JOURNAL_LOG':
                                "vmcheck/%s.journal.log" % host.hostname})
            subprocess.check_call([fetcher], env=fetcher_env)

    return 1 if failed else 0


def wait_for_next_available_host(hosts):
    while True:
        for host in hosts:
            if host.is_done():
                return host
        os.wait()


class Host:

    def __init__(self, hostname):
        self.hostname = hostname
        self.test = ""
        self._p = None
        self.saw_fail = False

    def is_done(self):
        if not self._p:
            return True
        return self._p.poll() is not None

    def dispatch(self, test):
        assert self.is_done()
        test = self._strip_test(test)
        env = dict(os.environ)
        env.update({'TESTS': test,
                    'VM': self.hostname,
                    'JOURNAL_LOG': "",  # we fetch the journal at the end
                    'LOG': "vmcheck/%s.out" % test})
        if not os.path.isdir("vmcheck"):
            os.mkdir("vmcheck")
        testsh = os.path.join(sys.path[0], "test.sh")
        self._p = subprocess.Popen([testsh], env=env,
                                   stdout=open("vmcheck/%s.log" % test, 'wb'),
                                   stderr=subprocess.STDOUT)
        self.test = test
        print "INFO: scheduled", self.test, "on host", self.hostname

    def flush(self):
        if not self._p:
            return
        rc = self._p.wait()

        # just merge the two files
        with open("vmcheck/%s.out" % self.test) as f:
            with open("vmcheck/%s.log" % self.test, 'a') as j:
                j.write(f.read())
        os.remove("vmcheck/%s.out" % self.test)

        rcs = "PASS" if rc == 0 else ("FAIL (rc %d)" % rc)
        print("%s: %s" % (rcs, self.test))

        self.test = ""
        self._p = None
        self.saw_fail = self.saw_fail or rc != 0
        return rc

    @staticmethod
    def _strip_test(test):
        test = os.path.basename(test)
        assert test.startswith('test-') and test.endswith('.sh')
        return test[5:-3]


if __name__ == '__main__':
    sys.exit(main())
