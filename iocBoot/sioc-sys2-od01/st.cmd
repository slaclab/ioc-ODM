#!../../bin/rhel6-x86_64/odm
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
epicsEnvSet("IOC_NAME"  ,"SIOC:SYS2:OD01")
epicsEnvSet("LOCATION"  ,"lcls-daemon1")

# Load generic environment variables and database
< ../common/st.cmd.soft
epicsEnvSet("IOC","sioc-sys2-od01")

# Initialize Modbus communications
cd ${IOC_BOOT}
< init_asyn.cmd
cd ${TOP}

# Load Additional databases:
dbLoadRecords("db/odm-sys2.db")

# Setup autosave/restore
< iocBoot/common/init_restore.cmd.soft
# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)
# Start autosave routines to save our data
< iocBoot/common/restore.cmd.soft

cd "${TOP}/iocBoot/${IOC}"
iocInit

## Start any sequence programs
#seq sncxxx,"user=gwbrown"

# End of file st.cmd