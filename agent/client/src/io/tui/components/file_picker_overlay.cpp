#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <filesystem>

using namespace ftxui;

// ---------------------------------------------------------------------------
// FilePickerOverlay
// ---------------------------------------------------------------------------

FilePickerOverlay::FilePickerOverlay(
    TUICtx&                             ctx,
    agentxx::agent::ModelCapabilityInfo capability,
    std::string                         initialDir
) :
    ctx_(ctx),
    capability_(std::move(capability)) {
    // 构建允许的扩展名白名单 (小写, 含 '.')
    if (capability_.imageInput) {
        for (auto ext : {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp"}) {
            allowedExtensions_.insert(ext);
        }
    }
    if (capability_.audioInput) {
        for (auto ext : {".wav", ".mp3", ".ogg", ".m4a", ".aac", ".flac"}) {
            allowedExtensions_.insert(ext);
        }
    }
    if (capability_.videoInput) {
        for (auto ext : {".mp4", ".mov", ".webm", ".mkv"}) {
            allowedExtensions_.insert(ext);
        }
    }

    // 初始目录
    if (initialDir.empty()) {
        std::error_code ec;
        auto            cwd = std::filesystem::current_path(ec);
        if (!ec) {
            initialDir = cwd.string();
        } else {
            initialDir = "/";
        }
    }

    // 过滤输入框
    auto option      = InputOption();
    option.multiline = false;
    filterInput_     = Input(&filterText_, option);

    navigateTo(initialDir);
}

void FilePickerOverlay::navigateTo(const std::string& dirPath) {
    currentDir_ = dirPath;
    entries_.clear();
    selectedIndex_ = 0;

    std::error_code ec;
    auto            canonical = std::filesystem::canonical(dirPath, ec);
    if (ec) {
        XX_LOGW("[FilePicker] cannot canonical {}: {}", dirPath, ec.message());
        return;
    }
    currentDir_ = canonical.string();

    // 上级目录
    if (canonical.has_parent_path() && canonical.parent_path() != canonical) {
        DirEntry parentEntry;
        parentEntry.name     = std::string(TuiI18n::instance().t("picker.parent"));
        parentEntry.fullPath = canonical.parent_path().string();
        parentEntry.isDir    = true;
        entries_.push_back(std::move(parentEntry));
    }

    // 遍历当前目录
    std::vector<DirEntry> dirs;
    std::vector<DirEntry> files;

    auto iter = std::filesystem::directory_iterator(canonical, ec);
    if (ec) {
        XX_LOGW("[FilePicker] cannot list {}: {}", currentDir_, ec.message());
        applyFilter();
        return;
    }
    for (const auto& entry : iter) {
        std::error_code ec2;
        auto            status = entry.status(ec2);
        if (ec2) {
            continue;
        }
        DirEntry de;
        de.name     = entry.path().filename().string();
        de.fullPath = entry.path().string();

        if (std::filesystem::is_directory(status)) {
            // 跳过 . 开头的隐藏目录
            if (!de.name.empty() && de.name[0] == '.') {
                continue;
            }
            de.isDir = true;
            de.name  = "📁 " + de.name + "/";
            dirs.push_back(std::move(de));
        } else if (std::filesystem::is_regular_file(status)) {
            auto ext = agentxx::util::toLower(entry.path().extension().string());
            auto mt  = agentxx::agent::mediaTypeFromExtension(ext);
            if (!mt.has_value()) {
                continue; // 非媒体文件不显示
            }
            de.isDir     = false;
            de.sizeBytes = std::filesystem::file_size(entry.path(), ec2);
            de.mediaType = *mt;
            de.supported = isSupportedMedia(ext);
            de.name      = fmt::format(
                "{} {}",
                agentxx::agent::MediaAttachment::mediaTypeIcon(de.mediaType),
                de.name
            );
            files.push_back(std::move(de));
        }
    }

    // 排序
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return a.fullPath < b.fullPath;
    });
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.fullPath < b.fullPath;
    });

    for (auto& d : dirs) {
        entries_.push_back(std::move(d));
    }
    for (auto& f : files) {
        entries_.push_back(std::move(f));
    }

    applyFilter();
}

bool FilePickerOverlay::isMediaFile(const std::string& ext) const {
    static const std::set<std::string> allMedia = {
        ".png",
        ".jpg",
        ".jpeg",
        ".webp",
        ".gif",
        ".bmp",
        ".wav",
        ".mp3",
        ".ogg",
        ".m4a",
        ".aac",
        ".flac",
        ".mp4",
        ".mov",
        ".webm",
        ".mkv",
    };
    return allMedia.count(ext) > 0;
}

bool FilePickerOverlay::isSupportedMedia(const std::string& ext) const {
    return allowedExtensions_.count(ext) > 0;
}

agentxx::agent::MediaType FilePickerOverlay::guessMediaType(const std::string& ext) const {
    static const std::set<std::string> imageExts
        = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp"};
    static const std::set<std::string> audioExts
        = {".wav", ".mp3", ".ogg", ".m4a", ".aac", ".flac"};
    if (imageExts.count(ext)) {
        return agentxx::agent::MediaType::Image;
    }
    if (audioExts.count(ext)) {
        return agentxx::agent::MediaType::Audio;
    }
    return agentxx::agent::MediaType::Video;
}

void FilePickerOverlay::applyFilter() {
    filteredEntries_.clear();
    if (filterText_.empty()) {
        filteredEntries_ = entries_;
    } else {
        for (const auto& e : entries_) {
            if (e.isDir || agentxx::util::isIgnoreCaseContains(e.name, filterText_)) {
                filteredEntries_.push_back(e);
            }
        }
    }
    if (selectedIndex_ >= static_cast<int>(filteredEntries_.size())) {
        selectedIndex_ = std::max(0, static_cast<int>(filteredEntries_.size()) - 1);
    }
}

void FilePickerOverlay::confirmSelection() {
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(filteredEntries_.size())) {
        return;
    }
    const auto& entry = filteredEntries_[selectedIndex_];
    if (entry.isDir) {
        filterText_.clear();
        navigateTo(entry.fullPath);
        ctx_.postRedraw();
    } else if (entry.supported) {
        if (onSelectFile_) {
            onSelectFile_(entry.fullPath);
        }
        if (onClose_) {
            onClose_();
        }
    }
}

Element FilePickerOverlay::OnRender() {
    const auto& theme = *ctx_.theme;

    // 构建支持类型描述
    Elements typeIndicators;
    if (capability_.imageInput) {
        typeIndicators.push_back(
            text(std::string(TuiI18n::instance().t("picker.image"))) | color(theme.accentColor)
        );
    }
    if (capability_.audioInput) {
        if (!typeIndicators.empty()) {
            typeIndicators.push_back(text(" | ") | dim);
        }
        typeIndicators.push_back(
            text(std::string(TuiI18n::instance().t("picker.audio"))) | color(theme.accentColor)
        );
    }
    if (capability_.videoInput) {
        if (!typeIndicators.empty()) {
            typeIndicators.push_back(text(" | ") | dim);
        }
        typeIndicators.push_back(
            text(std::string(TuiI18n::instance().t("picker.video"))) | color(theme.accentColor)
        );
    }
    std::string caps;
    // header 仍用单行组合, 避免 hbox 嵌套翻译占位复杂度
    auto header = hbox({
        text(trf(
            "picker.title",
            fmt::format(
                "{}{}{}",
                capability_.imageInput ? std::string(TuiI18n::instance().t("picker.image")) : "",
                capability_.audioInput ? std::string(TuiI18n::instance().t("picker.audio")) : "",
                capability_.videoInput ? std::string(TuiI18n::instance().t("picker.video")) : ""
            )
        )) | bold,
    });
    (void)typeIndicators;
    (void)caps;

    auto pathLine = hbox({
        text(std::string(TuiI18n::instance().t("picker.path"))) | color(theme.hintColor),
        paragraph(currentDir_) | color(theme.normalColor) | xflex_shrink,
    });

    auto filterLine = hbox({
        text(std::string(TuiI18n::instance().t("picker.filter"))) | color(theme.hintColor),
        filterInput_->Render() | flex | color(theme.inputTextColor),
    });

    // 构建文件列表项 (itemBoxes_ 与 filteredEntries_ 一一对应, 供鼠标命中)
    Elements items;
    itemBoxes_.assign(filteredEntries_.size(), Box{});
    const int maxVisible = std::max(5, Terminal::Size().dimy / 2);
    // 确保 selectedIndex_ 在滚动视口中可见
    const int scrollStart = std::max(0, selectedIndex_ - maxVisible + 2);
    const int scrollEnd
        = std::min(static_cast<int>(filteredEntries_.size()), scrollStart + maxVisible);

    for (int i = scrollStart; i < scrollEnd; ++i) {
        const auto& entry    = filteredEntries_[i];
        const bool  selected = (i == selectedIndex_);

        Elements rowItems;
        rowItems.push_back(text("  "));

        if (entry.isDir) {
            rowItems.push_back(text(entry.name) | color(theme.normalColor));
        } else if (entry.supported) {
            rowItems.push_back(text(entry.name) | color(theme.normalColor));
            rowItems.push_back(filler());
            rowItems.push_back(
                text(fmt::format("({}) ", agentxx::util::formatSize(entry.sizeBytes))) | dim
            );
        } else {
            const char* labelKey
                = entry.mediaType == agentxx::agent::MediaType::Image   ? "picker.image"
                  : entry.mediaType == agentxx::agent::MediaType::Audio ? "picker.audio"
                                                                        : "picker.video";
            rowItems.push_back(text(entry.name) | dim);
            rowItems.push_back(filler());
            rowItems.push_back(
                text(trf("picker.unsupported", std::string(TuiI18n::instance().t(labelKey)))) | dim
            );
        }

        auto row = hbox(std::move(rowItems));
        if (selected) {
            row = row | bgcolor(theme.buttonActiveBgColor) | color(theme.buttonActiveTextColor)
                  | focus;
        }
        if (!entry.isDir && !entry.supported) {
            row = row | dim;
        }
        items.push_back(row | reflect(itemBoxes_[static_cast<size_t>(i)]));
    }

    if (filteredEntries_.empty()) {
        items.push_back(text(std::string(TuiI18n::instance().t("picker.empty"))) | dim);
    }

    auto helpLine = hbox({
        text(std::string(TuiI18n::instance().t("picker.hint"))) | dim,
    });

    const int dimX     = Terminal::Size().dimx;
    const int dimY     = Terminal::Size().dimy;
    const int overlayW = std::max(40, dimX * 3 / 5);
    const int overlayH = std::max(12, dimY * 4 / 5);

    auto content = vbox({
                       header | bold | inverted,
                       pathLine,
                       filterLine,
                       separator() | color(theme.hintColor),
                       vbox(std::move(items)) | flex | size(HEIGHT, LESS_THAN, overlayH - 8),
                       separator() | color(theme.hintColor),
                       helpLine,
                   })
                   | border | size(WIDTH, EQUAL, overlayW) | size(HEIGHT, LESS_THAN, overlayH)
                   | bgcolor(theme.blockColor);

    return content | center;
}

bool FilePickerOverlay::OnEvent(Event event) {
    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
            for (size_t i = 0; i < itemBoxes_.size(); ++i) {
                if (!itemBoxes_[i].Contain(mouse.x, mouse.y)) {
                    continue;
                }
                selectedIndex_ = static_cast<int>(i);
                confirmSelection();
                return true;
            }
            return false;
        }
        if (mouse.button == Mouse::WheelUp && selectedIndex_ > 0) {
            --selectedIndex_;
            ctx_.postRedraw();
            return true;
        }
        if (mouse.button == Mouse::WheelDown
            && selectedIndex_ + 1 < static_cast<int>(filteredEntries_.size())) {
            ++selectedIndex_;
            ctx_.postRedraw();
            return true;
        }
        return false;
    }
    if (event == Event::Escape) {
        if (onClose_) {
            onClose_();
        }
        return true;
    }

    if (event == Event::Return) {
        confirmSelection();
        return true;
    }

    if (event == Event::ArrowUp) {
        if (selectedIndex_ > 0) {
            --selectedIndex_;
            ctx_.postRedraw();
        }
        return true;
    }

    if (event == Event::ArrowDown) {
        if (selectedIndex_ < static_cast<int>(filteredEntries_.size()) - 1) {
            ++selectedIndex_;
            ctx_.postRedraw();
        }
        return true;
    }

    // 其他按键交给过滤输入框
    auto oldFilter = filterText_;
    bool handled   = filterInput_->OnEvent(event);
    if (filterText_ != oldFilter) {
        applyFilter();
        ctx_.postRedraw();
    }
    return handled;
}
