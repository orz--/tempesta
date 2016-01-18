#!/bin/bash
#
# Tempesta FW service script.
#
# Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
# Copyright (C) 2015-2016 Tempesta Technologies, Inc.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License,
# or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.
# See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc., 59
# Temple Place - Suite 330, Boston, MA 02111-1307, USA.

root=$(dirname "$0")
name=`basename $0` # program name (comm name in ps)

# Resolve root to absolute path which is handy for kernel.
# pwd is used instead of readlink to avoid symlink resolution.
pushd "$root" > /dev/null
root="$(pwd)"
popd > /dev/null

NETDEV_PATH="/sys/class/net/"

tdb_path=${TDB_PATH:="$root/tempesta_db/core"}
tfw_path=${TFW_PATH:="$root/tempesta_fw"}
class_path="$tfw_path/classifier/"
tfw_cfg_path=${TFW_CFG_PATH:="$root/etc/tempesta_fw.conf"}
sched_ko_files=($(ls $tfw_path/sched/*.ko))

tdb_mod=tempesta_db
tfw_mod=tempesta_fw
tfw_sched_mod=tfw_sched_$sched
frang_mod="tfw_frang"
devs=$(ls $NETDEV_PATH)
declare frang_enable=
declare -r long_opts="help,load,unload,start,stop,restart"

usage()
{
	echo -e "\nUsage: ${name} [options] {action}\n"
	echo -e "Options:"
	echo -e "  -f          Load Frang, HTTP DoS protection module."
	echo -e "  -d <devs>   Ingress and egress network devices"
	echo -e "              (ex. -d \"lo ens3\").\n"
	echo -e "Actions:"
	echo -e "  --help      Show this message and exit."
	echo -e "  --load      Load Tempesta modules."
	echo -e "  --unload    Unload Tempesta modules."
	echo -e "  --start     Load modules and start."
	echo -e "  --stop      Stop and unload modules."
	echo -e "  --restart   Restart.\n"
}

error()
{
	echo "ERROR: $1" >&1
	exit 1
}

# Tempesta requires kernel module loading, so we need root credentials.
[ `id -u` -ne 0 ] && error "Please, run the script as root"

# The separate load_modules/unload_modules routines are used for unit testing.
load_modules()
{
	echo "Loading Tempesta kernel modules..."

	# Set verbose kernel logging,
	# so debug messages are shown on serial console as well.
	echo '8 7 1 7' > /proc/sys/kernel/printk

	insmod $tdb_path/$tdb_mod.ko
	[ $? -ne 0 ] && error "cannot load tempesta database module"

	insmod $tfw_path/$tfw_mod.ko tfw_cfg_path=$tfw_cfg_path
	[ $? -ne 0 ] && error "cannot load tempesta module"

	for ko_file in "${sched_ko_files[@]}"; do
		insmod $ko_file
		[ $? -ne 0 ] && error "cannot load tempesta scheduler module"
	done

	if [ "$frang_enable" ]; then
		echo "Load Frang"
		insmod $class_path/$frang_mod.ko
		[ $? -ne 0 ] && error "cannot load $frang_mod module"
	fi
}

# Enable RPS for specified, or all by default, networking interfaces.
# This is required for loopback interface for proper local delivery,
# but physical interfaces can have RSS.
# TODO assign RSS queues as well.
tfw_set_rps()
{
	cpu_n=$(grep -c processor /proc/cpuinfo)
	cpu_mask=$(perl -le 'printf("%x", (1 << '$cpu_n') - 1)')

	for dev in $devs; do
		echo "...setup interface $dev"
		for rx in $NETDEV_PATH/$dev/queues/rx-*; do
			echo $cpu_mask > $rx/rps_cpus
		done
	done
}

start()
{
	echo "Starting Tempesta..."

	# Tempesta builds socket buffers by itself, don't cork TCP segments.
	sysctl -w net.ipv4.tcp_autocorking=0 >/dev/null

	tfw_set_rps

	# Create database directory if it doesn't exist.
	mkdir -p /opt/tempesta/db/

	sysctl -w net.tempesta.state=start
	[ $? -ne 0 ] && error "cannot start Tempesta FW"

	echo "done"
}

stop()
{
	echo "Stopping Tempesta..."

	sysctl -w net.tempesta.state=stop

	echo "done"
}

unload_modules()
{
	echo "Un-loading Tempesta kernel modules..."

	for ko_file in "${sched_ko_files[@]}"; do
		rmmod $(basename "${ko_file%.ko}")
	done

	[ "`lsmod | grep \"\<$frang_mod\>\"`" ] && rmmod $frang_mod
	rmmod $tfw_mod
	rmmod $tdb_mod
}

args=$(getopt -o "d:f" -a -l "$long_opts" -- "$@")
eval set -- "${args}"
while :; do
	case "$1" in
		# Selectors for internal usage.
		--load)
			load_modules
			exit
			;;
		--unload)
			unload_modules
			exit
			;;
		# User CLI.
		--start)
			load_modules
			start
			exit
			;;
		--stop)
			stop
			unload_modules
			exit
			;;
		--restart)
			stop
			start
			exit
			;;
		# Ignore any options after action.
		-d)
			devs=$2
			shift 2
			;;
		-f)
			frang_enable=1
			shift
			;;
		--help)
			usage
			exit
			;;
		*)
			error "Bad command line argument: $opt"
			exit 2
			;;
	esac
done
