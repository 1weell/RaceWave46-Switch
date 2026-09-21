// Switch has no native desktop file dialog. Keep the RT64 API linkable and
// let the game/frontend provide paths through its normal configuration UI.
#include "rt64_file_dialog.h"

namespace RT64 {

std::atomic<bool> FileDialog::isOpen = false;

void FileDialog::initialize() {
}

void FileDialog::finish() {
}

std::filesystem::path FileDialog::getDirectoryPath() {
    return {};
}

std::filesystem::path FileDialog::getOpenFilename(const std::vector<FileFilter> &) {
    return {};
}

std::filesystem::path FileDialog::getSaveFilename(const std::vector<FileFilter> &) {
    return {};
}

} // namespace RT64
