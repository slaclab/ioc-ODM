#include <dbCommon.h>
#include <dbAccess.h>
#include <dbScan.h>
#include <alarm.h>
#include <dbDefs.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <devSup.h>
#include <epicsExport.h>
#include <recGbl.h>
#include <biRecord.h>
#include <boRecord.h>
#include <longinRecord.h>
#include <longoutRecord.h>
#include <waveformRecord.h>
#include <stringinRecord.h>
#include <dbScan.h>
#include <errlog.h>

#include "pnzDriver.h"

// Sentinel index for the ARM:CMD bo (not a real output bit 0..127)
static const int PNZ_ARMCMD_INDEX = -200;

// STEP 4 (Option F): PnzDpvt now carries the resolved PnzPlc* (routed by sector
// at init). All read/write use d->plc -- NO onlyPlc(). Unknown sector -> record
// fails init (INVALID), so a mis-typed/absent sector fails LOUD at boot and can
// NEVER mis-route a safety PV to the wrong PLC.
struct PnzDpvt {
	enum Kind {
    		BIT_IN,
    		BIT_OUT,
    		LONG_IN,
    		LONG_OUT,
    		WAVE_IN,
                WAVE_IN_HEX,
		WAVE_OUT_HEX,
    		WAVE_OUT,
    		WAVE_OUT_RB,
                WAVE_IDNAME,           // CIP Identity product name (CHAR string)
                WAVE_LASTDROP,         // formatted last-disruption timestamp (CHAR waveform)
                STRING_IN_DROPTIME,    // last-disruption timestamp as DBF_STRING (EDM-friendly)
                STRING_PROJSUM_HEX,    // project/overall check sum as "A1B2 / 0000"
                STRING_PROJDATE,       // project date/time "DD.MM.YYYY HH:MM"
                WAVE_PROJNAME          // PNOZmulti project name (CHAR string)
	} kind;
    	int index;
        PnzPlc* plc;                   // STEP 4: resolved at init from the sector prefix
};

static bool parseUnsigned(const char* s, const char* prefix, int& value)
{
    if (!s || !prefix)
        return false;
    const std::size_t n = std::strlen(prefix);
    if (std::strncmp(s, prefix, n) != 0)
        return false;
    char* end = nullptr;
    long v = std::strtol(s + n, &end, 10);
    if (end == s + n || *end != '\0' || v < 0 || v > 1000000)
        return false;
    value = static_cast<int>(v);
    return true;
}

// STEP 4: split "SECTOR:TOKEN", resolve PnzPlc* by sector.
// On success: tokenOut = the part after the first ':', plcOut = resolved PLC.
// On failure (no ':' or unknown sector): logs, returns false -> caller fails
// record init (fail-loud). This is the ONLY place routing is resolved; the
// resolved plc is stored in PnzDpvt so reads/writes never re-parse or re-lookup.
static bool splitSectorAndResolve(dbCommon* prec, const char* spec,
                                  std::string& tokenOut, PnzPlc*& plcOut)
{
    plcOut = nullptr;
    tokenOut.clear();
    if (!spec || !*spec) {
        errlogPrintf("pnzEtherIP: %s empty link (expected @SECTOR:TOKEN)\n",
                     prec->name);
        return false;
    }
    const char* colon = std::strchr(spec, ':');
    if (!colon) {
        errlogPrintf("pnzEtherIP: %s link '%s' missing sector "
                     "(expected @SECTOR:TOKEN, e.g. @B34:I0)\n",
                     prec->name, spec);
        return false;
    }
    std::string sector(spec, static_cast<std::size_t>(colon - spec));
    tokenOut = colon + 1;
    plcOut = PnzManager::instance()->plc(sector);
    if (!plcOut) {
        errlogPrintf("pnzEtherIP: %s unknown sector '%s' in link '%s' "
                     "(no matching pnzEtherIPConfigure); record set INVALID\n",
                     prec->name, sector.c_str(), spec);
        return false;
    }
    return true;
}

// STEP 4: initCommon now receives the FULL spec ("SECTOR:TOKEN"), splits the
// sector, resolves the plc, then runs the (unchanged) token-parse logic on the
// remaining token. The resolved plc is stored in PnzDpvt.
static long initCommon(dbCommon* prec, const char* spec, PnzDpvt::Kind kind,
                       int maxIndex, const char* usage)
{
    std::string token;
    PnzPlc* drv = nullptr;
    if (!splitSectorAndResolve(prec, spec, token, drv)) {
        recGblSetSevr(prec, COMM_ALARM, INVALID_ALARM);
        return S_db_badField;
    }

    const char* s = token.c_str();   // token WITHOUT the sector prefix
    int index = 0;

    if (kind == PnzDpvt::Kind::BIT_IN) {
        if (!parseUnsigned(s, "I", index) || index > maxIndex) {
            errlogPrintf("pnzEtherIP: %s invalid input bit syntax '%s' (use @I0..@I127)\n",
                         prec->name, s);
            return S_db_badField;
        }
    } else if (kind == PnzDpvt::Kind::BIT_OUT) {
        if (std::strcmp(s, "ARMCMD") == 0) {
            index = PNZ_ARMCMD_INDEX;
        } else if (!parseUnsigned(s, "O", index) || index > maxIndex) {
            errlogPrintf("pnzEtherIP: %s invalid output bit syntax '%s' (use @O0..@O127 or @ARMCMD)\n",
                         prec->name, s);
            return S_db_badField;
        }
    } else if (kind == PnzDpvt::Kind::LONG_IN) {
        if (std::strcmp(s, "LED") == 0) index = -1;
        else if (std::strcmp(s, "TABLE") == 0) index = -2;
        else if (std::strcmp(s, "SEGMENT") == 0) index = -3;
        else if (std::strcmp(s, "CONNECTED") == 0) index = -4;
        else if (std::strcmp(s, "RUNNING") == 0) index = -5;
        else if (std::strcmp(s, "RPI_US") == 0) index = -6;
	else if (std::strcmp(s, "ARMED") == 0) index = -7;
	else if (std::strcmp(s, "DROPCOUNT") == 0) index = -8;
	else if (std::strcmp(s, "IDVENDOR") == 0)  index = -10;
	else if (std::strcmp(s, "IDTYPE") == 0)    index = -11;
	else if (std::strcmp(s, "IDCODE") == 0)    index = -12;
	else if (std::strcmp(s, "IDREVMAJ") == 0)  index = -13;
	else if (std::strcmp(s, "IDREVMIN") == 0)  index = -14;
	else if (std::strcmp(s, "IDSTATUS") == 0)  index = -15;
	else if (std::strcmp(s, "IDSERIAL") == 0)  index = -16;
	else if (std::strcmp(s, "DEVSTATUS") == 0) index = -17;
	else if (std::strcmp(s, "PROJSUM") == 0)    index = -20;
	else if (std::strcmp(s, "PROJSUMALL") == 0) index = -21;
	else if (parseUnsigned(s, "IBYTE", index) && index <= 31) {
    		index = index + 1000;
	} else if (parseUnsigned(s, "OBYTE", index) && index <= 31) {
    		index = index + 2000;
	} else {
            errlogPrintf("pnzEtherIP: %s invalid longin syntax '%s'\n", prec->name, s);
            return S_db_badField;
        }
    } else if (kind == PnzDpvt::Kind::LONG_OUT) {
        if (!parseUnsigned(s, "OBYTE", index) || index > 31) {
            errlogPrintf("pnzEtherIP: %s invalid longout syntax '%s' (use @OBYTE0..@OBYTE31)\n",
                         prec->name, s);
            return S_db_badField;
	}
    } else if (kind == PnzDpvt::Kind::WAVE_IN) {
    		if (std::strcmp(s, "RAWIN") != 0) {
        		errlogPrintf("pnzEtherIP: %s invalid waveform input syntax '%s' (use @RAWIN)\n",
                     		prec->name, s);
        		return S_db_badField;
    		}
    } else if (kind == PnzDpvt::Kind::WAVE_IN_HEX) {
            if (std::strcmp(s, "RAWINHEX") != 0) {
                errlogPrintf("pnzEtherIP: %s invalid hex-waveform syntax '%s' (use @RAWINHEX)\n",
                             prec->name, s);
                return S_db_badField;
            }
    } else if (kind == PnzDpvt::Kind::WAVE_OUT_HEX) {
            if (std::strcmp(s, "RAWOUTHEX") != 0) {
                errlogPrintf("pnzEtherIP: %s invalid hex-waveform syntax '%s' (use @RAWOUTHEX)\n",
                             prec->name, s);
                return S_db_badField;
            }
    } else if (kind == PnzDpvt::Kind::WAVE_OUT) {
    		if (std::strcmp(s, "RAWOUT") != 0) {
        		errlogPrintf("pnzEtherIP: %s invalid waveform output syntax '%s' (use @RAWOUT)\n",
                     		prec->name, s);
        		return S_db_badField;
    		}
    } else if (kind == PnzDpvt::Kind::WAVE_OUT_RB) {
    		if (std::strcmp(s, "OUTDIAG") != 0) {
        		errlogPrintf("pnzEtherIP: %s invalid output diagnostic syntax '%s' "
                     		"(use @OUTDIAG)\n",
                     		prec->name, s);
        		return S_db_badField;
    		}
    } else if (kind == PnzDpvt::Kind::WAVE_IDNAME) {
                if (std::strcmp(s, "IDNAME") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid identity-name syntax '%s' (use @IDNAME)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    } else if (kind == PnzDpvt::Kind::WAVE_LASTDROP) {
                if (std::strcmp(s, "LASTDROPTIME") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid last-drop syntax '%s' (use @LASTDROPTIME)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    } else if (kind == PnzDpvt::Kind::WAVE_PROJNAME) {
                if (std::strcmp(s, "PROJNAME") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid project-name syntax '%s' (use @PROJNAME)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    } else if (kind == PnzDpvt::Kind::STRING_IN_DROPTIME) {
                if (std::strcmp(s, "LASTDROPTIME") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid droptime syntax '%s' (use @LASTDROPTIME)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    } else if (kind == PnzDpvt::Kind::STRING_PROJSUM_HEX) {
                if (std::strcmp(s, "PROJSUMHEX") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid projsum syntax '%s' (use @PROJSUMHEX)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    } else if (kind == PnzDpvt::Kind::STRING_PROJDATE) {
                if (std::strcmp(s, "PROJDATE") != 0) {
                    errlogPrintf("pnzEtherIP: %s invalid projdate syntax '%s' (use @PROJDATE)\n",
                                 prec->name, s);
                    return S_db_badField;
                }
    }

    PnzDpvt* dpvt = new PnzDpvt{kind, index, drv};   // STEP 4: store resolved plc
    prec->dpvt = dpvt;
    return 0;
}

static long init_bi_record(biRecord* prec)
{
    return initCommon(reinterpret_cast<dbCommon*>(prec), prec->inp.value.instio.string,
                      PnzDpvt::BIT_IN, 127, "@I0..@I127");
}

// STEP 4: get_ioint_info uses the record's ALREADY-RESOLVED plc (from dpvt, set
// by init_record which runs BEFORE get_ioint_info). NOT onlyPlc().
static long get_ioint_info(int, dbCommon* prec, IOSCANPVT* ppvt)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    *ppvt = (d && d->plc) ? d->plc->scanPvt() : nullptr;
    return 0;
}

// I/O Intr scan list for the last-drop timestamp record (per-PLC drop scan).
static long get_ioint_info_drop(int, dbCommon* prec, IOSCANPVT* ppvt)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    *ppvt = (d && d->plc) ? d->plc->dropScanPvt() : nullptr;
    return 0;
}

static long read_bi(biRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    prec->rval = drv->getBit(true, static_cast<unsigned>(d->index)) ? 1 : 0;
    prec->udf = false;
    if (!drv->connected())
        recGblSetSevr(reinterpret_cast<dbCommon*>(prec), COMM_ALARM, MINOR_ALARM);
    return 0;
}

static long init_bo_record(boRecord* prec)
{
    return initCommon(reinterpret_cast<dbCommon*>(prec), prec->out.value.instio.string,
                      PnzDpvt::BIT_OUT, 127, "@O0..@O127 or @ARMCMD");
}

static long write_bo(boRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    if (d->index == PNZ_ARMCMD_INDEX) {
        drv->setArmed(prec->rval != 0);
        errlogPrintf("pnzEtherIP: [%s] ARM:CMD -> O->T writes %s\n",
                     drv->sector().c_str(),
                     prec->rval ? "ARMED" : "DISARMED (outputs forced to 0)");
        return 0;
    }

    drv->setBit(static_cast<unsigned>(d->index), prec->rval != 0);
    return 0;
}

static long init_li_record(longinRecord* prec)
{
    return initCommon(reinterpret_cast<dbCommon*>(prec), prec->inp.value.instio.string,
                      PnzDpvt::LONG_IN, 31,
                      "@LED/@TABLE/@SEGMENT/@CONNECTED/@RUNNING/@RPI_US/@ARMED/@DROPCOUNT/"
                      "@IDVENDOR/@IDTYPE/@IDCODE/@IDREVMAJ/@IDREVMIN/@IDSTATUS/@IDSERIAL/"
                      "@DEVSTATUS/@PROJSUM/@PROJSUMALL/@IBYTE<n>");
}

static long read_li(longinRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    switch (d->index) {
    case -1: prec->val = drv->led(); break;
    case -2: prec->val = drv->table(); break;
    case -3: prec->val = drv->segment(); break;
    case -4: prec->val = drv->connected() ? 1 : 0; break;
    case -5: prec->val = drv->running() ? 1 : 0; break;
    case -6: prec->val = drv->rpiUs(); break;
    case -7: prec->val = drv->armed() ? 1 : 0; break;
    case -8: prec->val = static_cast<long>(drv->dropCount()); break;
    case -10: prec->val = drv->identVendor(); break;
    case -11: prec->val = drv->identType(); break;
    case -12: prec->val = drv->identCode(); break;
    case -13: prec->val = drv->identRevMajor(); break;
    case -14: prec->val = drv->identRevMinor(); break;
    case -15: prec->val = drv->identStatus(); break;
    case -16: prec->val = static_cast<long>(drv->identSerial()); break;
    case -17: prec->val = drv->deviceStatus(); break;
    case -20: prec->val = drv->projChecksum(); break;
    case -21: prec->val = drv->projChecksumAll(); break;
    default:
        if (d->index >= 2000 && d->index <= 2031) {
            prec->val =
                drv->getOutputByte(static_cast<unsigned>(d->index - 2000));
        } else if (d->index >= 1000 && d->index <= 1031) {
            prec->val =
                drv->getInputByte(static_cast<unsigned>(d->index - 1000));
        } else {
            return -1;
        }
        break;
    }

    prec->udf = false;
    return 0;
}

static long init_lo_record(longoutRecord* prec)
{
    return initCommon(reinterpret_cast<dbCommon*>(prec), prec->out.value.instio.string,
                      PnzDpvt::LONG_OUT, 31, "@OBYTE0..@OBYTE31");
}

static long write_lo(longoutRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    if (prec->val < 0 || prec->val > 255) {
        recGblSetSevr(reinterpret_cast<dbCommon*>(prec), WRITE_ALARM, INVALID_ALARM);
        return -1;
    }

    drv->setOutputByte(static_cast<unsigned>(d->index),
                       static_cast<std::uint8_t>(prec->val));
    return 0;
}

/* stringin: last-disruption timestamp + PNOZmulti project checksum/date.
 * STEP 4: the spec is now "SECTOR:TOKEN"; we split the sector first, then
 * dispatch on the TOKEN. */
static long init_si_record(stringinRecord* prec)
{
    const char* spec = prec->inp.value.instio.string;

    // Peek the token (after the sector) to choose the Kind, but let initCommon
    // do the authoritative split+resolve+store. We only need the token here to
    // pick the enum.
    const char* colon = spec ? std::strchr(spec, ':') : nullptr;
    const char* tok = colon ? colon + 1 : (spec ? spec : "");

    if (std::strcmp(tok, "LASTDROPTIME") == 0)
        return initCommon(reinterpret_cast<dbCommon*>(prec), spec,
                          PnzDpvt::STRING_IN_DROPTIME, 0, "@LASTDROPTIME");
    if (std::strcmp(tok, "PROJSUMHEX") == 0)
        return initCommon(reinterpret_cast<dbCommon*>(prec), spec,
                          PnzDpvt::STRING_PROJSUM_HEX, 0, "@PROJSUMHEX");
    if (std::strcmp(tok, "PROJDATE") == 0)
        return initCommon(reinterpret_cast<dbCommon*>(prec), spec,
                          PnzDpvt::STRING_PROJDATE, 0, "@PROJDATE");
    errlogPrintf("pnzEtherIP: %s invalid stringin syntax '%s' "
                 "(use @SECTOR:LASTDROPTIME, @SECTOR:PROJSUMHEX, or @SECTOR:PROJDATE)\n",
                 prec->name, spec ? spec : "");
    return S_db_badField;
}

static long read_si(stringinRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    switch (d->kind) {
    case PnzDpvt::STRING_IN_DROPTIME:
        drv->copyLastDropTime(prec->val, sizeof(prec->val));
        break;
    case PnzDpvt::STRING_PROJSUM_HEX:
        drv->copyProjChecksumHex(prec->val, sizeof(prec->val));
        break;
    case PnzDpvt::STRING_PROJDATE:
        drv->copyProjDate(prec->val, sizeof(prec->val));
        break;
    default:
        return -1;
    }
    prec->udf = false;
    return 0;
}

/* waveform: STEP 4 -- split sector first, dispatch on the TOKEN. */
static long init_wf_record(waveformRecord* prec)
{
    const char* spec = prec->inp.value.instio.string;
    const char* colon = spec ? std::strchr(spec, ':') : nullptr;
    const char* tok = colon ? colon + 1 : (spec ? spec : "");

    if (std::strcmp(tok, "RAWIN") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_IN, 0, "@RAWIN");
    }
    if (std::strcmp(tok, "RAWINHEX") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_IN_HEX, 0, "@RAWINHEX");
    }
    if (std::strcmp(tok, "RAWOUTHEX") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_OUT_HEX, 0, "@RAWOUTHEX");
    }
    if (std::strcmp(tok, "OUTDIAG") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_OUT_RB, 0, "@OUTDIAG");
    }
    if (std::strcmp(tok, "IDNAME") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_IDNAME, 0, "@IDNAME");
    }
    if (std::strcmp(tok, "LASTDROPTIME") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_LASTDROP, 0, "@LASTDROPTIME");
    }
    if (std::strcmp(tok, "PROJNAME") == 0) {
        return initCommon(reinterpret_cast<dbCommon*>(prec),
                          spec, PnzDpvt::WAVE_PROJNAME, 0, "@PROJNAME");
    }

    errlogPrintf(
        "pnzEtherIP: %s invalid waveform syntax '%s' "
        "(use @SECTOR:{RAWIN,RAWINHEX,RAWOUTHEX,OUTDIAG,IDNAME,LASTDROPTIME,PROJNAME})\n",
        prec->name, spec ? spec : "");

    return S_db_badField;
}

static long read_wf(waveformRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc)
        return -1;
    auto* drv = d->plc;

    if (d->kind == PnzDpvt::WAVE_IN_HEX) {
        if (prec->ftvl != menuFtypeCHAR) {
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          READ_ALARM, INVALID_ALARM);
            return -1;
        }
        drv->copyInputHex(static_cast<char*>(prec->bptr),
                          static_cast<std::size_t>(prec->nelm));
        prec->nord = static_cast<long>(
            std::strlen(static_cast<char*>(prec->bptr)) + 1);
        prec->udf = false;
        if (!drv->connected())
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          COMM_ALARM, MINOR_ALARM);
        return 0;
    }

    if (d->kind == PnzDpvt::WAVE_OUT_HEX) {
        if (prec->ftvl != menuFtypeCHAR) {
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          READ_ALARM, INVALID_ALARM);
            return -1;
        }
        drv->copyOutputHex(static_cast<char*>(prec->bptr),
                           static_cast<std::size_t>(prec->nelm));
        prec->nord = static_cast<long>(
            std::strlen(static_cast<char*>(prec->bptr)) + 1);
        prec->udf = false;
        if (!drv->connected())
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          COMM_ALARM, MINOR_ALARM);
        return 0;
    }

    if (d->kind == PnzDpvt::WAVE_IDNAME) {
        if (prec->ftvl != menuFtypeCHAR) {
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          READ_ALARM, INVALID_ALARM);
            return -1;
        }
        drv->copyIdentName(static_cast<char*>(prec->bptr),
                           static_cast<std::size_t>(prec->nelm));
        prec->nord = static_cast<long>(
            std::strlen(static_cast<char*>(prec->bptr)) + 1);
        prec->udf = false;
        return 0;
    }

    if (d->kind == PnzDpvt::WAVE_PROJNAME) {
        if (prec->ftvl != menuFtypeCHAR) {
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          READ_ALARM, INVALID_ALARM);
            return -1;
        }
        drv->copyProjName(static_cast<char*>(prec->bptr),
                          static_cast<std::size_t>(prec->nelm));
        prec->nord = static_cast<long>(
            std::strlen(static_cast<char*>(prec->bptr)) + 1);
        prec->udf = false;
        return 0;
    }

    if (d->kind == PnzDpvt::WAVE_LASTDROP) {
        if (prec->ftvl != menuFtypeCHAR) {
            recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                          READ_ALARM, INVALID_ALARM);
            return -1;
        }
        drv->copyLastDropTime(static_cast<char*>(prec->bptr),
                              static_cast<std::size_t>(prec->nelm));
        prec->nord = static_cast<long>(
            std::strlen(static_cast<char*>(prec->bptr)) + 1);
        prec->udf = false;
        return 0;
    }

    if (prec->ftvl != menuFtypeUCHAR) {
        recGblSetSevr(reinterpret_cast<dbCommon*>(prec),
                      READ_ALARM, INVALID_ALARM);
        return -1;
    }

    const std::size_t n = prec->nelm < 32 ? prec->nelm : 32;

    if (d->kind == PnzDpvt::WAVE_IN) {
        drv->copyInput(static_cast<std::uint8_t*>(prec->bptr), n);
    }
    else if (d->kind == PnzDpvt::WAVE_OUT_RB) {
        drv->copyOutput(static_cast<std::uint8_t*>(prec->bptr), n);
    }
    else {
        return -1;
    }

    prec->nord = static_cast<long>(n);
    prec->udf = false;

    return 0;
}

static long write_wf(waveformRecord* prec)
{
    auto* d = static_cast<PnzDpvt*>(prec->dpvt);
    if (!d || !d->plc || d->kind != PnzDpvt::WAVE_OUT)
        return -1;
    auto* drv = d->plc;

    if (prec->ftvl != menuFtypeUCHAR || prec->nord > 32) {
        recGblSetSevr(reinterpret_cast<dbCommon*>(prec), WRITE_ALARM, INVALID_ALARM);
        return -1;
    }

    drv->setOutput(static_cast<const std::uint8_t*>(prec->bptr),
                   static_cast<std::size_t>(prec->nord));
    return 0;
}

extern "C" {

typedef struct {
    dset common;
    DEVSUPFUN read_bi;
} biDset;

typedef struct {
    dset common;
    DEVSUPFUN write_bo;
} boDset;

typedef struct {
    dset common;
    DEVSUPFUN read_li;
} liDset;

typedef struct {
    dset common;
    DEVSUPFUN write_lo;
} loDset;

typedef struct {
    dset common;
    DEVSUPFUN read_si;
} siDset;

typedef struct {
    dset common;
    DEVSUPFUN read_wf;
    DEVSUPFUN special_linconv;
    DEVSUPFUN write_wf;
} wfDset;

biDset devPnzBi = {
    {
        6,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_bi_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info)
    },
    reinterpret_cast<DEVSUPFUN>(read_bi)
};

boDset devPnzBo = {
    {
        6,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_bo_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info)
    },
    reinterpret_cast<DEVSUPFUN>(write_bo)
};

liDset devPnzLi = {
    {
        6,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_li_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info)
    },
    reinterpret_cast<DEVSUPFUN>(read_li)
};

loDset devPnzLo = {
    {
        6,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_lo_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info)
    },
    reinterpret_cast<DEVSUPFUN>(write_lo)
};

siDset devPnzSi = {
    {
        5,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_si_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info_drop)
    },
    reinterpret_cast<DEVSUPFUN>(read_si)
};

wfDset devPnzWf = {
    {
        8,
        nullptr,
        nullptr,
        reinterpret_cast<DEVSUPFUN>(init_wf_record),
        reinterpret_cast<DEVSUPFUN>(get_ioint_info)
    },
    reinterpret_cast<DEVSUPFUN>(read_wf),
    nullptr,
    reinterpret_cast<DEVSUPFUN>(write_wf)
};

epicsExportAddress(dset, devPnzBi);
epicsExportAddress(dset, devPnzBo);
epicsExportAddress(dset, devPnzLi);
epicsExportAddress(dset, devPnzLo);
epicsExportAddress(dset, devPnzSi);
epicsExportAddress(dset, devPnzWf);

} // extern "C"
