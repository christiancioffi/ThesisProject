from Logging import Logging
from machine import Pin, I2S
import struct
import math


class I2SDriver():

    def __init__(self, 
                i2s_sck_pin: int, 
                i2s_ws_pin: int, 
                i2s_sd_pin: int,
                buffer_length_in_bytes: int,
                record_time_in_seconds: int,
                wav_sample_size_in_bits: int,
                format: int,
                sample_rate_in_hz: int,
                i2s_id: int = 0):

        self.I2S_SCK_PIN = i2s_sck_pin
        self.I2S_WS_PIN = i2s_ws_pin
        self.I2S_SD_PIN = i2s_sd_pin

        # MICROPHONE = Adafruit I2S MEMS Microphone Breakout - SPH0645LM4H

        # ======= I2S CONFIGURATION =======
        self.I2S_ID = i2s_id
        self.BUFFER_LENGTH_IN_BYTES = buffer_length_in_bytes

        # ======= AUDIO CONFIGURATION =======
        self.RECORD_TIME_IN_SECONDS = record_time_in_seconds
        self.WAV_SAMPLE_SIZE_IN_BITS = wav_sample_size_in_bits        #Slot bit width
        if format == "MONO":
            self.FORMAT = I2S.MONO
        elif format == "STEREO":
            self.FORMAT = I2S.STEREO
        else:
            raise ValueError("Format not valid")
        self.SAMPLE_RATE_IN_HZ = sample_rate_in_hz  #Sampling rate


        format_to_channels = {I2S.MONO: 1, I2S.STEREO: 2}
        self.NUM_CHANNELS = format_to_channels[self.FORMAT]
        self.WAV_SAMPLE_SIZE_IN_BYTES = self.WAV_SAMPLE_SIZE_IN_BITS // 8
        self.RECORDING_SIZE_IN_BYTES = (
            self.RECORD_TIME_IN_SECONDS * self.SAMPLE_RATE_IN_HZ * self.WAV_SAMPLE_SIZE_IN_BYTES * self.NUM_CHANNELS
        )

    def get_single_audio_chunk(self):

        # ======= I2S INITIALIZATION =======
        self.i2s_audio_in = I2S(
            self.I2S_ID,
            sck=Pin(self.I2S_SCK_PIN),
            ws=Pin(self.I2S_WS_PIN),
            sd=Pin(self.I2S_SD_PIN),
            mode=I2S.RX,
            bits=self.WAV_SAMPLE_SIZE_IN_BITS,
            format=self.FORMAT,
            rate=self.SAMPLE_RATE_IN_HZ,
            ibuf=self.BUFFER_LENGTH_IN_BYTES,
        )

        wav_data=bytes()

        # allocate sample arrays
        # memoryview used to reduce heap allocation in while loop
        mic_samples = bytearray(10000)
        mic_samples_mv = memoryview(mic_samples)

        num_sample_bytes_written_to_wav = 0

        Logging.log_info("Recording size: {} bytes".format(self.RECORDING_SIZE_IN_BYTES))
        Logging.log_info("==========  START RECORDING ==========")
        try:
            while num_sample_bytes_written_to_wav < self.RECORDING_SIZE_IN_BYTES:
                # read a block of samples from the I2S microphone
                num_bytes_read_from_mic = self.i2s_audio_in.readinto(mic_samples_mv)
                '''
                for i in range(0, len(mic_samples_mv), 4):
                    b0 = mic_samples_mv[i]
                    b1 = mic_samples_mv[i+1]
                    b2 = mic_samples_mv[i+2]
                    b3 = mic_samples_mv[i+3]
                    Logging.log_info(i, ":", hex(b0), hex(b1), hex(b2), hex(b3))
                '''
                if num_bytes_read_from_mic > 0:
                    num_bytes_to_write = min(
                        num_bytes_read_from_mic, self.RECORDING_SIZE_IN_BYTES - num_sample_bytes_written_to_wav
                    )
                    # write samples to WAV file
                    #num_bytes_written = wav.write(mic_samples_mv[:num_bytes_to_write])
                    wav_data+=mic_samples_mv[:num_bytes_to_write]
                    num_bytes_written = num_bytes_to_write
                    num_sample_bytes_written_to_wav += num_bytes_written

            Logging.log_info("==========  DONE RECORDING ==========")
        except (KeyboardInterrupt, Exception) as e:
            Logging.log_error("Caught exception {} {}".format(type(e).__name__, e))
            raise Exception("An error occurred during audio recording")
        finally:
            self.i2s_audio_in.deinit()

        #metadata=self.get_metadata(wav_data)

        # create header for WAV file and write to SD card
        #wav_header = self.create_wav_header(metadata)
        
        #wav_chunk=wav_header+wav_data

        return wav_data

    def create_wav_header(self, metadata=None):
        datasize = self.RECORD_TIME_IN_SECONDS * self.SAMPLE_RATE_IN_HZ * self.WAV_SAMPLE_SIZE_IN_BYTES * self.NUM_CHANNELS

        o = b"RIFF"
        filesize_pos = len(o)
        o += (0).to_bytes(4, "little")  # placeholder dimensione RIFF
        o += b"WAVE"

        # fmt chunk (PCM)
        o += b"fmt "
        o += (16).to_bytes(4, "little")
        o += (1).to_bytes(2, "little")  # AudioFormat = PCM
        o += self.NUM_CHANNELS.to_bytes(2, "little")
        o += self.SAMPLE_RATE_IN_HZ.to_bytes(4, "little")
        o += (self.SAMPLE_RATE_IN_HZ * self.NUM_CHANNELS * self.WAV_SAMPLE_SIZE_IN_BITS // 8).to_bytes(4, "little")
        o += (self.NUM_CHANNELS * self.WAV_SAMPLE_SIZE_IN_BITS // 8).to_bytes(2, "little")
        o += self.WAV_SAMPLE_SIZE_IN_BITS.to_bytes(2, "little")
        # LIST / INFO chunk (metadati)
        if metadata:
            info_data = b"INFO"

            for key, value in metadata.items():
                Logging.log_info(f"Adding metadata key: {key} value: {value}")
                if len(key) != 4:
                    raise ValueError("INFO key must be 4 characters long")

                data = str(value).encode("ascii") + b"\x00"

                if len(data) % 2 == 1:
                    data += b"\x00"  # padding a 2 byte

                info_data += key.encode("ascii")
                info_data += len(data).to_bytes(4, "little")
                info_data += data

            o += b"LIST"
            o += len(info_data).to_bytes(4, "little")
            o += info_data

        # data chunk
        o += b"data"
        o += datasize.to_bytes(4, "little")

        # aggiorna dimensione RIFF (file_size - 8)
        filesize = len(o) - 8 + datasize
        o = o[:filesize_pos] + filesize.to_bytes(4, "little") + o[filesize_pos + 4:]

        return o

    def calculate_RMS(self, audio_bytes):
            bytes_per_sample = self.WAV_SAMPLE_SIZE_IN_BITS // 8
            frame_size = bytes_per_sample * self.NUM_CHANNELS
            num_frames = len(audio_bytes) // frame_size

            if num_frames == 0:
                return 0.0

            sum_squares = 0.0
            sample_count = 0

            for i in range(0, num_frames * frame_size, bytes_per_sample):
                sample_bytes = audio_bytes[i:i + bytes_per_sample]

                if self.WAV_SAMPLE_SIZE_IN_BITS == 8:
                    # 8-bit PCM è unsigned
                    sample = sample_bytes[0] - 128  # trasforma in signed (-128..127)

                elif self.WAV_SAMPLE_SIZE_IN_BITS == 16:
                    # little-endian signed short
                    sample = struct.unpack("<h", sample_bytes)[0]

                elif self.WAV_SAMPLE_SIZE_IN_BITS == 24:
                    # little-endian signed 24-bit
                    # aggiungiamo un byte di estensione di segno manuale
                    b = sample_bytes
                    if b[2] & 0x80:
                        b += b'\xff'  # se il bit più alto è 1 -> negativo
                    else:
                        b += b'\x00'  # altrimenti positivo
                    sample = struct.unpack("<i", b)[0] >> 8

                elif self.WAV_SAMPLE_SIZE_IN_BITS == 32:
                    # little-endian signed int
                    sample = struct.unpack("<i", sample_bytes)[0]

                else:
                    raise ValueError("bits_per_sample non supportato")

                sum_squares += sample * sample
                sample_count += 1

            mean_square = sum_squares / sample_count
            rmsv=math.sqrt(mean_square)
            Logging.log_info(f"RMS value calculated: {rmsv}")
            return rmsv

