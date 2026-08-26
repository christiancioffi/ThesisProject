import re

from Logging import Logging


class ATCommandsParser():


    _ip_pattern=f"(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d){"(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d))"*3}"
    _simple_ip_pattern=f"\d(\d)?(\d)?{"\.\d(\d)?(\d)?"*3}"
    _simplest_ip_pattern=f"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"
    _time_pattern="\d\d\d\d/(0[1-9]|1[0-2])/(0[1-9]|[12]\d|3[01]),([01]\d|2[0-3]):([0-5]\d):([0-5]\d)[+-]\d\d"

    COMMAND_PATTERNS=[  # command : command pattern
        ("AT","AT"),
        ("ATE0","ATE0"),
        ("AT+QPOWD[=<n>]","AT\+QPOWD(=\d)?"),
        ("AT+CREG?","AT\+CREG\?"),
        ("AT+CPIN?","AT\+CPIN\?"),
        ("AT+CFUN=<fun>[,<rst>]","AT\+CFUN=[0-5](,[0-1])?"),
        ("AT+QICSGP=<contextID>[,<context_type>,<APN>[,<username>,<password>[,<authentication>]]]","AT\+QICSGP=\d(\d)?(,[1-3],\".*\"(,\".*\",\".*\"(,[0-3])?)?)?"),
        ("AT+QIACT?","AT\+QIACT\?"),
        ("AT+QIACT=<contextID>","AT\+QIACT=\d(\d)?"),
        ("AT+QIDEACT=<contextID>","AT\+QIDEACT=\d(\d)?"),
        ("AT+QLTS=<mode>","AT\+QLTS=[0-2]"),
        ("AT+QHTTPCFG","AT\+QHTTPCFG=\".+\"(,\d(\d)?)"),
        ("AT+QSSLCFG=\"sni\",<sslctxID>[,<sni>]","AT\+QSSLCFG=\"sni\",\d(\d)?(,[0-1])?"),
        ("AT+QHTTPURL=<URL_length>[,<timeout>]","AT\+QHTTPURL=\d+(,\d+)?"),
        ("AT+QHTTPGET[=<rsptime>]","AT\+QHTTPGET(=\d+)?"),  #Caso requestheader=0 di QHTTPGET
        ("AT+QHTTPGET=<rsptime>,<data_length>[,<input_time>]","AT\+QHTTPGET=\d+,\d+(,\d+)?"),   #Caso requestheader=1 di QHTTPGET
        ("GET_HEADERS","GET /[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]* HTTP/1\.1\r\n([A-Za-z0-9!#$%&'*+.^_`|~-]+:[ \t]*[^\r\n]+?\r\n)+?\r\n"),
        ("AT+QHTTPREAD[=<wait_time>]","AT\+QHTTPREAD(=\d+)?"),
        ("AT+QHTTPPOST=<data_length>[,<input_time>,<rsptime>]","AT\+QHTTPPOST=\d+(,\d+,\d+)?"),
        ("AT+QHTTPSTOP","AT\+QHTTPSTOP"),
        ("URL","https?://([a-zA-Z0-9-\.]+)(:[0-9]+)?(/[a-zA-Z0-9\-._~!$&'()*+,;=:@/?#%]*)?"),
        ("POST_BODY",".+")
    ]

    RESPONSE_PATTERNS={ # command : response pattern
        'AT' : "AT\r\r\nOK\r\n",
        'ATE0': "ATE0\r\r\nOK\r\n",
        "AT+QPOWD[=<n>]": "\r\nOK\r\n\r\nPOWERED DOWN\r\n",
        'AT+CREG?': f"\r\n\+CREG: \d+,\d+(,\"{"[0-9a-f]"*4}\",\"{"[0-9a-f]"*4}{"[0-9a-f]?"*3}\"(,\d+)?)?\r\n\r\nOK\r\n",
        'AT+CPIN?': "\r\n\+CPIN: (.+)\r\n\r\nOK\r\n",
        "AT+CFUN=<fun>[,<rst>]": "\r\nOK\r\n",
        'AT+QICSGP=<contextID>[,<context_type>,<APN>[,<username>,<password>[,<authentication>]]]': "\r\nOK\r\n",
        'AT+QIACT?': f"(\r\n\+QIACT: \d(\d)?,\d,\d(,\"{_simplest_ip_pattern}\")?\r\n)?\r\nOK\r\n",
        'AT+QIACT=<contextID>': "\r\nOK\r\n",
        'AT+QIDEACT=<contextID>': "\r\nOK\r\n",
        'AT+QLTS=<mode>': f"\r\n\+QLTS: \"{_time_pattern},\d\"\r\n\r\nOK\r\n",
        'AT+QHTTPCFG': "\r\nOK\r\n",
        'AT+QSSLCFG="sni",<sslctxID>[,<sni>]': "\r\nOK\r\n",
        'AT+QHTTPURL=<URL_length>[,<timeout>]': "\r\nCONNECT\r\n",
        'AT+QHTTPGET[=<rsptime>]': "\r\nOK\r\n\r\n\+QHTTPGET: \d+(,\d+(,\d+)?)?\r\n",   #Caso requestheader=0 di QHTTPGET
        'AT+QHTTPGET=<rsptime>,<data_length>[,<input_time>]': "\r\nCONNECT\r\n",    #Caso requestheader=1 di QHTTPGET
        'GET_HEADERS': "(\r\nOK\r\n\r\n\+QHTTPGET: \d+(,\d+(,\d+)?)?\r\n)",
        'AT+QHTTPREAD[=<wait_time>]': "\r\nCONNECT\r\n(.*?)\r\nOK\r\n\r\n\+QHTTPREAD: \d+\r\n",   #.+?
        'AT+QHTTPPOST=<data_length>[,<input_time>,<rsptime>]': "\r\nCONNECT\r\n",
        'AT+QHTTPSTOP': "\r\nOK\r\n",
        'URL': "\r\nOK\r\n",
        'POST_BODY': "\r\nOK\r\n\r\n\+QHTTPPOST: \d+(,\d+(,\d+)?)?\r\n",
        "ERROR": "\r\n(\+CME ERROR: \d+)|(ERROR)\r\n"
    }

    def __init__(self):
        self.COMMAND_PATTERNS = [
            (cmd, re.compile("^" + pattern + "$"))
            for cmd, pattern in self.COMMAND_PATTERNS
        ]

        self.RESPONSE_PATTERNS = {
            command: re.compile(pattern)
            for command, pattern in self.RESPONSE_PATTERNS.items()
        }

    def parse_response(self, command: str, data: str):
        try:
            #Logging.log_debug("Parsing data: {}".format(data))
            recognized_command=self._get_recognized_command(command)
            Logging.log_debug(f"Command pattern identified: {recognized_command}")
            response_pattern=self.RESPONSE_PATTERNS.get(recognized_command, None)
            response=None
            error=None

            if response_pattern is None:
                Logging.log_error(f"Unknown command: {command}")
                return None, None
            
            matched_response=response_pattern.search(data)

            if matched_response:
                Logging.log_debug(f"Matched response: {matched_response.group(0)}")
                response = matched_response.group(0)
            else:
                error_pattern=self.RESPONSE_PATTERNS["ERROR"]
                matched_error=error_pattern.search(data)
                if matched_error:
                    Logging.log_debug(f"Matched error response: {matched_error.group(0)}")
                    error= matched_error.group(0)
                else:
                    Logging.log_debug("No matching response or error found")
            
            return response, error
        except Exception as e:
            raise Exception(f"Error while parsing response for command '{command}': {str(e)}")
    
    
    def _get_recognized_command(self, command: str) -> str:
        for cmd, pattern in self.COMMAND_PATTERNS:
            if pattern.match(command):
                return cmd
        return None