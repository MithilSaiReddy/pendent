#pragma once
#include <zephyr/kernel.h>
#include <stdbool.h>
int transport_start();
int broadcast_audio_packets(uint8_t *buffer, size_t size);
bool transport_is_streaming(void);
