#!../../bin/rhel7-x86_64/odm
#==============================================================
#
#  Abs:  Startup Script for the Linac ODM
#
#  Name: st.cmd
#
#  Desc:  This is the EPICS startup script for a soft IOC
#
#  Facility:  LCLS ODM
#
#==============================================================

# Set environment variables
epicsEnvSet("IOC_NAME"  ,"SIOC:SYS0:OD02")
epicsEnvSet("LOCATION"  ,"lcls-daemon1")

# Load generic environment variables and database
< ../common/st.soft.cmd

# Set up autosave/restore
< $(TOP)/iocBoot/common/init_restore.soft.cmd

# **** Beckhoff driver initialization ****
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH01,BKH_IP=apc-li01-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH02,BKH_IP=apc-li01-od02")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH03,BKH_IP=apc-li02-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH04,BKH_IP=apc-li03-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH05,BKH_IP=apc-li04-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH06,BKH_IP=apc-li05-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH07,BKH_IP=apc-li06-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH08,BKH_IP=apc-li07-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm_supervisor.iocsh", "BKH_PORT=BKH09,BKH_IP=apc-li03-od02")

## Load databases
dbLoadRecords("db/odm-sys0-od02.db")

iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)

# Start autosave routines to save our data
< $(TOP)/iocBoot/common/restore.soft.cmd


# End of file st.cmd

