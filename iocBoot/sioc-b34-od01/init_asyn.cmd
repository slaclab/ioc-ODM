#==============================================================
#
#  Abs:  Initialize ModBus Asyn communications
#
#  Name: init_asyn.cmd
#
#  Facility:  SLAC PPS Controls
#
#  Auth: 20-May-2019, Anthony Andrews (AANDREWS)
#  Rev:  dd-mmm-yyyy, Reviewer's Name (USERNAME)
#--------------------------------------------------------------
#  Mod:
#        14-Oct-2019, K. Luchini      (LUCHINI):
#          extract from st.cmd
#
#==============================================================
#
# PILZ PLC ASYN Port Configuration
# Use the following commands for TCP/IP
#drvAsynIPPortConfigure(const char *portName,
#                       const char *hostInfo,
#                       unsigned int priority,
#                       int noAutoConnect,
#                       int noProcessEos);
drvAsynIPPortConfigure ("ODM_$(SECTOR)", "$(ODM_NODE):502"   ,0,0,0)
asynSetOption("ODM_$(SECTOR)",0,"disconnectOnReadTimeout", "Y")

# Initialize Pliz PLC MODBUS Interpose Layer
#modbusInterposeConfig(const char *portName, modbusLinkType linkType, int timeoutMsec, int writeDelayMsec)
modbusInterposeConfig ("ODM_$(SECTOR)", 0, 250, 0)
dbLoadRecords("${TOP}/db/asynRecord.db","P=PLC:$(SECTOR):OD01,R=:ASYNDRIVER,PORT=ODM_$(SECTOR),ADDR=0,IMAX=0,OMAX=0")


#drvModbusAsynConfigure(portName,
#                       tcpPortName,
#                       modbusFunction,
#                       modbusStartAddress,
#                       modbusLength,
#                       dataType,
#                       pollMsec,
#                       plcType);
#
# portName: Name of the modbus port to be created.
# tcpPortName: Name of the asyn IP or serial port previously created.
# modbusFunction: Modbus function code (1, 2, 3, 4, 5, 6, 15, or 16).
# modbusStartAddress: Start address for the Modbus data segment to be accessed.
#                     (0-65535 decimal, 0-0177777 octal).
# modbusLength: The length of the Modbus data segment to be accessed.
#               This is specified in bits for Modbus functions 1, 2, 5 and 15.
#               It is specified in 16-bit words for Modbus functions 3, 4, 6 and 16.
#               Length limit is 2000 for functions 1 and 2, 1968 for functions 5 and 15,
#               125 for functions 3 and 4, and 123 for functions 6 and 16.
# modbusDataType: Modbus data type:
#Supported Modbus data types
#modbusDataType value    drvUser field   Description
#                0       UINT16  Unsigned 16-bit binary integers
#                1       INT16SM         16-bit binary integers, sign and magnitude format.
#                2       BCD_UNSIGNED    Binary coded decimal (BCD), unsigned.
#                3       BCD_SIGNED      4-digit binary coded decimal (BCD), signed.
#                4       INT16   16-bit signed (2's complement) integers.
#                5       INT32_LE        32-bit integers, little endian
#                6       INT32_BE        32-bit integers, big endian
#                7       FLOAT32_LE      32-bit floating point, little endian
#                8       FLOAT32_BE      32-bit floating point, big endian
#                9       FLOAT64_LE      64-bit floating point, little endian
#                10      FLOAT64_BE
# pollMsec: Polling delay time in msec for the polling thread for read functions.
#           For write functions, a non-zero value means that the Modbus data should
#           be read once when the port driver is first created.
#  plcType: Type of PLC (e.g. Koyo, Modicon, etc.).
#           This parameter is currently used only to print information in asynReport.
#            In the future it could be used to modify the driver behavior for a specific PLC.
#drvModbusAsynConfigure(portName, tcpPortName, slaveAddress, modbusFunction, modbusStartAddress, modbusLength, dataType, pollMsec, plcType);

# PILZ PLC MODBUS ASYN Configuration for ODMs

## ODM
####XXXX With the PNOZ 2, switch to 16 bit word access instead of bit access. Should help with the bit ordering weirdness.
####drvModbusAsynConfigure ("ODM_RD",  "ODM", 1, 1,  0, 160,  0,  250, "PILZ")
####XXXX

drvModbusAsynConfigure ("ODM_$(SECTOR)_WDRD",  "ODM_$(SECTOR)", 1, 3,  0, 10,  0,   250, "PILZ")
####XXXX With the PNOZ 2, switch to 16 bit word access instead of bit access. Should help with the bit ordering weirdness.
# Switch from function code 5, write single coil (1 bit), to code 6, write single register (16 bit)
####drvModbusAsynConfigure ("ODM_WRT", "ODM", 1, 5,  16384 , 48,  0,  250, "PILZ")
####XXXX
drvModbusAsynConfigure ("ODM_$(SECTOR)_WDWT",  "ODM_$(SECTOR)", 1, 3,  0, 10,  0,   250, "PILZ")

# Oxigraf devices
#drvAsynIPPortConfigure( "OXI_CR11", "ts-b905-od01:2101", 0, 0, 0 )
#drvAsynIPPortConfigure( "OXI_CR12", "ts-b905-od02:2101", 0, 0, 0 )
#drvAsynIPPortConfigure( "OXI_CR01", "ts-b905-od03:2101", 0, 0, 0 )
#drvAsynIPPortConfigure( "OXI_CR02", "ts-b905-od04:2101", 0, 0, 0 )
#drvAsynIPPortConfigure( "OXI_CR21", "ts-b905-od05:2101", 0, 0, 0 )
#drvAsynIPPortConfigure( "OXI_CR22", "ts-b905-od06:2101", 0, 0, 0 )
#epicsEnvSet( "STREAM_PROTOCOL_PATH", "$(TOP)/db")

# End of script

