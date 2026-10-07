#include <zephyr/kernel.h>
#include "transport.h"
#include "mic.h"
#include "utils.h"
#include "led.h"
#include "config.h"
#include "audio.h"
#include "codec.h"

static void codec_handler(uint8_t *data, size_t len)
{
	broadcast_audio_packets(data, len); // Errors are logged inside
}

static void mic_handler(int16_t *buffer)
{
	codec_receive_pcm(buffer, MIC_BUFFER_SAMPLES); // Errors are logged inside
}

void bt_ctlr_assert_handle(char *name, int type)
{
	if (name != NULL)
	{
		printk("Bt assert-> %s", name);
	}
}

bool is_connected = false;

void set_led_state()
{
	// Disconnected - RED
	if (!is_connected)
	{
		set_led_red(true);
		set_led_green(false);
		set_led_blue(false);
		return;
	}

	// Connected and talking - GREEN
	if (transport_is_streaming() && voice_is_active())
	{
		set_led_red(false);
		set_led_green(true);
		set_led_blue(false);
		return;
	}

	// Connected but silent / not listening - BLUE
	set_led_red(false);
	set_led_green(false);
	set_led_blue(true);
}

// Main loop
int main(void)
{
	// Led start
	ASSERT_OK(led_start());
	set_led_blue(true);

	// Transport start
	ASSERT_OK(transport_start());

	// Codec start
	set_codec_callback(codec_handler);
	ASSERT_OK(codec_start());

	// Mic start
	set_mic_callback(mic_handler);
	ASSERT_OK(mic_start());

	while (1)
	{
		set_led_state();
		k_msleep(500);
	}

	// Unreachable
	return 0;
}
