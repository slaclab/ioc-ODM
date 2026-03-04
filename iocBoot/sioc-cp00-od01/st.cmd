#!../../bin/rhel7-x86_64/odm
#==============================================================
#
#  Abs:  Startup Script for the Cryoplant ODM
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
epicsEnvSet("IOC_NAME"  ,"SIOC:CP00:OD01")
epicsEnvSet("LOCATION"  ,"lcls-daemon1")

# Siemens supervisory PLC Node
# Note cannot use nodename must use ip
epicsEnvSet("ODM_NODE"      ,"plc-cryo-od01")

# Load generic environment variables and database
< ../common/st.cmd.soft
epicsEnvSet("IOC","sioc-cp00-od01")

# Initialize Modbus communications
cd ${IOC_BOOT}
< init_asyn.cmd
cd ${TOP}

# Load Additional databases:
dbLoadRecords("db/odm-cp00.db")

# Setup autosave/restore
< iocBoot/common/init_restore.cmd.soft

cd "${TOP}/iocBoot/${IOC}"
iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)

# Start autosave routines to save our data
< iocBoot/common/restore.cmd.soft

# End of file st.cmd