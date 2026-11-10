// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Owned byte stream for a session; cancellation never destroys resources under an I/O call.
#ifndef ENTRY_AAD_AA_TRANSPORT_HPP
#define ENTRY_AAD_AA_TRANSPORT_HPP

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

namespace aa {
class Transport {
public:
    virtual ~Transport() {}
    // Positive = progress, zero = bounded timeout/wake, negative = terminal error/EOF.
    // read() may return fewer bytes; it must retain any USB packet remainder internally.
    virtual ssize_t read(uint8_t* data, size_t size, int wakeFd, bool& woke) = 0;
    virtual ssize_t write(const uint8_t* data, size_t size) = 0;
    // End one logical AA frame. USB may need a zero-length packet after an exact max-packet multiple.
    virtual bool endWrite() { return true; }
    // Called from a different thread; wakes or bounds I/O without closing handles.
    virtual void cancel() = 0;
    virtual const char* name() const = 0;
};
// Owns fd, including failure paths. Socket send deadline is five seconds.
Transport* socketTransport(int fd);
}  // namespace aa
#endif
