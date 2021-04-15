#!../../bin/rhel6-x86_64/odm
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
< ../common/st.cmd.soft

# Set up autosave/restore
< $(TOP)/iocBoot/common/init_restore.cmd.soft

# **** Driver setup for Beckhoffs ****
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH01,BKH_IP=apc-li01-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH02,BKH_IP=apc-li01-od02")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH03,BKH_IP=apc-li02-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH04,BKH_IP=apc-li03-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm_supervisor.iocsh", "BKH_PORT=BKH05,BKH_IP=apc-li03-od02")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH06,BKH_IP=apc-li04-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH07,BKH_IP=apc-li05-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH08,BKH_IP=apc-li06-od01")
iocshLoad("$(TOP)/iocBoot/common/init_bkh_odm.iocsh", "BKH_PORT=BKH09,BKH_IP=apc-li07-od01")

# **** Load Beckhoff databases ****
dbLoadRecords("db/bkh_odm_li00.db", "P=ODM:LI00,M=BKH01,BKH=BKH:LI00:OD01,LOC=L2KA01-08")
dbLoadRecords("db/bkh_odm_li01.db", "P=ODM:LI01,M=BKH02,BKH=BKH:LI01:OD01,LOC=L2KA01-08")
dbLoadRecords("db/bkh_odm_li02.db", "P=ODM:LI02,M=BKH03,BKH=BKH:LI02:OD01,LOC=L2KA02-09")
dbLoadRecords("db/bkh_odm_li03.db", "P=ODM:LI03,M=BKH04,BKH=BKH:LI03:OD01,LOC=L2KA03-07")
dbLoadRecords("db/bkh_odm_supervisor.db", "P=ODM:LI03:OD02,M=BKH05,BKH=BKH:LI03:OD02,LOC=L2KA03-07")
dbLoadRecords("db/bkh_odm_li04.db", "P=ODM:LI04,M=BKH06,BKH=BKH:LI04:OD01,LOC=L2KA04-06")
dbLoadRecords("db/bkh_odm_li05.db", "P=ODM:LI05,M=BKH07,BKH=BKH:LI05:OD01,LOC=L2KA05-07")
dbLoadRecords("db/bkh_odm_li06.db", "P=ODM:LI06,M=BKH08,BKH=BKH:LI06:OD01,LOC=L2KA06-08")
dbLoadRecords("db/bkh_odm_li07.db", "P=ODM:LI07,M=BKH09,BKH=BKH:LI07:OD01,LOC=L2KA07-03")

iocInit

# Initialize caPutLog
caPutLogInit("${EPICS_CA_PUT_LOG_ADDR}",0)

# Start autosave routines to save our data
< $(TOP)/iocBoot/common/restore.cmd.soft


# End of file st.cmd

