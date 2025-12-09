#ifndef GAP_H
#define GAP_H

/* Includes */
/* NimBLE GAP APIs */
#include <services/gap/ble_svc_gap.h>
#include "common.h"

/* Defines */
#define BLE_GAP_APPEARANCE_SENSOR 0x0540
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/* Public function declarations */
void adv_init(void);
int gap_init(void);
void adv_update_sensor_data(weather_data_t *sensor_data);
void adv_stop(void);

#endif // GAP_H
