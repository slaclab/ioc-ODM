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

PROD_NAME = odm
PROD_IOC = $(PROD_NAME)
# odm.dbd will be created and installed
DBD += $(PROD_NAME).dbd

# odm.dbd will be made up from these files:
$(PROD_NAME)_DBD += base.dbd
$(PROD_NAME)_DBD += iocAdmin.dbd
$(PROD_NAME)_DBD += caPutLog.dbd
$(PROD_NAME)_DBD += asSupport.dbd

$(PROD_NAME)_DBD += asyn.dbd
$(PROD_NAME)_DBD += modbusSupport.dbd
$(PROD_NAME)_DBD += ModBusTCPClnt.dbd
$(PROD_NAME)_DBD += drvAsynIPPort.dbd
$(PROD_NAME)_DBD += stream.dbd
$(PROD_NAME)_DBD += drvBkhAsyn.dbd
$(PROD_NAME)_DBD += system.dbd

ifdef EPICS_QSRV_MAJOR_VERSION
$(PROD_NAME)_DBD += PVAServerRegister.dbd
$(PROD_NAME)_DBD += qsrv.dbd
endif

# Add all the support libraries needed by this IOC
#$(PROD_NAME)_LIBS += xxx

# odm_registerRecordDeviceDriver.cpp derives from odm.dbd
$(PROD_NAME)_SRCS += $(PROD_NAME)_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
$(PROD_NAME)_SRCS_DEFAULT += $(PROD_NAME)Main.cpp
#$(PROD_NAME)_SRCS_vxWorks += -nil-

# Note: order of libraries for the event system is very important.
$(PROD_NAME)_LIBS += devIocStats
$(PROD_NAME)_LIBS += caPutLog
$(PROD_NAME)_LIBS += autosave
$(PROD_NAME)_LIBS += ModBusTCPClnt
$(PROD_NAME)_LIBS += modbus
$(PROD_NAME)_LIBS += stream
$(PROD_NAME)_LIBS += bkhAsyn
$(PROD_NAME)_LIBS += asyn
# Add support from base/src/vxWorks if needed
#odm_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

ifdef EPICS_QSRV_MAJOR_VERSION
$(PROD_NAME)_LIBS += qsrv
$(PROD_NAME)_LIBS += $(EPICS_BASE_PVA_CORE_LIBS)
endif

# Finally link to the EPICS Base libraries
$(PROD_NAME)_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

