TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=====================================================
# Path to "NON EPICS" External PACKAGES: USER INCLUDES
#======================================================

# Support Perl C regular expression library
USR_INCLUDES += -I$(PCRE_INCLUDE)

#======================================================
# PATH TO "NON EPICS" EXTERNAL PACKAGES: USER LIBRARIES
#======================================================

pcre_DIR = $(PCRE_LIB)

# ======================================================
# LINK "NON EPICS" EXTERNAL PACKAGE LIBRARIES STATICALLY
#=======================================================

USR_LIBS_Linux += pcre

#=============================
# Build the IOC application

PROD_IOC = odm
# odm.dbd will be created and installed
DBD += odm.dbd

# odm.dbd will be made up from these files:
odm_DBD += base.dbd
odm_DBD += iocAdmin.dbd
odm_DBD += caPutLog.dbd
odm_DBD += asSupport.dbd

odm_DBD += asyn.dbd
odm_DBD += modbusSupport.dbd
odm_DBD += ModBusTCPClnt.dbd
odm_DBD += drvAsynIPPort.dbd
odm_DBD += stream.dbd
odm_DBD += system.dbd

# Add all the support libraries needed by this IOC
#odm_LIBS += xxx

# odm_registerRecordDeviceDriver.cpp derives from odm.dbd
odm_SRCS += odm_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
odm_SRCS_DEFAULT += odmMain.cpp
odm_SRCS_vxWorks += -nil-

# Note: order of libraries for the event system is very important.
odm_LIBS += devIocStats
odm_LIBS += caPutLog
odm_LIBS += autosave
odm_LIBS += ModBusTCPClnt
odm_LIBS += modbus
odm_LIBS += stream
odm_LIBS += asyn
# Add support from base/src/vxWorks if needed
#odm_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
odm_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

