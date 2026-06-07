#!/bin/sh
# RkLunch.sh — UVC camera launcher for Luckfox Pico Pro Max (RV1106, MIS5001)
# Replaces the factory rkipc launcher. Called on boot via post_chk chain.

program_kill() {
    local pids
    pids=$(ps | grep "$1" | grep -v grep | awk '{print $1}')
    [ -z "$pids" ] || kill -9 $pids 2>/dev/null
}

network_init() {
    ethaddr1=$(ifconfig -a | grep "eth.*HWaddr" | awk '{print $5}')
    if [ -f /data/ethaddr.txt ]; then
        ethaddr2=$(cat /data/ethaddr.txt)
        if [ "$ethaddr1" = "$ethaddr2" ]; then
            echo "eth HWaddr cfg ok"
        else
            ifconfig eth0 down
            ifconfig eth0 hw ether $ethaddr2
        fi
    else
        echo $ethaddr1 > /data/ethaddr.txt
    fi
    ifconfig eth0 up && udhcpc -i eth0 >/dev/null 2>&1
}

post_chk() {
    # Wait for /userdata to be mounted (same as factory)
    cnt=0
    while [ $cnt -lt 30 ]; do
        cnt=$((cnt + 1))
        if mount | grep -w userdata; then
            break
        fi
        sleep .1
    done

    # Load kernel modules if present
    local ko_dir=/oem/usr/ko
    if [ -f "$ko_dir/insmod_ko.sh" ]; then
        cd "$ko_dir" && sh insmod_ko.sh 2>/dev/null && cd -
    fi

    # Bring up Ethernet — this is what keeps SSH working across reboots
    network_init &

    # Stop factory rkipc and its USB watcher so we can take over the ISP and USB gadget
    program_kill rkipc
    program_kill usb_reset
    sleep 1

    # Set up USB gadget for UVC (usb_config.sh is deployed by our build, not in factory fw)
    if [ -f /oem/usr/bin/usb_config.sh ]; then
        echo '' > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null || true
        /oem/usr/bin/usb_config.sh
    fi

    # Copy ini to /tmp so runtime edits survive watchdog restarts
    local ini=/tmp/rkuvc.ini
    if [ ! -f "$ini" ]; then
        for src in /oem/usr/share/rkuvc.ini /usr/share/rkuvc.ini; do
            [ -f "$src" ] && cp "$src" "$ini" && break
        done
    fi

    export PATH=/oem/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH
    export rt_log_level=3
    export rk_mpi_uvc_log_level=2
    touch /tmp/uvc_no_timeout

    # Start rk_mpi_uvc and watchdog it
    if [ -d "/oem/usr/share/iqfiles" ]; then
        /oem/usr/bin/rk_mpi_uvc -c /tmp/rkuvc.ini -a /oem/usr/share/iqfiles &
    else
        /oem/usr/bin/rk_mpi_uvc &
    fi

    while true; do
        sleep 2
        [ -f /tmp/uvc_stop_check ] && echo "RkLunch: stop requested" && break
        if ! ps | grep -q "[r]k_mpi_uvc"; then
            echo "RkLunch: rk_mpi_uvc died — restarting..."
            if [ -d "/oem/usr/share/iqfiles" ]; then
                /oem/usr/bin/rk_mpi_uvc -c /tmp/rkuvc.ini -a /oem/usr/share/iqfiles &
            else
                /oem/usr/bin/rk_mpi_uvc &
            fi
        fi
    done
}

ulimit -c unlimited
echo "/data/core-%p-%e" > /proc/sys/kernel/core_pattern 2>/dev/null || true
echo 1 > /proc/sys/vm/overcommit_memory 2>/dev/null || true

post_chk &
