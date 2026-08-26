import time
import ujson
from Logging import Logging
from uart_rx_manager import UARTRXManager
from machine import UART, Pin
import esp32
import re
from timeout_exception import TimeoutException

class Eg91Sender():

    MAX_RESP_TIME = {
        # SIM/APN commands
        "AT+CPIN": 5000,
        "AT+QICSGP": 2000,
        "AT+QIACT": 150000,  # Increased timeout for network activation
        "AT+QIDEACT": 40000,


        # TIME commands
        "AT+QLTS": 5000,

        # HTTPS commands
        "AT+QHTTPCFG": 2000,
        "AT+CFUN": 20000,
        "AT+QHTTPURL": 5000,
        "AT+QHTTPGET": 80000,
        "AT+QHTTPREAD": 10000,
        "AT+QHTTPPOST": 80000,
        "AT+QHTTPSTOP": 5000,
        "GENERIC_INPUT": 80000,

        "AT+QPOWD": 60000
    }

    # HTTP(S) context
    PDP_CTXID = 1
    HTTPS_SSL_CTXID = 2

    # HTTP supported content types
    HTTP_CONTENT_TYPES = {
        "application/x-www-form-urlencoded": 0,
        "text/plain": 1,
        "application/octet-stream": 2,
        "multipart/form-data": 3,
        "application/json": 4,
        "image/jpeg": 5
    }

    MAX_RETRIES = 2

    URL_REGEX="https?://([a-zA-Z0-9-\.]+)(:[0-9]+)?(/[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]*)?"

    def __init__(self, uart_tx_pin, uart_rx_pin, lte_power_pin, lte_reset_pin):
        try:
            
            Logging.log_info("Initializing EG91 sender module...")

            self.UART_TX_PIN = uart_tx_pin
            self.UART_RX_PIN = uart_rx_pin
            self.LTE_POWER_PIN = lte_power_pin
            self.LTE_RESET_PIN = lte_reset_pin
            self.IDLE_TIME_POWER_CYCLE_STEP_1=1 #s
            self.IDLE_TIME_POWER_CYCLE_STEP_2=15    #s
            self.RESET_TIME_STEP_1=300 #ms
            self.RESET_TIME_STEP_2=15000 #ms
            self.POWERED_ON=False

            self._network_registered = False
            self._max_buffer_size = 2048
            self._min_wait_time = 7000  # Minimum wait time in ms
            self.ENABLED = False

            self._uart = UART(1, baudrate=115200, tx=Pin(self.UART_TX_PIN), rx=Pin(self.UART_RX_PIN), timeout=3000, rxbuf=self._max_buffer_size)

            self._uart_rx_manager=UARTRXManager(self._uart, self._max_buffer_size)

            self._power_on()

            self.POWERED_ON=True
        
            config={}
            try:
                with open("./" +"eg91_config.json", "r") as f:
                    config = ujson.loads(f.read())
            except Exception as e:
                raise OSError("Failed to read configuration file: {}".format(e))
            

            self._apn = config["apn"]
            self._endpoint = config["endpoint"]
            self._port = config["port"]
            self._client_id = config["client_id"]
            self._pub_topic = config["pub_topic"]
            self._sub_topic = config["sub_topic"]
            self._auth_type = config["auth_type"]

            if self._auth_type == "passwd":
                self._mqtt_user = config["mqtt_user"]
                self._mqtt_pass = config["mqtt_pass"]
            elif self._auth_type == "certs":
                self._ca = config["ca"]
                self._cert = config["cert"]
                self._key = config["key"]
            else:
                raise KeyError("auth_type")
            
            # Initialize interrupt-based communication
            self._init_uart_interrupt()

            if not self._enable():
                raise Exception("Failed to enable EG91 sender")
                
        except KeyError as key:
            self.deinit()
            raise ValueError(f"Invalid config value: {key}")
        except (KeyboardInterrupt, Exception) as e:
            Logging.log_error(f"EG91 initialization failed: \"{e}\"")
            self.deinit()
            raise RuntimeError(f"EG91 initialization failed")
        
    def deinit(self):
        """Deinitialize EG91 sender"""
        try:
            self._disable()
            self._uart.irq(trigger=0, handler=None)
            self._uart.deinit()
            self._uart_rx_manager.deinit()
            if self.POWERED_ON:
                self._shut_down()
                self.POWERED_ON=False
                '''
                try:
                    response = self._send_command_interrupt("AT+QPOWD", wait_time=self.MAX_RESP_TIME["AT+QPOWD"])
                    Logging.log_info("EG91 module shut down successfully")
                except Exception as e:
                    Logging.log_error("Failed to power down EG91 module via AT+QPOWD")
                    Logging.log_info("Shutting down EG91 module via power cycle...")
                    Pin(self.LTE_POWER_PIN, Pin.OUT).on()
                    time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_1)
                    Pin(self.LTE_POWER_PIN, Pin.OUT).off()
                    time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_2)
                    Logging.log_info("EG91 module shut down via power cycle")
                finally:
                    self.POWERED_ON=False
                '''
        except Exception as e:
            Logging.log_error(f"Error during EG91 deinitialization: \"{e}\"")

    def _power_on(self):
        # Power cycle
        Logging.log_info("Powering on EG91 module...")
        Pin(self.LTE_POWER_PIN, Pin.OUT).on()
        time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_1)
        Pin(self.LTE_POWER_PIN, Pin.OUT).off()
        time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_2)
        Logging.log_info("EG91 module powered on successfully")
    
    def _shut_down(self):
        Logging.log_info("Shutting down EG91 module...")
        Pin(self.LTE_POWER_PIN, Pin.OUT).on()
        time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_1)
        Pin(self.LTE_POWER_PIN, Pin.OUT).off()
        time.sleep(self.IDLE_TIME_POWER_CYCLE_STEP_2)
        Logging.log_info("EG91 module shut down successfully")
    
    def _hard_reset(self):
        Logging.log_info("Resetting EG91 module...")
        Pin(self.LTE_RESET_PIN, Pin.OUT).on()
        time.sleep_ms(self.RESET_TIME_STEP_1)
        Pin(self.LTE_RESET_PIN, Pin.OUT).off()
        time.sleep_ms(self.RESET_TIME_STEP_2)
        Logging.log_info("EG91 module reset successfully")
        self._init_uart_interrupt()

    def _soft_reset(self):
        try:
            self._send_command("AT+CFUN=1,1")
            self._init_uart_interrupt()
            return True
        except Exception as e:
            Logging.log_error(f"Failed to perform soft reset: \"{e}\"")
            return False

    def _init_uart_interrupt(self):
        """Initialize interrupt-based UART communication"""

        # Set up UART interrupt for incoming data
        self._uart.irq(trigger=UART.IRQ_RXIDLE, handler=self._uart_irq_handler) #RX
        Logging.log_debug("UART interrupt initialized")

    def _uart_irq_handler(self, uart_obj):          #Soft Interrupt (callback)
        """UART interrupt handler - called when data is received"""
        try:
            self._uart_rx_manager.awake_uart_listener()
        except Exception as e:
            Logging.untraced_log_error(f"UART IRQ handler error: \"{e}\"")

    def _send_command(self, command, wait_time=None) -> str:
        """
        Send AT command using interrupt-based communication with improved error handling

        :param str command: AT command to send
        :param int wait_time: maximum wait time in milliseconds
        :return str: response
        """

        try:
            Logging.log_debug(f"Sending AT command: {command}")
            
            response=None

            # Get timeout from command table if available
            if not wait_time:  # Default value
                cmd_key = command.split('=')[0].split('?')[0]
                wait_time = self.MAX_RESP_TIME.get(cmd_key, self._min_wait_time)

            wait_time=max(wait_time, self._min_wait_time)            

            self._uart_rx_manager.set_pending_state(command)

            # Send command
            self._uart.write(command + "\r")
            self._uart.flush()

            Logging.log_debug("Command sent!")

            '''
            Logging.log_debug("Command sent, sleeping before waiting...")
            time.sleep(10)
            Logging.log_debug("Command sent, awake before waiting...")
            '''

            response, success = self._uart_rx_manager.read_response(wait_time)
            if not success:
                raise Exception(f"Received an ERROR response: {response}")
            
            Logging.log_debug(f"Received response: {response}")

            return response
        except TimeoutException as te:
            Logging.log_error(f"Timeout occurred while sending command '{command}'")
            raise TimeoutException()
        except Exception as e:
            Logging.log_error(f"Error sending command '{command}': \"{e}\"")
            raise Exception(f"Error sending command '{command}'")

    def _check_network_registration(self) -> bool:
        """Check if device is registered to network"""
        try:
            response = self._send_command("AT+CREG?")
            # Parse: +CREG: <n>,<stat>
            for line in response.split('\n'):
                if '+CREG:' in line:
                    parts = line.replace('+CREG:', '').strip().split(',')
                    if len(parts) >= 2:
                        status = int(parts[1])
                        # 1=registered home, 5=registered roaming
                        self._network_registered = status in [1, 5]
                        return self._network_registered
        except Exception as e:
            Logging.log_error(f"Error checking network registration: \"{e}\"")
            return False

    def _enable(self):
        """
        Enable sender module with improved error handling and recovery
        """
        if not self.ENABLED:
            Logging.log_info("Enabling EG91 sender module...")

            for attempt in range(self.MAX_RETRIES):
                try:

                    # Clear rx buffer
                    self._uart_rx_manager.clear_state()

                    # Basic configuration with error checking
                    Logging.log_debug("Starting basic configuration")

                    # Test basic communication
                    response = self._send_command(command="AT")

                    # Disable echo
                    response = self._send_command(command="ATE0")

                    '''
                    # Configure context
                    self._send_command_interrupt(
                        f'AT+CGDCONT={self.PDP_CTXID},"IP","{self._apn}"')
                    '''

                    # Check SIM status
                    response = self._send_command("AT+CPIN?")
                    if "READY" not in response:
                        raise RuntimeError("SIM not ready")

                    # Check network registration
                    if not self._check_network_registration():
                        Logging.log_info("Waiting for network registration...")
                        time.sleep(5)
                        if not self._check_network_registration():
                            raise RuntimeError("Network registration failed")

                    Logging.log_info("Successfully connected to the cellular network")

                    self._activate_pdp_context()

                    Logging.log_info("EG91 sender module enabled successfully")

                    self.ENABLED=True

                    return True
                except TimeoutException as te:
                    if attempt < self.MAX_RETRIES - 1:
                        Logging.log_error(f"Enable attempt {attempt + 1} failed: \"{te}\".")
                        self._hard_reset()
                        Logging.log_info("Retrying to enable EG91 sender module...")
                    else:
                        Logging.log_error(f"EG91 enabling failed: \"{te}\"")
                        return False
                except Exception as e:
                    if attempt < self.MAX_RETRIES - 1:
                        Logging.log_error(f"Enable attempt {attempt + 1} failed: \"{e}\".")
                        #if not self._soft_reset():
                        #    self._hard_reset()
                        self._hard_reset()
                        Logging.log_info("Retrying to enable EG91 sender module...")
                    else:
                        Logging.log_error(f"EG91 enabling failed: \"{e}\"")
                        return False
        return True
    
    def _activate_pdp_context(self) -> bool:
        
        
        try:
            # Configure APN
            apn_cmd = f'AT+QICSGP={self.PDP_CTXID},1,"{self._apn}","","",1'
            response = self._send_command(apn_cmd)
        except Exception as e:
            raise RuntimeError("APN configuration failed")
        
        '''
        response, success = self._send_command_interrupt('AT+CFUN=1,1')
        if not success:
            Logging.log_error("Failed to set full functionality")
            return None
        '''

        try:
           # Verify activation
            response = self._send_command("AT+QIACT?")
        except Exception as e:
            raise RuntimeError("Context verification failed")
            
        try:
           # Activate context
            response = self._send_command(f"AT+QIACT={self.PDP_CTXID}")
        except Exception as e:
            raise RuntimeError("Context activation failed")

        try:
           # Verify activation
            response = self._send_command("AT+QIACT?")
        except Exception as e:
            raise RuntimeError("Context verification failed")

    def _deactivate_pdp_context(self) -> bool:

        try:
           # Deactivate context
            response = self._send_command(f'AT+QIDEACT={self.PDP_CTXID}')
        except Exception as e:
            raise RuntimeError("Context deactivation failed")                

    def _disable(self):
        try:
            if self.ENABLED:
                Logging.log_info("Disabling EG91 sender module")
                
                # Deactivate context
                self._deactivate_pdp_context()

                self.ENABLED=False

                Logging.log_info("EG91 sender module disabled successfully")
            else:
                Logging.log_info("EG91 sender module already disabled")
        except Exception as e:
            Logging.log_error(f"Error disabling EG91 sender module: \"{e}\"")
            self.ENABLED=False

    def get_time(self):
        """
        Query the current GMT time with improved error handling,
        restituisce <time> senza DST e il valore DST separato.
        """
        try:
            try:
                resp = self._send_command("AT+QLTS=2")  #=1 for GMT/UTC time, =2 for local time
            except Exception as e:
                raise RuntimeError("QLTS command failed")

            lines = resp.splitlines()
            for line in lines:
                if "+QLTS" in line:
                    # Pulizia stringa
                    time_str = line.replace('+QLTS: "', "").replace('"', "").replace("OK", "").strip()
                    
                    # Separiamo i campi
                    parts = time_str.split(',')
                    time_only = ','.join(parts[:2])  # '2026/01/22,12:35:25+04'
                    dst_flag = parts[2]               # '1' o '0'

                    return time_only, dst_flag

            return None, None

        except Exception as e:
            Logging.log_error(f"Time command failed: \"{e}\"")
            return None, None

    def https_get_request(self, url: str, timeout=80, headers=None) -> str:
        """
        Send HTTPS GET request

        :param str url: url string with format "https://xxx.xxx.xxx/file"
        :param int timeout: timeout to get and read HTTPS response in seconds
        :return str | None: string containing http response or None if any error occurs
        """
        try:

            Logging.log_info(f"Starting HTTPS GET request to: {url}")

            response=None
            
            try:
                # Configure the PDP context ID
                response = self._send_command(f'AT+QHTTPCFG="contextid",{self.PDP_CTXID}')
            except Exception as e:
                raise Exception("Failed to configure HTTP context ID")

            if headers:
                try:
                    # Allow to output HTTPS request header
                    response = self._send_command('AT+QHTTPCFG="requestheader",1')
                except Exception as e:
                    raise Exception("Failed to configure request header")

            try:
                # Set ssl context id
                response = self._send_command(f'AT+QHTTPCFG="sslctxid",{self.HTTPS_SSL_CTXID}')
            except Exception as e:
                raise Exception("Failed to configure SSL context ID")
            
            '''
            # Set SSL version as 3 which means TLS1.2
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="sslversion",{self.HTTPS_SSL_CTXID},3')
            if not success:
                Logging.log_error("Failed to configure SSL version")
                return None
            
            
            # Set SSL cipher suite as 0x0005 which means RC4-SHA
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="ciphersuite",{self.HTTPS_SSL_CTXID},0xFFFF')
            if not success:
                Logging.log_error("Failed to configure SSL cipher suite")
                return None
                
            # Set SSL verify level as 0 which means CA certificate is not needed
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="seclevel",{self.HTTPS_SSL_CTXID},0')
            if not success:
                Logging.log_error("Failed to configure SSL security level")
                return None
            '''

            try:
                # Set Server Name Indication feature 
                response = self._send_command(f'AT+QSSLCFG="sni",{self.HTTPS_SSL_CTXID},1')  #1 to enable SNI, 0 to disable
            except Exception as e:
                raise Exception("Failed to configure SSL Server Name Indication feature")
            
            try:
                # Set the URL which will be accessed
                response = self._send_command(f'AT+QHTTPURL={len(url)},{timeout}') 
            except Exception as e:
                raise Exception("Failed to set HTTPS URL")
            
            try:
                # Send the URL which will be accessed
                response  = self._send_command(url) 
            except Exception as e:
                raise Exception("Failed to send URL")

            
            if headers:
                regex_path = re.compile(self.URL_REGEX)

                match = regex_path.search(url)
                host=None
                path=None
                header_str=""

                if match:
                    host = match.group(1)
                    try:
                        path = match.group(3) if match.group(3) else "/"
                    except Exception as e:
                        path = "/"
                
                    header_str=f'GET {path} HTTP/1.1\r\n'
                    header_str+=f'Host: {host}\r\n'
                    header_str+='User-Agent: QUECTEL_MODULE\r\n'
                    header_str+='Accept: */*\r\n'
                    header_str+='Content-Length: 0\r\n'

                    for key, value in headers.items():
                        header_str+=f"{key}: {value}\r\n"
                    header_str+='\r\n'
                    
                else:
                    raise Exception("Invalid URL format or unsupported headers")
                
                qhttpget_cmd = f'AT+QHTTPGET={timeout},{len(header_str)}'
                
            else:
                qhttpget_cmd = f'AT+QHTTPGET={timeout}'
            
            try:
                # Send HTTPS GET request
                response = self._send_command(qhttpget_cmd)   
            except Exception as e:
                raise Exception(f"Failed to initiate HTTPS GET")

            if headers:
                try:
                    response = self._send_command(header_str)
                except Exception as e:
                    raise Exception("Failed to send URL headers")
            
            match = re.search(r"\+QHTTPGET: \d+,(\d+)", response)
            if not match:
                raise Exception("Failed to parse HTTPS GET response")
            http_status = int(match.group(1))
            if http_status <200 or http_status >=300:
                Logging.log_error(f"HTTPS GET request failed (STATUS CODE: {http_status}): {response}")
                raise Exception(f"STATUS CODE: {http_status}")
            else:
                Logging.log_info(f"HTTPS GET request succeeded with status code: {http_status}")
            
            
            try:
                response = self._send_command(f'AT+QHTTPREAD={timeout}')
            except Exception as e:
                raise Exception("Failed to read HTTPS GET response")
            
            # Cleanup connection
            self.close_https_connection()

            cleaned_response = response.replace("CONNECT", "").replace("OK", "").replace("+QHTTPREAD: 0", "").strip()
            Logging.log_info("HTTPS GET request completed successfully")

            return cleaned_response

        except Exception as e:
            Logging.log_error(f"HTTPS GET request error: \"{e}\"")
            self.close_https_connection()
            return None

    def https_post_request(self, url: str, body, timeout=80, content_type="text/plain", headers=None) -> str:
        """

        :param str url: url string with format https://xxx.xxx.xxx/file
        :param body: POST request body
        :param int timeout: timeout to get and read HTTPS response in seconds
        :param str content_type: supported content types are: 
                application/x-www-form-urlencoded,
                text/plain, 
                application/octet-stream, 
                multipart/form-data,
                application/json,
                image/jpeg
        :return str | None: string containing http response or None if any error occurs
        """
        try:

            Logging.log_info(f"Starting HTTPS POST request to: {url}")

            response=None

            if content_type not in self.HTTP_CONTENT_TYPES:
                raise Exception(f"Unsupported content type: {content_type}")
            
            try:
                # Configure the PDP context ID
                response = self._send_command(f'AT+QHTTPCFG="contextid",{self.PDP_CTXID}')
            except Exception as e:
                raise Exception("Failed to configure HTTP context ID")

            if headers:
                try:
                    # Allow to output HTTPS request header
                    response = self._send_command('AT+QHTTPCFG="requestheader",1')
                except Exception as e:
                    raise Exception("Failed to configure request header")

            if not headers:
                try:
                    # Set content type
                    response = self._send_command(
                        f'AT+QHTTPCFG="contenttype",{self.HTTP_CONTENT_TYPES[content_type]}'
                    )
                except Exception as e:
                    raise Exception("Failed to configure content type")
            
            '''
            # Allow to output HTTPS response header
            response, success = self._send_command_interrupt('AT+QHTTPCFG="responseheader",1')
            if not success:
                Logging.log_error("Failed to configure response header")
                return None
            '''

            try:
                # Set ssl context id
                response = self._send_command(f'AT+QHTTPCFG="sslctxid",{self.HTTPS_SSL_CTXID}')
            except Exception as e:
                raise Exception("Failed to configure SSL context ID")
            
            '''
            # Set SSL version as 3 which means TLS1.2
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="sslversion",{self.HTTPS_SSL_CTXID},3')
            if not success:
                Logging.log_error("Failed to configure SSL version")
                return None
            
            
            # Set SSL cipher suite as 0x0005 which means RC4-SHA
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="ciphersuite",{self.HTTPS_SSL_CTXID},0xFFFF')
            if not success:
                Logging.log_error("Failed to configure SSL cipher suite")
                return None
                
            # Set SSL verify level as 0 which means CA certificate is not needed
            response, success = self._send_command_interrupt(f'AT+QSSLCFG="seclevel",{self.HTTPS_SSL_CTXID},0')
            if not success:
                Logging.log_error("Failed to configure SSL security level")
                return None
            '''

            try:
                # Set Server Name Indication feature 
                response = self._send_command(f'AT+QSSLCFG="sni",{self.HTTPS_SSL_CTXID},1')  #1 to enable SNI, 0 to disable
            except Exception as e:
                raise Exception("Failed to configure SSL Server Name Indication feature")
            
            try:
                # Set the URL which will be accessed
                response = self._send_command(f'AT+QHTTPURL={len(url)},{timeout}')
            except Exception as e:
                raise Exception("Failed to set HTTPS URL")
            
            try:
                # Send the URL which will be accessed
                response  = self._send_command(url) 
            except Exception as e:
                raise Exception("Failed to send URL")

            if headers:
                regex_path = re.compile(self.URL_REGEX)

                match = regex_path.search(url)
                host=None
                path=None
                header_str=""

                if match:
                    host = match.group(1)
                    try:
                        path = match.group(3) if match.group(3) else "/"
                    except Exception as e:
                        path = "/"
                
                    header_str=f'POST {path} HTTP/1.1\r\n'
                    header_str+=f'Host: {host}\r\n'
                    header_str+='User-Agent: QUECTEL_MODULE\r\n'
                    header_str+='Accept: */*\r\n'
                    header_str+=f'Content-Type: {content_type}\r\n'
                    header_str+=f'Content-Length: {len(body)}\r\n'

                    for key, value in headers.items():
                        header_str+=f"{key}: {value}\r\n"
                    header_str+='\r\n'
                    body=header_str.encode()+body
                    
                else:
                    raise Exception("Invalid URL format or unsupported headers")
                

            try:
                # Send HTTPS POST request
                response = self._send_command(f'AT+QHTTPPOST={len(body)},{timeout},{timeout}',wait_time=10000) 
            except Exception as e:
                raise Exception(f"Failed to initiate HTTPS POST")

            try:
                # Send POST body
                response = self._send_https_post_body(body)
                match = re.search(r"\+QHTTPPOST: \d+,(\d+)", response)
                if not match:
                    raise Exception("Failed to parse HTTPS POST response")
                http_status = int(match.group(1))
                if http_status <200 or http_status >=300:
                    Logging.log_error(f"HTTPS POST request failed (STATUS CODE: {http_status}): {response}")
                    raise Exception(f"STATUS CODE: {http_status}")
                else:
                    Logging.log_info(f"HTTPS POST request succeeded with status code: {http_status}")
            except Exception as e:
                    raise Exception("Failed to send HTTPS POST body")
            
            try:
                response = self._send_command(f'AT+QHTTPREAD={timeout}')
            except Exception as e:
                raise Exception("Failed to read HTTPS POST response")
            
            # Cleanup connection
            self.close_https_connection()

            cleaned_response = response.replace("CONNECT", "").replace("OK", "").replace("+QHTTPREAD: 0", "").strip()
            Logging.log_info("HTTPS POST request completed successfully")

            return cleaned_response

        except Exception as e:
            Logging.log_error(f"HTTPS POST request error: \"{e}\"")
            self.close_https_connection()
            return None

    def _send_https_post_body(self, body: str):
        try:

            response=None
            success=False

            command="POST_BODY"

            wait_time = self.MAX_RESP_TIME.get(command, 5000)
            wait_time = max(wait_time, self._min_wait_time)       

            self._uart_rx_manager.set_pending_state(command)

            # Send body data in chunks
            Logging.log_debug("Sending POST body data...")
            chunk_size = 128
            body_bytes = body.encode('utf-8') if isinstance(body, str) else body
            
            for i in range(0, len(body_bytes), chunk_size):
                chunk = body_bytes[i:i+chunk_size]
                self._uart.write(chunk)
                self._uart.flush()
                time.sleep_ms(10)  # Small delay between chunks


            Logging.log_debug("Body sent!")

            response, success = self._uart_rx_manager.read_response(wait_time)
            if not success:
                raise Exception(f"Received an ERROR response: {response}")
            
            Logging.log_debug(f"Received response: {response}")

            return response
        except Exception as e:
            Logging.log_error(f"Error sending HTTPS POST body: \"{e}\"")
            raise Exception("Error sending HTTPS POST body")

    def close_https_connection(self):
        """
        Close HTTPS connection
        """
        try:
            Logging.log_debug("Closing HTTPS connection")
            
            # Stop HTTP service
            response = self._send_command("AT+QHTTPSTOP")

            Logging.log_debug("HTTPS connection closed")
            
        except Exception as e:
            Logging.log_error("Error occured while closing HTTPS connection")


    


