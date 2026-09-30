// pnzDriver.h - STEP 3+4 (multi-PLC + sector routing + per-sector arm)
//
// STATUS: MULTI-PLC. Single-sector behavior must remain bench-parity with
//         Step 1/2. Multi-PLC demux/routing is validatable ONLY in production
//         (bench has one PLC). MUST compile + single-sector bench regression +
//         routing-isolation test before od03.
//
// PnzPlc     - per-PLC state + logic (unchanged from Step 1/2).
// PnzManager - process-wide: ONE shared ConnectionManager, ONE worker thread,
//              map<sector, PnzPlc>. configure(ip,rpi,sector,armAtBoot) registers
//              one PnzPlc per sector. plc(sector) resolves for device support.
//              worker() iterates ALL PnzPlc contexts, sharing one CM.
//
// _armed default is per-PLC via armAtBoot arg (default DISARMED for staged
// bring-up). Source _armed{true} retained but ctor overrides from armAtBoot.

#ifndef PNZ_DRIVER_H
#define PNZ_DRIVER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <epicsTypes.h>
#include <epicsTime.h>
#include <dbScan.h>

namespace eipScanner {
class ConnectionManager;
class IOConnection;
class SessionInfoIf;
class SessionInfo;
class MessageRouter;
}

class PnzManager;   // fwd

// =============================================================================
// PnzPlc - one instance per PLC (per sector). Per-PLC state + logic.
//          (Bodies unchanged from Step 1/2; PnzManager is friend.)
// =============================================================================
class PnzPlc {
public:
    // armAtBoot: initial arm state (false = DISARMED, the safe staged-bring-up
    // default; true = ARMED). Overrides the source _armed initializer.
    PnzPlc(const std::string& sector, const std::string& ip,
           std::uint32_t rpiUs, bool armAtBoot);
    ~PnzPlc();

    PnzPlc(const PnzPlc&) = delete;
    PnzPlc& operator=(const PnzPlc&) = delete;

    // ---- Device-support accessors (signatures IDENTICAL to R1.1.8) ----
    bool getBit(bool input, unsigned bit) const;
    void setBit(unsigned bit, bool value);

    void setArmed(bool a) { _armed.store(a); }
    bool armed() const { return _armed.load(); }

    std::uint32_t dropCount() const { return _dropCount.load(); }
    void copyLastDropTime(char* dst, std::size_t cap) const;

    bool          diagValid()    const { return _diagValid.load(); }
    std::uint16_t identVendor()  const;
    std::uint16_t identType()    const;
    std::uint16_t identCode()    const;
    std::uint8_t  identRevMajor()const;
    std::uint8_t  identRevMinor()const;
    std::uint16_t identStatus()  const;
    std::uint32_t identSerial()  const;
    void          copyIdentName(char* dst, std::size_t cap) const;
    std::uint8_t  deviceStatus() const;

    bool          projValid()       const { return _projValid.load(); }
    std::uint16_t projChecksum()    const;
    std::uint16_t projChecksumAll() const;
    void          copyProjChecksumHex(char* dst, std::size_t cap) const;
    void          copyProjDate(char* dst, std::size_t cap) const;
    void          copyProjName(char* dst, std::size_t cap) const;

    std::uint8_t getInputByte(unsigned byte) const;
    std::uint8_t getOutputByte(unsigned byte) const;
    void setOutputByte(unsigned byte, std::uint8_t value);

    void copyInput(std::uint8_t* dst, std::size_t n) const;
    void copyInputHex(char* dst, std::size_t cap) const;
    void copyOutputHex(char* dst, std::size_t cap) const;
    void copyOutput(std::uint8_t* dst, std::size_t n) const;
    void setOutput(const std::uint8_t* src, std::size_t n);

    std::uint8_t led() const;
    std::uint8_t table() const;
    std::uint8_t segment() const;

    bool connected() const;
    bool running() const;

    std::uint32_t rpiUs() const;
    const std::string& ip() const { return _ip; }
    const std::string& sector() const { return _sector; }

    IOSCANPVT scanPvt() const { return _scanPvt; }
    IOSCANPVT dropScanPvt() const { return _dropScan; }

    /* scondam: 16-Aug-2026; source default ARMED (_armed{true}). In STEP 5
     * (Option F) the ctor overrides this from the armAtBoot arg so each sector
     * boots to a configured, per-sector arm state (staged bring-up boots
     * DISARMED). See PnzManager::configure / pnzEtherIPConfigure.
     *
     * SAFETY: with armAtBoot=false, outputs are forced 0 at boot regardless of
     * the _output image. Per-sector arm via pnzEtherIPArm(sector,1) or the
     * panel @SECTOR:ARMCMD bo.
     *
     * *** Per-sector fail-safe default (armAtBoot) approved by (Safety
     *     Engineer): ____________  Date: __________ ***
     */
    std::atomic<bool> _armed{true};

private:
    void received(std::uint32_t realTimeHeader,
                  std::uint16_t sequence,
                  const std::vector<std::uint8_t>& data);
    void connectionClosed();

    void pollDiagnostics();
    void pollProjectData();
    void safeResetExplicit() noexcept;
    void noteDisruption(const char* reason);
    bool pastStartupGrace() const;

    std::string _sector;
    std::string _ip;
    std::uint32_t _rpiUs;

    mutable std::mutex _mutex;
    std::array<std::uint8_t, 32> _input{};
    std::array<std::uint8_t, 32> _output{};

    std::atomic<bool> _connected{false};
    std::atomic<bool> _running{false};

    std::atomic<std::uint32_t> _dropCount{0};
    std::atomic<bool> _commFail{false};
    mutable std::mutex _dropTimeMutex;
    epicsTimeStamp _lastDropTime{};
    bool _lastDropValid{false};

    std::atomic<bool> _everConnected{false};
    epicsTimeStamp _startTime{};
    bool _startupCounted{false};

    std::uint16_t _idVendor{0}, _idType{0}, _idCode{0}, _idStatus{0};
    std::uint8_t  _idRevMaj{0}, _idRevMin{0};
    std::uint32_t _idSerial{0};
    std::string   _idName;
    std::uint8_t  _devStatus{0};

    std::uint16_t _projSum{0};
    std::uint16_t _projSumAll{0};
    std::uint8_t  _projDay{0}, _projMonth{0};
    std::uint16_t _projYear{0};
    std::uint8_t  _projHour{0}, _projMin{0};
    std::string   _projName;

    std::atomic<bool> _diagValid{false};
    std::atomic<bool> _identDone{false};
    std::atomic<bool> _projValid{false};
    std::atomic<bool> _projDone{false};

    unsigned _diagCycle{0};
    unsigned _diagBackoff{0};
    unsigned _disconnLogCycle{0};

    std::shared_ptr<eipScanner::SessionInfo>   _explicitSession;
    std::shared_ptr<eipScanner::MessageRouter> _messageRouter;

    IOSCANPVT _scanPvt{nullptr};
    IOSCANPVT _dropScan{nullptr};

    // Per-PLC session + implicit connection (within the SHARED CM in PnzManager).
    std::shared_ptr<eipScanner::SessionInfoIf> _session;
    std::weak_ptr<eipScanner::IOConnection>    _io;

    friend class PnzManager;
};

// =============================================================================
// PnzManager - process-wide. ONE shared ConnectionManager, ONE worker thread,
//              map<sector, PnzPlc>. Multi-PLC.
// =============================================================================
class PnzManager {
public:
    static PnzManager* instance();

    // Register one PLC for `sector`. armAtBoot sets its initial arm state.
    // Returns the PnzPlc* (nullptr on error). Starts the shared worker if
    // not running. Sector must be UNIQUE (duplicate -> error, returns existing).
    // NOTE: `sector` MUST match the dbLoadRecords SECTOR macro for routing.
    static PnzPlc* configure(const std::string& sector, const std::string& ip,
                             std::uint32_t rpiUs, bool armAtBoot);

    // Resolve a PLC by sector (used by device support). nullptr if unknown.
    PnzPlc* plc(const std::string& sector);

    // Per-sector arm (used by iocsh pnzEtherIPArm(sector,on)). Returns false if
    // sector unknown.
    bool arm(const std::string& sector, bool on);

    ~PnzManager();

private:
    PnzManager() = default;
    PnzManager(const PnzManager&) = delete;
    PnzManager& operator=(const PnzManager&) = delete;

    void worker();   // single shared loop; iterates ALL PnzPlc contexts.

    // Connection lifecycle for a given PLC via the SHARED ConnectionManager.
    bool openConnection(PnzPlc& plc);
    void closeConnection(PnzPlc& plc);

    static PnzManager* _instance;

    std::atomic<bool> _stop{false};
    std::thread       _thread;

    std::unique_ptr<eipScanner::ConnectionManager> _connectionManager;

    // Keyed by SECTOR (must match dbLoadRecords SECTOR + @SECTOR: links).
    std::map<std::string, std::unique_ptr<PnzPlc>> _plcs;
    mutable std::mutex _plcsMutex;
};

// =============================================================================
// iocsh-registered functions (signatures CHANGED for multi-PLC / per-sector).
//   pnzEtherIPConfigure(ip, rpi, sector, armAtBoot)
//   pnzEtherIPArm(sector, on)
// =============================================================================
extern "C" int pnzEtherIPConfigure(const char* ip, unsigned long rpiUs,
                                   const char* sector, int armAtBoot);
extern "C" int pnzEtherIPArm(const char* sector, int on);

#endif // PNZ_DRIVER_H
