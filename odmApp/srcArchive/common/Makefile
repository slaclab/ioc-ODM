#==============================================================
#
#  Abs: Generate Archive list  
#
#  Facility: Personnel Protection System Controls
#
#  Auth: 13-Dec-2016, Kristi Luchini  (USERNAME)
#  Rev:  dd-mmm-yyyy, Reviewers Name  (USERNAME)
#--------------------------------------------------------------
#  Mod:
#        19-Oct-2018, K. Luchini      (LUCHINI):
#          install odm_misc.tpl-sub
#        15-Oct-2018, K. Luchini      (LUCHINI):
#          move from odmApp/srcArchive to odmApp/srcArchive/common
#        04-Jun-2018, K. Luchini      (LUCHINI):
#          install sioc-in10-pp01.archive
#          install archive template for iocAdmin
#
#============================================================== 
#
TOP = ../../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#
ARCHIVE += $(IOCADMIN)/iocAdmin/Db/archive_iocAdmin.tpl-arch

# Install archive list into $TOP/archive
ARCHIVE += one_sec_monitor.tpl-arch
ARCHIVE += pilz_plc_request.tpl-arch
ARCHIVE += odm_pilz_diags.tpl-arch
ARCHIVE += odm_sector_pilz.tpl-arch
ARCHIVE += MO2i.tpl-arch


include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE
#
# End of File


