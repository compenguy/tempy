# Matter Temperature/Humidity Sensor

## Sensors

This application uses the BME280 sensor as a Matter-enabled temperature and
humidity source. Readings can also be broadcast as manufacturer-specific data
in a BLE advertisement for non-Matter receivers on the same network; this
side-channel is controlled by the `ENABLE_BLE_BEACON` option in menuconfig
(enabled by default).

This application creates the temperature sensor on endpoint 1, and the
humidity sensor on endpoint 2.

See the [docs](https://docs.espressif.com/projects/esp-matter/en/latest/esp32/developing.html)
for more information about building and flashing the firmware.

### Connecting the sensors

- Connecting the BME280, temperature and humidity sensor

| ESP32-H2 Pin | BME280 Pin |
|--------------|------------|
| GND          | GND        |
| 3V3          | VCC        |
| GPIO 13      | SDA        |
| GPIO 14      | SCL        |

**_NOTE:_**
- Above mentioned wiring connection is configured by default in the Kconfig
- Ensure that the GPIO pins used for the sensors are correctly configured through menuconfig.
- Modify the configuration parameters as needed for your specific hardware setup.

The BME280 module used with this project includes 10k pullups on CSB, SDI, and
SCK, plus a 10k pulldown on SDO. In I2C mode, SDI is SDA and SCK is SCL, so the
module already provides pullups for the I2C signal lines. Additional external
pullups are typically not needed for short wiring to a single sensor, but may be
useful if the bus has longer leads, more devices, or unreliable edges.

The SDO pulldown straps the BME280 I2C address to `0x76`, which is the default
configured by this project. If using a different module or tying SDO high,
change the BME280 I2C address in menuconfig to `0x77`.

Be aware that IO10-14, and IO22 are generally the safest to use as the others are typically
involved in flash communication, USB I/O, UART serial console, or function as strapping pins during
boot, but consult your board/chip documentation to be sure.

[This page](https://www.espboards.dev/esp32/esp32-h2-super-mini/) has a useful reference for the
ESP32-H2 Super Mini.

## Power Management

This project borrows from the esp-matter icd_app example, especially the esp32h2.lit
configuration to configure long idle time (LIT) features to reduce power usage.

Additional ideas for power reduction are discussed [here](https://tomasmcguinness.com/2025/08/29/matter-low-power-on-an-esp32-h2/).

## Building

Prerequisites:
* python3-venv (esp-idf prereq)
* esp-idf installed

Use the included go.sh script to configure, build, flash, or monitor the device over USB.

Examples:

```
$ ./go.sh --build
[build output]
$ ./go.sh --flash --monitor
[flashing output followed by the serial debug monitor tool]
```

## Usage

Commission the app using a Matter controller and read the attributes.

The ESP32-H2 is a Thread device, so it must be commissioned onto a Thread
network. The example below uses chip-tool to commission over BLE-to-Thread and
then subscribe to the sensor attributes.

```
# Commission over BLE onto an existing Thread network. Replace
# <hex-dataset> with the operational dataset of your Thread network
# (chip-tool otbr get-active-dataset-tlvs, or from your Border Router).
chip-tool pairing ble-thread 1 hex:<hex-dataset> 20202021 3840

# Start chip-tool in interactive mode
chip-tool interactive start

# Subscribe to attributes (node 1, min 3s, max 10s, endpoints 1 and 2)
> temperaturemeasurement subscribe measured-value 3 10 1 1
> relativehumiditymeasurement subscribe measured-value 3 10 1 2
```
