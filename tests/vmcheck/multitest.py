#!/bin/env python

import os
import sys
import glob
import time
import subprocess


def main():

    failed = False
    hosts = []
    for host in sys.argv[1:]:
        hosts.append(Host(host))

    if len(hosts) == 0:
        print("error: no hosts provided")
        sys.exit(1)

    requested_tests_spec = os.environ.get('TESTS')
    if requested_tests_spec is not None:
        requested_tests = requested_tests_spec.split()
    else:
        requested_tests = None

    tests = glob.iglob(os.path.join(sys.path[0], "test-*.sh"))
    matched_tests = []
    unmatched_tests = []
    for test in tests:
        testname = Host._strip_test(test)
        if requested_tests is None or testname in requested_tests:
            matched_tests.append(test)
        else:
            unmatched_tests.append(testname)
    if len(matched_tests) == 0:
        print("error: no tests match '{}': {}".format(requested_tests_spec, unmatched_tests))
        sys.exit(1)

    for test in matched_tests:
        host = wait_for_next_available_host(hosts)
        rc = host.flush()
        failed = failed or rc != 0
        host.dispatch(test)
    if len(unmatched_tests) > 0:
        print("NOTE: Skipping tests not matching {}: {}".format(requested_tests_spec, unmatched_tests))

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
        time.sleep(1)


class Host:

    def __init__(self, hostname):
        self.hostname = hostname
        self.test = ""
        self._p = None
        self._starttime = None
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
        self._starttime = time.time()
        self._p = subprocess.Popen([testsh], env=env,
                                   stdout=open("vmcheck/%s.log" % test, 'wb'),
                                   stderr=subprocess.STDOUT)
        self.test = test
        print "INFO: scheduled", self.test, "on host", self.hostname

    def flush(self):
        if not self._p:
            return 0
        print("Waiting for completion of {} (pid={})".format(self.test, self._p.pid))
        rc = self._p.wait()
        endtime = time.time()

        # just merge the two files
        outfile = "vmcheck/{}.out".format(self.test)
        if os.path.isfile(outfile):
            with open(outfile) as f:
                with open("vmcheck/%s.log" % self.test, 'a') as j:
                    j.write(f.read())
            os.remove(outfile)

        rcs = "PASS" if rc == 0 else ("FAIL (rc %d)" % rc)
        print("%s: %s" % (rcs, self.test))
        print("Execution took {} seconds".format(int(endtime - self._starttime)))

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
