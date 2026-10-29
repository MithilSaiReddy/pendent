#include <zephyr/sys/ring_buffer.h>
#include "codec.h"
#include "audio.h"
#include "config.h"
#include "utils.h"
#ifdef CODEC_OPUS
#include "lib/opus-1.2.1/opus.h"
#endif

//
// Output
//

static volatile codec_callback _callback = NULL;

void set_codec_callback(codec_callback callback)
{
    _callback = callback;
}

//
// Input
//

uint8_t codec_ring_buffer_data[AUDIO_BUFFER_SAMPLES * 2]; // 2 bytes per sample
struct ring_buf codec_ring_buf;

int codec_receive_pcm(int16_t *data, size_t len)
{
    int written = ring_buf_put(&codec_ring_buf, (uint8_t *)data, len * 2);
    if (written != len * 2)
    {
        printk("Failed to write %d bytes to codec ring buffer\n", len * 2);
        return -1;
    }
    return 0;
}

//
// Thread
//

int16_t codec_input_samples[CODEC_PACKAGE_SAMPLES];
uint8_t codec_output_bytes[CODEC_OUTPUT_MAX_BYTES];
K_THREAD_STACK_DEFINE(codec_stack, 32000);
static struct k_thread codec_thread;
uint16_t execute_codec();

#if CODEC_OPUS
#if (CONFIG_OPUS_MODE == CONFIG_OPUS_MODE_CELT)
#define OPUS_ENCODER_SIZE 7180
#endif
#if (CONFIG_OPUS_MODE == CONFIG_OPUS_MODE_HYBRID)
#define OPUS_ENCODER_SIZE 10916
#endif
__ALIGN(4)
static uint8_t m_opus_encoder[OPUS_ENCODER_SIZE];
static OpusEncoder *const m_opus_state = (OpusEncoder *)m_opus_encoder;
#endif

static volatile int64_t last_voice_ms = 0;

static bool frame_has_voice(int16_t *samples, size_t n)
{
    // RMS energy check: less sensitive to single-sample spikes than a
    // pure peak threshold. Compares mean square against threshold^2 to
    // avoid sqrt() on the MCU.
    int64_t sum_sq = 0;
    for (size_t i = 0; i < n; i++)
    {
        int32_t s = samples[i];
        sum_sq += (int64_t)s * (int64_t)s;
    }
    int64_t mean_sq = sum_sq / (int64_t)n;
    int64_t thresh_sq = (int64_t)VOICE_RMS_THRESHOLD * (int64_t)VOICE_RMS_THRESHOLD;
    return mean_sq > thresh_sq;
}

bool voice_is_active(void)
{
    if (last_voice_ms == 0)
    {
        return false;
    }
    return (k_uptime_get() - last_voice_ms) <= VOICE_HANGOVER_MS;
}

void codec_entry()
{
    uint16_t output_size;
    while (1)
    {

        // Check if we have enough data
        if (ring_buf_size_get(&codec_ring_buf) < CODEC_PACKAGE_SAMPLES * 2)
        {
            k_sleep(K_MSEC(10));
            continue;
        }

        // Read package
        ring_buf_get(&codec_ring_buf, (uint8_t *)codec_input_samples, CODEC_PACKAGE_SAMPLES * 2);

        // Voice activity check before encoding
        if (frame_has_voice(codec_input_samples, CODEC_PACKAGE_SAMPLES))
        {
            last_voice_ms = k_uptime_get();
        }

        // Run Codec
        output_size = execute_codec();

        // Notify
        if (_callback)
        {
            _callback(codec_output_bytes, output_size);
        }

        // Yield
        k_yield();
    }
}

int codec_start()
{
    
// OPUS
#if CODEC_OPUS
    ASSERT_TRUE(opus_encoder_get_size(1) == sizeof(m_opus_encoder));
    ASSERT_TRUE(opus_encoder_init(m_opus_state, 16000, 1, CODEC_OPUS_APPLICATION) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_BITRATE(CODEC_OPUS_BITRATE)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_VBR(CODEC_OPUS_VBR)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_VBR_CONSTRAINT(0)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_COMPLEXITY(CODEC_OPUS_COMPLEXITY)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_LSB_DEPTH(16)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_DTX(0)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_INBAND_FEC(0)) == OPUS_OK);
    ASSERT_TRUE(opus_encoder_ctl(m_opus_state, OPUS_SET_PACKET_LOSS_PERC(0)) == OPUS_OK);
#endif

    // Thread
    ring_buf_init(&codec_ring_buf, sizeof(codec_ring_buffer_data), codec_ring_buffer_data);
    k_thread_create(&codec_thread, codec_stack, K_THREAD_STACK_SIZEOF(codec_stack), (k_thread_entry_t)codec_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);

    // Success
    return 0;
}

//
// MU-Law codec
//

#if CODEC_MU_LAW

uint16_t execute_codec()
{
    for (int i = 0; i < CODEC_PACKAGE_SAMPLES / CODEC_DIVIDER; i++)
    {
        codec_output_bytes[i] = linear2ulaw(codec_input_samples[i * CODEC_DIVIDER]);
    }
    return CODEC_PACKAGE_SAMPLES / CODEC_DIVIDER;
}

#endif

//
// PCM codec
//

#if CODEC_PCM

uint16_t execute_codec()
{
    for (int i = 0; i < CODEC_PACKAGE_SAMPLES / CODEC_DIVIDER; i++)
    {
        codec_output_bytes[i * 2] = codec_input_samples[i * CODEC_DIVIDER] & 0xFF;
        codec_output_bytes[i * 2 + 1] = (codec_input_samples[i * CODEC_DIVIDER] >> 8) & 0xFF;
    }
    return (CODEC_PACKAGE_SAMPLES / CODEC_DIVIDER) * 2;
}
#endif

//
// Opus codec
//

#if CODEC_OPUS

uint16_t execute_codec()
{
    opus_int32 size = opus_encode(m_opus_state, codec_input_samples, CODEC_PACKAGE_SAMPLES, codec_output_bytes, sizeof(codec_output_bytes));
    if (size < 0)
    {
        printk("Opus encoding failed: %d\n", size);
        return 0;
    }
    // printk("Opus encoding success: %i\n", size);
    return size;
}

#endif