TOP=../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE

#----------------------------------------------------
# Create and install (or just install) into <top>/db
# databases, templates, substitutions like this
#DB += xxx.db
DB += odm-cp00.db
DB += odm-test.db

# Install databbases from EPICS modules
DB_INSTALLS += $(ASYN)/db/asynRecord.db
DB_INSTALLS += $(IOCADMIN)/db/iocAdminSoft.db
DB_INSTALLS += $(IOCADMIN)/db/iocAdminRTEMS.db
DB_INSTALLS += $(AUTOSAVE)/db/save_restoreStatus.db
DB_INSTALLS += $(OXIGRAFMO2I)/db/MO2i.template
DB_INSTALLS += $(OXIGRAFMO2I)/db/MO2i.proto
IOCRELEASE_DB += iocRelease.db

# Install CA security file
SECURITY += pps.acf
#----------------------------------------------------
# If <anyname>.db template is not named <anyname>*.template add
# <anyname>_template = <templatename>

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

