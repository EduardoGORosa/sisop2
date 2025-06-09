// File: src/FileManager.cpp
#include "FileManager.hpp"
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

FileManager::FileManager(const std::string& root) : root_(root) {
    fs::create_directories(root_);
}

void FileManager::ensureUserDir(const std::string& u) {
    std::lock_guard lk(mtx_);
    fs::create_directories(fs::path(root_)/u);
}

std::vector<std::string> FileManager::listFiles(const std::string& u) {
    std::lock_guard lk(mtx_);
    std::vector<std::string> v;
    auto p = fs::path(root_)/u;
    if (!fs::exists(p)) return v;
    for(auto& e: fs::directory_iterator(p))
      if (e.is_regular_file())
        v.push_back(e.path().filename().string());
    return v;
}

bool FileManager::saveFile(const std::string& u, const std::string& n, const std::vector<char>& d) {
    ensureUserDir(u);
    std::lock_guard lk(mtx_);
    auto p = fs::path(root_)/u/n;
    std::ofstream ofs(p, std::ios::binary);
    if(!ofs) return false;
    ofs.write(d.data(), d.size());
    return true;
}

bool FileManager::deleteFile(const std::string& u, const std::string& n) {
    std::lock_guard lk(mtx_);
    auto p = fs::path(root_)/u/n;
    return fs::remove(p);
}

bool FileManager::loadFile(const std::string& u, const std::string& n, std::vector<char>& out) {
    std::lock_guard lk(mtx_);
    auto p = fs::path(root_)/u/n;
    if(!fs::exists(p)) return false;
    std::ifstream ifs(p, std::ios::binary|std::ios::ate);
    auto sz = ifs.tellg();
    ifs.seekg(0);
    out.resize(sz);
    ifs.read(out.data(), sz);
    return true;
}

