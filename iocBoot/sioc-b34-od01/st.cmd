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
#  Mod: 29-Sep-2026, scondam - Disarm at boot (pnzEtherIPArm 0) for Option F
#       read-only muxing bring-up. Driver source default remains ARMED
#       (_armed{true}); this st.cmd forces DISARMED at startup so outputs are
#       held 0 while read paths are verified. Re-arm is a deliberate step.
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
# pnzEtherIPConfigure("134.79.217.31", 100000)
# NEW:
pnzEtherIPConfigure("134.79.217.31", 100000, "B34", 0)

# scondam: 25-Aug-2026 - fake IP to simulate the Comm disruption scenario.
# pnzEtherIPConfigure("134.79.217.251", 100000)
# NEW:
# pnzEtherIPConfigure("134.79.217.251", 100000, "B34", 0)

#--------------------------------------------------------------
# DISARM AT BOOT (Option F read-only bring-up)
# Force O->T writes DISARMED immediately after configure and BEFORE iocInit.
# While disarmed the driver sends an all-zero output image regardless of the
# _output bits, so read paths can be verified with no risk of driving outputs.
# NOTE: source default is still ARMED (_armed{true}); this line overrides it
# at startup. Remove/change this line (or use armAtBoot in a later release) to
# boot ARMED for write testing.
#--------------------------------------------------------------
# pnzEtherIPArm 0
# NEW:
pnzEtherIPArm("B34", 0)

# Load Additional databases:
dbLoadRecords("db/pnz.db","SECTOR=B34")
dbLoadRecords("db/pnz_obit_rb.db","SECTOR=B34")
dbLoadRecords("db/pnz_alias.db","SECTOR=B34") 
dbLoadRecords("db/pnz_project.db","SECTOR=B34") 
dbLoadTemplate("db/pnzValidBit.substitutions", "SECTOR=B34")

# Setup autosave/restore
# NOTE (scondam 29-Sep-2026): the only autosaved PV is SIOC:SYS0:OD01:ACCESS.VAL
# (see autosave-req/info_settings.req). No outputs, ARM:CMD, or AlarmReset are
# autosaved/restored, so the driver's arm/output defaults are authoritative at
# boot. (Previous "save-restore alarm reset high" comment was inaccurate and
# has been removed.)
< iocBoot/common/init_restore.soft.cmd
set_pass0_restoreFile("SIOC-B34-OD01.sav")

cd "${TOP}/iocBoot/${IOC}"
iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)
# Start autosave routines to save our data
< ../common/restore.soft.cmd

# End of file st.cmd
