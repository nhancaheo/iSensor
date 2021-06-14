/*
 * BH1750 Component
 *
 *
 * 
 *
 * 
 */

 
 // Error library
#include "esp_err.h"
 
// I2C driver
#include "driver/i2c.h"

 
#ifndef __ESP_BH1750_H__
#define __ESP_BH1750_H__

#define BH1750_ADDR 0x23   /*!< slave address for BH1750 sensor */
#define BH1750_CMD_START 0x23   /*!< Operation mode */

// variables
i2c_port_t _port;

esp_err_t bh1750_init(i2c_port_t port);
float bh1750_read_lux();

#endif  // __ESP_BH1750_H__