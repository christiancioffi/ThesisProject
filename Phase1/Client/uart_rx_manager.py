import _thread
import asyncio
from Logging import Logging
from at_commands_parser import ATCommandsParser
import time
from timeout_exception import TimeoutException

class UARTRXManager():

    def __init__(self, uart_obj, max_buffer_size):
        try:
            self._uart = uart_obj
            self._last_response=""
            self._response_ready = False
            self._command_pending = False
            self._is_response_erroneous = False
            self._current_command = None
            self._reception_event=asyncio.ThreadSafeFlag()
            self._data_ready_event=asyncio.Event()
            self._parser=ATCommandsParser()
            self._max_buffer_size=max_buffer_size
            self._uart_listener_task=asyncio.create_task(self._uart_listener())
            Logging.log_debug(f"UART listener task (ID: {id(self._uart_listener_task)}) created successfully")
        except Exception as e:
            raise Exception(f"Failed to initialize UART RX Manager: \"{e}\"")
        
    
    def deinit(self):
        try:
            if self._uart_listener_task.cancel():
                Logging.log_debug(f"UART listener task (ID: {id(self._uart_listener_task)}) cancelled successfully")
            else:
                Logging.log_error(f"Failed to cancel UART listener task (ID: {id(self._uart_listener_task)})")
        except Exception as e:
            Logging.log_error(f"Error while cancelling UART listener task (ID: {id(self._uart_listener_task)}): \"{e}\"")
    
    async def _uart_listener(self):
        while True:
            try:
                await self._reception_event.wait()
                try:
                    Logging.log_debug(f"UART listener task (ID: {id(asyncio.current_task())}) awakened")
                    num_bytes = self._uart.any()
                    if num_bytes > 0:

                        raw_data = self._uart.read(self._max_buffer_size)

                        if raw_data:
                            Logging.log_info(f"({self._uart_listener.__name__}) Data received!")
                            Logging.log_info(f"({self._uart_listener.__name__}) {len(raw_data)} bytes read from UART")
                            Logging.log_debug(f"({self._uart_listener.__name__}) Raw data: {raw_data}")

                            data=""

                            try:
                                data=raw_data.decode('utf-8')
                            except Exception as e:
                                data=''.join(chr(b) for b in raw_data)

                            if self._command_pending:

                                response, error = self._parser.parse_response(self._current_command, data)

                                if response:
                                    self._last_response = response
                                    self._response_ready = True
                                    self._is_response_erroneous = False
                                    Logging.log_debug(f"({self._uart_listener.__name__}) Pending command response detected")
                                    self._data_ready_event.set()
                                if error:
                                    self._last_response = error
                                    self._response_ready = True
                                    self._is_response_erroneous = True
                                    Logging.log_debug(f"({self._uart_listener.__name__}) Pending command error detected")
                                    self._data_ready_event.set()
                            else:
                                Logging.log_debug(f"({self._uart_listener.__name__}) No command pending, data ignored")
                        else:
                            Logging.log_debug(f"({self._uart_listener.__name__}) No data read from UART")
                    else:
                        Logging.log_debug(f"({self._uart_listener.__name__}) No data available in UART")
                except Exception as e:
                    Logging.log_error(f"UART RX data handler error: \"{e}\"")
            except Exception as e:
                Logging.log_error(f"Task couldn't wait for other data: \"{e}\"")
                break

    def awake_uart_listener(self):
        Logging.untraced_log_debug("Awakening UART listener task...")
        self._reception_event.set()

    def _reset_status(self):
        self._last_response = ""
        self._response_ready = False
        self._command_pending = False
        self._current_command = None
        self._is_response_erroneous = False
    
    
    def clear_state(self):
        try:
            self._uart.read(10000)
            self._reset_status()
        except Exception as e:
            Logging.log_error(f"Error while clearing RX Manager State: \"{e}\"")
    

    def set_pending_state(self, command: str):
        try:
                self._last_response = ""
                self._response_ready = False
                self._command_pending = True
                self._current_command = command
                self._is_response_erroneous=False
        except Exception as e:
            Logging.log_error(f"Error while initializing pending state: \"{e}\"")

    def read_response(self, wait_time):
        asyncio.run(self._wait_for_response(wait_time))

        self._data_ready_event.clear()         
        if self._response_ready:
            response = self._last_response
            success = not self._is_response_erroneous
            self._reset_status()
        else:                       #Timeout occurred
            self._reset_status()
            raise TimeoutException()

        return response, success
    

    async def _wait_for_response(self, timeout):
        try:
            Logging.log_debug(f"Waiting for response for {timeout} ms...")
            await asyncio.wait_for_ms(self._data_ready_event.wait(),timeout)
            Logging.log_debug("Waiting terminated")
        except asyncio.TimeoutError:
            Logging.log_error(f"TIMEOUT")