#ifndef PNZ_DRIVER_H
#define PNZ_DRIVER_H

#include <array>
#include <atomic>
#include <cstdint>
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

class PnzDriver {
public:
    static PnzDriver* configure(const std::string& ip, std::uint32_t rpiUs);
    static PnzDriver* instance();

    ~PnzDriver();

    bool getBit(bool input, unsigned bit) const;
    void setBit(unsigned bit, bool value);

    /* scondam: 16-Aug-2026 */
    void setArmed(bool a) { _armed.store(a); }
    bool armed() const { return _armed.load(); }

    /* comm-loss annunciation */
    std::uint32_t dropCount() const { return _dropCount.load(); }
    void copyLastDropTime(char* dst, std::size_t cap) const;   // formatted timestamp

    /* PLC diagnostics (explicit messaging) */
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

    /* scondam: 16-Aug-2026 */
    std::atomic<bool> _armed{false};   // O->T writes disabled by default (fail-safe)

    std::uint32_t rpiUs() const;

    IOSCANPVT scanPvt() const { return _scanPvt; }
    IOSCANPVT dropScanPvt() const { return _dropScan; }   // fires only on a new disruption

private:
    PnzDriver(const std::string& ip, std::uint32_t rpiUs);
    PnzDriver(const PnzDriver&) = delete;
    PnzDriver& operator=(const PnzDriver&) = delete;

    void worker();
    bool openConnection();
    void closeConnection();

    void received(std::uint32_t realTimeHeader,
                  std::uint16_t sequence,
                  const std::vector<std::uint8_t>& data);
    void connectionClosed();

    /* diagnostics (design A: called from worker() after handleConnections) */
    void pollDiagnostics();

    /* scondam: comm-loss crash fix ---------------------------------------
     * Tear down the explicit (diagnostics) session/router without letting a
     * throwing destructor escape. ~SessionInfo() sends UnRegisterSession;
     * on a dead link that socket write throws std::system_error. If that
     * happens while we are already handling an exception (e.g. after an
     * Identity timeout), the throw during unwinding calls std::terminate()
     * and the IOC core-dumps. This helper makes teardown noexcept.
     * ------------------------------------------------------------------- */
    void safeResetExplicit() noexcept;

    /* scondam: drop-count + timestamp ------------------------------------
     * Record ONE disruption episode (count + timestamp) on the failed-edge,
     * guarded by the _commFail latch so retry cycles don't inflate the count.
     * Also triggers _dropScan so the last-drop stringin re-processes and posts
     * a CA monitor exactly once per disruption (post-only-on-change).
     * ------------------------------------------------------------------- */
    void noteDisruption(const char* reason);

    /* scondam: startup grace ---------------------------------------------
     * True once we are past the startup grace window. A single sub-second
     * first-connect handshake retry on a HEALTHY boot must NOT be counted as
     * a disruption; but a genuine "booted into a dead network / PLC offline"
     * condition (still failing past the grace window, never connected) MUST
     * be counted. See openConnection() and worker() catch blocks.
     * ------------------------------------------------------------------- */
    bool pastStartupGrace() const;

    std::string _ip;
    std::uint32_t _rpiUs;

    mutable std::mutex _mutex;
    std::array<std::uint8_t, 32> _input{};
    std::array<std::uint8_t, 32> _output{};

    std::atomic<bool> _stop{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _running{false};

    /* comm-loss annunciation */
    std::atomic<std::uint32_t> _dropCount{0};
    std::atomic<bool> _commFail{false};        // true while in a failed/disconnected episode
    mutable std::mutex _dropTimeMutex;         // guards _lastDropTime/_lastDropValid
    epicsTimeStamp _lastDropTime{};            // time of most recent disruption edge
    bool _lastDropValid{false};                // false until first disruption

    /* startup-grace disruption accounting */
    std::atomic<bool> _everConnected{false};   // true after first successful connect
    epicsTimeStamp _startTime{};               // driver start time (for startup grace)
    bool _startupCounted{false};               // count "dead at boot" only once

    /* diagnostics data (guarded by _mutex) */
    std::uint16_t _idVendor{0}, _idType{0}, _idCode{0}, _idStatus{0};
    std::uint8_t  _idRevMaj{0}, _idRevMin{0};
    std::uint32_t _idSerial{0};
    std::string   _idName;
    std::uint8_t  _devStatus{0};

    std::atomic<bool> _diagValid{false};   // true once Identity read OK
    std::atomic<bool> _identDone{false};   // read Identity only once per connect
    unsigned _diagCycle{0};                 // worker-cycle counter for cadence
    unsigned _diagBackoff{0};               // cycles to wait after a failure
    unsigned _disconnLogCycle{0};           // rate-limit "still disconnected" heartbeat log

    std::shared_ptr<eipScanner::SessionInfo>  _explicitSession;
    std::shared_ptr<eipScanner::MessageRouter> _messageRouter;

    IOSCANPVT _scanPvt{nullptr};
    IOSCANPVT _dropScan{nullptr};   // scan list for the last-drop timestamp record
    std::thread _thread;

    std::unique_ptr<eipScanner::ConnectionManager> _connectionManager;
    std::shared_ptr<eipScanner::SessionInfoIf> _session;
    std::weak_ptr<eipScanner::IOConnection> _io;

    static PnzDriver* _instance;
};

extern "C" int pnzEtherIPConfigure(const char* ip, unsigned long rpiUs);

#endif
