#### Unit test for heartbeat status monitor
dbpf ODM:CP00:HEARTBEAT.SIMS "NO_ALARM"
dbpf ODM:CP00:HEARTBEAT.SIMM "YES"
dbpf ODM:CP00:HEARTBEAT.PROC 1
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# A and B should be 0
dbpr ODM:CP00:HEARTBEAT_OK 99
dbpr ODM:CP00:HEARTBEAT

epicsThreadSleep 2
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# D should be non-0, A and B 0, VAL 1
dbpr ODM:CP00:HEARTBEAT_OK 99
dbpr ODM:CP00:HEARTBEAT

dbpf ODM:CP00:HEARTBEAT.SVAL 1
dbpf ODM:CP00:HEARTBEAT.PROC 1
dbpr ODM:CP00:HEARTBEAT
epicsThreadSleep 0.5
dbpf ODM:CP00:HEARTBEAT.SVAL 0
dbpf ODM:CP00:HEARTBEAT.PROC 0
dbpr ODM:CP00:HEARTBEAT
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# D should be less than before, A and B 0 or 1, VAL 1
dbpr ODM:CP00:HEARTBEAT_OK 99
# Should not be in alarm state
dbpr ODM:CP00:HEARTBEAT

epicsThreadSleep 2
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# D should be more than before, A and B 0, Val 1
dbpr ODM:CP00:HEARTBEAT_OK 99
# Should not be in alarm state
dbpr ODM:CP00:HEARTBEAT

epicsThreadSleep 2
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
dbpr ODM:CP00:HEARTBEAT_OK 99
dbpr ODM:CP00:HEARTBEAT

epicsThreadSleep 2
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# D should be > C, A and B 0, VAL 0, alarm state
dbpr ODM:CP00:HEARTBEAT_OK 99
# Should be in alarm state
dbpr ODM:CP00:HEARTBEAT

dbpf ODM:CP00:HEARTBEAT.SVAL 1
dbpf ODM:CP00:HEARTBEAT.PROC 1
epicsThreadSleep 0.5
dbpf ODM:CP00:HEARTBEAT.SVAL 0
dbpf ODM:CP00:HEARTBEAT.PROC 0
epicsThreadSleep 0.5
dbpf ODM:CP00:HEARTBEAT.SVAL 1
dbpf ODM:CP00:HEARTBEAT.PROC 1
dbpr ODM:CP00:HEARTBEAT_OK_calc_ 99
# D should be less than before, A and B 0 or 1, VAL 1
dbpr ODM:CP00:HEARTBEAT_OK 99
# Should not be in alarm state
dbpr ODM:CP00:HEARTBEAT


# End of file st.cmd