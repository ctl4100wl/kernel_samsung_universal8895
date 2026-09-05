#!/system/bin/sh
# Read-only Note8/Exynos8895 clock and limit snapshots; no sysfs writes.
# Run as root while reproducing the workload: sh exynos-oc-capture.sh 60
samples=${1:-60}
case "$samples" in ''|*[!0-9]*) echo 'Expected a positive sample count' >&2; exit 1;; esac
[ "$samples" -gt 0 ] || exit 1
read_node() {
    [ -r "$1" ] || return 0
    printf '%s: ' "$1"
    cat "$1"
}
uname -a
for domain in little big; do
    for item in stock_max_freq hw_max_freq oc_max_freq max_freq available_freqs volt_table; do
        read_node "/sys/kernel/exynos_oc/${domain}_${item}"
    done
done
for cpu in 0 4; do
    read_node "/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_available_frequencies"
    read_node "/sys/devices/system/cpu/cpu${cpu}/cpufreq/stats/time_in_state"
done
i=0
while [ "$i" -lt "$samples" ]; do
    printf '\n--- sample %s ---\n' "$i"
    read_node /proc/uptime
    read_node /sys/devices/system/cpu/online
    for cpu in 0 4; do
        for item in scaling_governor scaling_min_freq scaling_max_freq scaling_cur_freq cpuinfo_cur_freq clock_status; do
            read_node "/sys/devices/system/cpu/cpu${cpu}/cpufreq/$item"
        done
    done
    read_node /sys/power/cpufreq_max_limit
    for cluster in 0 1; do
        read_node "/sys/kernel/debug/pm_qos/cluster${cluster}_freq_max"
    done
    for zone in /sys/class/thermal/thermal_zone*; do
        read_node "$zone/type"
        read_node "$zone/temp"
    done
    for cooling in /sys/class/thermal/cooling_device*; do
        read_node "$cooling/type"
        read_node "$cooling/cur_state"
    done
    i=$((i + 1))
    sleep 1
done
for cpu in 0 4; do
    read_node "/sys/devices/system/cpu/cpu${cpu}/cpufreq/stats/time_in_state"
done
