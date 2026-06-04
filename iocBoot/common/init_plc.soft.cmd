#==============================================================
#
#  Abs:  Script to Initialize PLC hardware for a soft IOC
#
#  Name: init_plc.cmd.soft
#
#  Rem:  Upon entry we expect to be at location TOP
#        and the following macros must be defined.
#        Note that all other environment variables #
#        used within this file are defined in envPaths.
#
#         PLC_NODE - plc node name or ip     ex)plc-in20-pp01
#         PLC_NAME - plc name defined in db  ex)PLC:IN20:PP01
#
#  Facility:  Personnel Protection System (PPS)
#
#  Auth: 22-Sep-2016, Kristi Luchini  (LUCHINI)
#  Rev:  dd-mmm-yyyy, Reviewer's Name (USERNAME)
#--------------------------------------------------------------
#  Mod:
#        dd-mmm-yyyy, First Lastname  (USERNAME):
#          add comment
#
#==============================================================
#
# Initialize PLC
drvEtherIP_init()
drvEtherIP_define_PLC(${PLC_NAME},${PLC_NODE}, 0)

EIP_verbosity(0)
drvEtherIP_default_rate(0.5)

# End of script init_plc.cmd.soft


