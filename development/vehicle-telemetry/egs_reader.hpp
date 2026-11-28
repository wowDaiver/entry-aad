// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>

#include "diagnostic_schedule.hpp"
namespace telemetry {
// Nonblocking, fixed read allowlist. All calls run on the HMI dispatcher.
// Physical builds use the local read-only MOST worker, with no ENET fallback.
// No DNS, session change, coding, tester-present or ECU control commands.
class EgsReader {
public:
    explicit EgsReader(unsigned ecu = 0x18, bool expanded = false);
    ~EgsReader();
    void start(double now);
    void stop();
    void poll(double now);
    bool take(double& value, bool& valid);
    bool take_record(DiagnosticReply&);
    static bool temperature(const unsigned char*, size_t, double&);

private:
    friend struct EgsReaderTest;
    enum Phase { Off, Backoff, Connecting, Sending, Awaiting, Idle } phase_;
    int fd_;
    bool identified_, result_, valid_;
    double value_, deadline_, due_, last_;
    unsigned did_, frames_, alive_pending_;
    unsigned ecu_, cursor_, target_, identity_mask_;
    bool expanded_;
    double retry_delay_;
    double next_due_[MaxDiagnosticSources];
    DiagnosticReply reply_;
    unsigned char input_[512], output_[16];
    size_t used_, out_used_, out_size_;
    void close_socket();
    void fail(double, unsigned reason = SocketFailure);
    void connect_socket(double);
    void request(unsigned, double);
    bool flush_output(double);
    bool parse(double);
    int record_index(unsigned) const;
    void complete(bool, const unsigned char*, unsigned, double);
    bool schedule(double);
    EgsReader(const EgsReader&);
    EgsReader& operator=(const EgsReader&);
};
}  // namespace telemetry
