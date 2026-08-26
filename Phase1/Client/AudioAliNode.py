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
        
        # ------------------------READING API-KEY -----------------------

        try:
            with open("./"+self.API_KEY_FILENAME, "r") as f:
                self.API_KEY = f.read()
        except Exception as e:
            raise OSError("Failed to read API key from file: {}".format(e))
        

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

        self.lte_sender = None

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

            # -----------------------CLOCK SYNCHRONIZATION AT STARTUP-----------------------
            self.z_offset="+0000"   #in formato ±HHMM
            self._initialize_LTE_module()
            self._deinitialize_LTE_module()
            
            self._last_clock_synchronization_time=time.time()
            self._last_logs_upload_time=time.time()
            self._last_configuration_download_time=time.time()
            # ------------------------------------------------------------------------------
        except (KeyboardInterrupt, Exception) as e:
            Logging.log_error("Failed to initialize AudioAliNode: \"{}\"".format(e))
            self.deinit()
            raise e
            
    def deinit(self):
        self._deinitialize_LTE_module()
        try:
            os.umount(self.SD_PATH)
            Logging.log_info("SD unmounted")
        except OSError as e:
            Logging.log_error("Error unmounting SD: {}".format(e))

    def _initialize_LTE_module(self):
        try:

            eg91 = Eg91Sender(self.UART_TX_PIN, self.UART_RX_PIN, self.LTE_POWER_PIN, self.LTE_RESET_PIN)

            self.lte_sender = eg91

            self._clock_synchronization()

        except (KeyboardInterrupt, Exception) as e:
            Logging.log_error("Failed to initialize LTE module: \"{}\"".format(e))
            self._deinitialize_LTE_module()

    def _deinitialize_LTE_module(self):
        if self.lte_sender:
            try:
                self.lte_sender.deinit()
                self.lte_sender = None
                Logging.log_info("LTE module deinitialized successfully")
            except (KeyboardInterrupt,Exception) as e:
                Logging.log_error("Failed to deinitialize LTE module: \"{}\"".format(e))

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

    def _test_request(self):
        response = self.lte_sender.https_get_request(url="https://webhook.site/47d1b971-ac5e-448e-b33b-5b1f49e608b3",
                                                      headers={"X-API-KEY": self.API_KEY})
        #response = self.LTESender.https_get_request(url=endpoint)
        if response:
            Logging.log_info("Response: {}".format(response))
        else:
            raise Exception("An error occurred while sending the chunk to the server")
    
    def _clock_synchronization(self):
        try:
            current_time, dst_flag = self.lte_sender.get_time()
            if current_time:
                regex_string=ure.compile(
                    "(\d\d\d\d)/(\d\d)/(\d\d),"   # yyyy/MM/dd
                    "(\d\d):(\d\d):(\d\d)"        # hh:mm:ss
                    "([+-]\d+)"                   # ±zz
                )
                m = ure.match(regex_string,current_time)

                if not m:
                    raise ValueError("Invalid time format")

                year   = int(m.group(1))
                month  = int(m.group(2))
                day    = int(m.group(3))
                hour   = int(m.group(4))
                minute = int(m.group(5))
                second = int(m.group(6))
                zz = int(m.group(7))
                sign = "+" if zz >= 0 else "-"
                abs_zz = abs(zz)
                total_minutes = abs_zz * 15
                hours = total_minutes // 60
                minutes = total_minutes % 60
                self.z_offset = "{}{:02d}{:02d}".format(sign, hours, minutes)


                rtc = RTC()
                rtc.datetime((
                    year, month, day, 0,  # weekday = 0
                    hour, minute, second,
                    0                     # subseconds
                ))

                self._last_clock_synchronization_time=time.time()
                Logging.log_info(f"Local clock synchronized: {self._get_current_time()}")
            else:
                raise Exception("Failed to get current time from network")

        except Exception as e:
            raise Exception("Error while synchronizing local clock: \"{}\"".format(e))
    
    def _get_current_time(self):
        tm=time.localtime(time.time())
        current_time = "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}{}".format(tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], self.z_offset)
        return current_time
    
    def _is_clock_synchronized(self):
        current_time = time.time()
        current_year = time.localtime(current_time)[0]
        return (((current_time-self._last_clock_synchronization_time) <= self.CONFIG["TIME_OUT_OF_SYNC"]) and (current_year>self.CONFIG["MIN_YEAR_FOR_SYNC_CHECK"]))
    
    def _send_chunk_to_server(self, chunk):
        #endpoint="http://ec2-18-197-151-12.eu-central-1.compute.amazonaws.com:8443/audio"
        response = self.lte_sender.https_post_request(url=self.CONFIG["AUDIO_ENDPOINT"],
                                                        body=chunk,
                                                        content_type="application/octet-stream",
                                                        headers={"X-API-KEY": self.API_KEY})
        #response = self.LTESender.https_get_request(url=endpoint)
        if response:
            Logging.log_info("Response: {}".format(response))
        else:
            raise Exception("An error occurred while sending the chunk to the server")

    def _send_logs_to_server(self):
        if (time.time()-self._last_logs_upload_time) >= self.CONFIG["LOG_UPLOAD_PERIOD"]:
            try:
                Logging.send_log_files_to_server(self.lte_sender, self.CONFIG["LOGS_ENDPOINT"], headers={"X-API-KEY": self.API_KEY})
                self._last_logs_upload_time=time.time()
            except Exception as e:
                Logging.log_error("Error while sending logs to the server: \"{}\"".format(e))

    def _update_configuration(self):
        if (time.time()-self._last_configuration_download_time) >= self.CONFIG["CONFIGURATION_DOWNLOAD_PERIOD"]:
            response = self.lte_sender.https_get_request(url=self.CONFIG["CONFIGURATION_ENDPOINT"], headers={"X-API-KEY": self.API_KEY})
            if response:
                config=ujson.loads(response)
                for key in self.CONFIG.keys():
                    if key in config:
                        self.CONFIG[key] = config[key]
                self._last_configuration_download_time=time.time()
                Logging.log_info("Configuration updated: {}".format(self.CONFIG))
            else:
                raise Exception("An error occurred while downloading the configuration from the server")
    
    def _is_configuration_stale(self):
        return (time.time()-self._last_configuration_download_time) >= self.CONFIG["CONFIGURATION_DOWNLOAD_PERIOD"]

    # Funzione principale 
    def start(self):
        Logging.log_info(f"Starting main loop at time: {self._get_current_time()}")
        while True:
            # Controllo che il clock sia sufficientemente sincronizzato, altrimenti lo sincronizzo prima di fare qualsiasi altra operazione
            if self._is_clock_synchronized():
                try:
                    # Catturo un chunk audio di 5s
                    chunk=self._get_audio_chunk()
                    # Salvo il chunk catturato sulla SD
                    self.sd_buffer.enqueue(chunk)
                    # Controllo se il buffer sia sufficienemente pieno (numero di file pari ad almeno FILES_THRESHOLD) 
                    # e se la batteria sia sufficientemente carica per inviare i dati al server
                    if self.sd_buffer.is_buffer_full_enough() and self._is_battery_sufficient():
                        Logging.log_info("Buffer full enough to send data (files: {})".format(self.sd_buffer.get_number_of_files()))
                        # Accendo e inizializzo il modulo LTE, che rimarrà acceso per tutta la durata 
                        # dell'invio di tutti i chunk presenti nel buffer (2 minuti a invio del singolo chunk)
                        self._initialize_LTE_module()
                        if self.lte_sender:
                            Logging.log_info("Network connection established and battery sufficient, sending data...")
                            buffer_len=self.sd_buffer.get_number_of_files()
                            for i in range(buffer_len):
                                Logging.log_info("Sending chunk {}/{}...".format(i+1, buffer_len))
                                try:
                                    # Considero il primo chunk del buffer (il più vecchio)
                                    chunk=self.sd_buffer.get_first_file()
                                    # Invio il chunk al server
                                    self._send_chunk_to_server(chunk)
                                    # Elimino il chunk inviato dal buffer (e quindi dalla SD)
                                    self.sd_buffer.dequeue()
                                except Exception as e:
                                    Logging.log_error("Error sending chunk {}/{}: {}".format(i+1, buffer_len, e))
                                    # Interrompo l'invio degli eventuali chunk rimanenti nel buffer quando si è verificato un errore (es. perdita di connessione) , 
                                    # per evitare di consumare la batteria dato che con alta probabilità il medesimo errore si ripeterà con i chunk successivi
                                    break               # continue se vuoi continuare ad inviare il resto, break se vuoi interrompere l'invio sequenziale di tutti i chunk
                            # Invio i log al server (se sono passati pià di LOG_UPLOAD_PERIOD secondi dall'ultimo invio)
                            self._send_logs_to_server()
                            # Aggiorno la configurazione scaricandola dal server (se sono passati più di CONFIGURATION_DOWNLOAD_PERIOD dall'ultimo aggiornamento)
                            self._update_configuration()
                            # Deinizializzo e spengo il modulo LTE, per risparmiare batteria
                            self._deinitialize_LTE_module()
                    else:
                        Logging.log_info("Buffer NOT full enough to send data (files: {})".format(self.sd_buffer.get_number_of_files()))
                except Exception as e:
                    Logging.log_error("A problem occurred during this iteration: \"{}\"".format(e))
            else:
                Logging.log_info("Clock NOT synchronized, trying to synchronize it...")
                # L'accensione e inizializzazione del modulo LTE sincronizza in automatico il clock
                self._initialize_LTE_module()
                self._deinitialize_LTE_module()

            # Attendo un certo tempo prima iniziare nuovamente un ciclo di cattura e invio dati
            Logging.log_info("Sleeping...")
            time.sleep(self.CONFIG["IDLE_TIME"])
            Logging.log_info("Awake!")

       # Funzione principale 
    
    def start_without_sending_to_the_server(self):
        Logging.log_info(f"Starting main loop at time: {self._get_current_time()}")
        while True:
            # Controllo che il clock sia sufficientemente sincronizzato, altrimenti lo sincronizzo prima di fare qualsiasi altra operazione
            if self._is_clock_synchronized():
                try:
                    # Catturo un chunk audio di 5s
                    chunk=self._get_audio_chunk()
                    # Salvo il chunk catturato sulla SD
                    self.sd_buffer.enqueue(chunk)
                    if self._is_configuration_stale() and self._is_battery_sufficient():
                        self._initialize_LTE_module()
                        if self.lte_sender:
                            Logging.log_info("Network connection established and battery sufficient, sending data...")
                            # Aggiorno la configurazione scaricandola dal server (se sono passati più di CONFIGURATION_DOWNLOAD_PERIOD dall'ultimo aggiornamento)
                            self._update_configuration()
                            # Deinizializzo e spengo il modulo LTE, per risparmiare batteria
                            self._deinitialize_LTE_module()
                except Exception as e:
                    Logging.log_error("A problem occurred during this iteration: \"{}\"".format(e))
            else:
                Logging.log_info("Clock NOT synchronized, trying to synchronize it...")
                # L'accensione e inizializzazione del modulo LTE sincronizza in automatico il clock
                self._initialize_LTE_module()
                self._deinitialize_LTE_module()

            # Attendo un certo tempo prima iniziare nuovamente un ciclo di cattura e invio dati
            Logging.log_info("Sleeping...")
            time.sleep(self.CONFIG["IDLE_TIME"])
            Logging.log_info("Awake!")

    #Da usare solo per test
    def test_start(self):
        Logging.log_info(f"Starting test at time: {self._get_current_time()}")
        if True:
            try:
                self._initialize_LTE_module()
                if self.lte_sender:
                    #chunk=self._get_audio_chunk()
                    #self.sd_buffer.enqueue(chunk)
                    #self._send_chunk_to_server(chunk)
                    #self.sd_buffer.dequeue()
                    self._send_logs_to_server()
                    self._update_configuration()
                    #self._test_request()
                    self._deinitialize_LTE_module()
            except KeyboardInterrupt as k:
                #break
                pass
            except Exception as e:
                Logging.log_error("A problem occurred during this iteration: \"{}\"".format(e))
            print("Sleeping...")
            time.sleep(10)
            print("Awake!")


if __name__ == "__main__":
    alinode=None
    try:
        alinode=AudioAliNode()
        #alinode.sd_buffer.clear_buffer()
        alinode.start_without_sending_to_the_server()
        #alinode.test_start()
    except (KeyboardInterrupt, Exception) as e:
        sys.print_exception(e)
    finally:
        if alinode:
            # Se l'oggetto alinode è stato configurato, lo deinizializzo per spegnerlo nella maniera più sicura possibile
            Logging.untraced_log_info("Stopping AliNode...")
            alinode.deinit()
            Logging.untraced_log_info("AliNode stopped.")
