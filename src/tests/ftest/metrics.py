#!/usr/bin/python2
'''
  (C) Copyright 2018 Intel Corporation.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
  The Government's rights to use, modify, reproduce, release, perform, display,
  or disclose this software are subject to the terms of the Apache License as
  provided in Contract No. B609815.
  Any reproduction of computer software, computer software documentation, or
  portions thereof marked with this legend must also reproduce the markings.
'''

import os
import time
import traceback
import sys
import fnmatch
import subprocess
import jira
from jira import JIRA


def filelist(directory):
    """
    Create a list of test files contained in the provided path.
    This is meant to deal primarily with the structure of tests
    in the daos repo and would need to be changed to deal with
    random directory trees of tests.
    """

    test_files = []
    test_pattern = "*.py"

    for path, dirs, files in os.walk(directory):
        if not (path == directory or path == os.path.join(directory, 'util')):
            for f in files:
                if fnmatch.fnmatch(f, test_pattern):
                    test_files.append(os.path.join(path,f))
    return test_files;

def yamlforpy(path):
    """
    Create the name of the yaml file for a given test file.
    """
    (base, ext) = os.path.splitext(path)
    return base + ".yaml"

if __name__ == "__main__":

    # make it easy to specify a directory as a parameter later
    test_directory = os.getcwd()

    # build a list of test classes
    test_files = filelist(test_directory)

    tests = 0
    variants = 0
    print "working ",
    for _file in test_files:
        cmd1 = "avocado list {}".format(_file)
        output = subprocess.check_output(cmd1, shell=True)
        tests += len(output.splitlines())
        yamlfile = yamlforpy(_file)
        cmd2 = "avocado variants -m {} --summary 0 --variants 1".format(yamlfile)
        output = subprocess.check_output(cmd2, shell=True)
        variants += len(output.splitlines())
        print ".",
        sys.stdout.flush()

    print
    print("existing avocado tests> {}".format(tests))
    print("existing avocado variants> {}".format(variants))

    options = {'server': 'https://jira.hpdd.intel.com'}
    jira = JIRA(options)

    issues = jira.search_issues('project=DAOS AND component=test AND assignee=daos-triage')
    print("test stories in backlog> {}".format(issues.total))

    q = """project=DAOS AND component=test AND project=DAOS AND
           status in ("In Progress","In Review")"""
    issues = jira.search_issues(q)

    print("total test stories in progress> {}".format(issues.total))

    q = """project=DAOS AND component=test AND project=DAOS AND
           status in ("Closed","Done","Resolved")"""
    issues = jira.search_issues(q)
    print("total test stories completed> {}".format(issues.total))
