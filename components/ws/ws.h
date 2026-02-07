#pragma once

void send_sensor_data(float temp, float humi);
void send_record_file(const char* filepath);
void ws_init(void);