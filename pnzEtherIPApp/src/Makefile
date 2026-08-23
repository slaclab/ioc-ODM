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

PROD_NAME = pnzEtherIP
PROD_IOC += $(PROD_NAME)

DBD += $(PROD_NAME).dbd
$(PROD_NAME)_DBD += base.dbd
$(PROD_NAME)_DBD += iocAdmin.dbd
$(PROD_NAME)_DBD += caPutLog.dbd
$(PROD_NAME)_DBD += asSupport.dbd
$(PROD_NAME)_DBD += pnzEtherIPSupport.dbd

ifdef EPICS_QSRV_MAJOR_VERSION
$(PROD_NAME)_DBD += PVAServerRegister.dbd
$(PROD_NAME)_DBD += qsrv.dbd
endif

$(PROD_NAME)_SRCS += pnzEtherIP_registerRecordDeviceDriver.cpp
$(PROD_NAME)_SRCS += pnzEtherIPMain.cpp
$(PROD_NAME)_SRCS += pnzDriver.cpp
$(PROD_NAME)_SRCS += devPnz.cpp

# Note: order of libraries for the event system is very important.
$(PROD_NAME)_LIBS += devIocStats
$(PROD_NAME)_LIBS += caPutLog
$(PROD_NAME)_LIBS += autosave

USR_CXXFLAGS += -std=c++17
USR_CXXFLAGS += -I$(EIPSCANNER_HOME)/include
USR_CXXFLAGS += -I$(EIPSCANNER_HOME)/include/EIPScanner

USR_LDFLAGS += -L$(EIPSCANNER_HOME)/lib
# USR_LDFLAGS += -Wl,-rpath,$(EIPSCANNER_HOME)/lib
USR_LDFLAGS += -Wl,-rpath,$(abspath $(EIPSCANNER_HOME)/lib)

ifdef EPICS_QSRV_MAJOR_VERSION
$(PROD_NAME)_LIBS += qsrv
$(PROD_NAME)_LIBS += $(EPICS_BASE_PVA_CORE_LIBS)
endif

$(PROD_NAME)_SYS_LIBS += EIPScanner
$(PROD_NAME)_LIBS += $(EPICS_BASE_IOC_LIBS)

include $(TOP)/configure/RULES
