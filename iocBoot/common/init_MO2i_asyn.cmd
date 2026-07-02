#==============================================================
#
#  Abs:  Initialize ModBus Asyn communications
#
#  Name: init_asyn.cmd
#
#  Facility:  SLAC PPS Controls
#
#  Auth: 20-May-2019, Anthony Andrews (AANDREWS)
#  Rev:  dd-mmm-yyyy, Reviewer's Name (USERNAME)
#--------------------------------------------------------------
#  Mod:
#        14-Oct-2019, K. Luchini      (LUCHINI):
#          extract from st.cmd
#
#==============================================================
#

# Oxigraf devices
drvAsynIPPortConfigure( "OXI_LI00", "ts-li00-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI01", "ts-li01-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI02", "ts-li02-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI03", "ts-li03-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI04", "ts-li04-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI05", "ts-li05-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI06", "ts-li06-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI07", "ts-li07-od01:2101", 0, 0, 0 )
drvAsynIPPortConfigure( "OXI_LI08", "ts-li08-od01:2101", 0, 0, 0 )
epicsEnvSet( "STREAM_PROTOCOL_PATH", "$(TOP)/db")

# End of script

