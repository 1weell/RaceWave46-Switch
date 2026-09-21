//
// RT64
//

#include "rt64_mapped_file.h"

#if !defined(_WIN32)
#   include <cstring>
#   include <cstdio>
#   if !defined(__SWITCH__)
#   include <fcntl.h>
#   include <unistd.h>
#   endif
#endif

namespace RT64 {
    MappedFile::MappedFile() {
        // Default constructor.
    }

    MappedFile::MappedFile(const std::filesystem::path &path) {
        open(path);
    }

    MappedFile::~MappedFile() {
#   if defined(_WIN32)
        if (fileView != nullptr) {
            UnmapViewOfFile(fileView);
        }
        
        if (fileMappingHandle != nullptr) {
            CloseHandle(fileMappingHandle);
        }

        if (fileHandle != nullptr) {
            CloseHandle(fileHandle);
        }
#   elif defined(__SWITCH__)
        // The Switch SDK does not expose POSIX mmap; keep a read-only copy.
#   else
        if (fileView != MAP_FAILED) {
            munmap(fileView, fileSize);
        }

        if (fileHandle != -1) {
            close(fileHandle);
        }
#   endif
    }

    bool MappedFile::open(const std::filesystem::path &path) {
#   if defined(_WIN32)
        fileHandle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE) {
            std::string pathStr = path.u8string();
            fprintf(stderr, "CreateFileW for %s failed with error %lu.\n", pathStr.c_str(), GetLastError());
            fileHandle = nullptr;
            return false;
        }

        if (!GetFileSizeEx(fileHandle, &fileSize)) {
            fprintf(stderr, "GetFileSizeEx failed with error %lu.\n", GetLastError());
            CloseHandle(fileHandle);
            fileHandle = nullptr;
            return false;
        }

        fileMappingHandle = CreateFileMappingW(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (fileMappingHandle == nullptr) {
            fprintf(stderr, "CreateFileMappingW failed with error %lu.\n", GetLastError());
            CloseHandle(fileHandle);
            fileHandle = nullptr;
            return false;
        }

        fileView = MapViewOfFile(fileMappingHandle, FILE_MAP_READ, 0, 0, 0);
        if (fileView == nullptr) {
            fprintf(stderr, "MapViewOfFile failed with error %lu.\n", GetLastError());
            CloseHandle(fileMappingHandle);
            CloseHandle(fileHandle);
            fileMappingHandle = nullptr;
            fileHandle = nullptr;
            return false;
        }

        return true;
#   elif defined(__SWITCH__)
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file == nullptr) {
            fprintf(stderr, "fopen for %s failed.\n", path.string().c_str());
            return false;
        }
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            return false;
        }
        long length = ftell(file);
        if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return false;
        }
        fileData.resize(static_cast<size_t>(length));
        size_t read = fread(fileData.data(), 1, fileData.size(), file);
        fclose(file);
        if (read != fileData.size()) {
            fileData.clear();
            return false;
        }
        return true;
#   else
        fileHandle = ::open(path.c_str(), O_RDONLY);
        if (fileHandle == -1) {
            fprintf(stderr, "open for %s failed with error %s.\n", path.c_str(), strerror(errno));
            return false;
        }

        fileSize = lseek(fileHandle, 0, SEEK_END);
        if (fileSize == (off_t)(-1)) {
            fprintf(stderr, "lseek failed with error %s.\n", strerror(errno));
            close(fileHandle);
            fileHandle = -1;
            return false;
        }

        fileView = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fileHandle, 0);
        if (fileView == MAP_FAILED) {
            fprintf(stderr, "mmap failed with error %s.\n", strerror(errno));
            close(fileHandle);
            fileHandle = -1;
            return false;
        }

        return true;
#   endif
    }

    bool MappedFile::isOpen() const {
#   if defined(_WIN32)
        return (fileView != nullptr);
#   elif defined(__SWITCH__)
        return !fileData.empty();
#   else
        return (fileView != MAP_FAILED);
#   endif
    }

    uint8_t *MappedFile::data() const {
#   if defined(__SWITCH__)
        return const_cast<uint8_t *>(fileData.data());
#   else
        return reinterpret_cast<uint8_t *>(fileView);
#   endif
    }
    size_t MappedFile::size() const {
#   if defined(_WIN32)
        return fileSize.QuadPart;
#   elif defined(__SWITCH__)
        return fileData.size();
#   else
        return static_cast<size_t>(fileSize);
#   endif
    }
};
