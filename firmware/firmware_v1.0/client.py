import asyncio
import bleak
from bleak import BleakClient
import wave
from datetime import datetime
import os

DEVICE_NAME = "Tanu"
SERVICE_UUID = "19B10000-E8F2-537E-4F6C-D104768A1214"
CHARACTERISTIC_UUID = "19B10001-E8F2-537E-4F6C-D104768A1214"

CODEC = "opus"  # "opus" (firmware default, ID 20), "pcm" or "mulaw" for legacy firmware
SAMPLE_RATE = 16000  # Firmware encodes at 16 kHz
SAMPLE_WIDTH = 2  # 16-bit audio
CHANNELS = 1  # Mono audio
CAPTURE_TIME = 10  # Time to capture audio in seconds
OPUS_FRAME_SAMPLES = 160  # 10 ms @ 16 kHz

try:
    from opuslib import Decoder as OpusDecoder
    HAS_OPUSLIB = True
except ImportError:
    HAS_OPUSLIB = False


def ulaw2linear(ulaw_byte):
    """Convert a mu-law byte to a 16-bit linear PCM value."""
    EXPONENT_LUT = [0, 132, 396, 924, 1980, 4092, 8316, 16764]
    ulaw_byte = ~ulaw_byte
    sign = (ulaw_byte & 0x80)
    exponent = (ulaw_byte >> 4) & 0x07
    mantissa = ulaw_byte & 0x0F
    sample = EXPONENT_LUT[exponent] + (mantissa << (exponent + 3))
    if sign != 0:
        sample = -sample
    return sample


class FrameReassembler:
    """Reassemble codec frames from BLE notifications.

    Notify layout (see src/transport.c): [frame_id_lo, frame_id_hi,
    frag_index, payload...]. A new codec frame always starts with
    frag_index 0, possibly split across fragments with index 1, 2, ...
    """

    def __init__(self):
        self.frames = []
        self.current = bytearray()
        self.expected_index = 0
        self.started = False
        self.lost_fragments = 0
        self.lost_frames = 0

    def feed(self, data):
        if len(data) < 3:
            self.lost_fragments += 1
            return
        frag_index = data[2]
        payload = data[3:]
        if frag_index == 0:
            if self.started and len(self.current) > 0:
                self.frames.append(bytes(self.current))
            self.current = bytearray(payload)
            self.expected_index = 1
            self.started = True
        else:
            if not self.started:
                self.lost_frames += 1
                return
            if frag_index != self.expected_index:
                self.lost_fragments += 1
            self.current.extend(payload)
            self.expected_index = frag_index + 1

    def flush(self):
        if self.started and len(self.current) > 0:
            self.frames.append(bytes(self.current))
        self.current = bytearray()
        self.started = False
        self.expected_index = 0


def decode_opus_frames(frames):
    if not HAS_OPUSLIB:
        return None
    decoder = OpusDecoder(SAMPLE_RATE, CHANNELS)
    pcm = bytearray()
    for frame in frames:
        try:
            pcm.extend(decoder.decode(frame, OPUS_FRAME_SAMPLES))
        except Exception as e:
            print(f"Opus decode failed for {len(frame)}-byte frame: {e}")
            return None
    return bytes(pcm)


def export_wav(pcm_bytes, prefix="recording"):
    recordings_dir = "recordings"
    os.makedirs(recordings_dir, exist_ok=True)
    filename = os.path.join(recordings_dir, datetime.now().strftime("%H-%M-%S-%f") + ".wav")
    with wave.open(filename, "wb") as wav_file:
        wav_file.setnchannels(CHANNELS)
        wav_file.setsampwidth(SAMPLE_WIDTH)
        wav_file.setframerate(SAMPLE_RATE)
        wav_file.writeframes(pcm_bytes)
    print(f"Wrote {filename} ({len(pcm_bytes)} bytes PCM)")
    return filename


def export_opus_frames(frames, prefix="recording"):
    recordings_dir = "recordings"
    os.makedirs(recordings_dir, exist_ok=True)
    filename = os.path.join(recordings_dir, datetime.now().strftime("%H-%M-%S-%f") + ".opus.frames")
    with open(filename, "wb") as f:
        for frame in frames:
            f.write(len(frame).to_bytes(2, "little"))
            f.write(frame)
    print(f"Wrote {filename} ({len(frames)} Opus frames, install 'opuslib' to decode to WAV)")
    return filename


async def main():
    print("Discovering Tanu...")
    devices = await bleak.discover(timeout=2.0)
    audio_recorder = None
    for device in devices:
        if device.name:
            print(device.name, device.address)
        if device.name == DEVICE_NAME:
            audio_recorder = device
            break

    if not audio_recorder:
        print("Device not found")
        return

    def handle_ble_disconnect(client):
        print("Disconnected from device")

    async with BleakClient(audio_recorder.address, services=[SERVICE_UUID],
                           disconnect_callback=handle_ble_disconnect) as client:
        print("Connected to device")
        services = await client.get_services()
        audio_service = services.get_service(SERVICE_UUID)
        audio_characteristic = audio_service.get_characteristic(CHARACTERISTIC_UUID)

        reassembler = FrameReassembler()

        def handle_audio_data(sender, data):
            reassembler.feed(bytes(data))

        async def record_audio():
            reassembler.frames.clear()
            reassembler.flush()
            await client.start_notify(audio_characteristic.uuid, handle_audio_data)
            print("Recording audio...")
            await asyncio.sleep(CAPTURE_TIME)
            await client.stop_notify(audio_characteristic.uuid)
            reassembler.flush()
            print(f"Recording stopped: {len(reassembler.frames)} frames, "
                  f"lost fragments={reassembler.lost_fragments}")

        async def record_and_process():
            while True:
                await record_audio()
                frames = list(reassembler.frames)
                reassembler.frames.clear()
                if not frames:
                    print("Warning: no audio frames received.")
                    continue
                if CODEC == "opus":
                    pcm = decode_opus_frames(frames)
                    if pcm is not None:
                        export_wav(pcm)
                    else:
                        if not HAS_OPUSLIB:
                            print("opuslib not installed: saving raw Opus frames. pip install opuslib")
                        export_opus_frames(frames)
                elif CODEC == "mulaw":
                    import numpy as np
                    raw = b"".join(frames)
                    pcm16 = [ulaw2linear(b) for b in raw]
                    export_wav(np.array(pcm16, dtype=np.int16).tobytes())
                else:  # pcm
                    import numpy as np
                    raw = b"".join(frames)
                    raw = raw[:len(raw) - len(raw) % 2]
                    export_wav(np.frombuffer(raw, dtype=np.int16).tobytes())

        await record_and_process()


asyncio.run(main())
