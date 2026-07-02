#!../../bin/rhel7-x86_64/odm
#==============================================================
#
#  Abs:  Startup Script for the Linac ODM
#
#  Name: st.cmd
#
#  Desc:  This is the EPICS startup script for a soft IOC
#         for the ODM PLC
#
#  Facility:  LCLS Personnel Protection System (PPS)
#
#  Auth: 20-May-2019, Anthony Andrews  (AANDREWS)
#  Rev:  dd-mmm-yyyy, Reviewer's Name  (USERNAME)
#--------------------------------------------------------------
#  Mod:
#         14-Oct-2019, K. Luchini      (LUCHINI):
#           load common scripts
#           load caPutLogInit
#
#==============================================================
#

# Set environment variables
epicsEnvSet("IOC_NAME"  ,"SIOC:SYS0:OD01")
epicsEnvSet("LOCATION"  ,"lcls-daemon1")

# Load generic environment variables and database
< ../common/st.soft.cmd
epicsEnvSet("IOC","sioc-sys0-od01")

# Initialize Modbus communications
cd ${EPICS_IOC_APP}
iocshLoad( "../common/init_MO2i_asyn.cmd")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI00")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI01")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI02")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI03")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI04")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI05")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI06")
iocshLoad( "../common/init_pilz_asyn.cmd", "SECTOR=LI07")
iocshLoad( "../common/init_pnoz2_asyn.cmd", "SECTOR=LI08")
iocshLoad( "../common/init_pnoz2_asyn.cmd", "SECTOR=LI09")
iocshLoad( "../common/init_pnoz2_asyn.cmd", "SECTOR=LI10")
cd ${TOP}

# Load Additional databases:
dbLoadRecords("db/odm-sys0-od01.db")

# Setup autosave/restore
< iocBoot/common/init_restore.soft.cmd
# Also save-restore alarm reset high
set_pass0_restoreFile("SIOC-SYS0-OD01.sav")

cd "${TOP}/iocBoot/${IOC}"
iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)
# Start autosave routines to save our data
< iocBoot/common/restore.soft.cmd

# End of file st.cmd