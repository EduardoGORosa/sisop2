// File: include/FileManager.hpp
#pragma once
#include <string>
#include <vector>
#include <mutex>

class FileManager {
public:
    FileManager(const std::string& root);
    void ensureUserDir(const std::string& user);
    std::vector<std::string> listFiles(const std::string& user);
    bool saveFile(const std::string& user, const std::string& name, const std::vector<char>& data);
    bool deleteFile(const std::string& user, const std::string& name);
    bool loadFile(const std::string& user, const std::string& name, std::vector<char>& out);
private:
    std::string root_;
    std::mutex mtx_;
};

