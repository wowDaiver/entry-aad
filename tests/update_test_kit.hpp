// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the update tests share: release archives built as tools/package_release.py builds them (USTAR, UPDATE and its
// signature), signed by a key made for the test, never the project's.
#ifndef ENTRY_AAD_TESTS_UPDATE_TEST_KIT_HPP
#define ENTRY_AAD_TESTS_UPDATE_TEST_KIT_HPP

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../platform/update_package.hpp"

namespace kit {

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

// ---- archives ---------------------------------------------------------------------------------------------------
// width - 1 octal digits, zero-padded, then NUL.
inline void octalField(char* header, size_t offset, size_t width, unsigned long long value) {
    for (size_t i = width - 1; i-- > 0;) {
        header[offset + i] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    header[offset + width - 1] = 0;
}

// One USTAR header as Python's tarfile writes it; a name over 100 characters goes partly into the prefix field.
inline std::string header(const std::string& name, size_t size, char type, unsigned mode) {
    char h[512];
    memset(h, 0, sizeof(h));
    std::string base = name, prefix;
    if (name.size() > 100) {
        const size_t slash = name.rfind('/', name.size() - 2);
        prefix = name.substr(0, slash);
        base = name.substr(slash + 1);
    }
    memcpy(h, base.data(), base.size());
    memcpy(h + 345, prefix.data(), prefix.size());
    octalField(h, 100, 8, mode);
    octalField(h, 108, 8, 0);
    octalField(h, 116, 8, 0);
    octalField(h, 124, 12, size);
    octalField(h, 136, 12, 1763856000);
    h[156] = type;
    memcpy(h + 257,
           "ustar\0"
           "00",
           8);
    memcpy(h + 265, "root", 4);
    memcpy(h + 297, "root", 4);
    memset(h + 148, ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof(h); ++i) {
        sum += static_cast<unsigned char>(h[i]);
    }
    octalField(h, 148, 7, sum);
    h[155] = ' ';
    return std::string(h, sizeof(h));
}

inline void addFolder(std::string& tar, const std::string& name) {
    tar += header(name + "/", 0, '5', 0755);
}

inline void addFile(std::string& tar, const std::string& name, const std::string& content, unsigned mode = 0644,
                    char type = '0') {
    tar += header(name, content.size(), type, mode);
    tar += content;
    tar += std::string((512 - content.size() % 512) % 512, '\0');
}

inline void endArchive(std::string& tar) {
    tar += std::string(1024, '\0');
    tar += std::string((10240 - tar.size() % 10240) % 10240, '\0');
}

// ---- keys ---------------------------------------------------------------------------------------------------------
inline EVP_PKEY* makeKey(int bits) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* key = nullptr;
    CHECK(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) == 1 &&
          EVP_PKEY_keygen(ctx, &key) == 1);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

inline std::string publicDer(EVP_PKEY* key) {
    const int size = i2d_PUBKEY(key, nullptr);
    CHECK(size > 0);
    std::string der(static_cast<size_t>(size), '\0');
    unsigned char* p = reinterpret_cast<unsigned char*>(&der[0]);
    CHECK(i2d_PUBKEY(key, &p) == size);
    return der;
}

inline std::string sign(EVP_PKEY* key, const std::string& data) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_create();
    std::string sig(static_cast<size_t>(EVP_PKEY_size(key)), '\0');
    unsigned int size = 0;
    CHECK(ctx && EVP_SignInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
          EVP_SignUpdate(ctx, data.data(), data.size()) == 1 &&
          EVP_SignFinal(ctx, reinterpret_cast<unsigned char*>(&sig[0]), &size, key) == 1);
    EVP_MD_CTX_destroy(ctx);
    sig.resize(size);
    return sig;
}

// ---- releases -----------------------------------------------------------------------------------------------------
struct File {
    std::string path;
    std::string content;
    unsigned mode;
};
struct Release {
    std::string version;
    std::vector<File> files;
};

inline std::string plugin(const char* name) {
    return std::string(evo::kUpdatePluginDir) + "/" + name;
}

inline void le(std::string& s, size_t at, uint32_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        s[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
}

// The smallest ELF file of the kind the car runs, around payload: a header, a load segment over the whole file, and
// for a program the request for the loader (interpreter). The other fields let a test build what the car refuses.
struct Elf {
    bool library;
    std::string interpreter;
    unsigned char fileClass;  // 1: 32-bit
    uint32_t machine;         // 40: ARM
    uint32_t flags;           // EABI 5, as the car's own programs
    Elf(bool lib = false) : library(lib), interpreter(evo::kCarLoader), fileClass(1), machine(40), flags(0x05000002) {}
};
inline std::string elf(const Elf& e, const std::string& payload) {
    const size_t headers = e.library ? 1 : 2, interpAt = 52 + 32 * headers;
    std::string file(interpAt, '\0');
    memcpy(&file[0], "\177ELF", 4);
    file[4] = static_cast<char>(e.fileClass);
    file[5] = 1;  // little-endian
    file[6] = 1;
    le(file, 16, e.library ? 3 : 2, 2);
    le(file, 18, e.machine, 2);
    le(file, 20, 1, 4);
    le(file, 28, 52, 4);
    le(file, 36, e.flags, 4);
    le(file, 40, 52, 2);
    le(file, 42, 32, 2);
    le(file, 44, static_cast<uint32_t>(headers), 2);
    if (!e.library) {
        file += e.interpreter + std::string(1, '\0');
    }
    file += payload;
    size_t h = 52;
    if (!e.library) {
        le(file, h, 3, 4);  // PT_INTERP
        le(file, h + 4, static_cast<uint32_t>(interpAt), 4);
        le(file, h + 16, static_cast<uint32_t>(e.interpreter.size() + 1), 4);
        h += 32;
    }
    le(file, h, 1, 4);  // PT_LOAD: the whole file
    le(file, h + 16, static_cast<uint32_t>(file.size()), 4);
    le(file, h + 20, static_cast<uint32_t>(file.size()), 4);
    return file;
}

// The files of a release as the packager lists them: two programs and the app, their content tagged with the version.
inline Release release(const std::string& version) {
    Release r;
    r.version = version;
    r.files.push_back({"opt/aa/aa-supervisor", elf(Elf(), "supervisor " + version), 0755});
    r.files.push_back({"opt/aa/entry-aad", elf(Elf(), "service " + version + std::string(3000, 's')), 0755});
    r.files.push_back({plugin("EntryAad.manifest"), "manifest " + version, 0644});
    r.files.push_back({plugin("EntryAad.xml"), "<pluginApps/> " + version, 0644});
    r.files.push_back({plugin("EntryAad_ALL_Images.zip"), "images", 0644});
    r.files.push_back({plugin("EntryAad_ALL_Texts.zip"), "texts " + version, 0644});
    r.files.push_back({plugin("libEntryAad.so"), elf(Elf(true), "app " + version), 0755});
    return r;
}

inline std::string listText(const Release& r) {
    std::string text = std::string(evo::kUpdateFirstLine) + "\nversion " + r.version + "\n";
    for (size_t i = 0; i < r.files.size(); ++i) {
        char line[64];
        snprintf(line, sizeof(line), "file %o %u ", r.files[i].mode, static_cast<unsigned>(r.files[i].content.size()));
        text += line + evo::sha256Hex(r.files[i].content) + " " + r.files[i].path + "\n";
    }
    return text;
}

struct Options {
    bool withList, withSignature;
    std::string folder;  // empty: entrynav2-android-auto-<version>
    std::string list;    // empty: listText(release)
    Options() : withList(true), withSignature(true) {}
};

inline std::string archive(const Release& r, EVP_PKEY* key, const Options& o = Options()) {
    const std::string top = o.folder.empty() ? std::string(evo::kUpdateTopPrefix) + r.version : o.folder;
    const std::string list = o.list.empty() ? listText(r) : o.list;
    std::string tar;
    addFolder(tar, top);
    addFolder(tar, top + "/opt");
    addFolder(tar, top + "/opt/aa");
    addFile(tar, top + "/LICENSE.md", "licence");
    addFile(tar, top + "/MD5SUMS", "sums");
    for (size_t i = 0; i < r.files.size(); ++i) {
        addFile(tar, top + "/" + r.files[i].path, r.files[i].content, r.files[i].mode);
    }
    if (o.withList) {
        addFile(tar, top + "/UPDATE", list);
    }
    if (o.withSignature) {
        addFile(tar, top + "/UPDATE.sig", sign(key, list));
    }
    endArchive(tar);
    return tar;
}

}  // namespace kit

#endif  // ENTRY_AAD_TESTS_UPDATE_TEST_KIT_HPP
