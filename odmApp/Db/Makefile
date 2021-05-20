TOP=../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE

#----------------------------------------------------
# Create and install (or just install) into <top>/db
# databases, templates, substitutions like this

# Install databbases from EPICS modules
DB_INSTALLS += $(ASYN)/db/asynRecord.db
DB_INSTALLS += $(IOCADMIN)/db/iocAdminSoft.db
DB_INSTALLS += $(IOCADMIN)/db/iocAdminRTEMS.db
DB_INSTALLS += $(AUTOSAVE)/db/save_restoreStatus.db
DB_INSTALLS += $(OXIGRAFMO2I)/db/MO2i.template
DB_INSTALLS += $(OXIGRAFMO2I)/db/MO2i.proto
DB_INSTALLS += $(ASYN)/db/asynRecord.db
DB_INSTALLS += $(MODBUS)/db/statistics.template
IOCRELEASE_DB += iocRelease.db

#DB += xxx.db
DB += odm-sys0-pilz.db
DB += bkh_odm_common.db
DB += bkh_odm_supervisor.db
DB += odm-sys0-od01.db
DB += odm-sys0-od02.db
DB += odm-cp00.db
DB += odm-test.db

#----------------------------------------------------
# If <anyname>.db template is not named <anyname>*.template add
# <anyname>_template = <templatename>

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

