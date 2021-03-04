#!/bin/bash
##########################################################################
# If not stated otherwise in this file or this component's Licenses.txt
# file the following copyright and licenses apply:
#
# Copyright 2017 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################

#
# Build Framework standard script for
#
# Audioserver component

# use -e to fail on any shell issue
# -e is the requirement from Build Framework
set -e


# default PATHs - use `man readlink` for more info
# the path to combined build
export RDK_PROJECT_ROOT_PATH=${RDK_PROJECT_ROOT_PATH-`readlink -m ../../..`}
export COMBINED_ROOT=$RDK_PROJECT_ROOT_PATH

# path to build script (this script)
export RDK_SCRIPTS_PATH=${RDK_SCRIPTS_PATH-`readlink -m $0 | xargs dirname`}

# default component name
export RDK_COMPONENT_NAME=${RDK_COMPONENT_NAME-`basename $RDK_SCRIPTS_PATH`}

# path to components sources and target
export RDK_SOURCE_PATH=${RDK_SOURCE_PATH-$RDK_SCRIPTS_PATH/../..}
export RDK_TARGET_PATH=${RDK_TARGET_PATH-$RDK_SOURCE_PATH}

# fsroot and toolchain (valid for all devices)
export RDK_FSROOT_PATH=${RDK_FSROOT_PATH-`readlink -m $RDK_PROJECT_ROOT_PATH/sdk/fsroot/ramdisk`}
export RDK_TOOLCHAIN_PATH=${RDK_TOOLCHAIN_PATH-`readlink -m $RDK_PROJECT_ROOT_PATH/sdk/toolchain/staging_dir`}
export RDK_TOOLCHAIN_PRED_PATH=${RDK_TOOLCHAIN_PRED_PATH-`readlink -m $RDK_PROJECT_ROOT_PATH/sdk/toolchain`}

# cda these aren't set...
if [ -z "$RDK_PLATFORM_SOC" ]; then
    RDK_PLATFORM_SOC=broadcom
fi

if [ -z "$RDK_PLATFORM_DEVICE" ]; then
    RDK_PLATFORM_DEVICE=xg1
fi

export CC_DEVICE=${RDK_PLATFORM_DEVICE-xg1}
export WORK_DIR=${RDK_PROJECT_ROOT_PATH}/work${CC_DEVICE^^}
export NEXUS=${WORK_DIR}/Refsw/nexus
export TOOLS=${WORK_DIR}/tools

export CXX=mipsel-linux-uclibc-g++

export LINUX_BUILD=1

case "$RDK_PLATFORM_SOC" in
    broadcom )
        source ${RDK_PROJECT_ROOT_PATH}/build_scripts/setBCMenv.sh
	export CROSS_COMPILE=mipsel-linux-
	export GCC=${CROSS_COMPILE}gcc
	export GXX=${CROSS_COMPILE}g++
	export LD=${CROSS_COMPILE}ld
	export CC=${CROSS_COMPILE}gcc
	export CXX=${CROSS_COMPILE}g++
	export DEFAULT_HOST=mipsel-linux

        case "$RDK_PLATFORM_DEVICE" in
            rng150 )
                echo building broadcom rng150
                ;;

            xg1 )
                echo building broadcom xg1;
                ;;

            xi3 )
                echo building broadcom xi3 ;
                ;;

            * )
                echo dont know this broadcom build ;;
        esac  # case "$RDK_PLATFORM_DEVICE"
        ;;

    intel )
        case "$RDK_PLATFORM_DEVICE" in
            xg1 )
                echo building intel xg1
                export CROSS_HOST=i686-cm-linux
                export CROSS_TOOLCHAIN=${RDK_TOOLCHAIN_PRED_PATH}/i686-linux-elf
                export CROSS_COMPILE=$CROSS_TOOLCHAIN/bin/i686-cm-linux-
                export GCC=${CROSS_COMPILE}gcc
                export GXX=${CROSS_COMPILE}g++
                export LD=${CROSS_COMPILE}ld
                export CC=${CROSS_COMPILE}gcc
                export CXX=${CROSS_COMPILE}g++
                export SCANNER_TOOL=$RDK_PROJECT_ROOT_PATH/opensource/bin/wayland-scanner
                ;;

            * )
                echo dont know this intel build ;;
        esac  # case "$RDK_PLATFORM_DEVICE"
        ;;
    stm )
        export TOOLCHAIN_NAME=`find ${RDK_TOOLCHAIN_PATH} -name environment-setup-* | sed -r 's#.*environment-setup-##'`
        source $RDK_TOOLCHAIN_PATH/environment-setup-${TOOLCHAIN_NAME}
        source $RDK_PROJECT_ROOT_PATH/opensource/qt/apps_helpers.sh

        case "$RDK_PLATFORM_DEVICE" in
            xi4 )
                echo building stm xi4 ;;

            * )
                echo dont know this stm build ;;
        esac  # case "$RDK_PLATFORM_DEVICE"

esac # case "$RDK_PLATFORM_SOC"

DEBUG=0
COMBINED=1
DEFAULTCFG=0
REBUILD=0
UPLOAD=0
JOBS_NUM=0 # 0 means detect automatically

# parse arguments
INITIAL_ARGS=$@

function usage()
{
    set +x
    echo "Usage: `basename $0` [-cdujn] [-h|--help] [-v|--verbose] [action]"
    echo "    -h    --help                  : this help"
    echo "    -v    --verbose               : verbose output"
    echo "    -c                            : combined build [default]"
    echo "    -d                            : debug build"
    echo "    -u                            : enable dump debug symbols for crash portal"
    echo "    -j                            : specify number of jobs to be used by make"
    echo "    -n                            : use empty project configuration"
    echo
    echo "Supported actions:"
    echo "      configure, clean, build (DEFAULT), rebuild, install"
}

# options may be followed by one colon to indicate they have a required argument
if ! GETOPT=$(getopt -n "build.sh" -o hvdcnuj:h -l help,verbose -- "$@")
then
    usage
    exit 1
fi

eval set -- "$GETOPT"

while true; do
  case "$1" in
    -h | --help ) usage; exit 0 ;;
    -v | --verbose ) set -x ;;
    -d ) DEBUG=1 ;;
    -c ) COMBINED=1 ;;
    -n ) DEFAULTCFG=1 ;;
    -u ) UPLOAD=1 ;;
    -j ) JOBS_NUM="$2" ;;
    -- ) shift; break;;
    * ) break;;
  esac
  shift
done

ARGS=$@


# component-specific vars
export SCRIPTS_DIR=${SCRIPTS_DIR-$RDK_PROJECT_ROOT_PATH/build_scripts}
export DUMP_SYMS=${DUMP_SYMS-$SCRIPTS_DIR/tools/linux/dump_syms}
export EXTRACT_SYMS_PATH=${EXTRACT_SYMS_PATH-$RDK_PROJECT_ROOT_PATH/build/packager_scripts}

export PKG_CONFIG_PATH=${RDK_FSROOT_PATH}/usr/local/lib/pkgconfig:${RDK_TOOLCHAIN_PATH}/lib/pkgconfig/
export PKG_CONFIG_LIBDIR="$RDK_FSROOT_PATH/usr/local/lib/pkgconfig"
export CFLAGS="-I${RDK_TOOLCHAIN_PATH}/include -I${RDK_TOOLCHAIN_PATH}/include/linux_user -I${RDK_FSROOT_PATH}/usr/local/include"
export CXXFLAGS=${CFLAGS}
export LDFLAGS=-L${RDK_FSROOT_PATH}/usr/local/lib

# functional modules

function configure()
{
   pd=`pwd`
   cd ${RDK_SOURCE_PATH}

   aclocal
   libtoolize --automake
   autoheader
   automake --foreign --add-missing
   rm -f configure
   autoconf
   ./configure --prefix=${RDK_FSROOT_PATH}/usr/local --host=${CROSS_HOST} 

   cd $pd
}

function clean()
{
   pd=`pwd`
   cd ${RDK_SOURCE_PATH}
   if [ -f Makefile ]; then
     make distclean
   fi
   cd $pd
}

function build()
{
   pd=`pwd`
   cd ${RDK_SOURCE_PATH}
   make
   cd $pd
}

function rebuild()
{
    clean
    configure
    build
}

function install()
{
   pd=`pwd`
   cd ${RDK_SOURCE_PATH}
   make install
   mkdir -p ${RDK_FSROOT_PATH}/usr/local/bin
 
   #work around for intel stdc++ .la file error (remove this when have a real fix)
   #pass -i to all make calls to skip errors (remove these when have real fix)
   #copy libraries that should have been intalled with make install
   cp -aP ${RDK_SOURCE_PATH}/.libs/*.so* ${RDK_FSROOT_PATH}/usr/local/lib/
   cd $pd
}

# run the logic

#these args are what left untouched after parse_args
HIT=false

for i in "$ARGS"; do
    case $i in
        configure)  HIT=true; configure ;;
        clean)      HIT=true; clean ;;
        build)      HIT=true; build ;;
        rebuild)    HIT=true; rebuild ;;
        install)    HIT=true; install ;;
        *)
            #skip unknown
        ;;
    esac
done

# if not HIT do build by default
if ! $HIT; then
  build
fi
