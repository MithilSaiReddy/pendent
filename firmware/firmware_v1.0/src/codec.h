#pragma once
#include <zephyr/kernel.h>
#include <stdbool.h>

// Callback
typedef void (*codec_callback)(uint8_t *data, size_t len);
void set_codec_callback(codec_callback callback);

// Integration
int codec_receive_pcm(int16_t *data, size_t len);
int codec_start();
bool voice_is_active(void);