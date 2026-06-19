#!/usr/bin/env bash

set -euo pipefail

OTBR_SERVICE="${OTBR_SERVICE:-otbr-agent}"
OTBR_BACKBONE_IF="${OTBR_BACKBONE_IF:-wlp2s0}"
OTBR_THREAD_IF="${OTBR_THREAD_IF:-wpan0}"
OTBR_RADIO_DEV="${OTBR_RADIO_DEV:-}"
OTBR_RADIO_URL="${OTBR_RADIO_URL:-}"

find_radio_dev() {
    local matches=()

    if [[ -n "${OTBR_RADIO_DEV}" ]]; then
        printf '%s\n' "${OTBR_RADIO_DEV}"
        return 0
    fi

    while IFS= read -r line; do
        matches+=("${line}")
    done < <(compgen -G "/dev/serial/by-id/*Espressif*if00" || true)

    if [[ "${#matches[@]}" -eq 1 ]]; then
        printf '%s\n' "${matches[0]}"
        return 0
    fi

    if [[ "${#matches[@]}" -gt 1 ]]; then
        printf 'Multiple Espressif serial candidates were found:\n' >&2
        printf ' - %s\n' "${matches[@]}" >&2
        printf 'Set OTBR_RADIO_DEV explicitly before rerunning.\n' >&2
        return 1
    fi

    printf 'No Espressif serial-by-id port was found.\n' >&2
    printf 'Check the USB cable, hub power, and that the mini board is connected.\n' >&2
    return 1
}

build_radio_url() {
    if [[ -n "${OTBR_RADIO_URL}" ]]; then
        printf '%s\n' "${OTBR_RADIO_URL}"
        return 0
    fi

    local radio_dev
    radio_dev="$(find_radio_dev)"
    printf 'spinel+hdlc+uart://%s\n' "${radio_dev}"
}

print_header() {
    printf '\n== %s ==\n' "$1"
}

show_status() {
    print_header "OTBR service"
    systemctl status "${OTBR_SERVICE}" --no-pager -l || true

    print_header "OTBR session"
    ot-ctl state || true

    print_header "Known addresses"
    ot-ctl ipaddr || true
}

show_port_usage() {
    local radio_dev
    radio_dev="$(find_radio_dev)"

    print_header "Radio device"
    printf '%s\n' "${radio_dev}"

    if command -v fuser >/dev/null 2>&1; then
        print_header "Processes holding the radio port"
        fuser -v "${radio_dev}" || true
    else
        printf 'fuser is not installed; skipping port ownership check.\n'
    fi
}

show_recommendation() {
    local radio_url
    radio_url="$(build_radio_url)"

    print_header "Recommended /etc/default/otbr-agent"
    printf 'OTBR_AGENT_OPTS="-I %s -B %s %s trel://%s"\n' \
        "${OTBR_THREAD_IF}" \
        "${OTBR_BACKBONE_IF}" \
        "${radio_url}" \
        "${OTBR_BACKBONE_IF}"
    printf 'OTBR_NO_AUTO_ATTACH=0\n'
}

recover_otbr() {
    local radio_dev
    local radio_url

    radio_dev="$(find_radio_dev)"
    radio_url="$(build_radio_url)"

    print_header "Pre-flight"
    printf 'Radio device: %s\n' "${radio_dev}"
    printf 'Radio URL:    %s\n' "${radio_url}"

    if command -v fuser >/dev/null 2>&1; then
        local owners
        owners="$(fuser "${radio_dev}" 2>/dev/null || true)"
        if [[ -n "${owners}" ]]; then
            printf 'Warning: the radio port is busy.\n' >&2
            printf 'Close any `idf.py monitor`, `screen`, `picocom`, or serial terminal using %s first.\n' "${radio_dev}" >&2
            fuser -v "${radio_dev}" || true
            return 1
        fi
    fi

    print_header "Restarting service"
    systemctl restart "${OTBR_SERVICE}"
    sleep 3

    print_header "Validating session"
    if ot-ctl state >/tmp/otbr_state.out 2>/tmp/otbr_state.err; then
        cat /tmp/otbr_state.out
        rm -f /tmp/otbr_state.out /tmp/otbr_state.err
        return 0
    fi

    cat /tmp/otbr_state.err >&2 || true
    rm -f /tmp/otbr_state.out /tmp/otbr_state.err

    print_header "Recent journal"
    journalctl -u "${OTBR_SERVICE}" -n 40 --no-pager -l || true

    printf '\nRecovery failed. Most common causes:\n' >&2
    printf ' 1. The mini board is not exposing the expected USB serial port.\n' >&2
    printf ' 2. Another program still owns the serial/JTAG port.\n' >&2
    printf ' 3. The board is running monitor/flash mode instead of idle RCP mode.\n' >&2
    printf ' 4. The USB hub is under-powering the radio board.\n' >&2
    return 1
}

usage() {
    cat <<'EOF'
Usage:
  sudo tools/otbr_resilience.sh status
  sudo tools/otbr_resilience.sh port
  sudo tools/otbr_resilience.sh recommend
  sudo tools/otbr_resilience.sh recover

Notes:
  - Prefer /dev/serial/by-id over /dev/ttyACM*.
  - Do not keep `idf.py monitor` attached to the mini OTBR board while otbr-agent is running.
  - If you move boards across ports, this script helps verify the current radio device.
EOF
}

main() {
    local cmd="${1:-status}"
    case "${cmd}" in
        status)
            show_status
            ;;
        port)
            show_port_usage
            ;;
        recommend)
            show_recommendation
            ;;
        recover)
            recover_otbr
            ;;
        *)
            usage
            exit 1
            ;;
    esac
}

main "$@"
