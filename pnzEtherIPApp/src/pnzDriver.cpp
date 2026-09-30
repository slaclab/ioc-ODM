/*
	Mod: Shantha Condamoor (scondam)
	STEP 3+4 (Option F): MULTI-PLC + SECTOR ROUTING + PER-SECTOR ARM.

	PnzDriver split (Step 1) into PnzPlc (per-PLC) + PnzManager (process-wide).
	Path A (Step 2): one shared 0.0.0.0:2222 receive socket in the shared
	ConnectionManager; inbound demuxed by T2O_ID.
	Step 3+4: PnzManager holds map<sector,PnzPlc>; configure() registers one
	PnzPlc per sector; plc(sector) routes device support; worker() iterates ALL
	PnzPlc contexts over the ONE shared ConnectionManager. Per-sector arm.

	ARM: source default _armed{false} (test release, disarmed at source layer);
	ctor sets _armed = armAtBoot (authoritative). Staged bring-up: armAtBoot=0.

	*** BENCH validates SINGLE-SECTOR regression only. Multi-PLC demux and
	    per-PLC drop detection are PRODUCTION-FIRST-EXECUTION (bench has one
	    PLC). See change record. ***

	(Per-PLC safety logic - received/noteDisruption/pastStartupGrace/
	 pollDiagnostics/pollProjectData/safeResetExplicit - is UNCHANGED from
	 Step 1/2, only PnzDriver::->PnzPlc::. DIFF against Step 1/2 to verify.)
*/
#include "pnzDriver.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

static const double kStartupGraceSec = 5.0;

// =============================================================================
// PnzManager
// =============================================================================
PnzManager* PnzManager::_instance = nullptr;

PnzManager* PnzManager::instance()
{
    if (!_instance) _instance = new PnzManager();
    return _instance;
}

PnzPlc* PnzManager::configure(const std::string& sector, const std::string& ip,
                              std::uint32_t rpiUs, bool armAtBoot)
{
    if (sector.empty()) {
        errlogPrintf("pnzEtherIP: configure requires a non-empty sector\n");
        return nullptr;
    }
    if (rpiUs < 1000) {
        errlogPrintf("pnzEtherIP: RPI must be at least 1000 us (sector %s)\n",
                     sector.c_str());
        return nullptr;
    }
    PnzManager* m = instance();
    std::lock_guard<std::mutex> lk(m->_plcsMutex);

    if (m->_plcs.find(sector) != m->_plcs.end()) {
        errlogPrintf("pnzEtherIP: sector %s already configured\n", sector.c_str());
        return m->_plcs[sector].get();
    }

    auto plc = std::unique_ptr<PnzPlc>(new PnzPlc(sector, ip, rpiUs, armAtBoot));
    PnzPlc* raw = plc.get();
    m->_plcs.emplace(sector, std::move(plc));

    if (!m->_connectionManager)
        m->_connectionManager.reset(new ConnectionManager());
    if (!m->_thread.joinable())
        m->_thread = std::thread(&PnzManager::worker, m);

    errlogPrintf("pnzEtherIP: configured sector %s -> %s RPI=%u us armAtBoot=%d\n",
                 sector.c_str(), ip.c_str(), static_cast<unsigned>(rpiUs),
                 armAtBoot ? 1 : 0);
    return raw;
}

PnzPlc* PnzManager::plc(const std::string& sector)
{
    std::lock_guard<std::mutex> lk(_plcsMutex);
    auto it = _plcs.find(sector);
    return (it == _plcs.end()) ? nullptr : it->second.get();
}

bool PnzManager::arm(const std::string& sector, bool on)
{
    PnzPlc* p = plc(sector);
    if (!p) return false;
    p->setArmed(on);
    return true;
}

PnzManager::~PnzManager()
{
    _stop.store(true);
    if (_thread.joinable())
        _thread.join();
    std::lock_guard<std::mutex> lk(_plcsMutex);
    for (auto& kv : _plcs) {
        closeConnection(*kv.second);
        kv.second->safeResetExplicit();
    }
}

// ---- Connection lifecycle for ONE PLC via the SHARED ConnectionManager. ------
//      (Mirrors Step 1 openConnection, but per-PLC state on `plc`, listeners
//       capture &plc.) ----------------------------------------------------------
bool PnzManager::openConnection(PnzPlc& plc)
{
    auto si = std::make_shared<SessionInfo>(plc._ip, 0xAF12);
    plc._session = si;

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
    p.o2tRPI = plc._rpiUs;
    p.t2oRPI = plc._rpiUs;
    p.transportTypeTrigger |= NetworkConnectionParams::CLASS1;

    auto weak = _connectionManager->forwardOpen(si, p);
    auto ptr = weak.lock();
    if (!ptr) {
        errlogPrintf("pnzEtherIP: [%s] Forward Open failed for %s\n",
                     plc._sector.c_str(), plc._ip.c_str());
        plc._connected.store(false);
        if (plc._everConnected.load()) {
            plc.noteDisruption("Forward Open failed");
        } else if (plc.pastStartupGrace() && !plc._startupCounted) {
            plc._startupCounted = true;
            plc.noteDisruption("PLC unreachable at startup");
        }
        return false;
    }
    plc._io = ptr;
    ptr->setDataToSend(std::vector<std::uint8_t>(32, 0));
    ptr->setReceiveDataListener(
        [&plc](auto realTimeHeader, auto sequence, const std::vector<std::uint8_t>& data) {
            plc.received(realTimeHeader, sequence, data);
        });
    ptr->setCloseListener([&plc]() { plc.connectionClosed(); });

    plc._connected.store(true);
    plc._running.store(false);
    plc._commFail.store(false);
    plc._everConnected.store(true);
    plc._disconnLogCycle = 0;
    errlogPrintf("pnzEtherIP: [%s] Class-1 connection established to %s, RPI=%u us\n",
                 plc._sector.c_str(), plc._ip.c_str(),
                 static_cast<unsigned>(plc._rpiUs));
    return true;
}

void PnzManager::closeConnection(PnzPlc& plc)
{
    try {
        auto ptr = plc._io.lock();
        if (ptr && plc._session)
            _connectionManager->forwardClose(plc._session, plc._io);
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: [%s] forwardClose threw: %s (ignored)\n",
                     plc._sector.c_str(), e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: [%s] forwardClose threw unknown exception (ignored)\n",
                     plc._sector.c_str());
    }
    plc._io.reset();
    plc._connected.store(false);
    plc._running.store(false);
}

// ---- MULTI-PLC WORKER (NEW; production-first-execution) ----------------------
//  Iterates ALL PnzPlc contexts each cycle over the ONE shared ConnectionManager.
//
//  *** CRITICAL DIFFERENCE from Step 1: drop detection is PER-PLC, not via the
//      global hasOpenConnections() (which is true if ANY PLC is connected).
//      Each PLC's drop is detected by its own _io expiring / connection absence.
//      This logic is NOT bench-testable with one PLC. ***
//
//  Structure per cycle:
//    1. For each PLC down: try openConnection (grace/drop accounting inside).
//    2. For each PLC up: set arm-gated output image on its IOConnection.
//    3. ONE handleConnections() - services ALL connections on the shared socket.
//    4. For each PLC: detect drop (its _io gone), run per-PLC diag cadence.
//
//  The whole per-cycle body is wrapped so no comm throw kills the IOC (Step 1
//  crash-fix property preserved). A throw is attributed to the PLC being
//  serviced where possible; the loop continues.
void PnzManager::worker()
{
    Logger::setLogLevel(LogLevel::INFO);
    static const unsigned kDisconnLogEvery = 30;

    while (!_stop.load()) {
        // Snapshot the current PLC set (pointers stable: never removed in Step 3).
        std::vector<PnzPlc*> plcs;
        {
            std::lock_guard<std::mutex> lk(_plcsMutex);
            plcs.reserve(_plcs.size());
            for (auto& kv : _plcs) plcs.push_back(kv.second.get());
        }
        if (plcs.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 1+2: ensure each PLC connected; set its arm-gated output image.
        for (PnzPlc* pp : plcs) {
            PnzPlc& plc = *pp;
            try {
                if (!plc._connected.load()) {
                    if (!openConnection(plc)) {
                        if (++plc._disconnLogCycle >= kDisconnLogEvery) {
                            plc._disconnLogCycle = 0;
                            char ts[40] = {0};
                            epicsTimeStamp now; epicsTimeGetCurrent(&now);
                            epicsTimeToStrftime(ts, sizeof(ts),
                                                "%Y-%m-%d %H:%M:%S.%03f", &now);
                            errlogPrintf("pnzEtherIP: [%s] [%s] still disconnected "
                                         "(no connection), retrying...\n",
                                         plc._sector.c_str(), ts);
                        }
                        continue;   // try other PLCs; no per-PLC sleep here
                    }
                }
                auto ptr = plc._io.lock();
                if (!ptr) { plc._connected.store(false); continue; }

                std::vector<std::uint8_t> out(32, 0);
                if (plc._armed.load()) {
                    std::lock_guard<std::mutex> lock(plc._mutex);
                    std::copy(plc._output.begin(), plc._output.end(), out.begin());
                }
                ptr->setDataToSend(out);
            }
            catch (const std::exception& e) {
                // Per-PLC fault during setup: account + reset that PLC only.
                if (plc._everConnected.load()) {
                    plc.noteDisruption(e.what());
                } else if (plc.pastStartupGrace() && !plc._startupCounted) {
                    plc._startupCounted = true;
                    plc.noteDisruption(std::string("startup: ").append(e.what()).c_str());
                }
                plc._connected.store(false);
                plc._running.store(false);
                plc._identDone.store(false);
                plc._diagValid.store(false);
                plc._projDone.store(false);
                plc._projValid.store(false);
                plc.safeResetExplicit();
                plc._diagCycle = 0;
                plc._diagBackoff = 0;
            }
            catch (...) {
                if (plc._everConnected.load()) {
                    plc.noteDisruption("unknown exception");
                } else if (plc.pastStartupGrace() && !plc._startupCounted) {
                    plc._startupCounted = true;
                    plc.noteDisruption("startup: unknown exception");
                }
                plc._connected.store(false);
                plc._running.store(false);
                plc._identDone.store(false);
                plc._diagValid.store(false);
                plc._projDone.store(false);
                plc._projValid.store(false);
                plc.safeResetExplicit();
                plc._diagCycle = 0;
                plc._diagBackoff = 0;
            }
        }

        // 3: ONE handleConnections services ALL connections on the shared socket.
        try {
            _connectionManager->handleConnections(std::chrono::milliseconds(100));
        } catch (const std::exception& e) {
            errlogPrintf("pnzEtherIP: handleConnections threw: %s (ignored)\n", e.what());
        } catch (...) {
            errlogPrintf("pnzEtherIP: handleConnections threw unknown exception (ignored)\n");
        }

        // 4: PER-PLC drop detection + diag cadence.
        //    A PLC is considered dropped if its _io weak_ptr has expired (its
        //    IOConnection was closed/removed by handleConnections' notifyTick
        //    timeout) OR its close listener already flagged it. This REPLACES
        //    Step 1's global hasOpenConnections() check. *** NEW / production-
        //    first ***.
        for (PnzPlc* pp : plcs) {
            PnzPlc& plc = *pp;

            if (plc._connected.load()) {
                auto ptr = plc._io.lock();
                if (!ptr) {
                    // Our IOConnection is gone -> this PLC dropped.
                    plc.noteDisruption("connection removed (per-PLC)");
                    plc._connected.store(false);
                    plc._running.store(false);
                    plc._identDone.store(false);
                    plc._diagValid.store(false);
                    plc._projDone.store(false);
                    plc._projValid.store(false);
                    plc.safeResetExplicit();
                    plc._diagCycle = 0;
                    plc._diagBackoff = 0;
                    continue;
                }
            }

            // Diagnostics cadence (per PLC), only while connected+running.
            if (plc._connected.load() && plc._running.load() &&
                (!plc._identDone.load() || !plc._projDone.load())) {
                if (plc._diagBackoff > 0) {
                    --plc._diagBackoff;
                } else if (++plc._diagCycle >= 50) {   // 50 * 100ms = 5s
                    plc._diagCycle = 0;
                    try {
                        plc.pollDiagnostics();
                        plc.pollProjectData();
                    } catch (const std::exception& e) {
                        errlogPrintf("pnzEtherIP: [%s] diag threw: %s (ignored)\n",
                                     plc._sector.c_str(), e.what());
                    } catch (...) {
                        errlogPrintf("pnzEtherIP: [%s] diag threw unknown (ignored)\n",
                                     plc._sector.c_str());
                    }
                    plc.safeResetExplicit();
                }
            }
        }

        // No connection currently: brief sleep to avoid a hot spin when all down.
        bool anyConnected = false;
        for (PnzPlc* pp : plcs) if (pp->_connected.load()) { anyConnected = true; break; }
        if (!anyConnected)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// =============================================================================
// PnzPlc  (per-PLC; bodies UNCHANGED from Step 1/2 except ctor takes armAtBoot,
//          and log lines gain a [sector] tag. DIFF against Step 1/2.)
// =============================================================================
PnzPlc::PnzPlc(const std::string& sector, const std::string& ip,
               std::uint32_t rpiUs, bool armAtBoot)
    : _sector(sector), _ip(ip), _rpiUs(rpiUs)
{
    _armed.store(armAtBoot);   // authoritative boot arm state (source default {false})
    scanIoInit(&_scanPvt);
    scanIoInit(&_dropScan);
    _output.fill(0);
    _input.fill(0);
    epicsTimeGetCurrent(&_startTime);
}

PnzPlc::~PnzPlc()
{
    safeResetExplicit();
}

bool PnzPlc::getBit(bool input, unsigned bit) const
{
    if (bit >= 128) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    const auto& a = input ? _input : _output;
    return (a[bit / 8] & (std::uint8_t(1u) << (bit % 8))) != 0;
}

void PnzPlc::setBit(unsigned bit, bool value)
{
    if (bit >= 128) return;
    std::lock_guard<std::mutex> lock(_mutex);
    auto& b = _output[bit / 8];
    const std::uint8_t mask = std::uint8_t(1u << (bit % 8));
    if (value) b |= mask; else b &= std::uint8_t(~mask);
}

std::uint8_t PnzPlc::getInputByte(unsigned byte) const
{
    if (byte >= 32) return 0;
    std::lock_guard<std::mutex> lock(_mutex);
    return _input[byte];
}

std::uint8_t PnzPlc::getOutputByte(unsigned byte) const
{
    if (byte >= 32) return 0;
    std::lock_guard<std::mutex> lock(_mutex);
    return _output[byte];
}

void PnzPlc::setOutputByte(unsigned byte, std::uint8_t value)
{
    if (byte >= 32) return;
    std::lock_guard<std::mutex> lock(_mutex);
    _output[byte] = value;
}

void PnzPlc::copyInput(std::uint8_t* dst, std::size_t n) const
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(dst, _input.data(), n);
}

void PnzPlc::copyInputHex(char* dst, std::size_t cap) const
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

void PnzPlc::copyOutputHex(char* dst, std::size_t cap) const
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

void PnzPlc::copyOutput(std::uint8_t* dst, std::size_t n) const
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(dst, _output.data(), n);
}

void PnzPlc::setOutput(const std::uint8_t* src, std::size_t n)
{
    n = std::min<std::size_t>(n, 32);
    std::lock_guard<std::mutex> lock(_mutex);
    std::memcpy(_output.data(), src, n);
}

std::uint8_t PnzPlc::led() const     { return getInputByte(16); }
std::uint8_t PnzPlc::table() const   { return getInputByte(17); }
std::uint8_t PnzPlc::segment() const { return getInputByte(18); }

bool PnzPlc::connected() const { return _connected.load(); }
bool PnzPlc::running() const   { return _running.load(); }
std::uint32_t PnzPlc::rpiUs() const { return _rpiUs; }

std::uint16_t PnzPlc::identVendor() const { std::lock_guard<std::mutex> l(_mutex); return _idVendor; }
std::uint16_t PnzPlc::identType()   const { std::lock_guard<std::mutex> l(_mutex); return _idType; }
std::uint16_t PnzPlc::identCode()   const { std::lock_guard<std::mutex> l(_mutex); return _idCode; }
std::uint8_t  PnzPlc::identRevMajor() const { std::lock_guard<std::mutex> l(_mutex); return _idRevMaj; }
std::uint8_t  PnzPlc::identRevMinor() const { std::lock_guard<std::mutex> l(_mutex); return _idRevMin; }
std::uint16_t PnzPlc::identStatus() const { std::lock_guard<std::mutex> l(_mutex); return _idStatus; }
std::uint32_t PnzPlc::identSerial() const { std::lock_guard<std::mutex> l(_mutex); return _idSerial; }
std::uint8_t  PnzPlc::deviceStatus() const { std::lock_guard<std::mutex> l(_mutex); return _devStatus; }

void PnzPlc::copyIdentName(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::lock_guard<std::mutex> lock(_mutex);
    std::size_t n = std::min(cap - 1, _idName.size());
    std::memcpy(dst, _idName.data(), n);
    dst[n] = '\0';
}

void PnzPlc::copyLastDropTime(char* dst, std::size_t cap) const
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

std::uint16_t PnzPlc::projChecksum()    const { std::lock_guard<std::mutex> l(_mutex); return _projSum; }
std::uint16_t PnzPlc::projChecksumAll() const { std::lock_guard<std::mutex> l(_mutex); return _projSumAll; }

void PnzPlc::copyProjChecksumHex(char* dst, std::size_t cap) const
{
    if (!dst || cap == 0) return;
    std::uint16_t proj, all;
    { std::lock_guard<std::mutex> l(_mutex); proj = _projSum; all = _projSumAll; }
    std::snprintf(dst, cap, "%04X / %04X",
                  static_cast<unsigned>(proj), static_cast<unsigned>(all));
}

void PnzPlc::copyProjDate(char* dst, std::size_t cap) const
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

void PnzPlc::copyProjName(char* dst, std::size_t cap) const
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
void PnzPlc::safeResetExplicit() noexcept
{
    try {
        _explicitSession.reset();
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: [%s] explicit session teardown threw: %s (ignored)\n",
                     _sector.c_str(), e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: [%s] explicit session teardown threw unknown exception (ignored)\n",
                     _sector.c_str());
    }

    try {
        _messageRouter.reset();
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: [%s] message router teardown threw: %s (ignored)\n",
                     _sector.c_str(), e.what());
    } catch (...) {
        errlogPrintf("pnzEtherIP: [%s] message router teardown threw unknown exception (ignored)\n",
                     _sector.c_str());
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
void PnzPlc::noteDisruption(const char* reason)
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
        errlogPrintf("pnzEtherIP: [%s] DISRUPTION #%u at %s (%s); IOC staying up, "
                     "outputs/arm retained, retrying\n",
                     _sector.c_str(), static_cast<unsigned>(n), ts,
                     reason ? reason : "unknown");
        _disconnLogCycle = 0;

        if (_dropScan) scanIoRequest(_dropScan);
    }
}

/* True once we are past the startup grace window. */
bool PnzPlc::pastStartupGrace() const
{
    epicsTimeStamp now;
    epicsTimeGetCurrent(&now);
    double dt = epicsTimeDiffInSeconds(&now, &_startTime);
    return dt >= kStartupGraceSec;
}

void PnzPlc::received(std::uint32_t, std::uint16_t,
                      const std::vector<std::uint8_t>& data)
{
    if (data.size() < 32) {
        errlogPrintf("pnzEtherIP: [%s] received %zu bytes, expected 32\n",
                     _sector.c_str(), data.size());
        return;
    }
    { std::lock_guard<std::mutex> lock(_mutex); std::copy_n(data.begin(), 32, _input.begin()); }
    _connected.store(true);
    _running.store(true);
    _commFail.store(false);
    _everConnected.store(true);
    if (_scanPvt) scanIoRequest(_scanPvt);
}

void PnzPlc::connectionClosed()
{
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
void PnzPlc::pollDiagnostics()
{
    try {
        if (!_explicitSession) {
            _explicitSession = std::make_shared<SessionInfo>(
                _ip, 0xAF12, std::chrono::milliseconds(250));
            _messageRouter = std::make_shared<MessageRouter>();
        }
    } catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: [%s] diag session open failed: %s\n",
                     _sector.c_str(), e.what());
        safeResetExplicit();
        _diagBackoff = 300;
        return;
    } catch (...) {
        errlogPrintf("pnzEtherIP: [%s] diag session open failed (unknown exception)\n",
                     _sector.c_str());
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
            errlogPrintf("pnzEtherIP: [%s] Identity OK: '%s' code=%u rev=%u.%u sn=0x%08X status=0x%04X\n",
                         _sector.c_str(), _idName.c_str(),
                         static_cast<unsigned>(_idCode),
                         static_cast<unsigned>(_idRevMaj),
                         static_cast<unsigned>(_idRevMin),
                         static_cast<unsigned>(_idSerial),
                         static_cast<unsigned>(_idStatus));
        } catch (const std::exception& e) {
            errlogPrintf("pnzEtherIP: [%s] Identity read failed: %s\n",
                         _sector.c_str(), e.what());
            _diagBackoff = 300;
        } catch (...) {
            errlogPrintf("pnzEtherIP: [%s] Identity read failed (unknown exception)\n",
                         _sector.c_str());
            _diagBackoff = 300;
        }
    }
    // NO Device-Status read: unsupported on this device (Assembly Get -> 0x16).
    // Explicit session teardown is done by worker() after pollProjectData().
}

/*
    PNOZmulti "Project data" (Operating Manual sec. 8.7). CIP class 0xB0,
    Instance 6, Attr 1 (check sums + date), Attr 2 (project name). Read ONCE
    per connect (guarded by _projDone), reusing the explicit session created by
    pollDiagnostics(). Teardown via safeResetExplicit() in worker().
    Byte order: check sums big-endian (manual A1B2); project name big-endian per
    UNICODE char, 0xFFFF end marker. 0x16 (OBJECT_DOES_NOT_EXIST) marks
    _projDone to stop retrying (unsupported path).
*/
void PnzPlc::pollProjectData()
{
    if (_projDone.load())
        return;
    if (!_explicitSession || !_messageRouter)
        return;

    try {
        auto r1 = _messageRouter->sendRequest(
            _explicitSession,
            static_cast<eipScanner::cip::CipUsint>(ServiceCodes::GET_ATTRIBUTE_SINGLE),
            EPath(0xB0, 6, 1),
            {});

        if (r1.getGeneralStatusCode() != GeneralStatusCodes::SUCCESS) {
            errlogPrintf("pnzEtherIP: [%s] project-data (0xB0/6/1) read status=0x%02X; "
                         "skipping project data\n",
                         _sector.c_str(),
                         static_cast<unsigned>(r1.getGeneralStatusCode()));
            if (r1.getGeneralStatusCode() == 0x16) {
                _projDone.store(true);
            } else {
                _diagBackoff = 300;
            }
            return;
        }

        const auto& d1 = r1.getData();
        if (d1.size() < 24) {
            errlogPrintf("pnzEtherIP: [%s] project-data attr1 too short (%zu bytes)\n",
                         _sector.c_str(), d1.size());
            _diagBackoff = 300;
            return;
        }

        std::uint16_t projSum    = static_cast<std::uint16_t>((d1[0] << 8) | d1[1]);
        std::uint16_t projSumAll = static_cast<std::uint16_t>((d1[2] << 8) | d1[3]);
        std::uint8_t  day    = d1[12];
        std::uint8_t  month  = d1[13];
        std::uint16_t year   = static_cast<std::uint16_t>((d1[14] << 8) | d1[15]);
        std::uint8_t  hour   = d1[20];
        std::uint8_t  minute = d1[21];

        std::string name;
        auto r2 = _messageRouter->sendRequest(
            _explicitSession,
            static_cast<eipScanner::cip::CipUsint>(ServiceCodes::GET_ATTRIBUTE_SINGLE),
            EPath(0xB0, 6, 2),
            {});

        if (r2.getGeneralStatusCode() == GeneralStatusCodes::SUCCESS) {
            const auto& d2 = r2.getData();
            for (std::size_t i = 0; i + 1 < d2.size() && i < 64; i += 2) {
                std::uint16_t ch = static_cast<std::uint16_t>((d2[i] << 8) | d2[i + 1]);
                if (ch == 0xFFFF || ch == 0x0000) break;
                if (ch >= 0x20 && ch <= 0x7E)
                    name.push_back(static_cast<char>(ch & 0x7F));
                else
                    name.push_back('?');
            }
        } else {
            errlogPrintf("pnzEtherIP: [%s] project-name (0xB0/6/2) read status=0x%02X "
                         "(name left blank)\n",
                         _sector.c_str(),
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

        errlogPrintf("pnzEtherIP: [%s] Project data OK: safety=%04X total=%04X "
                     "date=%02u.%02u.%04u %02u:%02u name='%s'\n",
                     _sector.c_str(),
                     static_cast<unsigned>(projSum),
                     static_cast<unsigned>(projSumAll),
                     static_cast<unsigned>(day), static_cast<unsigned>(month),
                     static_cast<unsigned>(year), static_cast<unsigned>(hour),
                     static_cast<unsigned>(minute),
                     name.c_str());
    }
    catch (const std::exception& e) {
        errlogPrintf("pnzEtherIP: [%s] project-data read failed: %s\n",
                     _sector.c_str(), e.what());
        _diagBackoff = 300;
    }
    catch (...) {
        errlogPrintf("pnzEtherIP: [%s] project-data read failed (unknown exception)\n",
                     _sector.c_str());
        _diagBackoff = 300;
    }
}

// =============================================================================
// iocsh registration  (SIGNATURES CHANGED: sector + armAtBoot)
// =============================================================================
extern "C" int pnzEtherIPConfigure(const char* ip, unsigned long rpiUs,
                                   const char* sector, int armAtBoot)
{
    if (!ip || !*ip) { errlogPrintf("pnzEtherIP: configure needs an IP\n"); return -1; }
    if (!sector || !*sector) { errlogPrintf("pnzEtherIP: configure needs a sector\n"); return -1; }
    return PnzManager::configure(sector, ip,
                                 static_cast<std::uint32_t>(rpiUs),
                                 armAtBoot != 0) ? 0 : -1;
}

extern "C" int pnzEtherIPArm(const char* sector, int on)
{
    if (!sector || !*sector) {
        errlogPrintf("pnzEtherIP: pnzEtherIPArm needs a sector\n");
        return -1;
    }
    if (!PnzManager::instance()->arm(sector, on != 0)) {
        errlogPrintf("pnzEtherIP: pnzEtherIPArm unknown sector '%s'\n", sector);
        return -1;
    }
    errlogPrintf("pnzEtherIP: [%s] O->T writes %s\n",
                 sector,
                 on ? "ARMED (output image sent)"
                    : "DISARMED (outputs forced to 0)");
    return 0;
}

static const iocshArg configureArg0 = {"IP address", iocshArgString};
static const iocshArg configureArg1 = {"RPI (microseconds)", iocshArgInt};
static const iocshArg configureArg2 = {"sector", iocshArgString};
static const iocshArg configureArg3 = {"armAtBoot (1=armed,0=disarmed)", iocshArgInt};
static const iocshArg* const configureArgs[] =
    {&configureArg0, &configureArg1, &configureArg2, &configureArg3};
static const iocshFuncDef configureFuncDef = {"pnzEtherIPConfigure", 4, configureArgs};
static void configureCallFunc(const iocshArgBuf* args)
{ pnzEtherIPConfigure(args[0].sval, static_cast<unsigned long>(args[1].ival),
                      args[2].sval, args[3].ival); }

static const iocshArg armArg0 = {"sector", iocshArgString};
static const iocshArg armArg1 = {"on (1=arm, 0=disarm)", iocshArgInt};
static const iocshArg* const armArgs[] = {&armArg0, &armArg1};
static const iocshFuncDef armFuncDef = {"pnzEtherIPArm", 2, armArgs};
static void armCallFunc(const iocshArgBuf* args) { pnzEtherIPArm(args[0].sval, args[1].ival); }

static void pnzEtherIPRegistrar()
{
    iocshRegister(&configureFuncDef, configureCallFunc);
    iocshRegister(&armFuncDef, armCallFunc);
}
extern "C" { epicsExportRegistrar(pnzEtherIPRegistrar); }
