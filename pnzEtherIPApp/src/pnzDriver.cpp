/*
	Mod: Shantha Condamoor (scondam)
	Date: 16-Aug-2026

	Arm gate: default DISARMED (_armed{false}); disarmed -> outputs forced 0.
	iocsh: pnzEtherIPArm 1/0. PV: ODM:$(SECTOR):ARM:CMD.

	Comm-loss annunciation: _dropCount increments once per disruption episode.

	PLC diagnostics (design A): explicit messaging on the WORKER THREAD, after
	handleConnections(), using a SEPARATE short-timeout SessionInfo.
	NOTE: This device serves the Identity Object (0x01) via explicit messaging,
	and the PNOZmulti "Project data" (60-byte block) via Assembly class 0x04,
	Instance 6, Attributes 1 (check sums + date) and 2 (project name) -- see
	Operating Manual sec. 8.7. The Assembly Object (0x04) instance 1 does NOT
	support Get_Attribute_Single (verified on bench: returns 0x16
	OBJECT_DOES_NOT_EXIST), so there is NO Device-Status read. Identity and
	project data are read ONCE at connect; run/stop state comes from the cyclic
	LED byte (see devPnz.cpp @LED and pnz.db PLC:Running).

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
	Mod: Shantha Condamoor (scondam)  --  PNOZmulti PROJECT DATA (8.7)
	  - pollProjectData() reads Assembly 0x04 / Instance 6 / Attr 1 (check
	    sums + date) and Attr 2 (project name), ONCE per connect, reusing the
	    same explicit session created by pollDiagnostics().
	  - safeResetExplicit() moved OUT of pollDiagnostics() into worker() so the
	    session survives long enough for both Identity and project-data reads.
	  - Byte order: checksum big-endian (manual example A1B2); project name
	    little-endian per UNICODE char. *** VERIFY ON BENCH ***
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

/* PNOZmulti project-data getters (guarded) */
std::uint16_t PnzDriver::projChecksum()    const { std::lock_guard<std::mutex> l(_mutex); return _projSum; }
std::uint16_t PnzDriver::projChecksumAll() const { std::lock_guard<std::mutex> l(_mutex); return _projSumAll; }

void PnzDriver::copyProjChecksumHex(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::uint16_t proj, all;
    { std::lock_guard<std::mutex> l(_mutex); proj = _projSum; all = _projSumAll; }
    // e.g. "A1B2 / 0000"  (project sum / overall sum)
    std::snprintf(dst, cap, "%04X / %04X",
                  static_cast<unsigned>(proj), static_cast<unsigned>(all));
}

void PnzDriver::copyProjDate(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    bool valid;
    std::uint8_t  d, mo, h, mi;
    std::uint16_t y;
    {
        std::lock_guard<std::mutex> l(_mutex);
        valid = _projValid.load();
        d = _projDay; mo = _projMonth; y = _projYear; h = _projHour; mi = _projMin;
    }
    if (!valid) { std::snprintf(dst, cap, "project data not read"); return; }

    // Some projects have no creation date set (day/month/year all zero) but a
    // valid time. Show date and time honestly rather than "00.00.0000".
    if (d == 0 && mo == 0 && y == 0) {
        std::snprintf(dst, cap, "date not set  %02u:%02u",
                      static_cast<unsigned>(h), static_cast<unsigned>(mi));
    } else {
        std::snprintf(dst, cap, "%02u.%02u.%04u %02u:%02u",
                      static_cast<unsigned>(d), static_cast<unsigned>(mo),
                      static_cast<unsigned>(y), static_cast<unsigned>(h),
                      static_cast<unsigned>(mi));
    }
}

void PnzDriver::copyProjName(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::lock_guard<std::mutex> l(_mutex);
    std::size_t n = std::min(cap - 1, _projName.size());
    std::memcpy(dst, _projName.data(), n);
    dst[n] = '\0';
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
    auto si = std::make_shared<SessionInfo>(_ip, 0xAF12, std::chrono::milliseconds(3000));
    _session = si;

    ConnectionParameters p;
    p.connectionPath = {0x20, 0x04, 0x24, 151, 0x2C, 150, 0x2C, 100};
    p.o2tRealTimeFormat = true;
    p.originatorVendorId = 342;
    p.originatorSerialNumber = 0x12345;
    p.connectionTimeoutMultiplier = 4;   // TEST: x64 of RPI (6.4s@100ms) - ride through intermittent UDP 2222 loss
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
    Identity-only diagnostics. Read ONCE per connect (guarded by _identDone).
    Creates the explicit session/router if needed; DOES NOT tear it down --
    the worker does that (via safeResetExplicit()) after pollProjectData(), so
    both reads can share one session. See file header notes.
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
    }
    // NO Device-Status read: unsupported on this device (Assembly Get -> 0x16).
    // NOTE: explicit session teardown is done by worker() after
    // pollProjectData(), so both reads can share this session.
}

/*
    PNOZmulti "Project data" (Operating Manual sec. 8.7).
    EtherNet/IP access: the service data is instanced from CIP class 0xB0
    (manual sec. 4.8.3), NOT the Assembly class 0x04. Within class 0xB0:
        Check sums + Date : Instance 6, Attribute 1 (bytes 0..23)
        Project name      : Instance 6, Attribute 2 (UNICODE, 2 bytes/char)

    Read ONCE per connect (guarded by _projDone), reusing the explicit session
    created by pollDiagnostics(). All teardown is via safeResetExplicit() in
    worker(), so both Identity and project reads share one session.

    Byte order (verified on bench against a known project, checksum 7E20):
      - Check sums  : big-endian (byte0=high, byte1=low; manual example A1B2)
      - Date/time   : day(b12), month(b13), year(b14..15 BE), hour(b20), min(b21)
      - Project name: big-endian per UNICODE char, 0xFFFF end marker. This
                      firmware returns up to 36 bytes (18 chars); longer names
                      are truncated BY THE DEVICE (not by this code).

    Fail-safe: on any error/rejection this logs and no-ops without throwing, so
    the IOC is unaffected. A 0x16 (OBJECT_DOES_NOT_EXIST) marks _projDone to
    stop retrying (unsupported path on this firmware/config).
*/
void PnzDriver::pollProjectData()
{
    if (_projDone.load())
        return;

    // Requires a live explicit session (pollDiagnostics() creates it first).
    if (!_explicitSession || !_messageRouter)
        return;

    try {
        // ---- Attribute 1: check sums (0..3) + date (12..23) => >= 24 bytes ----
        auto r1 = _messageRouter->sendRequest(
            _explicitSession,
            static_cast<eipScanner::cip::CipUsint>(ServiceCodes::GET_ATTRIBUTE_SINGLE),
            EPath(0xB0, 6, 1),
            {});

        if (r1.getGeneralStatusCode() != GeneralStatusCodes::SUCCESS) {
            errlogPrintf("pnzEtherIP: project-data (0xB0/6/1) read status=0x%02X; "
                         "skipping project data\n",
                         static_cast<unsigned>(r1.getGeneralStatusCode()));
            // 0x16 = OBJECT_DOES_NOT_EXIST: this firmware/config does not serve
            // project data via this path. Stop retrying every cycle.
            if (r1.getGeneralStatusCode() == 0x16) {
                _projDone.store(true);
            } else {
                _diagBackoff = 300;      // transient: back off and retry later
            }
            return;
        }

        const auto& d1 = r1.getData();
        if (d1.size() < 24) {
            errlogPrintf("pnzEtherIP: project-data attr1 too short (%zu bytes)\n",
                         d1.size());
            _diagBackoff = 300;
            return;
        }

        // Check sums: byte0=high, byte1=low (manual example A1B2).
        std::uint16_t projSum    = static_cast<std::uint16_t>((d1[0] << 8) | d1[1]);
        std::uint16_t projSumAll = static_cast<std::uint16_t>((d1[2] << 8) | d1[3]);

        // Date (8.7.2): b12=day, b13=month, b14..15=year(BE),
        //               b20=hour, b21=minute.
        std::uint8_t  day    = d1[12];
        std::uint8_t  month  = d1[13];
        std::uint16_t year   = static_cast<std::uint16_t>((d1[14] << 8) | d1[15]);
        std::uint8_t  hour   = d1[20];
        std::uint8_t  minute = d1[21];

        // ---- Attribute 2: project name (UNICODE, 2 bytes/char, FFFF-terminated) ----
        std::string name;
        auto r2 = _messageRouter->sendRequest(
            _explicitSession,
            static_cast<eipScanner::cip::CipUsint>(ServiceCodes::GET_ATTRIBUTE_SINGLE),
            EPath(0xB0, 6, 2),
            {});

        if (r2.getGeneralStatusCode() == GeneralStatusCodes::SUCCESS) {
            const auto& d2 = r2.getData();
            // Read all characters the device returns (this firmware returns up
            // to 36 bytes = 18 chars). 2 bytes/char, big-endian, 0xFFFF end.
            for (std::size_t i = 0; i + 1 < d2.size() && i < 64; i += 2) {
                std::uint16_t ch = static_cast<std::uint16_t>((d2[i] << 8) | d2[i + 1]);
                if (ch == 0xFFFF || ch == 0x0000) break;
                if (ch >= 0x20 && ch <= 0x7E)
                    name.push_back(static_cast<char>(ch & 0x7F));
                else
                    name.push_back('?');
            }
        } else {
            errlogPrintf("pnzEtherIP: project-name (0xB0/6/2) read status=0x%02X "
                         "(name left blank)\n",
                         static_cast<unsigned>(r2.getGeneralStatusCode()));
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _projSum    = projSum;
            _projSumAll = projSumAll;
            _projDay    = day;
            _projMonth  = month;
            _projYear   = year;
            _projHour   = hour;
            _projMin    = minute;
            _projName   = name;
        }
        _projValid.store(true);
        _projDone.store(true);

        errlogPrintf("pnzEtherIP: Project data OK: safety=%04X total=%04X "
                     "date=%02u.%02u.%04u %02u:%02u name='%s'\n",
                     static_cast<unsigned>(projSum),
                     static_cast<unsigned>(projSumAll),
                     static_cast<unsigned>(day), static_cast<unsigned>(month),
                     static_cast<unsigned>(year), static_cast<unsigned>(hour),
                     static_cast<unsigned>(minute),
                     name.c_str());
    }
    catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: project-data read failed: %s\n", e.what());
        _diagBackoff = 300;
    }
    catch (...) {
        errlogPrintf("pnzEtherIP: project-data read failed (unknown exception)\n");
        _diagBackoff = 300;
    }
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
                _projDone.store(false);
                _projValid.store(false);
                safeResetExplicit();
                _diagCycle = 0;
                _diagBackoff = 0;
                continue;
            }
// ---- TEST 2026-09-01: diagnostics DISABLED to isolate PLC single-
            // session limit. No 2nd (explicit) session is opened; only the
            // Class-1 cyclic connection runs. If the connection stays up with
            // this disabled, the PNOZ cannot tolerate the concurrent explicit
            // session and we must redesign diagnostics.
#if 0
            // Identity + project-data read: poll only until BOTH succeed, then
            // stop entirely. Both share one explicit session; the worker tears
            // it down after each attempt via safeResetExplicit().
            if (_connected.load() && _running.load() &&
                (!_identDone.load() || !_projDone.load())) {
                if (_diagBackoff > 0) {
                    --_diagBackoff;
                } else if (++_diagCycle >= 50) {   // 50 * 100ms = 5s
                    _diagCycle = 0;
                    pollDiagnostics();     // creates the explicit session + reads Identity
                    pollProjectData();     // reuses that session for the 60-byte block
                    safeResetExplicit();   // done with the explicit session this cycle
                }
            }
#endif

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
            _projDone.store(false);
            _projValid.store(false);
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
            _projDone.store(false);
            _projValid.store(false);
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
