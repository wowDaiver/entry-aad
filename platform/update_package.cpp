// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update archive read from a USB stick (update_package.hpp).
#include "update_package.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <stdlib.h>
#include <string.h>

#include <set>

namespace evo {

namespace {

// ---- versions -------------------------------------------------------------------------------------------------------
bool allDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

// A numeric identifier of SemVer: digits, no leading zero but for "0" itself.
bool numericId(const std::string& s) {
    return allDigits(s) && (s.size() == 1 || s[0] != '0');
}

bool idChars(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-')) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t end = s.find(sep, start);
        parts.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

// Two numeric identifiers without leading zeros: the longer is the larger.
int compareNumbers(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

struct Version {
    std::vector<std::string> core;  // three numbers
    std::vector<std::string> pre;   // empty for a release
};

bool parseVersion(const std::string& text, Version& v) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    const size_t dash = text.find('-');
    v.core = split(text.substr(0, dash), '.');
    v.pre.clear();
    if (v.core.size() != 3) {
        return false;
    }
    for (size_t i = 0; i < v.core.size(); ++i) {
        if (!numericId(v.core[i])) {
            return false;
        }
    }
    if (dash == std::string::npos) {
        return true;
    }
    v.pre = split(text.substr(dash + 1), '.');
    for (size_t i = 0; i < v.pre.size(); ++i) {
        if (!idChars(v.pre[i]) || (allDigits(v.pre[i]) && !numericId(v.pre[i]))) {
            return false;
        }
    }
    return true;
}

// ---- the archive ----------------------------------------------------------------------------------------------------
const size_t kBlock = 512;
const size_t kEntriesMax = 256;

std::string field(const char* header, size_t offset, size_t size) {
    const char* start = header + offset;
    const void* nul = memchr(start, 0, size);
    return std::string(start, nul ? static_cast<const char*>(nul) - start : size);
}

// An octal number field: optional leading spaces, octal digits, then NUL, spaces or the field's end.
bool octal(const char* header, size_t offset, size_t size, uint64_t& value) {
    size_t i = 0;
    while (i < size && header[offset + i] == ' ') {
        ++i;
    }
    value = 0;
    size_t digits = 0;
    for (; i < size; ++i) {
        const char c = header[offset + i];
        if (c == 0 || c == ' ') {
            break;
        }
        if (c < '0' || c > '7' || value > (static_cast<uint64_t>(1) << 60)) {
            return false;
        }
        value = value * 8 + static_cast<uint64_t>(c - '0');
        ++digits;
    }
    for (; i < size; ++i) {
        if (header[offset + i] != 0 && header[offset + i] != ' ') {
            return false;
        }
    }
    return digits > 0;
}

bool zeroBlock(const char* block) {
    for (size_t i = 0; i < kBlock; ++i) {
        if (block[i]) {
            return false;
        }
    }
    return true;
}

// A member name the packager can write: relative, plain characters, no empty, "." or ".." part.
bool plainPath(const std::string& name) {
    if (name.empty() || name.size() > 255 || name[0] == '/') {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
              c == '-' || c == '/')) {
            return false;
        }
    }
    const std::vector<std::string> parts = split(name, '/');
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty() || parts[i] == "." || parts[i] == "..") {
            return false;
        }
    }
    return true;
}

// ---- the list of files ----------------------------------------------------------------------------------------------
bool plainName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.' ||
        (name.size() >= 4 && name.compare(name.size() - 4, 4, ".new") == 0)) {
        return false;
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

bool lowerHex(const std::string& s, size_t size) {
    if (s.size() != size) {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

const UpdateFile* findFile(const UpdateList& list, const std::string& path) {
    for (size_t i = 0; i < list.files.size(); ++i) {
        if (list.files[i].path == path) {
            return &list.files[i];
        }
    }
    return nullptr;
}

const TarEntry* findEntry(const std::vector<TarEntry>& entries, const std::string& name) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == name) {
            return &entries[i];
        }
    }
    return nullptr;
}

}  // namespace

bool validVersion(const std::string& version) {
    Version v;
    return parseVersion(version, v);
}

int compareVersions(const std::string& a, const std::string& b) {
    Version va, vb;
    if (!parseVersion(a, va) || !parseVersion(b, vb)) {
        return 0;
    }
    for (size_t i = 0; i < 3; ++i) {
        const int c = compareNumbers(va.core[i], vb.core[i]);
        if (c) {
            return c;
        }
    }
    // A release comes after its pre-releases.
    if (va.pre.empty() || vb.pre.empty()) {
        return va.pre.empty() == vb.pre.empty() ? 0 : (va.pre.empty() ? 1 : -1);
    }
    for (size_t i = 0; i < va.pre.size() && i < vb.pre.size(); ++i) {
        const bool na = allDigits(va.pre[i]), nb = allDigits(vb.pre[i]);
        int c;
        if (na && nb) {
            c = compareNumbers(va.pre[i], vb.pre[i]);
        } else if (na != nb) {
            c = na ? -1 : 1;  // numbers come before words
        } else {
            const int s = va.pre[i].compare(vb.pre[i]);
            c = s < 0 ? -1 : (s > 0 ? 1 : 0);
        }
        if (c) {
            return c;
        }
    }
    return va.pre.size() == vb.pre.size() ? 0 : (va.pre.size() < vb.pre.size() ? -1 : 1);
}

bool readTar(const std::string& archive, std::vector<TarEntry>& entries, std::string& why) {
    entries.clear();
    std::set<std::string> names;
    size_t at = 0;
    for (;;) {
        if (at + kBlock > archive.size()) {
            why = "the archive ends without its end blocks";
            return false;
        }
        const char* h = archive.data() + at;
        if (zeroBlock(h)) {
            for (size_t i = at; i < archive.size(); ++i) {
                if (archive[i]) {
                    why = "data after the end of the archive";
                    return false;
                }
            }
            return true;
        }
        uint64_t sum = 0, stored = 0, size = 0;
        for (size_t i = 0; i < kBlock; ++i) {
            sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
        }
        if (!octal(h, 148, 8, stored) || stored != sum) {
            why = "a damaged member header (checksum)";
            return false;
        }
        if (memcmp(h + 257, "ustar", 6) != 0) {  // POSIX, as the packager writes it; GNU's has other fields there
            why = "not a POSIX USTAR archive";
            return false;
        }
        if (!octal(h, 124, 12, size)) {
            why = "a member size that cannot be read";
            return false;
        }
        const char type = h[156];
        const std::string prefix = field(h, 345, 155), base = field(h, 0, 100);
        std::string name = prefix.empty() ? base : prefix + "/" + base;
        const bool folder = type == '5';
        if (folder && !name.empty() && name[name.size() - 1] == '/') {
            name.erase(name.size() - 1);
        }
        if (!folder && type != '0' && type != 0) {
            why = "a member that is neither a file nor a folder: " + name;
            return false;
        }
        if (!plainPath(name)) {
            why = "a member name outside the archive or with unusual characters";
            return false;
        }
        if (!names.insert(name).second) {
            why = "a member given twice: " + name;
            return false;
        }
        if ((folder && size) || size > kUpdatePackageMax) {
            why = "a member of an impossible size: " + name;
            return false;
        }
        const size_t content = at + kBlock;
        if (size > archive.size() - content) {
            why = "the archive is cut short in " + name;
            return false;
        }
        if (entries.size() >= kEntriesMax) {
            why = "too many members";
            return false;
        }
        TarEntry e;
        e.name = name;
        e.folder = folder;
        e.offset = content;
        e.size = static_cast<size_t>(size);
        entries.push_back(e);
        at = content + (e.size + kBlock - 1) / kBlock * kBlock;
    }
}

bool parseUpdateList(const std::string& text, UpdateList& list, std::string& why) {
    list = UpdateList();
    if (text.empty() || text.size() > kUpdateListMax || text[text.size() - 1] != '\n' ||
        text.find('\r') != std::string::npos || text.find('\0') != std::string::npos) {
        why = "UPDATE is not a list of lines";
        return false;
    }
    std::vector<std::string> lines = split(text.substr(0, text.size() - 1), '\n');
    if (lines.size() < 3 || lines[0] != kUpdateFirstLine) {
        why = "UPDATE is not in a format this version knows";
        return false;
    }
    if (lines[1].compare(0, 8, "version ") != 0 || lines[1].size() > 8 + 32 || !validVersion(lines[1].substr(8))) {
        why = "UPDATE has no valid version";
        return false;
    }
    list.version = lines[1].substr(8);
    if (lines.size() - 2 > kUpdateFilesMax) {
        why = "UPDATE lists too many files";
        return false;
    }
    const std::string programs = std::string(kUpdateProgramDir) + "/", plugin = std::string(kUpdatePluginDir) + "/";
    std::set<std::string> paths;
    for (size_t i = 2; i < lines.size(); ++i) {
        const std::vector<std::string> f = split(lines[i], ' ');
        if (f.size() != 5 || f[0] != "file" || (f[1] != "644" && f[1] != "755") || !numericId(f[2]) ||
            f[2].size() > 9 || !lowerHex(f[3], 64)) {
            why = "UPDATE line " + lines[i].substr(0, 40) + " cannot be read";
            return false;
        }
        const std::string& path = f[4];
        const bool inPrograms = path.compare(0, programs.size(), programs) == 0;
        const bool inPlugin = path.compare(0, plugin.size(), plugin) == 0;
        if ((!inPrograms && !inPlugin) || !plainName(path.substr((inPrograms ? programs : plugin).size()))) {
            why = "UPDATE names a file outside the two folders of Android Auto: " + path.substr(0, 80);
            return false;
        }
        if (!paths.insert(path).second) {
            why = "UPDATE names a file twice: " + path;
            return false;
        }
        UpdateFile file;
        file.path = path;
        file.mode = f[1] == "755" ? 0755 : 0644;
        file.size = static_cast<size_t>(strtoul(f[2].c_str(), nullptr, 10));
        file.sha256 = f[3];
        if (file.size > kUpdateFileMax) {
            why = "UPDATE names a file too large: " + path;
            return false;
        }
        list.files.push_back(file);
    }
    // Without these, the update would leave a car that can neither run Android Auto nor be updated again.
    static const char* const kExecutables[] = {"opt/aa/aa-supervisor", "opt/aa/entry-aad",
                                               "opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so"};
    static const char* const kAppFiles[] = {"opt/hmi/data/ro/Plugins/EntryAad/EntryAad.xml",
                                            "opt/hmi/data/ro/Plugins/EntryAad/EntryAad.manifest"};
    for (size_t i = 0; i < sizeof(kExecutables) / sizeof(kExecutables[0]); ++i) {
        const UpdateFile* f = findFile(list, kExecutables[i]);
        if (!f || f->mode != 0755) {
            why = std::string("UPDATE lacks the executable ") + kExecutables[i];
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(kAppFiles) / sizeof(kAppFiles[0]); ++i) {
        if (!findFile(list, kAppFiles[i])) {
            why = std::string("UPDATE lacks ") + kAppFiles[i];
            return false;
        }
    }
    return true;
}

std::string sha256Hex(const void* data, size_t size) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data, size, md, &len, EVP_sha256(), nullptr) != 1 || len != 32) {
        ERR_clear_error();
        return std::string();
    }
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (unsigned int i = 0; i < len; ++i) {
        hex += kHex[md[i] >> 4];
        hex += kHex[md[i] & 15];
    }
    return hex;
}

bool signedByOneOf(const std::string& data, const std::string& signature, const std::vector<std::string>& keys) {
    bool valid = false;
    for (size_t i = 0; i < keys.size() && !valid && !signature.empty(); ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(keys[i].data());
        const unsigned char* end = p + keys[i].size();
        EVP_PKEY* key = d2i_PUBKEY(nullptr, &p, static_cast<long>(keys[i].size()));
        if (!key) {
            continue;
        }
        EVP_MD_CTX* ctx = EVP_MD_CTX_create();
        valid = p == end && ctx && EVP_VerifyInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                EVP_VerifyUpdate(ctx, data.data(), data.size()) == 1 &&
                EVP_VerifyFinal(ctx, reinterpret_cast<const unsigned char*>(signature.data()),
                                static_cast<unsigned int>(signature.size()), key) == 1;
        if (ctx) {
            EVP_MD_CTX_destroy(ctx);
        }
        EVP_PKEY_free(key);
    }
    ERR_clear_error();  // a refused signature leaves errors queued; nothing else reads them
    return valid;
}

namespace {
uint32_t le16(const std::string& s, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[at])) |
           static_cast<uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8;
}
uint32_t le32(const std::string& s, size_t at) {
    return le16(s, at) | le16(s, at + 2) << 16;
}
}  // namespace

bool programFitsCar(const std::string& content, bool library, std::string& why) {
    const size_t kHeader = 52, kProgramHeader = 32;
    if (content.size() < kHeader || content.compare(0, 4, "\177ELF") != 0) {
        why = "not a program (no ELF header)";
        return false;
    }
    if (content[4] != 1 || content[5] != 1) {
        why = "not a 32-bit little-endian program";
        return false;
    }
    const uint32_t type = le16(content, 16), machine = le16(content, 18), flags = le32(content, 36);
    if (machine != 40 || (flags >> 24) != 5) {
        why = "not built for ARM EABI 5";
        return false;
    }
    if (library ? type != 3 : (type != 2 && type != 3)) {
        why = library ? "not a shared library" : "not an executable program";
        return false;
    }
    const uint32_t phoff = le32(content, 28), phentsize = le16(content, 42), phnum = le16(content, 44);
    if (phentsize != kProgramHeader || phnum == 0 || phnum > 64 || phoff > content.size() ||
        phnum * kProgramHeader > content.size() - phoff) {
        why = "program headers outside the file";
        return false;
    }
    std::string interpreter;
    for (uint32_t i = 0; i < phnum; ++i) {
        const size_t h = phoff + i * kProgramHeader;
        const uint32_t kind = le32(content, h), offset = le32(content, h + 4), size = le32(content, h + 16);
        if (offset > content.size() || size > content.size() - offset) {
            why = "a segment beyond the end of the file: cut short";
            return false;
        }
        if (kind == 3) {  // PT_INTERP: the loader's path, NUL-terminated
            interpreter = std::string(content.data() + offset, size);
            interpreter = interpreter.substr(0, interpreter.find('\0'));
        }
    }
    if (!library && interpreter != kCarLoader) {
        why = "asks for the loader " + (interpreter.empty() ? std::string("(none)") : interpreter.substr(0, 60)) +
              " instead of " + kCarLoader;
        return false;
    }
    return true;
}

bool readUpdatePackage(const std::string& archive, const std::vector<std::string>& keys, UpdatePackage& package,
                       std::string& why) {
    package = UpdatePackage();
    std::vector<TarEntry> entries;
    if (!readTar(archive, entries, why)) {
        return false;
    }
    if (entries.empty()) {
        why = "an empty archive";
        return false;
    }
    const std::string top = entries[0].name.substr(0, entries[0].name.find('/'));
    if (top.compare(0, strlen(kUpdateTopPrefix), kUpdateTopPrefix) != 0) {
        why = "not an EntryNav2 Android Auto archive";
        return false;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name != top && entries[i].name.compare(0, top.size() + 1, top + "/") != 0) {
            why = "members outside the archive's folder";
            return false;
        }
    }
    const TarEntry* list = findEntry(entries, top + "/UPDATE");
    const TarEntry* sig = findEntry(entries, top + "/UPDATE.sig");
    if (!list || list->folder) {
        why = "no UPDATE: an archive older than the updates by USB stick";
        return false;
    }
    if (!sig || sig->folder) {
        why = "no signature (UPDATE.sig): built without the project's key, install it by SSH";
        return false;
    }
    if (list->size > kUpdateListMax || sig->size > 1024) {
        why = "UPDATE or its signature too large";
        return false;
    }
    package.listText = archive.substr(list->offset, list->size);
    package.signature = archive.substr(sig->offset, sig->size);
    if (!signedByOneOf(package.listText, package.signature, keys)) {
        why = "the signature is not the project's: not an official archive, or damaged";
        return false;
    }
    if (!parseUpdateList(package.listText, package.list, why)) {
        return false;
    }
    if (top != kUpdateTopPrefix + package.list.version) {
        why = "the archive's folder does not match its version";
        return false;
    }
    for (size_t i = 0; i < package.list.files.size(); ++i) {
        const UpdateFile& f = package.list.files[i];
        const TarEntry* e = findEntry(entries, top + "/" + f.path);
        if (!e || e->folder) {
            why = "missing from the archive: " + f.path;
            return false;
        }
        if (e->size != f.size || sha256Hex(archive.data() + e->offset, e->size) != f.sha256) {
            why = "damaged in the archive: " + f.path;
            return false;
        }
        package.contents.push_back(archive.substr(e->offset, e->size));
    }
    return true;
}

}  // namespace evo
