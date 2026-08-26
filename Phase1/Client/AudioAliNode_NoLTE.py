import os
import sys
from Logging import Logging
from machine import Pin, I2S, idle, UART, SoftI2C, RTC, SDCard, SPI
import time
from eg91_sender_v5 import Eg91Sender
import ure
import ujson
from sd_buffer import SDBuffer
from soc_driver import SocDriver
from bus_service import I2cAdapter
from wav_metadata import WAVMetadata
from i2s_driver import I2SDriver

class AudioAliNode():

    LTE_RESET_PIN = 3
    LTE_POWER_PIN = 4
    UART_TX_PIN = 41
    UART_RX_PIN = 42
    I2S_SCK_PIN = 46
    I2S_WS_PIN = 47
    I2S_SD_PIN = 48
    SPI_SCK_PIN = 12
    SPI_MOSI_PIN = 11
    SPI_MISO_PIN = 13
    SPI_CS_PIN = 1
    MIN_VOLTAGE_LEVEL = 3600   # mv
    MAX_VOLTAGE_LEVEL = 4200   # mv
    DEFAULT_CONFIG_FILE = "default_alinode_conf.json"
    SD_PATH = "/sd"
    FILES_THRESHOLD = 100     # 1 invio ogni 12 ore circa (7 minuti circa per chunk)
    MAX_SDBUFFER_SIZE = 13958643712 #13GB
    LOGS_PATH="/logs"
    MAX_LOG_BUFFER_SIZE = 387973 #0.37MB (circa)
    MAX_NUMBER_OF_LOG_FILES = 10
    BUFFER_LENGTH_IN_BYTES = 40000
    RECORD_TIME_IN_SECONDS = 5
    WAV_SAMPLE_SIZE_IN_BITS = 32
    SAMPLE_RATE_IN_HZ = 16000
    API_KEY_FILENAME = ".api-key"
    API_KEY=""
    


    def __init__(self):

        # -----------------------DEFAULT CONFIGURATION LOADING-----------------------

        try:

            with open("./" + self.DEFAULT_CONFIG_FILE, "r") as f:
                data = ujson.load(f)

            self.CONFIG={}

            for key in data.keys():
                self.CONFIG[key] = data[key]

        except Exception as e:
            raise OSError("Failed to read default configuration file: {}".format(e))
        

        try:
            # -----------------------SD CARD INITIALIZATION-----------------------

            # Monta SD
            self.sd = SDCard(slot=2,sck=Pin(self.SPI_SCK_PIN),mosi=Pin(self.SPI_MOSI_PIN),miso=Pin(self.SPI_MISO_PIN), cs=Pin(self.SPI_CS_PIN, Pin.OUT))
            self.vfs = os.VfsFat(self.sd)
            os.mount(self.vfs, self.SD_PATH)
        except Exception as e:
            raise OSError("Failed to initialize SD card: {}".format(e))
        
        Logging.initialize_configuration(
                        logs_path=self.SD_PATH+self.LOGS_PATH, 
                         max_number_of_files=self.MAX_NUMBER_OF_LOG_FILES, 
                         max_buffer_size=self.MAX_LOG_BUFFER_SIZE
                         )
        Logging.log_info(f"SD mounted on {self.SD_PATH}")

        try:


            # -----------------------SD BUFFER INITIALIZATION-----------------------

            self.sd_buffer = None

            self.sd_buffer = SDBuffer(
                sd_path=self.SD_PATH,
                files_dir="Audio",
                file_prefix="audio_",
                file_suffix=".wav",
                max_buffer_size=self.MAX_SDBUFFER_SIZE,
                files_threshold=self.FILES_THRESHOLD,
            )

            # -----------------------I2S DRIVER INITIALIZATION-----------------------

            self.i2s_driver = None

            self.i2s_driver=I2SDriver(
                i2s_sck_pin=self.I2S_SCK_PIN,
                i2s_ws_pin=self.I2S_WS_PIN,
                i2s_sd_pin=self.I2S_SD_PIN,
                i2s_id=0,
                buffer_length_in_bytes=self.BUFFER_LENGTH_IN_BYTES,
                record_time_in_seconds=self.RECORD_TIME_IN_SECONDS,
                wav_sample_size_in_bits=self.WAV_SAMPLE_SIZE_IN_BITS,
                format="MONO",
                sample_rate_in_hz=self.SAMPLE_RATE_IN_HZ
            )

            # -----------------------SOC DRIVER INITIALIZATION-----------------------

            config={}
            try:
                with open("soc_config.json") as f:
                    config = ujson.loads(f.read())
            except Exception as e:
                raise OSError("Failed to read configuration file: {}".format(e))


            i2c = SoftI2C(scl=Pin(9), sda=Pin(8), freq=100000)
            adapter = I2cAdapter(i2c)

            Logging.log_info(f"I2C scan: {i2c.scan()}")
            
            self.soc_driver = SocDriver(adapter, config)
            #self.soc_driver.data_memory_test()
            #self.soc_driver.start()
            self.soc_driver.write_qmax_cell_0()    #Commentare per test
            #self._get_battery_level()

            self.z_offset="+0000"   #in formato ±HHMM

            # ------------------------------------------------------------------------------
        except (KeyboardInterrupt, Exception) as e:
            Logging.log_error("Failed to initialize AudioAliNode: \"{}\"".format(e))
            self.deinit()
            raise e
            
    def deinit(self):
        try:
            os.umount(self.SD_PATH)
            Logging.log_info("SD unmounted")
        except OSError as e:
            Logging.log_error("Error unmounting SD: {}".format(e))

    def _get_battery_level(self):   #Commentare per test
        data = self.soc_driver.get_data()
        battery_voltage_level=data[3]["v"]
        Logging.log_info("Battery voltage level: {} mV".format(battery_voltage_level))
        return battery_voltage_level
        #return self.MAX_VOLTAGE_LEVEL

    def _is_battery_sufficient(self):   #Commentare per test
        bvlv=self._get_battery_level()
        return bvlv >= self.MIN_VOLTAGE_LEVEL
        #return True

    def _get_chunk_metadata(self, chunk):
        timestamp=self._get_current_time()
        nodeID=self.CONFIG["NODEID"]
        batteryLevel=self._get_battery_level()
        rmsv=self.i2s_driver.calculate_RMS(chunk)
        return WAVMetadata(
            tmst=timestamp,
            noId=nodeID,
            blvl=batteryLevel,
            rmsv=rmsv).to_dict()

    def _get_audio_chunk(self):
        wav_data=self.i2s_driver.get_single_audio_chunk()
        metadata=self._get_chunk_metadata(wav_data)

        # create header for WAV file
        wav_header = self.i2s_driver.create_wav_header(metadata)
        
        wav_chunk=wav_header+wav_data
        return wav_chunk

    
    def _get_current_time(self):
        tm=time.localtime(time.time())
        current_time = "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}{}".format(tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], self.z_offset)
        return current_time

    # Funzione principale 
    def start(self):
        Logging.log_info(f"Starting main loop at time: {self._get_current_time()}")
        while True:
            try:
                # Catturo un chunk audio di 5s
                chunk=self._get_audio_chunk()
                # Salvo il chunk catturato sulla SD
                self.sd_buffer.enqueue(chunk)
            except Exception as e:
                Logging.log_error("A problem occurred during this iteration: \"{}\"".format(e))
            # Attendo un certo tempo prima iniziare nuovamente un ciclo di cattura dati
            Logging.log_info("Sleeping...")
            time.sleep(self.CONFIG["IDLE_TIME"])
            Logging.log_info("Awake!")



if __name__ == "__main__":
    alinode=None
    try:
        alinode=AudioAliNode()
        #alinode.sd_buffer.clear_buffer()
        alinode.start()
        #alinode.test_start()
    except (KeyboardInterrupt, Exception) as e:
        sys.print_exception(e)
    finally:
        if alinode:
            # Se l'oggetto alinode è stato configurato, lo deinizializzo per spegnerlo nella maniera più sicura possibile
            Logging.untraced_log_info("Stopping AliNode...")
            alinode.deinit()
            Logging.untraced_log_info("AliNode stopped.")
