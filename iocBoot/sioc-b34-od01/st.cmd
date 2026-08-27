#!../../bin/rhel9-x86_64/pnzEtherIP
#==============================================================
#
#  Abs:  Startup Script for the Linac ODM
#
#  Name: st.cmd
#
#  Desc:  This is the EPICS startup script for a soft IOC
#         for the ODM PLC that uses the EtherNet/IP comm module
#
#  Facility:  LCLS Personnel Protection System (PPS)
#
#  Auth: 14-Aug-2026, Shantha Condamoor  (SCONDAM)
#  Rev:  dd-mmm-yyyy, Reviewer's Name  (USERNAME)
#--------------------------------------------------------------
#  Mod:
#==============================================================
#

# Set environment variables
epicsEnvSet("IOC_NAME"  ,"SIOC:B34:OD01")
epicsEnvSet("LOCATION"  ,"lcls-daemon1")

# Load generic environment variables and database
#
# Set environment variables
< envPaths

# iocAdmin environment variables
epicsEnvSet("ENGINEER"   , "Shantha Condamoor")
epicsEnvSet("IOC_BOOT"   , "${TOP}/iocBoot/${IOC}")
epicsEnvSet("STARTUP"    , "${EPICS_IOCS}/${IOC}")
epicsEnvSet("ST_CMD"     , "startup.cmd")
epicsEnvSet("SUBSYS"     ,"odm")

# Change to the top of the app
cd ${TOP}

#==============================================================
# Load IOC Application object
#==============================================================
#
# Load EPICS Database

# Load EPICS Database
dbLoadDatabase("dbd/pnzEtherIP.dbd")
pnzEtherIP_registerRecordDeviceDriver(pdbbase)


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

epicsEnvSet("IOC","sioc-b34-od01")

# PNOZ m ES EtherNet/IP 772137
# RPI is 100 ms for this example. Keep all outputs zero during initial commissioning.
pnzEtherIPConfigure("134.79.217.31", 100000)

# scondam: 25-Aug-2026 - fake IP to simulate the Comm disruption scenario.
# pnzEtherIPConfigure("134.79.217.251", 100000)

# Load Additional databases:
dbLoadRecords("db/pnz.db","SECTOR=B34")
dbLoadRecords("db/pnz_obit_rb.db","SECTOR=B34")
dbLoadRecords("db/pnz_alias.db","SECTOR=B34") 

# Setup autosave/restore
< iocBoot/common/init_restore.soft.cmd
# Also save-restore alarm reset high
set_pass0_restoreFile("SIOC-B34-OD01.sav")

cd "${TOP}/iocBoot/${IOC}"
iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)
# Start autosave routines to save our data
< iocBoot/common/restore.soft.cmd

# End of file st.cmd
