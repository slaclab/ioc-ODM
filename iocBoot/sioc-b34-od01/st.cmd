#!../../bin/rhel9-x86_64/pnzEtherIP
#==============================================================
#
#  Abs:  Startup Script for the Linac ODM
#
#  Name: st.cmd
#
#  Desc:  EPICS startup script for the ODM soft IOC.
#         R1.1.10: sioc-sys0-od03 consolidates LI08, LI09, LI10
#         onto ONE IOC (od04/od05 decommissioned).
#         Boots DISARMED (read-only) for phase-1 comm bring-up.
#
#  Facility:  LCLS Personnel Protection System (PPS)
#
#  Auth: 14-Aug-2026, Shantha Condamoor  (SCONDAM)
#  Rev:  29-Sep-2026, Shantha Condamoor  (SCONDAM) - multi-PLC R1.1.10
#--------------------------------------------------------------
#  Mod:
#==============================================================
#

# Set environment variables
epicsEnvSet("IOC_NAME"  ,"SIOC:SYS0:OD03")
epicsEnvSet("LOCATION"  ,"lcls-daemon0")

# Load generic environment variables and database
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
dbLoadDatabase("dbd/pnzEtherIP.dbd")
pnzEtherIP_registerRecordDeviceDriver(pdbbase)

# Load record instances
dbLoadRecords("db/iocAdminSoft.db","IOC=${IOC_NAME}")
dbLoadRecords("db/iocRelease.db"  ,"IOC=${IOC_NAME}")

#==============================================================
#  Load Channel Access Security if configuration file exists
#==============================================================
< ${ACF_INIT}

#==============================================================
# Start IOC Log Client
#==============================================================
< ${LOG_INIT}

epicsEnvSet("IOC","sioc-sys0-od03")

#==============================================================
# PNOZ m ES EtherNet/IP PLC connections  (R1.1.10 multi-PLC)
#
#   pnzEtherIPConfigure(IP, RPI_us, SECTOR, armAtBoot)
#     RPI = 500000 us (500 ms). Default multiplier 4 => 2 s comm-fault timeout.
#     armAtBoot: 0 = DISARMED (read-only) for phase-1 bring-up
#                1 = ARMED (writes enabled) for phase-2 write testing
#
#   *** PHASE 1 (this file): all three sectors DISARMED (read-only). ***
#   Verify RO PVs read correctly from all three PLCs with NO cross-talk
#   (e.g. LI08 values must NOT appear on LI09/LI10 PVs) before arming.
#
#   PHASE 2: change armAtBoot 0 -> 1 per sector (or use pnzEtherIPArm
#   "LIxx",1 at runtime) and reboot to test write PVs.
#==============================================================
# LI08  plc-li08-od01
pnzEtherIPConfigure("172.27.143.38",  500000, "LI08", 0)
# LI09  plc-li09-od01
pnzEtherIPConfigure("172.27.143.202", 500000, "LI09", 0)
# LI10  plc-li10-od01
pnzEtherIPConfigure("172.27.143.104", 500000, "LI10", 0)

#==============================================================
# Load per-sector databases
#==============================================================
# --- LI08 ---
dbLoadRecords("db/pnz.db","SECTOR=LI08")
dbLoadRecords("db/pnz_obit_rb.db","SECTOR=LI08")
dbLoadRecords("db/pnz_alias.db","SECTOR=LI08")
dbLoadRecords("db/pnz_project.db","SECTOR=LI08")
dbLoadTemplate("db/pnzValidBit.substitutions", "SECTOR=LI08")

# --- LI09 ---
dbLoadRecords("db/pnz.db","SECTOR=LI09")
dbLoadRecords("db/pnz_obit_rb.db","SECTOR=LI09")
dbLoadRecords("db/pnz_alias.db","SECTOR=LI09")
dbLoadRecords("db/pnz_project.db","SECTOR=LI09")
dbLoadTemplate("db/pnzValidBit.substitutions", "SECTOR=LI09")

# --- LI10 ---
dbLoadRecords("db/pnz.db","SECTOR=LI10")
dbLoadRecords("db/pnz_obit_rb.db","SECTOR=LI10")
dbLoadRecords("db/pnz_alias.db","SECTOR=LI10")
dbLoadRecords("db/pnz_project.db","SECTOR=LI10")
dbLoadTemplate("db/pnzValidBit.substitutions", "SECTOR=LI10")

#==============================================================
# Setup autosave/restore
#   NOTE: autosave is IOC-scoped and generic (info_positions/info_settings).
#   makeAutosaveFiles() auto-includes ALL loaded records, so LI08/LI09/LI10
#   autosave PVs (incl. alarm-reset-HIGH) are captured automatically under
#   ${IOC_DATA}/sioc-sys0-od03/autosave.  No per-sector .sav files needed.
#==============================================================
< iocBoot/common/init_restore.soft.cmd

cd "${TOP}/iocBoot/${IOC}"
iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)
# Start autosave routines to save our data
< ../common/restore.soft.cmd

# End of file st.cmd
