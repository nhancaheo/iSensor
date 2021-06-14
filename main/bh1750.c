/*
 * BH1750 Component
 *
 *
 *
 *
 * Luca Dentella, www.lucadentella.it
 */
 
 
// Component header file
#include "bh1750.h"

esp_err_t bh1750_init(i2c_port_t port) {
	_port = port;

    int ret;
	
	// verify if a sensor is present
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (BH1750_ADDR << 1) | I2C_MASTER_WRITE, true);
	i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(port, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

	return ret;
}

float bh1750_read_lux() {
    int ret;
    uint8_t data_h, data_l;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, BH1750_ADDR << 1 | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, BH1750_CMD_START, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(_port, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) {
        return -999;
    }

    vTaskDelay(30 / portTICK_RATE_MS);
    
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, BH1750_ADDR << 1 | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data_h, 0x0);
    i2c_master_read_byte(cmd, &data_l, 0x1);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(_port, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);

    return (data_h << 8 | data_l) / 1.2;
}