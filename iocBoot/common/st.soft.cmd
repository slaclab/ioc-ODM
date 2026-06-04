#==============================================================
#
#  Abs:  Generic piece of startup script for soft IOCs
#
#  Name: st.soft.cmd
#
#  Side: Upon entry to this script, the following macros
#        are expected:
#            IOC_NAME - IOC devices name (i.e. SIOC:LI20:KY01)
#
#        All other macros used within this script
#        are set from the file iocBoot/<ioc>/envPaths
#
#  Facility:  Personnel Protection System (PPS)
#
#  Auth: 22-Sep-2016, Kristi Luchini  (LUCHINI)
#  Rev:  dd-mmm-yyyy, Reviewer's Name (USERNAME)
#--------------------------------------------------------------
#  Mod:
#        14-Oct-2019, K. Luchini      (LUCHINI):
#         load script to setup caSecurity
#         and SUBSYS env var
#         load script to setup iocLog
#
#==============================================================
#
# Set environment variables
< envPaths

# iocAdmin environment variables
epicsEnvSet("ENGINEER"   , "Garth Brown")
epicsEnvSet("IOC_BOOT"   , "${TOP}/iocBoot/${IOC}")
epicsEnvSet("STARTUP"    , "${EPICS_IOCS}/${IOC}")
epicsEnvSet("ST_CMD"     , "startup.cmd")
epicsEnvSet("SUBSYS"     ,"odm")

# Change to the top of the app
cd ${TOP}

# Location of stream protocol files
epicsEnvSet("STREAM_PROTOCOL_PATH", "${TOP}/db")

#==============================================================
# Load IOC Application object
#==============================================================
#
# Load EPICS Database
dbLoadDatabase("dbd/odm.dbd")
odm_registerRecordDeviceDriver(pdbbase)

# Load record instances
dbLoadRecords("db/iocAdminSoft.db","IOC=${IOC_NAME}")
dbLoadRecords("db/iocRelease.db"  ,"IOC=${IOC_NAME}")

#==============================================================
#  Load Channe Access Security if configuration file exists
#==============================================================
< ${ACF_INIT}

#==============================================================
# Start IOC Log Client
#==============================================================
< ${LOG_INIT}

# End of script st.soft.cmd

