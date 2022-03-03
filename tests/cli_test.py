#!/usr/bin/env python3

# This script tests that the compiled binaries produce expected output in
# response to the `--help` option. It also contains a couple of additional
# CLI-related checks as well as test cases for CLI bugfixes.

import subprocess
import re


def amnod_help_test():
    """Test that amnod help contains option descriptions"""
    help_text = subprocess.check_output(["./programs/amnod/amnod", "--help"])

    assert(re.search(b'Application.*Options', help_text))
    assert(re.search(b'Options for .*_plugin', help_text))


def amcli_help_test(args):
    """Test that amcli help contains option and subcommand descriptions"""
    help_text = subprocess.check_output(["./programs/amcli/amcli"] + args)

    assert(b'Options:' in help_text)
    assert(b'Subcommands:' in help_text)


def cli11_bugfix_test():
    """Test that subcommand names can be used as option arguments"""
    completed_process = subprocess.run(
        ['./programs/amcli/amcli', '-u', 'http://localhost:0/',
         'push', 'action', 'accout', 'action', '["data"]', '-p', 'wallet'],
        check=False,
        stderr=subprocess.PIPE)

    # The above command must fail because there is no server running
    # on localhost:0
    assert(completed_process.returncode != 0)

    # Make sure that the command failed because of the connection error,
    # not the command line parsing error.
    assert(b'Failed to connect to amnod' in completed_process.stderr)


def cli11_optional_option_arg_test():
    """Test that options like --password can be specified without a value"""
    chain = 'cf057bbfb72640471fd910bcb67639c22df9f92470936cddc1ade0e2f2e7dc4f'
    key = '5Jgfqh3svgBZvCAQkcnUX8sKmVUkaUekYDGqFakm52Ttkc5MBA4'

    output = subprocess.check_output(['./programs/amcli/amcli', 'sign',
                                      '-c', chain, '-k', '{}'],
                                     input=key.encode(),
                                     stderr=subprocess.DEVNULL)
    assert(b'signatures' in output)

    output = subprocess.check_output(['./programs/amcli/amcli', 'sign',
                                      '-c', chain, '-k', key, '{}'])
    assert(b'signatures' in output)


amnod_help_test()

amcli_help_test(['--help'])
amcli_help_test(['system', '--help'])
amcli_help_test(['version', '--help'])
amcli_help_test(['wallet', '--help'])

cli11_bugfix_test()

cli11_optional_option_arg_test()
