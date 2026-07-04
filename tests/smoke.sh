#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root}"

provider_pid=""
vhal_pid=""
cleanup() {
    [[ -z "${vhal_pid}" ]] || kill "${vhal_pid}" 2>/dev/null || true
    [[ -z "${provider_pid}" ]] || kill "${provider_pid}" 2>/dev/null || true
    rm -f /tmp/virtual-sdv-vhal.sock
}
trap cleanup EXIT

./build/qnx_provider_sim > /tmp/virtual-sdv-provider.log 2>&1 &
provider_pid=$!
./build/linux_vhal_stub > /tmp/virtual-sdv-vhal.log 2>&1 &
vhal_pid=$!

for _ in $(seq 1 30); do
    if ./build/vehicle_client health > /tmp/virtual-sdv-health.out 2>/dev/null; then break; fi
    sleep 0.1
done

grep -q 'HEALTHY provider_connected=true' /tmp/virtual-sdv-health.out
./build/vehicle_client configs | grep -q 'PERF_VEHICLE_SPEED id=0x11600207'
./build/vehicle_client get PERF_VEHICLE_SPEED | grep -q 'status=VEHICLE_PROPERTY_STATUS_AVAILABLE'
./build/vehicle_client set HVAC_TEMPERATURE_SET 23.5 | grep -q '^STATUS_OK:'
sleep 0.2
./build/vehicle_client get HVAC_TEMPERATURE_SET | grep -q 'value=23.5'

set +e
timeout 2 stdbuf -oL ./build/vehicle_client subscribe PERF_VEHICLE_SPEED > /tmp/virtual-sdv-subscribe.out
subscribe_status=$?
set -e
[[ ${subscribe_status} -eq 0 || ${subscribe_status} -eq 124 ]]
grep -q 'PERF_VEHICLE_SPEED' /tmp/virtual-sdv-subscribe.out

echo "Linux VHAL smoke test passed"
