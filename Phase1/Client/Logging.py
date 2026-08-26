import os
import time

class Logging():

    WHITE    = "\033[37m"
    RED     = "\033[31m"
    YELLOW     = "\033[33m"
    GREEN = "\033[32m"
    RESET   = "\033[0m"

    _ERROR_COLOR=RED
    _INFO_COLOR=WHITE
    _DEBUG_COLOR=YELLOW
    _LOGS_PATH = None
    _MAX_NUMBER_OF_FILES=None
    _MAX_BUFFER_SIZE = None
    _FILE_NAME="log"
    _FILE_EXTENSION=".txt"
    _current_file_index=0
    _buffer=[]
    _buffer_size=0

    def require_initialization(func):
        """Decoratore che controlla che la classe sia inizializzata"""
        def wrapper(cls, *args, **kwargs):
            if cls._LOGS_PATH is None or cls._MAX_NUMBER_OF_FILES is None or cls._MAX_BUFFER_SIZE is None:
                raise RuntimeError(f"Class not initialized.")
            return func(cls, *args, **kwargs)
        return wrapper


    @classmethod
    def initialize_configuration(cls, 
                 logs_path: str, 
                 max_number_of_files: int, 
                 max_buffer_size: int):
        
        if logs_path is not None and max_number_of_files is not None and max_buffer_size is not None:
            cls._LOGS_PATH = logs_path
            cls._MAX_NUMBER_OF_FILES = max_number_of_files
            cls._MAX_BUFFER_SIZE = max_buffer_size
            try:
                os.mkdir(cls._LOGS_PATH)
                Logging.untraced_log_debug(f"Created logs directory: {cls._LOGS_PATH}")
            except Exception as e:
                Logging.untraced_log_debug(f"Logs directory already exists: {cls._LOGS_PATH}")
                files=os.listdir(cls._LOGS_PATH)
                if len(files)>0:
                    Logging.untraced_log_info("Cleaning existing log files...")
                    for file in files:
                        try:
                            os.remove(cls._LOGS_PATH + "/" + file)
                            Logging.untraced_log_info(f"Deleted existing log file: {file}")
                        except Exception as e:
                            Logging.untraced_log_error(f"Failed to delete log file {file}: {e}")
                    Logging.untraced_log_info(f"Logs directory cleaned.")
    
    
    @classmethod
    @require_initialization
    def log_info(cls, message: str):
        tm=time.localtime(time.time())
        current_time = "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(tm[0], tm[1], tm[2], tm[3], tm[4], tm[5])
        log_message = f"[INFO][{current_time}] {message}"
        print(f"{cls._INFO_COLOR}{log_message}{cls.RESET}")
        cls._add_to_buffer(log_message)
    
    @classmethod
    @require_initialization
    def log_error(cls, message: str):
        tm=time.localtime(time.time())
        current_time = "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(tm[0], tm[1], tm[2], tm[3], tm[4], tm[5])
        log_message = f"[ERROR][{current_time}] {message}"
        print(f"{cls._ERROR_COLOR}{log_message}{cls.RESET}")
        cls._add_to_buffer(log_message)
    
    @classmethod
    @require_initialization
    def log_debug(cls, message: str):
        tm=time.localtime(time.time())
        current_time = "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(tm[0], tm[1], tm[2], tm[3], tm[4], tm[5])
        log_message = f"[DEBUG][{current_time}] {message}"
        print(f"{cls._DEBUG_COLOR}{log_message}{cls.RESET}")
        cls._add_to_buffer(log_message)
    
    @classmethod
    def untraced_log_info(cls, message: str):
        log_message = f"[{cls.__name__} INFO] {message}"
        print(f"{cls._INFO_COLOR}{log_message}{cls.RESET}")
        
    @classmethod
    def untraced_log_error(cls, message: str):
        log_message = f"[{cls.__name__} ERROR] {message}"
        print(f"{cls._ERROR_COLOR}{log_message}{cls.RESET}")
    
    @classmethod
    def untraced_log_debug(cls, message: str):
        log_message = f"[{cls.__name__} DEBUG] {message}"
        print(f"{cls._DEBUG_COLOR}{log_message}{cls.RESET}")

    @classmethod
    def _add_to_buffer(cls, msg: str):
        try:
            msg = msg.replace("\\", "\\\\").replace("\r", "\\r").replace("\n", "\\n") + "\n"
            if len(msg) > cls._MAX_BUFFER_SIZE:
                cls.untraced_log_error(f"Message size {len(msg)} exceeds max buffer size {cls._MAX_BUFFER_SIZE}, message will be discarded")
                return
            if cls._buffer_size + len(msg) >= cls._MAX_BUFFER_SIZE:
                cls._flush_buffer()
            cls._buffer.append(msg)
            cls._buffer_size += len(msg)
        except Exception as e:
            cls.untraced_log_error(f"Failed to update the buffer: \"{e}\"")
    
    @classmethod
    def _flush_buffer(cls):
        try:
            path=cls._LOGS_PATH + "/" + cls._FILE_NAME+f"_{cls._current_file_index}"+cls._FILE_EXTENSION
            Logging.untraced_log_info(f"Flushing logs buffer (current size: {cls._buffer_size}) to file: {path}...")
            with open(path, "w") as log_file:
                for msg in cls._buffer:
                    log_file.write(msg)
            cls._current_file_index = (cls._current_file_index + 1) % cls._MAX_NUMBER_OF_FILES
            cls._buffer = []
            cls._buffer_size = 0
        except Exception as e:
            cls.untraced_log_error(f"Failed to flush logs buffer: \"{e}\"")

    @classmethod
    @require_initialization
    def send_log_files_to_server(cls, sender: Eg91Sender, endpoint:str, headers:dict=None):
        try:
                cls._flush_buffer()
                files=[]
                
                for i in range(cls._MAX_NUMBER_OF_FILES):
                    index=(cls._current_file_index+i+1)%cls._MAX_NUMBER_OF_FILES
                    file_path=cls._LOGS_PATH + "/" + cls._FILE_NAME+f"_{index}"+cls._FILE_EXTENSION
                    files.append(file_path)
                
                for file in files:
                    try:            #Check if file exists
                        os.stat(file)
                    except OSError:
                        continue
                    with open(file, "r") as f:
                        data=f.read()
                        cls.untraced_log_info(f"Sending file {file} to server...")
                        response = sender.https_post_request(url=endpoint, body=data, content_type="application/octet-stream", headers=headers)
                        if response:
                            cls.untraced_log_info(f"Sent file {file} to server successfully.")
                            try:
                                os.remove(file)
                                cls.untraced_log_info(f"Deleted log file: {file}")
                            except Exception as e:
                                cls.untraced_log_error(f"Failed to delete log file {file}: \"{e}\"")
                        else:
                            raise Exception(f"Failed to send file {file} to server.")
        except Exception as e:
            raise Exception(e)



from eg91_sender_v5 import Eg91Sender   #Dipendenza ciclica
