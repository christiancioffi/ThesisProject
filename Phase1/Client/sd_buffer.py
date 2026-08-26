import math
import os
from Logging import Logging
import time
import sys

class SDBuffer():
    def __init__(self,
                 sd_path,
                 files_dir,
                 file_prefix,
                 file_suffix,
                 max_buffer_size,
                 files_threshold):  # 13 GB di default (su 14,83 GB a disposizione)


        self.SD_PATH = sd_path
        self.FILES_DIR = files_dir
        self.prefix = file_prefix
        self.suffix = file_suffix
        self.queue = []  # lista dei file in ordine FIFO
        self.files_threshold = files_threshold
        self.max_buffer_size = max_buffer_size  
        self.used_space = 0

        # Crea directory per i file audio se non esiste
        try:
            os.mkdir(self.SD_PATH + "/" + self.FILES_DIR)
        except Exception as e:
            pass

        Logging.log_info(f"Audio files directory: {self.SD_PATH}/{self.FILES_DIR}")

        sd_stats = os.statvfs(self.SD_PATH)

        self.cluster_size = sd_stats[0]    # Dimensione del cluster in byte

        # Inizializza la coda dai file esistenti
        try:
            self._load_queue()
            free_space = sd_stats[0] * sd_stats[3]
            if max_buffer_size > (free_space+self.used_space):
                raise OSError("Buffer size ({}) too large for the available free space ({})".format(max_buffer_size, free_space+self.used_space))
        except Exception as e:
            raise Exception("SD Buffer initialization error: \"{}\"".format(e))

    # -----------------------
    # Load the queue from existing files on the SD
    # -----------------------
    def _load_queue(self):
        try:
            files = [f for f in os.listdir(self.SD_PATH + "/" + self.FILES_DIR)
                    if f.startswith(self.prefix) and f.endswith(self.suffix)]

            def extract_num(name):
                try:
                    return int(name[len(self.prefix):-len(self.suffix)])
                except ValueError:
                    Logging.log_error("Filename not valid:", name)
                    return 0

            files.sort(key=extract_num)
            self.queue = files
            for f in self.queue:
                try:
                    path=self.SD_PATH + "/" + self.FILES_DIR + "/" + f
                    st = os.stat(path)
                    file_size = st[6]
                    file_used_space = math.ceil(file_size / self.cluster_size) * self.cluster_size
                    self.used_space += file_used_space
                except OSError:
                    pass
            
            Logging.log_info("Loaded {} files from SD, used space: {} bytes".format(len(self.queue), self.used_space))
            Logging.log_info("Available space: {} bytes".format(self.available_space()))
        except Exception as e:
            Logging.log_error("Error loading existing files from SD: {}".format(e))
            raise Exception("SD Buffer load queue error")

    def available_space(self):
        return max(0, self.max_buffer_size - self.used_space)

    # -----------------------
    # Elimina i file più vecchi finché c'è spazio sufficiente
    # -----------------------
    def _free_space(self, requested_space):
        if requested_space > self.max_buffer_size:
            raise OSError("File too large for buffer")
        while requested_space > self.available_space() and len(self.queue) > 0:
            try:
                Logging.log_info("Freeing space from the buffer...")
                oldest = self.queue[0]
                path=self.SD_PATH + "/" + self.FILES_DIR + "/" + oldest
                st = os.stat(path)
                file_size = st[6]
                file_used_space = math.ceil(file_size / self.cluster_size) * self.cluster_size
                os.remove(path)
                self.queue.pop(0)
                Logging.log_info("Removed old file '{}' of {} bytes (real size: {}) from buffer".format(oldest, file_size, file_used_space))
                self.used_space = max(0, self.used_space - file_used_space)
                Logging.log_info("Space freed successfully")
            except Exception as e:
                Logging.log_error("Error removing old file {}: {}".format(oldest, e))
                raise Exception("SD Buffer free space error")

    # -----------------------
    # Salva dati come file binario (buffer circolare)
    # -----------------------
    def enqueue(self, data_bytes):
        
        file_size = len(data_bytes)
        file_used_space = math.ceil(file_size / self.cluster_size) * self.cluster_size

        try:
            self._free_space(file_used_space)

            filename = self.prefix + str(time.time()) + self.suffix
            path = self.SD_PATH + "/" + self.FILES_DIR + "/" + filename

            with open(path, "wb") as f:
                f.write(data_bytes)

            self.queue.append(filename)
            self.used_space += file_used_space
            Logging.log_info("Added file to buffer: {}, file size: {} bytes (real size: {} bytes)".format(filename, file_size, file_used_space))

        except Exception as e:
            Logging.log_error("Error while adding file to the buffer: {}".format(e))
            sys.print_exception(e)
            raise Exception("SD Buffer enqueue error")


    # -----------------------
    # Elimina un file
    # -----------------------
    def dequeue(self):
        if len(self.queue) == 0:
            raise Exception("Buffer is empty")

        try:
            filename = self.queue[0]
            path = self.SD_PATH + "/" + self.FILES_DIR + "/" + filename
            
            st = os.stat(path)
            file_size = st[6]
            os.remove(path)
            self.queue.pop(0)
            file_used_space = math.ceil(file_size / self.cluster_size) * self.cluster_size
            self.used_space = max(0, self.used_space - file_used_space)
            Logging.log_info("Removed file '{}' of {} bytes (real size: {}) from buffer".format(filename, file_size, file_used_space))
        except Exception as e:
            Logging.log_error("Error removing file {}: {}".format(filename, e))
            raise Exception("SD Buffer dequeue error")

    
    # -----------------------
    # Estrapola il primo file della coda
    # -----------------------
    def get_first_file(self):
        if len(self.queue) == 0:
            raise Exception("Buffer is empty")

        try:
            filename = self.queue[0]
            path = self.SD_PATH + "/" + self.FILES_DIR + "/" + filename

            # Legge il file
            with open(path, "rb") as f:
                file_data = f.read()
            
            return file_data
        except Exception as e:
            raise Exception("Error retrieving file {}: {}".format(filename, e))
        
    def clear_buffer(self):

        Logging.log_info("Available total space in the SD before clearing: {} bytes".format(self._get_total_free_space()))

        while len(self.queue) > 0:
            self.dequeue()

        Logging.log_info("Available total space in the SD after clearing: {} bytes".format(self._get_total_free_space()))


    # -----------------------
    # Lista dei file sulla SD
    # -----------------------
    def get_number_of_files(self):
        return len(self.queue)
    
    def is_buffer_full_enough(self):
        return len(self.queue) >= self.files_threshold
    
    def get_used_space(self):
        return self.used_space

    def _get_total_free_space(self):    # In FAT blocks=clusters
        stat = os.statvfs(self.SD_PATH)
        free_clusters = stat[3]
        return self.cluster_size * free_clusters