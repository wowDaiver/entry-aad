// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update archive read from a USB stick: the release archive itself (tools/package_release.py), unchanged.
//
// Besides the files at their place on the car and MD5SUMS for the installation by hand, the archive holds UPDATE, the
// list of what the update installs, and UPDATE.sig, its signature by the project's key (RSA, PKCS #1 v1.5, SHA-256):
//
//     entrynav2-android-auto update 1
//     version 0.14.0
//     file 755 412345 <sha256 in lowercase hex> opt/aa/aa-supervisor
//     ...
//
// One file line per installed file: its mode, size and SHA-256, and its path on the car, in /opt/aa or in the app's
// folder and nowhere else. The signature covers the list, the list covers every byte installed. An archive built
// without the project's key has no signature: it installs by SSH only (docs/INSTALLATION.md).
//
// Everything here is checked before anything is written, from the archive read once into memory: a stick pulled out
// or changed meanwhile changes nothing. The tar reader only knows what the packager writes (USTAR, files and folders):
// links, devices, long names, duplicates or a path that leaves the archive's folder refuse the whole archive.
// Hashes and signatures use the car's libcrypto (OpenSSL 1.0.1). tests/test_update_package.cpp.
#ifndef ENTRY_AAD_PLATFORM_UPDATE_PACKAGE_HPP
#define ENTRY_AAD_PLATFORM_UPDATE_PACKAGE_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace evo {

// The folder every member of a release archive sits in: this prefix, then the version.
const char kUpdateTopPrefix[] = "entrynav2-android-auto-";
const char kUpdateFirstLine[] = "entrynav2-android-auto update 1";
const size_t kUpdatePackageMax = 32 * 1024 * 1024;  // a release is about 2 MB
const size_t kUpdateFileMax = 16 * 1024 * 1024;
const size_t kUpdateListMax = 16 * 1024;
const size_t kUpdateFilesMax = 64;

// ---- versions -------------------------------------------------------------------------------------------------------
// SemVer: MAJOR.MINOR.PATCH, then an optional -pre-release (0.13.0-rc.9, 0.13.0-essai.1). No build metadata.
bool validVersion(const std::string& version);
// <0, 0 or >0 as a is older than, the same as or newer than b, by SemVer precedence. Both must be valid.
int compareVersions(const std::string& a, const std::string& b);

// ---- the archive ----------------------------------------------------------------------------------------------------
struct TarEntry {
    std::string name;  // as in the archive, without a trailing slash
    bool folder;
    size_t offset;  // of the content, in the archive
    size_t size;
};
// The members of a USTAR archive: regular files and folders only, each header's checksum right, every name relative,
// without "." or ".." parts, given once. False with why otherwise.
bool readTar(const std::string& archive, std::vector<TarEntry>& entries, std::string& why);

// ---- the list of files ----------------------------------------------------------------------------------------------
struct UpdateFile {
    std::string path;  // on the car, without the leading slash: opt/aa/entry-aad
    unsigned mode;     // 0644 or 0755
    size_t size;
    std::string sha256;  // lowercase hex
};
struct UpdateList {
    std::string version;
    std::vector<UpdateFile> files;
};
// The program folder and the app's folder, without the leading slash: the only places an update writes to.
const char kUpdateProgramDir[] = "opt/aa";
const char kUpdatePluginDir[] = "opt/hmi/data/ro/Plugins/EntryAad";
// UPDATE as written by tools/package_release.py, exactly: the first line, one version line, then one line per file,
// in the two folders only, each file a plain name (letters, digits, '.', '_', '-'; not starting with '.', not ending
// with ".new"); the supervisor, the service and the app always among them, executable. False with why otherwise.
bool parseUpdateList(const std::string& text, UpdateList& list, std::string& why);

// ---- hashes and signatures (libcrypto) ------------------------------------------------------------------------------
std::string sha256Hex(const void* data, size_t size);
inline std::string sha256Hex(const std::string& data) {
    return sha256Hex(data.data(), data.size());
}
// True when signature is data's signature (RSA PKCS #1 v1.5, SHA-256) by one of keys, each a DER public key
// (SubjectPublicKeyInfo).
bool signedByOneOf(const std::string& data, const std::string& signature, const std::vector<std::string>& keys);

// ---- the programs -----------------------------------------------------------------------------------------------
// The head unit's loader, the interpreter every program of the car asks for.
const char kCarLoader[] = "/lib/ld-linux-armhf.so.3";
// content is a program (library: a shared library) built for this car: ELF 32-bit little-endian ARM, EABI 5, its
// program headers and segments within the file (not cut short), and a program asks for the car's loader. The car's
// loader is told nothing about a file it cannot even map: a 64-bit or cut program makes it fail before it lists any
// library. False with why otherwise.
bool programFitsCar(const std::string& content, bool library, std::string& why);

// ---- an update archive, checked -------------------------------------------------------------------------------------
struct UpdatePackage {
    UpdateList list;
    std::string listText;               // UPDATE, as signed
    std::string signature;              // UPDATE.sig
    std::vector<std::string> contents;  // of each file of the list, in its order
};
// The archive's UPDATE signed by one of keys, its folder named after its version, each file of the list in it with
// that size and SHA-256. Fills package, or says why not.
bool readUpdatePackage(const std::string& archive, const std::vector<std::string>& keys, UpdatePackage& package,
                       std::string& why);

}  // namespace evo

#endif  // ENTRY_AAD_PLATFORM_UPDATE_PACKAGE_HPP
