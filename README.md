# Matter Temperature Sensor

## Sensors

This application uses the BME280 sensor function as a matter-enabled temperature and humidity source.

This application creates the temperature sensor on endpoint 1, and humidity sensor on endpoint 2.

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

**_NOTE:_**:
- Above mentioned wiring connection is configured by default in the Kconfig
- Ensure that the GPIO pins used for the sensors are correctly configured through menuconfig.
- Modify the configuration parameters as needed for your specific hardware setup.

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

Use the included go.sh script to configure, build, flash, or monitor the device over USB.

Examples:

```
$ ./go.sh --build
[build output]
$ ./go.sh --flash --monitor
[flashing output followed by the serial debug monitor tool]
```

## Usage

- Commission the app using Matter controller and read the attributes.

Below, we are using chip-tool to commission and subscribe the sensor attributes.
- 
```
# Commission
chip-tool pairing ble-wifi 1 (SSID) (PASSPHRASE) 20202021 3840

# Start chip-tool in interactive mode
chip-tool interactive start

# Subscribe to attributes
> temperaturemeasurement subscribe measured-value 3 10 1 1
> relativehumiditymeasurement subscribe measured-value 3 10 1 2
> occupancysensing subscribe occupancy 3 10 1 3
```
