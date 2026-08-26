import time

from bus_service import I2cAdapter
from base_sensor import BaseSensor

REG_CONTROL_STATUS = 0x00

UNSEAL_KEY_1 = 0x0414
UNSEAL_KEY_2 = 0x3672


#/* ____________________STANDART COMMANDS____________________ */

CONTROL_CMD=0x00
AT_RATE_CMD=0x02
UNFILTERED_SOC=0x04
TEMPERATURE_CMD=0x06
VOLTAGE_CMD=0x08
FLAGS_CMD=0x0A
NOMINAL_AVALIABLE_CAPACITY_CMD=0x0C
FULL_AVALIABLE_CAPACITY_CMD=0x0E
REMAINING_CAPACITY_CMD=0x10
FULL_CHARGE_CAPACITY_CMD=0x12
AVERAGE_CURRENT_CMD=0x14
TIME_TO_EMPTY=0x16
FILTERED_FCC=0x18
STANDBY_CURRENT_CMD=0x1A
UNFILTERED_FCC=0x1C
MAX_LOAD_CURRENT_CMD=0x1E
UNFILTERED_RM=0x20
FILTERED_RM=0x22
AVERAGE_POWER_CMD=0x24
INTERNAL_TEMPERATURE_CMD=0x28
CYCLE_COUNT_CMD=0x2A
STATE_OF_CHARGE_CMD=0x2C
STATE_OF_HEALTH_CMD=0x2E
PASSED_CHARGE_CMD=0x34
DOD0_CMD=0x36
SELF_DISCHARGE_CURRENT_CMG=0x38

# /* ____________BQ27741-G1_CONTROL_CMD SUBCOMMANDS____________ */

CONTROL_STATUS_SUBCMD=0x0000
DEVICE_TYPE_SUBCMD=0x0001
FW_VERSION_SUBCMD=0x0002
HW_VERSION_SUBCMD=0x0003
PROTECTOR_VERSION_SUBCMD=0x0004
RESET_DATA_SUBCMD=0x0005
PREV_MACWRITE_SUBCMD=0x0007
CHEM_ID_SUBCMD=0x0008
BOARD_OFFSET_SUBCMD=0x0009
CC_OFFSET_SUBCMD=0x000A
CC_OFFSET_SAVE_SUBCMD=0x000B
DF_VERSION_SUBCMD=0x000C
SET_FULLSLEEP_SUBCMD=0x0010
SET_SHUTDOWN_SUBCMD=0x0013
CLEAR_SHUTDOWN_SUBCMD=0x0014
SET_HDQINTEN_SUBCMD=0x0015
CLEAR_HDQINTEN_SUBCMD=0x0016
STATIC_CHEM_CHKSUM_SUBCMD=0x0017
ALL_DF_CHKSUM_SUBCMD=0x0018
STATIC_DF_CHKSUM_SUBCMD=0x0019
SEALED_SUBCMD=0x0020
IT_ENABLE_SUBCMD=0x0021
START_FET_TEST_SUBCMD=0x0024
CAL_ENABLE_SUBCMD=0x002D
RESET_SUBCMD=0x0041
EXIT_CAL_SUBCMD=0x0080
ENTER_CAL_SUBCMD=0x0081
OFFSET_CAL_SUBCMD=0x0082

#/* ____________________EXTENDED COMMANDS____________________ */

PACK_CONFIG_EXT_CMD = 0x3A # to 0x3B
DESIGN_CAPACITY_EXT_CMD = 0x3C # to 0x3D

DATA_FLASH_CLASS_EXT_CMD = 0x3E
DATA_FLASH_BLOCK_EXT_CMD = 0x3F
BLOCK_DATA_EXT_CMD = 0x40 # to 0x5F
BLOCK_DATA_CHECKSUM_EXT_CMD = 0x60
BLOCK_DATA_CONTROL_EXT_CMD = 0x61

DEVICE_NAME_LENGTH_EXT_CMD = 0x62
DEVICE_NAME_EXT_CMD = 0x63 # to 0x6C


# Configuration values
PACK_CONFIG_VALUE = 0b0000000101110000
DESIGN_CAPACITY_VALUE = 6000

class SocDriver(BaseSensor):
    def __init__(self, adapter: I2cAdapter, config: dict):
        try:
            
            #check the correct adapter type
            if config["adapter"] != "i2c" or not isinstance(adapter, I2cAdapter):
                self.log_error("invalid adapter type, must be i2c")
                raise ValueError("Invalid adapter type, must be i2c")

            BaseSensor.__init__(self, adapter, int(config["i2c_address"], 16), False)
            
            self.registry = config["registry"]
    
            self.status = {}
            self.flags = {}

        except KeyError as key:
            raise ValueError(f"Error config value not found: {key}")
        

    def __del__(self):
        raise NotImplementedError("Method not implemented")

    def get_data(self) -> dict:
        self.get_raw_data()
        
        self.ai =  self.unsigned_to_signed_16bit(self.exec_function(AVERAGE_CURRENT_CMD))
        self.volt = self.exec_function(VOLTAGE_CMD)
        self.soc = self.exec_function(STATE_OF_CHARGE_CMD)
        self.nac = self.exec_function(NOMINAL_AVALIABLE_CAPACITY_CMD)
        self.fac = self.exec_function(FULL_AVALIABLE_CAPACITY_CMD)
        self.rm = self.exec_function(REMAINING_CAPACITY_CMD)
        self.ap = self.unsigned_to_signed_16bit(self.exec_function(AVERAGE_POWER_CMD))
        self.soh = self.exec_function(STATE_OF_HEALTH_CMD)
        temp =  self.exec_function(TEMPERATURE_CMD)
        self.temperature = temp * 0.1 - 273.15
        self.cycle_count = self.exec_function(CYCLE_COUNT_CMD)
        self.passed_charge = self.unsigned_to_signed_16bit(self.exec_function(PASSED_CHARGE_CMD))
        self.dod0 = self.exec_function(DOD0_CMD)
        self.discharge_current = self.exec_function(SELF_DISCHARGE_CURRENT_CMG)
        self.fcc = self.exec_function(FULL_CHARGE_CAPACITY_CMD)

        self._read_status()
        self.log_info(f"Status: {self.status}")

        self._read_flags()
        self.log_info(f"Flags(): {self.flags}")
        self.log_info(f"raw volt: {self.raw_volt}")
        self.log_info(f"raw ampere: {self.raw_amp}")
        self.log_info(f"soc (StateOfCharge()): {self.soc} %")
        self.log_info(f"ai (AverageCurrent()): {self.ai} mA")
        self.log_info(f"volt (Voltage()) : {self.volt} mV")
        self.log_info(f"nac (NominalAvailableCapacity): {self.nac} mAh")
        self.log_info(f"fac (FullAvailableCapacity()): {self.fac} mAh")
        self.log_info(f"rm (RemainingCapacity()): {self.rm} mAh")
        self.log_info(f"ap (AveragePower()): {self.ap} mW")
        self.log_info(f"soh (StateOfHealth()): {self.soh} % / num ")
        self.log_info(f"temperature (Temperature()): {self.temperature} °C")
        self.log_info(f"cycle count (CycleCount()): {self.cycle_count}")
        self.log_info(f"passed_chg (PassedCharge()): {self.passed_charge} mAh")
        self.log_info(f"dod0 (Dod0()): {hex(self.dod0)}")
        self.log_info(f"discharge_current (SelfDischargeCurrent()): {self.discharge_current} mA")
        self.log_info(f"fcc (FullChargeCapacity()): {self.fcc} mAh")

        return [
            {"n":self.registry["v_raw"], "v": self.raw_volt},
            {"n":self.registry["a_raw"], "v": self.raw_amp},
            {"n":self.registry["ai"], "v":self.ai},
            {"n":self.registry["volt"], "v":self.volt},
            {"n":self.registry["soc"], "v":self.soc},
            {"n":self.registry["nom_capacity"], "v":self.nac},
            {"n":self.registry["full_available_capacity"], "v":self.fac},
            {"n":self.registry["remain_capacity"], "v":self.rm},
            {"n":self.registry["avg_power"], "v":self.ap},
            {"n":self.registry["soh"], "v":self.soh},
            {"n":self.registry["temperature"], "v":self.temperature},
            {"n":self.registry["cycle_count"], "v":self.cycle_count},
            {"n":self.registry["passed_charge"], "v":self.passed_charge},
            {"n":self.registry["dod0"], "v":self.dod0},
            {"n":self.registry["discharge_current"], "v":self.discharge_current},
            {"n":self.registry["full_charge_capacity"], "v":self.fcc},
        ]
    
    def set_data(self) -> bool:
        raise NotImplementedError("Method not implemented")
        #return False
    
    def start(self) -> bool:
        #self.board_offset_calibration()
        self._set_configuration()

        return True
    
    def stop(self) -> bool:
        raise NotImplementedError("Method not implemented")
        # return True


    def _set_configuration(self):
        pack_config = self.run_extended_command(PACK_CONFIG_EXT_CMD, 2)
        self.log_debug(f"PackConfig() = {self.to_fixed_bin(pack_config, 16)}")

        des_cap = self.run_extended_command(DESIGN_CAPACITY_EXT_CMD, 2)
        self.log_debug(f"DesignCapacity() = {des_cap} mAh")
        
        if pack_config != PACK_CONFIG_VALUE:
            #set pack config value
            self.write_data_to_flash(subclass_id=64, offset=0, bytes_count=2, value = PACK_CONFIG_VALUE)
            pack_config = self.run_extended_command(PACK_CONFIG_EXT_CMD, 2)
            self.log_debug(f"updated PackConfig() = {self.to_fixed_bin(pack_config, 16)}")

        
        if des_cap != DESIGN_CAPACITY_VALUE:
            #set design capacity
            self.write_data_to_flash(subclass_id=48, offset=23, bytes_count=2, value=DESIGN_CAPACITY_VALUE)
            des_cap = self.run_extended_command(DESIGN_CAPACITY_EXT_CMD, 2)
            self.log_debug(f"updated DesignCapacity() = {des_cap} mAh")

        self._read_status()
        if not self.status["VOK"] or not self.status["QEN"]:
            self.log_debug("running it enable")
            self._write_register(REG_CONTROL_STATUS, IT_ENABLE_SUBCMD)
            time.sleep(1)
            self.log_debug("done it enable")

            
    def write_qmax_cell_0(self):
        #Set Qmax Cell 0 to 6000mAh pag 75 SLUUAA3.pdf
        self.write_data_to_flash(subclass_id=82, offset=0, bytes_count=2, value=6000)



    def board_offset_calibration(self) -> bool:
        """
            Set board offset calibration, based on pag. 91 sluuuaa4.pdf.
        """
        if(not self._enter_calibration_mode()):
            return False
        
        self.log_debug("entered calibration mode")
        attempts = 0
        cca = False
        bca = False

        while (attempts<10 and not (cca and bca) ):
            self._write_register(REG_CONTROL_STATUS, BOARD_OFFSET_SUBCMD)
            time.sleep(1)
            self._read_status()
            cca = self.status["CCA"]
            bca = self.status["BCA"]
            attempts+=1 

        if not(cca and bca):
            return False
        
        attempts = 0
        while (attempts<10 and bca):
            time.sleep(1)
            self._read_status()
            bca = self.status["BCA"]
            cca = self.status["CCA"]
            attempts+=1 
            print(f" attmt{attempts} cca {cca}, bca {bca}")


        if cca :
            return False

        self._write_register(REG_CONTROL_STATUS, CC_OFFSET_SAVE_SUBCMD)
        time.sleep(1)
        if(not self._exit_calibration_mode()):
            return False
        
        self.log_debug("exited calibration mode")

        return True


    def cc_offset_calibration(self) -> bool:
        """
            Set CC offset calibration, based on pag. 89 sluuuaa4.pdf.
        """
        if(not self._enter_calibration_mode()):
            return False
        
        attempts = 0
        cca = 0
        while (attempts<5 and cca != 1):
            self._write_register(REG_CONTROL_STATUS, CC_OFFSET_SUBCMD)
            time.sleep_ms(100)
            self._read_status()
            cca = self.status["CCA"]
            attempts+=1 
        
        if cca != 1:
            return False
        
        attempts = 0
        while (attempts<5 and cca != 0):
            time.sleep(1)
            self._read_status()
            cca = self.status["CCA"]
            attempts+=1 

        if cca != 0:
            return False

        self._write_register(REG_CONTROL_STATUS, CC_OFFSET_SAVE_SUBCMD)
        
        if(not self._exit_calibration_mode()):
            return False
        
        return True
    
        
    def _enter_calibration_mode(self) -> bool:
        attempts = 0
        cal_mod = 0

        while (attempts<5 and cal_mod != 1):
            self._write_register(REG_CONTROL_STATUS, ENTER_CAL_SUBCMD)
            time.sleep_ms(100)
            self._read_status()
            cal_mod = self.status["CALMODE"]
            time.sleep_ms(100)

            attempts+=1 
        
        return cal_mod==1


    def _exit_calibration_mode(self) -> bool:
        attempts = 0
        cal_mod = 1

        while (attempts<5 and cal_mod != 0):
            self._write_register(REG_CONTROL_STATUS, EXIT_CAL_SUBCMD)
            time.sleep_ms(100)
            self._read_status()
            cal_mod = self.status["CALMODE"]

            attempts+=1 
        
        return cal_mod==0
    

    def _read_status(self):
        self._write_register(REG_CONTROL_STATUS, CONTROL_STATUS_SUBCMD)
        time.sleep_ms(100)
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        # Bits 15 to 0 (High Byte + Low Byte)
        key_order = [
            "RSVD1","FAS", "SS","CALMODE","CCA","BCA", "RSVD2","HDQHOSTIN",
            "SHUTDN_EN","FETTST","FULLSLEEP","SLEEP","LDMD","RUP_DIS","VOK","QEN"
        ]

        for i, key in enumerate(key_order):
            self.status[key] = bool((byte_value >> (15 - i)) & 0x01)


    def _read_flags(self):
        raw_flags = self._read_register(FLAGS_CMD, bytes_count=2)
        #print(bin(raw_flags))
        self.flags = {}

        key_order = [
            "OTC","OTD", "BATHI","BATLOW","CHG_INH","OT_FET", "FC","CHG",
            "OCVTAKEN","ISD","TDD","RSVD1","RSVD2","SOC1","SOCF","DSG"
        ]

        for i, key in enumerate(key_order):
            self.flags[key] = bool((raw_flags >> (15 - i)) & 0x01)

        
    def _write_register(self, reg_addr, value: int, bytes_count=2) -> int:
        """Write value in given register address."""
        try:
            byte_order = self._get_byteorder_as_str()[0]
            return self.adapter.write_register(self.address, reg_addr, value, bytes_count, byte_order)
        except Exception as e:
            self.log_error(f"Error while writing register {reg_addr} -> {e}")    


    def _read_register(self, reg_addr, bytes_count=2, byte_order = "little") -> int:
        try:
            data = self.adapter.read_register(self.address, reg_addr, bytes_count)
            if len(data) != bytes_count:
                raise ValueError(f"Incomplete register read: expected {bytes_count}, got {len(data)}")
            converted = int.from_bytes(data, byte_order)
            return converted
        
        except Exception as e:
            self.log_error(f"Error while reading register {reg_addr} -> {e}")
            return bytes([0x00] * bytes_count)


    def exec_function(self, func: int) -> int:
        bytes_val = self._read_register(func, 2)
        return bytes_val


    def get_raw_data(self):
        self._enter_calibration_mode()
        msb = self._read_register(0x7D, 1)
        lsb = self._read_register(0x7C, 1)
        self.raw_volt = (msb << 8 ) + lsb
        
        msb = self._read_register(0x7B, 1)
        lsb = self._read_register(0x7A, 1)
        
        self.raw_amp = self.unsigned_to_signed_16bit((msb << 8 ) + lsb)

        self._exit_calibration_mode()

        self.log_info(f"raw volt: {self.raw_volt}")

        return [
            {"n":self.registry["v_raw"], "v": self.raw_volt},
            {"n":self.registry["a_raw"], "v": self.raw_amp}
        ]

    @staticmethod
    def unsigned_to_signed_16bit(n):
        """
        Converts a 16-bit unsigned integer to a signed integer using two's complement.
        
        :param n: Integer (0 to 65535)
        :return: Signed integer (-32768 to 32767)
        """
        if not (0 <= n <= 0xFFFF):
            raise ValueError("Input must be a 16-bit unsigned integer (0 to 65535).")
        
        if n & 0x8000:  # if the sign bit is set (i.e., bit 15 == 1)
            return n - 0x10000
        else:
            return n
        

    def start_test(self):
        self._read_status()
        self.log_debug(f"Status: {self.status}")

        self.log_debug("#Device type")
        self._write_register(REG_CONTROL_STATUS, DEVICE_TYPE_SUBCMD)
        time.sleep_ms(100)
        msb = self._read_register(0x00, 1)
        lsb = self._read_register(0x01, 1)
        self.log_debug(f"msb: {hex(msb)}, lsb:{hex(lsb)}")
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        self.log_debug(f"Device type: dec:{byte_value}, bin:{bin(byte_value)}, hex:{hex(byte_value)}")

        self.log_debug("#Firmware type")
        self._write_register(REG_CONTROL_STATUS, FW_VERSION_SUBCMD)
        time.sleep_ms(100)
        msb = self._read_register(0x00, 1)
        lsb = self._read_register(0x01, 1)
        self.log_debug(f"msb: {hex(msb)}, lsb:{hex(lsb)}")
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        self.log_debug(f"Firmware version: dec:{byte_value}, bin:{bin(byte_value)}, hex:{hex(byte_value)}")

        self.log_debug("#Chem type")
        self._write_register(REG_CONTROL_STATUS, CHEM_ID_SUBCMD)
        time.sleep_ms(100)
        msb = self._read_register(0x00, 1)
        lsb = self._read_register(0x01, 1)
        self.log_debug(f"msb: {hex(msb)}, lsb:{hex(lsb)}")
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        self.log_debug(f"Chem type: dec:{byte_value}, bin:{bin(byte_value)}, hex:{hex(byte_value)}")
        
        self.log_debug("#Prev command type")
        self._write_register(REG_CONTROL_STATUS, PREV_MACWRITE_SUBCMD)
        time.sleep_ms(100)
        msb = self._read_register(0x00, 1)
        lsb = self._read_register(0x01, 1)
        self.log_debug(f"msb: {hex(msb)}, lsb:{hex(lsb)}")
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        self.log_debug(f"Prev mac cmd: dec:{byte_value}, bin:{bin(byte_value)}, hex:{hex(byte_value)}")

        
    def print_prev_cmd(self):
        self.log_debug("#prev command type")
        self._write_register(REG_CONTROL_STATUS, PREV_MACWRITE_SUBCMD)
        time.sleep_ms(100)
        msb = self._read_register(0x00, 1)
        lsb = self._read_register(0x01, 1)
        self.log_debug(f"msb: {hex(msb)}, lsb:{hex(lsb)}")
        byte_value = self._read_register(REG_CONTROL_STATUS, 2)
        time.sleep_ms(100)
        self.log_debug(f"prev mac cmd: dec:{byte_value}, bin:{bin(byte_value)}, hex:{hex(byte_value)}")


    def pretty_print_status(self):
        self.log_debug("###STATUS")
        self.log_debug(f"FAS = {self.status['FAS']} - Status bit indicating the fuel gauge is in FULL ACCESS SEALED state. Active when set (no data flash access).")
        self.log_debug(f"SS = {self.status['SS']} - Status bit indicating the fuel gauge is in the SEALED state. Active when set (no ROM access).")
        self.log_debug(f"CALMODE = {self.status['CALMODE']} - Status bit indicating the calibration function is active. True when set. Default is 0.")
        self.log_debug(f"CCA = {self.status['CCA']} - Status bit indicating the Coulomb Counter Calibration routine is active.")
        self.log_debug(f"BCA = {self.status['BCA']} - Status bit indicating the Board Calibration routine is active. Active when set.")
        self.log_debug(f"HDQHOSTIN = {self.status['HDQHOSTIN']} - Status bit indicating the HDQ interrupt function is active. True when set. Default is 0.")
        self.log_debug(f"SHUTDN_EN = {self.status['SHUTDN_EN']} - Control bit indicating SET_SHUTDOWN subcommand has been sent, signaling external shutdown when conditions permit.")
        self.log_debug(f"FETTST = {self.status['FETTST']} - Status bit indicating the state of the FET test. True when set. Default is 0.")
        self.log_debug(f"FULLSLEEP = {self.status['FULLSLEEP']} - Status bit indicating the fuel gauge is in FULLSLEEP mode. True when set.")
        self.log_debug(f"SLEEP = {self.status['SLEEP']} - Status bit indicating the fuel gauge is in SLEEP mode. True when set.")
        self.log_debug(f"LDMD = {self.status['LDMD']} - Status bit indicating use of constant-power model in Impedance Track™ algorithm. Default is 0 (constant-current model).")
        self.log_debug(f"RUP_DIS = {self.status['RUP_DIS']} - Status bit indicating the Ra table updates are disabled. True when set.")
        self.log_debug(f"VOK = {self.status['VOK']} - Status bit indicating cell voltages are OK for Qmax updates. True when set.")
        self.log_debug(f"QEN = {self.status['QEN']} - Status bit indicating Qmax updates are enabled. True when set.")


    def pretty_print_flags(self):
        self.log_debug("###FLAGS")
        self.log_debug(f"OTC = {self.flags['OTC']} - Over-Temperature in Charge condition is detected. True when set.")
        self.log_debug(f"OTD = {self.flags['OTD']} - Over-Temperature in Discharge condition is detected. True when set.")
        self.log_debug(f"BATHI = {self.flags['BATHI']} - Battery High bit indicating a high battery voltage condition.")
        self.log_debug(f"BATLOW = {self.flags['BATLOW']} - Battery Low bit indicating a low battery voltage condition.")
        self.log_debug(f"CHG_INH = {self.flags['CHG_INH']} - Charge Inhibit indicates the temperature is outside the range. True when set.")
        self.log_debug(f"OT_FET = {self.flags['OT_FET']} - Indicates when overtemperature condition has been reached. True when set.")
        self.log_debug(f"FC = {self.flags['FC']} - Full-charged is detected. Set when charge termination is reached. True when set.")
        self.log_debug(f"CHG = {self.flags['CHG']} - (Fast) charging allowed. True when set.")
        self.log_debug(f"OCVTAKEN = {self.flags['OCVTAKEN']} - Cleared on relax mode entry, set to 1 when OCV measurement occurs.")
        self.log_debug(f"ISD = {self.flags['ISD']} - Internal Short is detected. True when set.")
        self.log_debug(f"TDD = {self.flags['TDD']} - Tab Disconnect is detected. True when set.")
        self.log_debug(f"SOC1 = {self.flags['SOC1']} - State-of-Charge Threshold 1 (SOC1 Set) reached. True when set.")
        self.log_debug(f"SOCF = {self.flags['SOCF']} - State-of-Charge Threshold Final (SOCF Set %) reached. True when set.")
        self.log_debug(f"DSG = {self.flags['DSG']} - Discharging detected. True when set.")


    def get_stats(self):
        self._read_status()
        self._read_flags()
        self.pretty_print_status()
        self.pretty_print_flags()


    @staticmethod
    def to_fixed_bin(n, width):
        b = bin(n)[2:]  # remove '0b' prefix
        padding = width - len(b)
        if padding > 0:
            b = '0' * padding + b
        return b


    def run_extended_command(self, command:int, bytes_count:int) -> int:
        return self._read_register(command, bytes_count)


    # Data Flash Access - SLUUAA3.pdf - sez 5.1 - pag 40
    def write_data_to_flash(self, subclass_id:int, offset:int, bytes_count:int, value:int):

        #4.2.3  set subclass access
        self._write_register(DATA_FLASH_CLASS_EXT_CMD, subclass_id, bytes_count=1)

        #choose if first or second block: 0x00 first 32 bystes block, 0x01 second 32 bytes block
        self._write_register(DATA_FLASH_BLOCK_EXT_CMD, 0x00, bytes_count=1)

        #enable write, access configured block
        self._write_register(BLOCK_DATA_CONTROL_EXT_CMD, 0x00, bytes_count=1)

        #calc offset from block data
        address = BLOCK_DATA_EXT_CMD + offset

        #read old value
        block_data = self._read_register(address, bytes_count=bytes_count, byte_order="big")
        print("old_block_data")
        print(self.to_fixed_bin(block_data, 16))
        print(hex(block_data))

        #convert value
        value_big_order = int.from_bytes(value.to_bytes(bytes_count, "little"), "big" )
        self._write_register(address, value_big_order, bytes_count)

        updated_block_data = self._read_register(address, bytes_count=bytes_count, byte_order="big")
        print("updated_block_data")
        print(self.to_fixed_bin(updated_block_data, 16))
        print(hex(updated_block_data))

        #read all block
        full_block_data_bytes = self.adapter.read_register(self.address, BLOCK_DATA_EXT_CMD, bytes_count=32)
        print(full_block_data_bytes)
        checksum = self.calculate_checksum(full_block_data_bytes)
        print(hex(checksum))
        #write checksum 

        self._write_register(BLOCK_DATA_CHECKSUM_EXT_CMD, checksum, bytes_count=1)
        time.sleep(1)


    def data_memory_test(self):
        pack_config = self.run_extended_command(PACK_CONFIG_EXT_CMD, 2)
        self.log_debug(f"PackConfig() = {self.to_fixed_bin(pack_config, 16)}")

        des_cap = self.run_extended_command(DESIGN_CAPACITY_EXT_CMD, 2)
        self.log_debug(f"DesignCapacity() = {des_cap} mAh")


    @staticmethod
    def calculate_checksum(block_data: bytes) -> int:
        """
        Calculates the checksum for a 32-byte block of data.
        
        Args:
            block_data (bytes): A bytes object of 32 bytes.
            
        Returns:
            int: The checksum byte to write at address 0x60.
        """
        if len(block_data) != 32:
            raise ValueError("block_data must be exactly 32 bytes long")

        checksum = 255 - (sum(block_data) & 0xFF)
        return checksum
    
    def log_info(self, message: str):
        print(f"[SOC DRIVER] {message}")
    
    def log_error(self, message: str):
        RED     = "\033[31m"
        RESET   = "\033[0m"
        print(f"{RED}[SOC DRIVER ERROR] {message}{RESET}")
    
    def log_debug(self, message: str):
        YELLOW     = "\033[33m"
        RESET   = "\033[0m"
        print(f"{YELLOW}[SOC DRIVER DEBUG] {message}{RESET}")

'''
from machine import SoftI2C, Pin
from drivers.sensor_pack.bus_service import I2cAdapter
import ujson

if __name__ == '__main__':
    json_config="""{
        "adapter" : "i2c",
        "i2c_address": "0x55",
        "registry": {
            "v_raw": "106",
            "a_raw": "107",
            "ai": "108",
            "volt": "109",
            "soc": "110",
            "nom_capacity": "111",
            "full_available_capacity": "112",
            "remain_capacity": "114",
            "avg_power": "115",
            "soh": "118",
            "temperature": "119",
            "cycle_count": "120",
            "passed_charge": "121",
            "dod0": "122",
            "discharge_current": "123",
            "full_charge_capacity": "124"
        }
    }"""


    i2c = SoftI2C(scl=Pin(9), sda=Pin(8), freq=100000)
    adapter = I2cAdapter(i2c)
    config = ujson.loads(json_config)

    print(i2c.scan())
    
    soc = SocDriver(adapter, config)
    #soc.data_memory_test()
    #soc.start()
    soc.write_qmax_cell_0()
    while True:
        soc._read_status()
        soc._read_flags()
        soc.pretty_print_status()
        soc.pretty_print_flags()

        soc.get_data()
        time.sleep_ms(5000)

    
'''
    

