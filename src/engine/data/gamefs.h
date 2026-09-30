#pragma once
// Read-only access to the game files: the original discs as ISO 9660 images (.iso) or as copied
// folders. GameData combines several discs; DOS paths are looked up case-insensitively on all of
// them (the three discs of Down in the Dumps never contain different files with the same name).
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "util/bytes.h"

namespace blub {

// an open file (on disk or inside an image)
class ReadFile {
public:
    virtual ~ReadFile() = default;
    virtual size_t read(void *dst, size_t n) = 0;
    virtual bool seek(uint64_t pos) = 0;
    virtual uint64_t tell() const = 0;
    virtual uint64_t size() const = 0;
    Bytes read_at(uint64_t pos, size_t n);
};

std::unique_ptr<ReadFile> open_host_file(const std::string &path);   // nullptr if it cannot be opened

struct DirEntry {
    std::string name;       // as stored (ISO: without ";1")
    bool dir = false;
    uint64_t size = 0;
};

// one disc: a folder or an ISO image
class FileSource {
public:
    virtual ~FileSource() = default;
    virtual std::string description() const = 0;          // for the user, e.g. "CD1 (Disc 1.iso)"
    virtual std::string label() const = 0;                // volume label ("CD1") or folder name
    // path components (any case); nullptr if missing
    virtual const DirEntry *find(const std::vector<std::string> &path) = 0;
    virtual std::vector<DirEntry> list(const std::vector<std::string> &dir) = 0;
    virtual std::unique_ptr<ReadFile> open(const std::vector<std::string> &path) = 0;
};

std::unique_ptr<FileSource> open_folder_source(const std::string &dir);
std::unique_ptr<FileSource> open_iso_source(const std::string &iso_path, std::string *error = nullptr);

// DOS path ("D:\TOON1\CARTOON1.GAP", "itoon\itoon.exp") -> upper-case components; cwd for relative paths
std::vector<std::string> dos_components(const std::string &path, const std::string &cwd = "\\");

class GameData {
public:
    // location: a folder with the disc contents (DID.EXE) and/or ISO images (also in its subfolders),
    // or a single .iso file
    bool open(const std::string &location, std::string *error = nullptr);
    bool empty() const { return discs_.empty(); }
    const std::vector<std::unique_ptr<FileSource>> &discs() const { return discs_; }

    const DirEntry *find(const std::vector<std::string> &path, FileSource **on = nullptr);
    std::unique_ptr<ReadFile> open_file(const std::vector<std::string> &path);
    Bytes read_all(const std::string &dos_path);            // empty if missing
    std::vector<DirEntry> list(const std::vector<std::string> &dir);   // merged over the discs

private:
    std::vector<std::unique_ptr<FileSource>> discs_;
};

// candidates for the game data folder next to the program (portable installation)
std::string find_portable_game_data(const std::string &program_dir);

}  // namespace blub
