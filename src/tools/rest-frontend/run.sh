#!/bin/sh
cd @CMAKE_INSTALL_PREFIX@/@install_directory@ && ./node_modules/grunt-cli/bin/grunt server &
echo $! > /run/elektra-@tool@.pid
