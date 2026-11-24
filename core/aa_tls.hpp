// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// TLS 1.2 client of the Android Auto session over memory buffers: records in, records out, no socket.
#ifndef ENTRY_AAD_CORE_AA_TLS_HPP
#define ENTRY_AAD_CORE_AA_TLS_HPP

#include <openssl/ssl.h>
#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

namespace aa {

// The phone is the TLS server; the head unit is the client and presents its own certificate
// (core/include/hu_certificate.h). The phone's certificate is not checked. The records travel in session frames.
class TlsClient {
public:
    TlsClient();

    // A fresh engine for a new handshake; any previous one is freed first. False, and logged, when OpenSSL refuses.
    bool start();
    // The ClientHello. False without an engine, or when OpenSSL wrote nothing.
    bool hello(std::vector<uint8_t>& records);
    // The phone's handshake records in, the answer for the phone out (empty when there is none). False without an
    // engine or without input.
    bool handshake(const uint8_t* records, size_t size, std::vector<uint8_t>& answer);
    bool established() const { return established_; }

    // Application data, once established. False when nothing came out: for open, also while a record is cut over
    // frames and its rest is still to come; for seal, records is then empty.
    bool seal(const uint8_t* plain, size_t size, std::vector<uint8_t>& records);
    bool open(const uint8_t* records, size_t size, std::vector<uint8_t>& plain);
    // OpenSSL failed for good (a record that failed its check, or refused by the phone's TLS): no later record can be
    // decrypted, nothing more is sealed. Logged once. A new start() clears it.
    bool broken() const { return broken_; }

private:
    typedef std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)> Context;
    typedef std::unique_ptr<SSL, void (*)(SSL*)> Connection;

    bool configure(SSL_CTX* context);
    bool takeOutput(std::vector<uint8_t>& records);  // what OpenSSL wrote for the phone
    void endForGood(const char* step);

    Context context_;
    Connection connection_;  // owns the two memory buffers below once attached
    BIO* fromPhone_;
    BIO* toPhone_;
    bool established_;
    bool broken_;
};

}  // namespace aa

#endif  // ENTRY_AAD_CORE_AA_TLS_HPP
