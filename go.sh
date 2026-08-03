#!/bin/bash
set -e

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
SCRIPT_NAME="$(basename "$0")"

die() { echo "$*" 1>&2; exit 1; }
log() { echo "TEMPY-BUILD: $*"; }

ESP_IDF_DIR="${HOME}/.local/share/esp/esp-idf"
ESP_IDF_VERSION="v5.5.1"
ESP_MATTER_DIR="${HOME}/.local/share/matter/esp-matter"
ESP_TARGET="esp32h2"
ESP_SERIAL="/dev/ttyACM0"
ESP_SERIAL_ARG=()
[ -n "${ESP_SERIAL}" ] && ESP_SERIAL_ARG=(-p "${ESP_SERIAL}")

function init_esp_idf() {
	(
		if ! [ -d "${ESP_IDF_DIR}" ] || ! [ -d "${ESP_IDF_DIR}"/.git ] ; then
			log "Cloning esp-idf repository..."
			mkdir -p "${ESP_IDF_DIR}"/..
		 	cd "${ESP_IDF_DIR}"/..
			git clone -b "${ESP_IDF_VERSION}" --recursive https://github.com/espressif/esp-idf.git
		 	cd "${ESP_IDF_DIR}"/
		fi
		cd "${ESP_IDF_DIR}"/
		log "Refreshing esp-idf submodules..."
		git submodule update --init --depth 1
		log "Refreshing esp-idf components..."
		./install.sh
		log "Setting esp-idf target device..."
		source "${ESP_IDF_DIR}"/export.sh
		idf.py set-target "${ESP_TARGET}"
	)
}

function activate_esp_idf() {
	#init_esp_idf
	log "Activating esp-idf..."
	source "${ESP_IDF_DIR}"/export.sh
}

function init_esp_matter() {
	(
		if ! [ -d "${ESP_MATTER_DIR}" ] || ! [ -d "${ESP_MATTER_DIR}"/.git ] ; then
			log "Cloning esp-matter repository..."
			mkdir -p "${ESP_MATTER_DIR}"/..
			cd "${ESP_MATTER_DIR}"/..
			git clone --depth 1 https://github.com/espressif/esp-matter.git
			cd "${ESP_MATTER_DIR}"
		fi
		cd "${ESP_MATTER_DIR}"
		log "Refreshing esp-matter submodules..."
		git submodule update --init --depth 1
		log "Refreshing CHIP submodules..."
		cd ./connectedhomeip/connectedhomeip
		./scripts/checkout_submodules.py --platform esp32 linux --shallow
		cd ../..
		log "Refreshing esp-matter components..."
		./install.sh
	)
}

function activate_esp_matter() {
	#init_esp_matter
	log "Activating esp-matter..."
	source "${ESP_MATTER_DIR}"/export.sh
}

# Default values for command-line arguments
ARG_CONFIG=0
ARG_RECONFIG=0
ARG_BUILD=0
ARG_FLASH=0
ARG_FULLCLEAN=0
ARG_FULL_FLASH=0
ARG_MONITOR=0

# Argument parsing
usage() {
cat << EOF
Usage: ${SCRIPT_NAME} [config-args] [build-args] [program-args] [debug-args]
General Arguments:
  -h, --help		explanation of command line argument and environment
            		variable options
Configuration Arguments:
      --full-clean	invoke idf.py fullclean
  -c, --menuconfig	invoke idf.py menuconfig
  -r, --reconfigure	invoke idf.py reconfigure
Build Arguments:
  -b, --build		invoke idf.py to build the specified target
Programming Arguments:
  -F, --full-flash	wipes out the entire flash and starts fresh (\`erase-flash\`)
  -f, --flash		write the built firmware to flash
Debugging Arguments:
  -m, --monitor		starts serial debug montoring of device via the USB serial interface (ESP_SERIAL=${ESP_SERIAL})
EOF
}

while [[ $# -gt 0 ]]; do
	case $1 in
		-b|--build)
			shift
			ARG_BUILD=1
			;;
		-c|--menuconfig)
			shift
			ARG_CONFIG=1
			;;
		-r|--reconfigure)
			shift
			ARG_RECONFIG=1
			;;
		-f|--flash)
			shift
			ARG_FLASH=1
			;;
		--full-clean)
			shift
			ARG_FULLCLEAN=1
			;;
		-F|--full-flash)
			shift
			ARG_FULL_FLASH=1
			;;
		-m|--monitor)
			shift
			ARG_MONITOR=1
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			usage
			echo ""
			die "Unrecognized argument '$1'"
			;;
	esac
done

# Task tracking variables
CONFIG_ACTIVITY=0
RECONFIG_ACTIVITY=0
BUILD_ACTIVITY=0
FLASH_ACTIVITY=0
MONITOR_ACTIVITY=0

activate_esp_idf
activate_esp_matter

# Perform the requested work
echo "Task(s) starting at $(date --rfc-3339=seconds)"
echo "Configured build settings:"
echo "    ESP target=${ESP_TARGET}"
echo "    esp-idf version=${ESP_IDF_VERSION}"
if [ "${ARG_FULLCLEAN}" -ne 0 ]; then
	log "Cleaning project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py fullclean
	)
	CONFIG_ACTIVITY=1
fi
if [ "${ARG_CONFIG}" -ne 0 ]; then
	log "Configuring project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py menuconfig
	)
	CONFIG_ACTIVITY=1
fi
if [ "${ARG_RECONFIG}" -ne 0 ]; then
	log "Reconfiguring project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py reconfigure
	)
	RECONFIG_ACTIVITY=1
fi
if [ "${ARG_BUILD}" -ne 0 ]; then
	log "Building project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py build
	)
	BUILD_ACTIVITY=1
fi
if [ "${ARG_FLASH}" -ne 0 ]; then
	log "Flashing project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py "${ESP_SERIAL_ARG[@]}" flash
	)
	FLASH_ACTIVITY=1
fi
if [ "${ARG_FULL_FLASH}" -ne 0 ]; then
	log "Full-storage flashing project..."
	(
		cd "${SCRIPT_DIR}"
		idf.py "${ESP_SERIAL_ARG[@]}" erase-flash
		idf.py "${ESP_SERIAL_ARG[@]}" flash
	)
	FLASH_ACTIVITY=1
fi
if [ "${ARG_MONITOR}" -ne 0 ]; then
	log "Monitoring device..."
	(
		cd "${SCRIPT_DIR}"
		idf.py "${ESP_SERIAL_ARG[@]}" monitor
	)
	MONITOR_ACTIVITY=1
fi

# Wrap-up
if [[ ${CONFIG_ACTIVITY} -eq 0 ]] && [[ ${RECONFIG_ACTIVITY} -eq 0 ]] && [[ ${BUILD_ACTIVITY} -eq 0 ]] && [[ ${FLASH_ACTIVITY} -eq 0 ]] && [[ ${MONITOR_ACTIVITY} -eq 0 ]]; then
	usage
	echo ""
	echo "Nothing to do! Did you mean to specify --menuconfig, --build, --flash, or --monitor?"
else
	echo "Task(s) completed at $(date --rfc-3339=seconds)"
	echo "Configured build settings:"
	echo "    ESP target=${ESP_TARGET}"
	echo "    esp-idf version=${ESP_IDF_VERSION}"
fi

