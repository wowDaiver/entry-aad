// Copyright (c) 2026 Jiraiya43
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The update archive (platform/update_package.hpp): versions, the tar reader against hostile archives, the list of
// files, hashes and signatures, a whole archive, and the project's own key. On the PC and under QEMU with the car's
// libcrypto (OpenSSL 1.0.1).
//
// --verify <archive> <public key, DER>: checks a release archive made by tools/package_release.py
// (tests/test_update_release.py).
#include <fstream>
#include <sstream>

#include "../platform/update_key.hpp"
#include "update_test_kit.hpp"

namespace {

using kit::addFile;
using kit::addFolder;
using kit::endArchive;

std::string readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream all;
    all << in.rdbuf();
    return all.str();
}

void versions() {
    CHECK(evo::validVersion("0.13.0") && evo::validVersion("0.13.0-rc.9") && evo::validVersion("0.13.0-essai.1"));
    CHECK(evo::validVersion("10.200.3000-alpha-2.x"));
    const char* const invalid[] = {"",        "1.2",          "1.2.3.4",  "01.2.3",        "1.02.3", "1.2.3-",
                                   "1.2.3+b", "1.2.3-rc..1",  "1.2.3-01", "1.2.3-r c",     "v1.2.3", "1.2.3 ",
                                   "-1.2.3",  "1.2.3-rc.1+x", "1..3",     "1.2.3-\xc3\xa9"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!evo::validVersion(invalid[i]));
    }
    // From the oldest to the newest, as SemVer orders them.
    const char* const order[] = {"0.9.0",       "0.12.1-rc.4", "0.13.0-alpha.2", "0.13.0-essai.1",
                                 "0.13.0-rc.2", "0.13.0-rc.9", "0.13.0-rc.10",   "0.13.0-rc.10.1",
                                 "0.13.0",      "0.13.1",      "0.14.0",         "1.0.0"};
    const size_t n = sizeof(order) / sizeof(order[0]);
    for (size_t i = 0; i < n; ++i) {
        CHECK(evo::compareVersions(order[i], order[i]) == 0);
        for (size_t j = i + 1; j < n; ++j) {
            CHECK(evo::compareVersions(order[i], order[j]) < 0);
            CHECK(evo::compareVersions(order[j], order[i]) > 0);
        }
    }
    CHECK(evo::compareVersions("1.0.0-1", "1.0.0-a") < 0);  // numbers before words
    printf("PASS versions\n");
}

void hashes() {
    CHECK(evo::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(evo::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    printf("PASS hashes\n");
}

std::string tarWith(const std::string& name, char type = '0', const std::string& content = "x") {
    std::string tar;
    addFolder(tar, "a");
    addFile(tar, name, content, 0644, type);
    endArchive(tar);
    return tar;
}

// The header at offset changed by hand: its checksum made right again, so that only the change is refused.
void fixChecksum(std::string& tar, size_t offset) {
    memset(&tar[offset + 148], ' ', 8);
    unsigned sum = 0;
    for (size_t i = 0; i < 512; ++i) {
        sum += static_cast<unsigned char>(tar[offset + i]);
    }
    kit::octalField(&tar[0], offset + 148, 7, sum);
    tar[offset + 155] = ' ';
}

bool readable(const std::string& tar) {
    std::vector<evo::TarEntry> entries;
    std::string why;
    return evo::readTar(tar, entries, why);
}

void tarReader() {
    std::string tar;
    addFolder(tar, "top");
    addFile(tar, "top/file.txt", "hello");
    const std::string longName = "top/" + std::string(60, 'd') + "/" + std::string(60, 'e') + "/file.bin";
    addFile(tar, longName, std::string(1000, 'z'), 0755);
    addFile(tar, "top/empty", "");
    endArchive(tar);
    std::vector<evo::TarEntry> entries;
    std::string why;
    CHECK(evo::readTar(tar, entries, why));
    CHECK(entries.size() == 4 && entries[0].name == "top" && entries[0].folder);
    CHECK(entries[1].name == "top/file.txt" && !entries[1].folder && tar.substr(entries[1].offset, 5) == "hello");
    CHECK(entries[2].name == longName && entries[2].size == 1000);  // name split into the prefix field
    CHECK(entries[3].size == 0);

    // Anything the packager never writes refuses the whole archive.
    CHECK(readable(tarWith("a/b")));
    CHECK(!readable(tarWith("a/link", '2')));  // symbolic link
    CHECK(!readable(tarWith("a/hard", '1')));  // hard link
    CHECK(!readable(tarWith("a/dev", '3')));
    CHECK(!readable(tarWith("a/long", 'L')));  // GNU long name
    CHECK(!readable(tarWith("../a")));
    CHECK(!readable(tarWith("a/../../etc/passwd")));
    CHECK(!readable(tarWith("/etc/passwd")));
    CHECK(!readable(tarWith("a//b")));
    CHECK(!readable(tarWith("a/./b")));
    CHECK(!readable(tarWith("a/b c")));
    std::string twice;
    addFile(twice, "a/b", "1");
    addFile(twice, "a/b", "2");
    endArchive(twice);
    CHECK(!readable(twice));

    std::string broken = tarWith("a/b");
    broken[512 + 20] ^= 1;  // the second header's name: its checksum no longer matches
    CHECK(!readable(broken));
    std::string gnu = tarWith("a/b");
    memcpy(&gnu[512 + 257], "ustar  ", 8);  // GNU's magic
    fixChecksum(gnu, 512);
    CHECK(!readable(gnu));
    std::string fixed = tarWith("a/b");
    fixChecksum(fixed, 512);
    CHECK(readable(fixed));  // the checksum helper itself changes nothing
    std::string cut = tarWith("a/b", '0', std::string(2000, 'q'));
    CHECK(!readable(cut.substr(0, 512 + 512 + 1024)));  // content cut short
    std::string after = tarWith("a/b");
    after[after.size() - 1] = 'x';  // something after the end blocks
    CHECK(!readable(after));
    CHECK(!readable(std::string(512 * 3, 'x')));
    CHECK(!readable(std::string()));
    CHECK(!readable(tar.substr(0, tar.find(std::string(1024, '\0'), 1024 + 512))));  // no end blocks
    std::string huge = tarWith("a/b");
    kit::octalField(&huge[0], 512 + 124, 12, 07777777777);  // larger than the archive
    fixChecksum(huge, 512);
    CHECK(!readable(huge));
    std::string folderWithData = tarWith("a/b");
    kit::octalField(&folderWithData[0], 124, 12, 1);  // a folder with content
    fixChecksum(folderWithData, 0);
    CHECK(!readable(folderWithData));
    printf("PASS tar reader\n");
}

std::string listWith(const std::string& line) {
    kit::Release r = kit::release("0.14.0");
    return kit::listText(r) + line + "\n";
}

bool parses(const std::string& text) {
    evo::UpdateList list;
    std::string why;
    return evo::parseUpdateList(text, list, why);
}

void updateList() {
    const kit::Release r = kit::release("0.14.0");
    const std::string good = kit::listText(r);
    evo::UpdateList list;
    std::string why;
    CHECK(evo::parseUpdateList(good, list, why));
    CHECK(list.version == "0.14.0" && list.files.size() == r.files.size());
    CHECK(list.files[0].path == "opt/aa/aa-supervisor" && list.files[0].mode == 0755);
    CHECK(list.files[1].size == r.files[1].content.size() &&
          list.files[1].sha256 == evo::sha256Hex(r.files[1].content));
    CHECK(list.files[2].mode == 0644);

    const std::string hash(64, 'a');
    CHECK(parses(listWith("file 644 3 " + hash + " opt/aa/extra-file")));
    CHECK(!parses(good.substr(0, good.size() - 1)));  // no final newline
    std::string crlf = good;
    crlf.insert(crlf.find('\n'), "\r");
    CHECK(!parses(crlf));
    CHECK(!parses("entrynav2-android-auto update 2" + good.substr(good.find('\n'))));
    CHECK(!parses(std::string(evo::kUpdateFirstLine) + "\nversion 1.2\n" + good.substr(good.find("file"))));
    const char* const refused[] = {
        "file 777 3 %s opt/aa/x",                         // mode
        "file 644 03 %s opt/aa/x",                        // leading zero
        "file 644 3 %s etc/passwd",                       // outside the two folders
        "file 644 3 %s opt/aa/../etc/passwd",             //
        "file 644 3 %s opt/aa/sub/x",                     // a sub-folder
        "file 644 3 %s opt/aa/.installed-by-entryaad",    // our marker
        "file 644 3 %s opt/aa/x.new",                     // a name the swap uses
        "file 644 3 %s opt/hmi/data/ro/Plugins/Other/x",  //
        "file 644 3 %s opt/aa/entry-aad",                 // given twice
        "file 644 3 %s opt/aa/x y",                       //
        "file  644 3 %s opt/aa/x",                        //
        "file 644 99999999999 %s opt/aa/x",               // too large
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char line[200];
        snprintf(line, sizeof(line), refused[i], hash.c_str());
        CHECK(!parses(listWith(line)));
    }
    CHECK(!parses(listWith("file 644 3 " + std::string(64, 'A') + " opt/aa/x")));  // uppercase hex
    CHECK(!parses(listWith("file 644 3 " + std::string(63, 'a') + " opt/aa/x")));
    CHECK(!parses(listWith("")));  // an empty line
    // Without the supervisor, the service or the app, or with one of them not executable: refused.
    for (size_t drop = 0; drop < r.files.size(); ++drop) {
        kit::Release without = r;
        without.files.erase(without.files.begin() + static_cast<long>(drop));
        const bool needed = r.files[drop].path.find("_ALL_") == std::string::npos;
        CHECK(parses(kit::listText(without)) == !needed);
    }
    kit::Release notExecutable = r;
    notExecutable.files[0].mode = 0644;
    CHECK(!parses(kit::listText(notExecutable)));
    std::string many = good;
    for (int i = 0; i < 64; ++i) {
        char line[128];
        snprintf(line, sizeof(line), "file 644 1 %s opt/aa/f%d\n", hash.c_str(), i);
        many += line;
    }
    CHECK(!parses(many));
    printf("PASS update list\n");
}

void signatures(EVP_PKEY* key) {
    const std::string der = kit::publicDer(key);
    std::vector<std::string> keys(1, der);
    const std::string data = "the list";
    const std::string sig = kit::sign(key, data);
    CHECK(evo::signedByOneOf(data, sig, keys));
    CHECK(!evo::signedByOneOf(data + " ", sig, keys));
    std::string bent = sig;
    bent[bent.size() / 2] ^= 0x10;
    CHECK(!evo::signedByOneOf(data, bent, keys));
    CHECK(!evo::signedByOneOf(data, sig.substr(1), keys));
    CHECK(!evo::signedByOneOf(data, std::string(), keys));
    CHECK(!evo::signedByOneOf(data, sig, std::vector<std::string>()));
    CHECK(!evo::signedByOneOf(data, sig, std::vector<std::string>(1, "not a key")));
    CHECK(!evo::signedByOneOf(data, sig, std::vector<std::string>(1, der + "x")));  // bytes after the key
    CHECK(!evo::signedByOneOf(data, sig, evo::trustedUpdateKeys()));                // the project's key did not sign it
    std::vector<std::string> both = evo::trustedUpdateKeys();
    both.push_back(der);
    CHECK(evo::signedByOneOf(data, sig, both));  // any trusted key will do
    printf("PASS signatures\n");
}

bool packageRefused(const std::string& archive, const std::vector<std::string>& keys, const char* reason) {
    evo::UpdatePackage package;
    std::string why;
    if (evo::readUpdatePackage(archive, keys, package, why)) {
        return false;
    }
    if (why.find(reason) == std::string::npos) {
        fprintf(stderr, "refused for another reason: %s\n", why.c_str());
        return false;
    }
    return true;
}

void packages(EVP_PKEY* key) {
    const std::vector<std::string> keys(1, kit::publicDer(key));
    const kit::Release r = kit::release("0.14.0-rc.1");
    evo::UpdatePackage package;
    std::string why;
    CHECK(evo::readUpdatePackage(kit::archive(r, key), keys, package, why));
    CHECK(package.list.version == "0.14.0-rc.1" && package.contents.size() == r.files.size());
    for (size_t i = 0; i < r.files.size(); ++i) {
        CHECK(package.contents[i] == r.files[i].content);
    }
    CHECK(package.listText == kit::listText(r));

    CHECK(packageRefused(kit::archive(r, key), evo::trustedUpdateKeys(), "signature is not the project's"));
    kit::Options unsigned_;
    unsigned_.withSignature = false;
    CHECK(packageRefused(kit::archive(r, key, unsigned_), keys, "no signature"));
    kit::Options old;
    old.withList = old.withSignature = false;
    CHECK(packageRefused(kit::archive(r, key, old), keys, "older than the updates by USB stick"));
    kit::Options renamed;
    renamed.folder = "entrynav2-android-auto-0.15.0";
    CHECK(packageRefused(kit::archive(r, key, renamed), keys, "does not match its version"));
    kit::Options other;
    other.folder = "something-else-0.14.0-rc.1";
    CHECK(packageRefused(kit::archive(r, key, other), keys, "not an EntryNav2"));
    // A file changed after the list was signed.
    kit::Release changed = r;
    changed.files[1].content[10] ^= 1;
    kit::Options signedOld;
    signedOld.list = kit::listText(r);
    CHECK(packageRefused(kit::archive(changed, key, signedOld), keys, "damaged in the archive: opt/aa/entry-aad"));
    // A file listed but missing from the archive.
    kit::Release missing = r;
    missing.files.pop_back();
    CHECK(packageRefused(kit::archive(missing, key, signedOld), keys, "missing from the archive"));
    // The signature of another list: what it signs is not what the archive lists.
    kit::Release extra = r;
    extra.files.push_back({"opt/aa/evil", "x", 0755});
    std::string swapped = kit::archive(extra, key);
    const std::string goodList = kit::listText(r), badList = kit::listText(extra);
    CHECK(swapped.find(badList) != std::string::npos);
    std::string tar;
    addFolder(tar, "entrynav2-android-auto-0.14.0-rc.1");
    for (size_t i = 0; i < extra.files.size(); ++i) {
        addFile(tar, "entrynav2-android-auto-0.14.0-rc.1/" + extra.files[i].path, extra.files[i].content, 0755);
    }
    addFile(tar, "entrynav2-android-auto-0.14.0-rc.1/UPDATE", badList);
    addFile(tar, "entrynav2-android-auto-0.14.0-rc.1/UPDATE.sig", kit::sign(key, goodList));
    endArchive(tar);
    CHECK(packageRefused(tar, keys, "signature is not the project's"));
    // A member outside the archive's folder.
    std::string outside = kit::archive(r, key);
    std::string stray;
    addFile(stray, "elsewhere/file", "x");
    outside.insert(512, stray);
    CHECK(packageRefused(outside, keys, "outside the archive's folder"));
    printf("PASS packages\n");
}

bool fits(const std::string& content, bool library) {
    std::string why;
    return evo::programFitsCar(content, library, why);
}

// The ELF check run before the car's loader: that loader stops on a file it cannot map without naming any library.
void programs() {
    CHECK(fits(kit::elf(kit::Elf(), "a program"), false));
    CHECK(fits(kit::elf(kit::Elf(true), "a library"), true));
    kit::Elf other;
    other.interpreter = "/lib/ld-linux.so.3";  // soft-float ARM
    CHECK(!fits(kit::elf(other, "x"), false));
    other.interpreter = "/lib64/ld-linux-x86-64.so.2";
    CHECK(!fits(kit::elf(other, "x"), false));
    kit::Elf noLoader(true);  // a library in place of a program: it asks for no loader
    CHECK(!fits(kit::elf(noLoader, "x"), false));
    CHECK(!fits(kit::elf(kit::Elf(), "x"), true));  // a program in place of the library
    kit::Elf wide;
    wide.fileClass = 2;  // 64-bit
    CHECK(!fits(kit::elf(wide, "x"), false));
    kit::Elf x86;
    x86.machine = 3;
    CHECK(!fits(kit::elf(x86, "x"), false));
    kit::Elf oldAbi;
    oldAbi.flags = 0x04000000;  // EABI 4
    CHECK(!fits(kit::elf(oldAbi, "x"), false));
    const std::string whole = kit::elf(kit::Elf(), std::string(4000, 'p'));
    CHECK(!fits(whole.substr(0, whole.size() - 100), false));  // cut short: the load segment goes past the end
    CHECK(!fits(whole.substr(0, 60), false));                  // program headers past the end
    CHECK(!fits(whole.substr(0, 40), false));                  // not even a header
    CHECK(!fits("#!/bin/sh\necho\n", false));
    CHECK(!fits(std::string(), false));
    // This test program itself: refused on the PC (x86-64), taken under QEMU, where it is a program for the car.
    const std::string self = readAll("/proc/self/exe");
    CHECK(!self.empty());
#if defined(__arm__)
    std::string why;
    CHECK(evo::programFitsCar(self, false, why));
#else
    CHECK(!fits(self, false));
#endif
    printf("PASS programs for the car\n");
}

void projectKey() {
    const std::vector<std::string> keys = evo::trustedUpdateKeys();
    CHECK(!keys.empty());
    for (size_t i = 0; i < keys.size(); ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(keys[i].data());
        EVP_PKEY* key = d2i_PUBKEY(nullptr, &p, static_cast<long>(keys[i].size()));
        CHECK(key && p == reinterpret_cast<const unsigned char*>(keys[i].data()) + keys[i].size());
        CHECK(EVP_PKEY_id(key) == EVP_PKEY_RSA && EVP_PKEY_bits(key) >= 3072);
        EVP_PKEY_free(key);
    }
    printf("PASS project key\n");
}

int verify(const char* archivePath, const char* keyPath) {
    evo::UpdatePackage package;
    std::string why;
    const std::vector<std::string> keys(1, readAll(keyPath));
    if (!evo::readUpdatePackage(readAll(archivePath), keys, package, why)) {
        printf("FAIL %s: %s\n", archivePath, why.c_str());
        return 1;
    }
    printf("PASS verify %s: version %s, %u files\n", archivePath, package.list.version.c_str(),
           static_cast<unsigned>(package.list.files.size()));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "--verify")) {
        return verify(argv[2], argv[3]);
    }
    versions();
    hashes();
    tarReader();
    updateList();
    programs();
    projectKey();
    EVP_PKEY* key = kit::makeKey(2048);
    signatures(key);
    packages(key);
    EVP_PKEY_free(key);
    printf("PASS update package\n");
    return 0;
}
