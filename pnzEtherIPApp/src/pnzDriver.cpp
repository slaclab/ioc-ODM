/*
	Mod: Shantha Condamoor (scondam)
	Date: 16-Aug-2026

	Arm gate: default DISARMED (_armed{false}); disarmed -> outputs forced 0.
	iocsh: pnzEtherIPArm 1/0. PV: ODM:$(SECTOR):ARM:CMD.

	Comm-loss annunciation: _dropCount increments once per disruption episode.

	PLC diagnostics (design A): explicit messaging on the WORKER THREAD, after
	handleConnections(), using a SEPARATE short-timeout SessionInfo.
	NOTE: This device serves ONLY the Identity Object (0x01) via explicit
	messaging. The Assembly Object (0x04) does NOT support Get_Attribute_Single
	(verified on bench: instance 1 returns 0x16 OBJECT_DOES_NOT_EXIST), so there
	is NO Device-Status read. Identity is read ONCE at connect; run/stop state
	comes from the cyclic LED byte (see devPnz.cpp @LED and pnz.db PLC:Running).

	------------------------------------------------------------------------
	Mod: Shantha Condamoor (scondam)  --  COMM-LOSS CRASH FIX
	Bench cable-unplug caused core-dump/restart loop:
	    terminate called after throwing 'std::system_error'
	      what():  Connection timed out
	Root cause: throwing ~SessionInfo() (UnRegisterSession) on a dead link
	during exception unwinding -> std::terminate().
	Fixes: safeResetExplicit() noexcept for all explicit teardown; entire
	worker() body wrapped in try/catch(...) so no comm throw can kill the IOC.
	------------------------------------------------------------------------
	Mod: Shantha Condamoor (scondam)  --  DROP-COUNT + TIMESTAMP
	  - noteDisruption() counts + timestamps ONE episode per outage using the
	    _commFail latch (cleared on successful receive/connect).
	  - All retry/heartbeat log lines are timestamped.
	  - Last-disruption time exposed via copyLastDropTime()/@LASTDROPTIME.
	  - noteDisruption() triggers _dropScan (I/O Intr) so the last-drop
	    stringin posts a CA monitor exactly once per disruption (no periodic
	    monitor traffic; EDM updates the instant a drop occurs).
	------------------------------------------------------------------------
	Mod: Shantha Condamoor (scondam)  --  STARTUP-GRACE DISRUPTION ACCOUNTING
	Requirement: a comm disruption present BEFORE, DURING, or AFTER an IOC
	reboot must all be counted. But a normal HEALTHY boot performs a brief
	first-connect handshake that can fail once for ~1-2 s before succeeding;
	that transient must NOT be counted.
	Solution: a startup grace window (kStartupGraceSec). Rules:
	  * After first successful connect (_everConnected): count EVERY failure
	    immediately (a real drop of an established link).
	  * Never connected yet, still within grace: do NOT count (handshake).
	  * Never connected yet, PAST grace: count ONCE (_startupCounted) -- the
	    IOC booted into a genuine outage (PLC offline / network down).
	------------------------------------------------------------------------
*/
#include "pnzDriver.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <epicsExport.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <errlog.h>
#include <iocsh.h>
#include <dbScan.h>

#include "ConnectionManager.h"
#include "IOConnection.h"
#include "SessionInfo.h"
#include "MessageRouter.h"
#include "IdentityObject.h"
#include "cip/EPath.h"
#include "cip/Services.h"
#include "cip/MessageRouterResponse.h"
#include "cip/GeneralStatusCodes.h"
#include "cip/CipRevision.h"
#include "cip/connectionManager/ConnectionParameters.h"
#include "cip/connectionManager/NetworkConnectionParams.h"
#include "utils/Logger.h"

using eipScanner::ConnectionManager;
using eipScanner::IOConnection;
using eipScanner::SessionInfo;
using eipScanner::MessageRouter;
using eipScanner::IdentityObject;
using eipScanner::cip::connectionManager::ConnectionParameters;
using eipScanner::cip::connectionManager::NetworkConnectionParams;
using eipScanner::cip::EPath;
using eipScanner::cip::MessageRouterResponse;
using eipScanner::cip::GeneralStatusCodes;
using eipScanner::cip::ServiceCodes;
using eipScanner::utils::LogLevel;
using eipScanner::utils::Logger;

// Startup grace window: ignore a transient first-connect handshake failure on
// a healthy boot, but count a genuine boot-into-outage once this elapses.
static const double kStartupGraceSec = 5.0;

PnzDriver* PnzDriver::_instance = nullptr;

PnzDriver* PnzDriver::configure(const std::string& ip, std::uint32_t rpiUs)
{
    if (_instance) {
        errlogPrintf("pnzEtherIP: already configured\n");
        return _instance;
    }
    if (rpiUs < 1000) {
        errlogPrintf("pnzEtherIP: RPI must be at least 1000 us\n");
        return nullptr;
    }
    _instance = new PnzDriver(ip, rpiUs);
    return _instance;
}

PnzDriver* PnzDriver::instance() { return _instance; }

PnzDriver::PnzDriver(const std::string& ip, std::uint32_t rpiUs)
    : _ip(ip),
      _rpiUs(rpiUs),
      _connectionManager(new ConnectionManager())
{
    scanIoInit(&_scanPvt);
    scanIoInit(&_dropScan);          // last-drop timestamp record scan list
    _output.fill(0);
    _input.fill(0);
    epicsTimeGetCurrent(&_startTime);   // for the startup grace window
    _thread = std::thread(&PnzDriver::worker, this);
}

PnzDriver::~PnzDriver()
{
    _stop.store(true);
    if (_thread.joinable())
        _thread.join();
    closeConnection();
    safeResetExplicit();
}

bool PnzDriver::getBit(bool input, unsigned bit) const
{
    if (bit >= 128) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    const auto& a = input ? _input : _output;
    return (a[bit / 8] & (std::uint8_t(1u) << (bit % 8))) != 0;
}

void PnzDriver::setBit(unsigned bit, bool value)
{
    if (bit >= 128) return;
    std::lock_guard<std::mutex> lock(_mutex);
    auto& b = _output[bit / 8];
    const std::uint8_t mask = std::uint8_t(1u << (bit % 8));
    if (value) b |= mask; else b &= std::uint8_t(~mask);
}

std::uint8_t PnzDriver::getInputByte(unsigned byte) const
{
    if (byte >= 32) return 0;
    std::lock_guard<std::mutex> lock(_mutex);
    return _input[byte];
}

std::uint8_t PnzDriver::getOutputByte(unsigned byte) const
{
    if (byte >= 32) return 0;
    std::lock_guard<std::mutex> lock(_mutex);
    return _output[byte];
}

void PnzDriver::setOutputByte(unsigned byte, std::uint8_t value)
{
    if (byte >= 32) return;
    std::lock_guard<std::mutex> lock(_mutex);
    _output[byte] = value;
}

void PnzDriver::copyInput(std::uint8_t* dst, std::size_t n) const
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(dst, _input.data(), n);
}

void PnzDriver::copyInputHex(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::array<std::uint8_t, 32> snap;
    { std::lock_guard<std::mutex> lock(_mutex); snap = _input; }
    std::size_t pos = 0;
    for (std::size_t i = 0; i < snap.size(); ++i) {
        if (pos + 3 >= cap) break;
        int w = std::snprintf(dst + pos, cap - pos, "%02X ", snap[i]);
        if (w <= 0) break;
        pos += static_cast<std::size_t>(w);
    }
    if (pos > 0 && dst[pos - 1] == ' ') --pos;
    if (pos >= cap) pos = cap - 1;
    dst[pos] = '\0';
}

void PnzDriver::copyOutputHex(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::array<std::uint8_t, 32> snap;
    { std::lock_guard<std::mutex> lock(_mutex); snap = _output; }
    std::size_t pos = 0;
    for (std::size_t i = 0; i < snap.size(); ++i) {
        if (pos + 3 >= cap) break;
        int w = std::snprintf(dst + pos, cap - pos, "%02X ", snap[i]);
        if (w <= 0) break;
        pos += static_cast<std::size_t>(w);
    }
    if (pos > 0 && dst[pos - 1] == ' ') --pos;
    if (pos >= cap) pos = cap - 1;
    dst[pos] = '\0';
}

void PnzDriver::copyOutput(std::uint8_t* dst, std::size_t n) const
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(dst, _output.data(), n);
}

void PnzDriver::setOutput(const std::uint8_t* src, std::size_t n)
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(_output.data(), src, n);
}

std::uint8_t PnzDriver::led() const     { return getInputByte(16); }
std::uint8_t PnzDriver::table() const   { return getInputByte(17); }
std::uint8_t PnzDriver::segment() const { return getInputByte(18); }

bool PnzDriver::connected() const { return _connected.load(); }
bool PnzDriver::running() const   { return _running.load(); }
std::uint32_t PnzDriver::rpiUs() const { return _rpiUs; }

/* diagnostics getters (guarded) */
std::uint16_t PnzDriver::identVendor() const { std::lock_guard<std::mutex> l(_mutex); return _idVendor; }
std::uint16_t PnzDriver::identType()   const { std::lock_guard<std::mutex> l(_mutex); return _idType; }
std::uint16_t PnzDriver::identCode()   const { std::lock_guard<std::mutex> l(_mutex); return _idCode; }
std::uint8_t  PnzDriver::identRevMajor() const { std::lock_guard<std::mutex> l(_mutex); return _idRevMaj; }
std::uint8_t  PnzDriver::identRevMinor() const { std::lock_guard<std::mutex> l(_mutex); return _idRevMin; }
std::uint16_t PnzDriver::identStatus() const { std::lock_guard<std::mutex> l(_mutex); return _idStatus; }
std::uint32_t PnzDriver::identSerial() const { std::lock_guard<std::mutex> l(_mutex); return _idSerial; }
std::uint8_t  PnzDriver::deviceStatus() const { std::lock_guard<std::mutex> l(_mutex); return _devStatus; }

void PnzDriver::copyIdentName(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::lock_guard<std::mutex> lock(_mutex);
    std::size_t n = std::min(cap - 1, _idName.size());
    std::memcpy(dst, _idName.data(), n);
    dst[n] = '\0';
}

/* Formatted last-disruption timestamp (see noteDisruption()). */
void PnzDriver::copyLastDropTime(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    epicsTimeStamp ts;
    bool valid;
    {
        std::lock_guard<std::mutex> lk(_dropTimeMutex);
        ts = _lastDropTime;
        valid = _lastDropValid;
    }
    if (!valid) {
        std::snprintf(dst, cap, "no disruption since IOC start");
        return;
    }
    char buf[40] = {0};
    epicsTimeToStrftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S.%03f", &ts);
    std::snprintf(dst, cap, "%s", buf);
}

/* ------------------------------------------------------------------------
 * safeResetExplicit()  (comm-loss crash fix)
 * Release the explicit diagnostics SessionInfo/MessageRouter without letting
 * a throwing destructor (~SessionInfo -> UnRegisterSession on a dead link)
 * escape. This was the core-dump root cause.
 * --------------------------------------------------------------------- */
void PnzDriver::safeResetExplicit() noexcept
{
    try {
        _explicitSession.reset();
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: explicit session teardown threw: %s "
                     "(ignored)\n", e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: explicit session teardown threw unknown "
                     "exception (ignored)\n");
    }

    try {
        _messageRouter.reset();
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: message router teardown threw: %s "
                     "(ignored)\n", e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: message router teardown threw unknown "
                     "exception (ignored)\n");
    }
}

/* ------------------------------------------------------------------------
 * noteDisruption()  (drop-count + timestamp)
 * Record ONE disruption episode. The _commFail latch guarantees exactly one
 * count + timestamp per outage regardless of retry-cycle count. _commFail is
 * cleared by received()/openConnection() success so the NEXT outage counts.
 * Triggers _dropScan so the last-drop stringin re-processes and posts a CA
 * monitor exactly once per disruption (post-only-on-change).
 * --------------------------------------------------------------------- */
void PnzDriver::noteDisruption(const char* reason)
{
    bool was = _commFail.exchange(true);
    if (!was) {
        std::uint32_t n = _dropCount.fetch_add(1) + 1;

        epicsTimeStamp now;
        epicsTimeGetCurrent(&now);
        {
            std::lock_guard<std::mutex> lk(_dropTimeMutex);
            _lastDropTime = now;
            _lastDropValid = true;
        }

        char ts[40] = {0};
        epicsTimeToStrftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S.%03f", &now);
        errlogPrintf("pnzEtherIP: DISRUPTION #%u at %s (%s); IOC staying up, "
                     "outputs/arm retained, retrying\n",
                     static_cast<unsigned>(n), ts,
                     reason ? reason : "unknown");
        _disconnLogCycle = 0;

        // Post exactly one monitor update for the last-drop timestamp record.
        if (_dropScan) scanIoRequest(_dropScan);
    }
}

/* True once we are past the startup grace window. */
bool PnzDriver::pastStartupGrace() const
{
    epicsTimeStamp now;
    epicsTimeGetCurrent(&now);
    double dt = epicsTimeDiffInSeconds(&now, &_startTime);
    return dt >= kStartupGraceSec;
}

bool PnzDriver::openConnection()
{
    auto si = std::make_shared<SessionInfo>(_ip, 0xAF12);
    _session = si;

    ConnectionParameters p;
    p.connectionPath = {0x20, 0x04, 0x24, 151, 0x2C, 150, 0x2C, 100};
    p.o2tRealTimeFormat = true;
    p.originatorVendorId = 342;
    p.originatorSerialNumber = 0x12345;
    p.t2oNetworkConnectionParams |= NetworkConnectionParams::P2P;
    p.t2oNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
    p.t2oNetworkConnectionParams |= 32;
    p.o2tNetworkConnectionParams |= NetworkConnectionParams::P2P;
    p.o2tNetworkConnectionParams |= NetworkConnectionParams::SCHEDULED_PRIORITY;
    p.o2tNetworkConnectionParams |= 32;
    p.o2tRPI = _rpiUs;
    p.t2oRPI = _rpiUs;
    p.transportTypeTrigger |= NetworkConnectionParams::CLASS1;

    auto weak = _connectionManager->forwardOpen(si, p);
    auto ptr = weak.lock();
    if (!ptr) {
        errlogPrintf("pnzEtherIP: Forward Open failed for %s\n", _ip.c_str());
        _connected.store(false);

        if (_everConnected.load()) {
            // Had a good connection before -> any failure now is a real drop.
            noteDisruption("Forward Open failed");
        } else if (pastStartupGrace() && !_startupCounted) {
            // Never connected AND past grace -> booted into a genuine outage
            // (PLC offline / network down before/during reboot). Count once.
            _startupCounted = true;
            noteDisruption("PLC unreachable at startup");
        }
        // else: within grace, never connected yet -> transient handshake; skip.
        return false;
    }
    _io = ptr;
    ptr->setDataToSend(std::vector<std::uint8_t>(32, 0));
    ptr->setReceiveDataListener(
        [this](auto realTimeHeader, auto sequence, const std::vector<std::uint8_t>& data) {
            received(realTimeHeader, sequence, data);
        });
    ptr->setCloseListener([this]() { connectionClosed(); });

    _connected.store(true);
    _running.store(false);
    _commFail.store(false);       // fresh connection: ready to detect next disruption
    _everConnected.store(true);   // had at least one good connection
    _disconnLogCycle = 0;
    errlogPrintf("pnzEtherIP: Class-1 connection established to %s, RPI=%u us\n",
                 _ip.c_str(), static_cast<unsigned>(_rpiUs));
    return true;
}

void PnzDriver::closeConnection()
{
    /* Guard forwardClose(): it sends a ForwardClose over the (possibly dead)
     * session and can throw. Also called from ~PnzDriver. */
    try {
        auto ptr = _io.lock();
        if (ptr && _session)
            _connectionManager->forwardClose(_session, _io);
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: forwardClose threw: %s (ignored)\n", e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: forwardClose threw unknown exception "
                     "(ignored)\n");
    }
    _io.reset();
    _connected.store(false);
    _running.store(false);
}

void PnzDriver::received(std::uint32_t, std::uint16_t,
                         const std::vector<std::uint8_t>& data)
{
    if (data.size() < 32) {
        errlogPrintf("pnzEtherIP: received %zu bytes, expected 32\n", data.size());
        return;
    }
    { std::lock_guard<std::mutex> lock(_mutex); std::copy_n(data.begin(), 32, _input.begin()); }
    _connected.store(true);
    _running.store(true);
    _commFail.store(false);       // healthy traffic -> episode over; next drop counts
    _everConnected.store(true);   // confirmed a good connection
    if (_scanPvt) scanIoRequest(_scanPvt);
}

void PnzDriver::connectionClosed()
{
    // Route through noteDisruption() so counting/timestamp is centralized.
    if (_connected.load())
        noteDisruption("connection closed listener");
    _connected.store(false);
    _running.store(false);
}

/*
    Identity-only diagnostics. Read ONCE per connect, then explicit session is
    dropped and this no-ops (guarded by _identDone). See file header notes.
    All teardown goes through safeResetExplicit() (crash-fix).
*/
void PnzDriver::pollDiagnostics()
{
    try {
        if (!_explicitSession) {
            _explicitSession = std::make_shared<SessionInfo>(
                _ip, 0xAF12, std::chrono::milliseconds(250));
            _messageRouter = std::make_shared<MessageRouter>();
        }
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: diag session open failed: %s\n", e.what());
        safeResetExplicit();
        _diagBackoff = 300;
        return;
    } catch (...) {
        errlogPrintf("pnzEtherIP: diag session open failed (unknown exception)\n");
        safeResetExplicit();
        _diagBackoff = 300;
        return;
    }

    if (!_identDone.load()) {
        try {
            IdentityObject id(1, _explicitSession);
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _idVendor = id.getVendorId();
                _idType   = id.getDeviceType();
                _idCode   = id.getProductCode();
                _idRevMaj = id.getRevision().getMajorRevision();
                _idRevMin = id.getRevision().getMinorRevision();
                _idStatus = id.getStatus();
                _idSerial = id.getSerialNumber();
                _idName   = id.getProductName();
            }
            _identDone.store(true);
            _diagValid.store(true);
            errlogPrintf("pnzEtherIP: Identity OK: '%s' code=%u rev=%u.%u sn=0x%08X status=0x%04X\n",
                         _idName.c_str(),
                         static_cast<unsigned>(_idCode),
                         static_cast<unsigned>(_idRevMaj),
                         static_cast<unsigned>(_idRevMin),
                         static_cast<unsigned>(_idSerial),
                         static_cast<unsigned>(_idStatus));
        } catch (const std::exception& e) {
            errlogPrintf("pnzEtherIP: Identity read failed: %s\n", e.what());
            _diagBackoff = 300;
        } catch (...) {
            errlogPrintf("pnzEtherIP: Identity read failed (unknown exception)\n");
            _diagBackoff = 300;
        }
        safeResetExplicit();
    }
    // NO Device-Status read: unsupported on this device (Assembly Get -> 0x16).
}

void PnzDriver::worker()
{
    Logger::setLogLevel(LogLevel::INFO);

    // ~30 retry cycles (~1 s each in the catch) between "still disconnected".
    static const unsigned kDisconnLogEvery = 30;

    while (!_stop.load()) {
        try {
            if (!_connected.load()) {
                if (!openConnection()) {
                    // openConnection() handles disruption accounting (startup
                    // grace vs. real drop). Rate-limited heartbeat while down.
                    if (++_disconnLogCycle >= kDisconnLogEvery) {
                        _disconnLogCycle = 0;
                        char ts[40] = {0};
                        epicsTimeStamp now; epicsTimeGetCurrent(&now);
                        epicsTimeToStrftime(ts, sizeof(ts),
                                            "%Y-%m-%d %H:%M:%S.%03f", &now);
                        errlogPrintf("pnzEtherIP: [%s] still disconnected "
                                     "(no connection), retrying...\n", ts);
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
            }

            auto ptr = _io.lock();
            if (!ptr) { _connected.store(false); continue; }

            // Arm gate: disarmed -> all zeros regardless of _output.
            std::vector<std::uint8_t> out(32, 0);
            if (_armed.load()) {
                std::lock_guard<std::mutex> lock(_mutex);
                std::copy(_output.begin(), _output.end(), out.begin());
            }
            ptr->setDataToSend(out);

            _connectionManager->handleConnections(std::chrono::milliseconds(100));

            // Graceful drop (handleConnections closed the connection).
            if (!_connectionManager->hasOpenConnections()) {
                noteDisruption("connection closed by handleConnections");
                _connected.store(false);
                _running.store(false);
                _identDone.store(false);
                _diagValid.store(false);
                safeResetExplicit();
                _diagCycle = 0;
                _diagBackoff = 0;
                continue;
            }

            // Identity read: poll only until it succeeds, then stop entirely.
            if (_connected.load() && _running.load() && !_identDone.load()) {
                if (_diagBackoff > 0) {
                    --_diagBackoff;
                } else if (++_diagCycle >= 50) {   // 50 * 100ms = 5s
                    _diagCycle = 0;
                    pollDiagnostics();
                }
            }
        }
        catch (const std::exception& e) {
            // Count per startup-grace rules (mirrors openConnection()).
            if (_everConnected.load()) {
                noteDisruption(e.what());
            } else if (pastStartupGrace() && !_startupCounted) {
                _startupCounted = true;
                noteDisruption(std::string("startup: ").append(e.what()).c_str());
            }
            if (++_disconnLogCycle >= kDisconnLogEvery) {
                _disconnLogCycle = 0;
                char ts[40] = {0};
                epicsTimeStamp now; epicsTimeGetCurrent(&now);
                epicsTimeToStrftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S.%03f", &now);
                errlogPrintf("pnzEtherIP: [%s] still disconnected (%s), retrying...\n",
                             ts, e.what());
            }
            _connected.store(false);
            _running.store(false);
            _identDone.store(false);
            _diagValid.store(false);
            safeResetExplicit();
            _diagCycle = 0;
            _diagBackoff = 0;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        catch (...) {
            if (_everConnected.load()) {
                noteDisruption("unknown exception");
            } else if (pastStartupGrace() && !_startupCounted) {
                _startupCounted = true;
                noteDisruption("startup: unknown exception");
            }
            if (++_disconnLogCycle >= kDisconnLogEvery) {
                _disconnLogCycle = 0;
                char ts[40] = {0};
                epicsTimeStamp now; epicsTimeGetCurrent(&now);
                epicsTimeToStrftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S.%03f", &now);
                errlogPrintf("pnzEtherIP: [%s] still disconnected (unknown "
                             "exception), retrying...\n", ts);
            }
            _connected.store(false);
            _running.store(false);
            _identDone.store(false);
            _diagValid.store(false);
            safeResetExplicit();
            _diagCycle = 0;
            _diagBackoff = 0;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

extern "C" int pnzEtherIPConfigure(const char* ip, unsigned long rpiUs)
{
    if (!ip || !*ip) return -1;
    return PnzDriver::configure(ip, static_cast<std::uint32_t>(rpiUs)) ? 0 : -1;
}

extern "C" int pnzEtherIPArm(int on)
{
    PnzDriver* d = PnzDriver::instance();
    if (!d) {
        errlogPrintf("pnzEtherIP: not configured; call pnzEtherIPConfigure first\n");
        return -1;
    }
    d->setArmed(on != 0);
    errlogPrintf("pnzEtherIP: O->T writes %s\n",
                 on ? "ARMED (output image sent)"
                    : "DISARMED (outputs forced to 0)");
    return 0;
}

static const iocshArg configureArg0 = {"IP address", iocshArgString};
static const iocshArg configureArg1 = {"RPI (microseconds)", iocshArgInt};
static const iocshArg* const configureArgs[] = {&configureArg0, &configureArg1};
static const iocshFuncDef configureFuncDef = {"pnzEtherIPConfigure", 2, configureArgs};
static void configureCallFunc(const iocshArgBuf* args)
{ pnzEtherIPConfigure(args[0].sval, static_cast<unsigned long>(args[1].ival)); }

static const iocshArg armArg0 = {"on (1=arm, 0=disarm)", iocshArgInt};
static const iocshArg* const armArgs[] = {&armArg0};
static const iocshFuncDef armFuncDef = {"pnzEtherIPArm", 1, armArgs};
static void armCallFunc(const iocshArgBuf* args) { pnzEtherIPArm(args[0].ival); }

static void pnzEtherIPRegistrar()
{
    iocshRegister(&configureFuncDef, configureCallFunc);
    iocshRegister(&armFuncDef, armCallFunc);
}
extern "C" { epicsExportRegistrar(pnzEtherIPRegistrar); }
