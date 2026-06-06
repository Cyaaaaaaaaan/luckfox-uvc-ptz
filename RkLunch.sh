#!/bin/sh
# RkLunch.sh — UVC camera launcher for Luckfox Pico Pro Max (RV1106, MIS5001)
# Replaces the factory rkipc launcher. Called by /usr/bin/t on boot via S99test.

program_kill() {
    local pids
    pids=$(ps | grep "$1" | grep -v grep | awk '{print $1}')
    [ -z "$pids" ] || kill -9 $pids 2>/dev/null
}

stop_conflicting() {
    # Kill ISP-using factory applications
    program_kill rkipc
    program_kill rkisp_demo
    program_kill rk_mpi_uvc
    # Kill the usb_reset watcher spawned by S99usb0config — it calls
    # "S50usbdevice restart" on USB disconnect which wipes our UVC gadget config
    program_kill usb_reset
    sleep 0.5
}

setup_env() {
    export PATH=/oem/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH

    local ko_dir=/oem/usr/ko
    [ -d "$ko_dir" ] && [ -f "$ko_dir/insmod_ko.sh" ] && \
        cd "$ko_dir" && sh insmod_ko.sh 2>/dev/null && cd -

    # Prefer /tmp copy of ini so runtime edits survive watchdog restarts
    local ini=/tmp/rkuvc.ini
    if [ ! -f "$ini" ]; then
        for src in /oem/usr/share/rkuvc.ini /usr/share/rkuvc.ini; do
            [ -f "$src" ] && cp "$src" "$ini" && break
        done
    fi

    export rt_log_level=3
    export rk_mpi_uvc_log_level=2
    touch /tmp/uvc_no_timeout
    ifconfig lo 127.0.0.1 2>/dev/null || true
}

setup_usb() {
    # Detach gadget from UDC before reconfiguring. Without this, the umount in
    # usb_config.sh fails (gadget still active), leaving configfs mounted with
    # the old config — then all mkdir/write calls fail with "Device or resource busy".
    echo '' > /sys/kernel/config/usb_gadget/rockchip/UDC 2>/dev/null || true
    /oem/usr/bin/usb_config.sh
}

start_uvc() {
    if [ -d "/oem/usr/share/iqfiles" ]; then
        /oem/usr/bin/rk_mpi_uvc -c /tmp/rkuvc.ini -a /oem/usr/share/iqfiles
    else
        /oem/usr/bin/rk_mpi_uvc
    fi
}

ulimit -c unlimited
echo "/data/core-%p-%e" > /proc/sys/kernel/core_pattern 2>/dev/null || true

stop_conflicting
setup_env
setup_usb

# Foreground watchdog: restart rk_mpi_uvc whenever it exits
while true; do
    start_uvc
    echo "RkLunch: rk_mpi_uvc exited (code=$?) — restarting in 2s..."
    sleep 2
done
