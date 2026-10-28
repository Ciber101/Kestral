// Small zip reading and writing for 3MF files (deflate through zlib). Whole files in memory: print
// files are a few MB to a few hundred MB.
#pragma once
#include <string>
#include <vector>

namespace kst::print {

struct ZipEntry {
    std::string name;
    std::string data;   // unpacked
};

// Every file in the archive, unpacked. False (and error) when it cannot be read.
bool readZip(const std::string& path, std::vector<ZipEntry>& out, std::string& error);
// Writes the entries as a new archive (deflated, store for tiny ones).
bool writeZip(const std::string& path, const std::vector<ZipEntry>& entries, std::string& error);

} // namespace kst::print
