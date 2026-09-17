#include "agentxx-client/io/tui/components/overlays.h"
#include "agentxx-client/io/tui/framework/tui_i18n.h"
#include "agentxx-client/io/tui/surface.h"
#include "agentxx/util/log.h"
#include "agentxx/util/string_util.h"
#include "fmt/format.h"
#include "ftxui/component/component.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/terminal.hpp"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>

namespace agentxx::client {

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

    // 本地初始目录
    if (initialDir.empty()) {
        std::error_code ec;
        auto            cwd = std::filesystem::current_path(ec);
        if (!ec) {
            initialDir = cwd.string();
        } else {
            initialDir = "/";
        }
    }

    localTab_.currentDir = initialDir;
    navigateToLocal(localTab_.currentDir);

    // 若当前为跨设备, 初始化服务端目录 (初始为服务端工作目录)
    if (ctx_.isServerDifferentDevice()) {
        serverTab_.currentDir = ctx_.serverWorkDir.empty() ? "/" : ctx_.serverWorkDir;
    }
}

void FilePickerOverlay::navigateToLocal(std::string dirPath) {
    localTab_.currentDir    = dirPath;
    localTab_.selectedIndex = 0;
    localTab_.entries.clear();

    std::error_code ec;
    auto            canonical = std::filesystem::canonical(dirPath, ec);
    if (ec) {
        XX_LOGW("[FilePicker] cannot canonical {}: {}", dirPath, ec.message());
        return;
    }
    localTab_.currentDir = canonical.string();

    // 上级目录
    if (canonical.has_parent_path() && canonical.parent_path() != canonical) {
        DirEntry parentEntry;
        parentEntry.name     = std::string(TuiI18n::instance().t("picker.parent"));
        parentEntry.fullPath = canonical.parent_path().string();
        parentEntry.isDir    = true;
        localTab_.entries.push_back(std::move(parentEntry));
    }

    // 遍历当前目录
    std::vector<DirEntry> dirs;
    std::vector<DirEntry> files;

    auto iter = std::filesystem::directory_iterator(canonical, ec);
    if (ec) {
        XX_LOGW("[FilePicker] cannot list {}: {}", localTab_.currentDir, ec.message());
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
            de.isDir     = true;
            de.supported = true;
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
        localTab_.entries.push_back(std::move(d));
    }
    for (auto& f : files) {
        localTab_.entries.push_back(std::move(f));
    }
}

void FilePickerOverlay::navigateToServer(std::string dirPath) {
    serverTab_.loading = true;
    serverTab_.error.clear();
    serverTab_.currentDir    = dirPath;
    serverTab_.selectedIndex = 0;
    serverTab_.entries.clear();
    ctx_.postRedraw();

    if (!ctx_.requestServerListDir) {
        serverTab_.loading = false;
        serverTab_.error   = "requestServerListDir handler not configured";
        ctx_.postRedraw();
        return;
    }

    std::vector<std::string> exts(allowedExtensions_.begin(), allowedExtensions_.end());
    ctx_.requestServerListDir(
        dirPath,
        std::move(exts),
        [this](const agentxx::agent::WireListDirResult& res) {
            serverTab_.loading = false;
            if (!res.ok) {
                serverTab_.error = res.error.empty() ? "unknown error" : res.error;
                ctx_.postRedraw();
                return;
            }
            serverTab_.currentDir    = res.currentDir;
            serverTab_.selectedIndex = 0;
            serverTab_.entries.clear();

            // 若有上级目录, 添加上级目录条目
            if (!res.parentDir.empty()) {
                DirEntry parentEntry;
                parentEntry.name     = std::string(TuiI18n::instance().t("picker.parent"));
                parentEntry.fullPath = res.parentDir;
                parentEntry.isDir    = true;
                serverTab_.entries.push_back(std::move(parentEntry));
            }

            for (const auto& e : res.entries) {
                DirEntry de;
                de.name      = e.name;
                de.fullPath  = e.fullPath;
                de.isDir     = e.isDir;
                de.supported = e.supported;
                de.sizeBytes = e.sizeBytes;
                de.mediaType = e.mediaType;
                serverTab_.entries.push_back(std::move(de));
            }
            ctx_.postRedraw();
        }
    );
}

void FilePickerOverlay::switchTab(PickerTab tab) {
    if (activeTab_ == tab) {
        return;
    }
    activeTab_ = tab;
    if (activeTab_ == PickerTab::Server && serverTab_.entries.empty() && !serverTab_.loading) {
        navigateToServer(serverTab_.currentDir);
    }
    ctx_.postRedraw();
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

void FilePickerOverlay::confirmSelection() {
    auto& curTab = (activeTab_ == PickerTab::Local) ? localTab_ : serverTab_;
    if (curTab.selectedIndex < 0
        || curTab.selectedIndex >= static_cast<int>(curTab.entries.size())) {
        return;
    }
    const auto& entry = curTab.entries[curTab.selectedIndex];
    if (entry.isDir) {
        if (activeTab_ == PickerTab::Local) {
            navigateToLocal(entry.fullPath);
        } else {
            navigateToServer(entry.fullPath);
        }
        ctx_.postRedraw();
        return;
    }

    if (!entry.supported) {
        return;
    }

    // 选中支持的媒体文件
    if (activeTab_ == PickerTab::Local) {
        // 本地附件: 预检大小、读取并 Base64 编码为 Data URL
        std::error_code ec;
        auto            fileSize = std::filesystem::file_size(entry.fullPath, ec);
        if (ec) {
            if (ctx_.showToast) {
                ctx_.showToast(trf("toast.attachReadFail", ec.message()));
            }
            return;
        }
        auto ext
            = agentxx::util::toLower(std::filesystem::path(entry.fullPath).extension().string());
        auto mime = agentxx::agent::mimeTypeFromExtension(ext);
        if (mime.empty()) {
            if (ctx_.showToast) {
                ctx_.showToast(std::string(TuiI18n::instance().t("toast.attachBadType")));
            }
            return;
        }
        uint64_t maxSize = agentxx::agent::maxBytesForMediaType(entry.mediaType);
        if (fileSize > maxSize) {
            if (ctx_.showToast) {
                ctx_.showToast(
                    trf("toast.attachTooLarge",
                        fmt::format("{:.1f} MB", static_cast<double>(fileSize) / (1024.0 * 1024.0)),
                        fmt::format("{:.0f} MB", static_cast<double>(maxSize) / (1024.0 * 1024.0)))
                );
            }
            return;
        }

        std::ifstream ifs(entry.fullPath, std::ios::binary);
        if (!ifs) {
            if (ctx_.showToast) {
                ctx_.showToast(std::string(TuiI18n::instance().t("toast.attachOpenFail")));
            }
            return;
        }
        std::string fileData(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>()
        );
        ifs.close();

        agentxx::agent::MediaAttachment att;
        att.type        = entry.mediaType;
        att.displayName = entry.name;
        att.mimeType    = std::string(mime);
        att.pathOrUrl   = entry.fullPath;
        att.dataUrl = fmt::format("data:{};base64,{}", mime, agentxx::util::base64Encode(fileData));
        att.sizeBytes = fileSize;

        if (onSelectAttachment_) {
            onSelectAttachment_(std::move(att));
        } else if (onSelectFile_) {
            onSelectFile_(entry.fullPath);
        }
    } else {
        // 服务端附件: 直接构造服务端路径附件 (dataUrl 留空, 服务端自主加载并转 Base64)
        auto ext
            = agentxx::util::toLower(std::filesystem::path(entry.fullPath).extension().string());
        auto mime = agentxx::agent::mimeTypeFromExtension(ext);

        agentxx::agent::MediaAttachment att;
        att.type        = entry.mediaType;
        att.displayName = entry.name;
        att.mimeType    = std::string(mime);
        att.pathOrUrl   = entry.fullPath;
        att.dataUrl     = "";
        att.sizeBytes   = entry.sizeBytes;

        if (onSelectAttachment_) {
            onSelectAttachment_(std::move(att));
        } else if (onSelectFile_) {
            onSelectFile_(entry.fullPath);
        }
    }

    if (onClose_) {
        onClose_();
    }
}

std::string FilePickerOverlay::itemHitId(PickerTab tab, size_t index) {
    return fmt::format("item/{}/{}", tab == PickerTab::Server ? "server" : "local", index);
}

int FilePickerOverlay::itemIndexOfHitId(PickerTab tab, std::string_view hitId) {
    const std::string prefix = itemHitId(tab, 0);
    const std::string head   = prefix.substr(0, prefix.rfind('/') + 1);
    if (!hitId.starts_with(head)) {
        return -1;
    }
    int         index  = -1;
    const auto  number = hitId.substr(head.size());
    const auto* begin  = number.data();
    const auto* end    = number.data() + number.size();
    if (number.empty() || std::from_chars(begin, end, index).ec != std::errc{} || index < 0) {
        return -1;
    }
    return index;
}

FilePickerOverlay::TabState& FilePickerOverlay::currentTab() {
    return (activeTab_ == PickerTab::Local) ? localTab_ : serverTab_;
}

const FilePickerOverlay::TabState& FilePickerOverlay::currentTab() const {
    return (activeTab_ == PickerTab::Local) ? localTab_ : serverTab_;
}

ftxui::Box FilePickerOverlay::hitBox(std::string_view id) const {
    for (const auto& entry : hits_.entries()) {
        if (entry.payload.id == id) {
            return *entry.box;
        }
    }
    return agentxx::client::kNoBox;
}

ftxui::Box FilePickerOverlay::itemBox(size_t index) const {
    return hitBox(itemHitId(activeTab_, index));
}

ftxui::Box FilePickerOverlay::tabButtonBox(bool serverTab) const {
    return hitBox(serverTab ? kServerTabHitId : kLocalTabHitId);
}

Element FilePickerOverlay::OnRender() {
    const auto& theme         = *ctx_.theme;
    const bool  isCrossDevice = ctx_.isServerDifferentDevice();
    auto&       curTab        = currentTab();

    // 帧首清空命中表: 本帧未渲染出来的控件 (非跨设备时的标签页按钮、当前 tab 之外
    // 的目录条目) 不会占用任何点击区域
    hits_.beginFrame();

    // 标题: 单行组合
    const std::string titleText = trf(
        "picker.title",
        fmt::format(
            "{}{}{}",
            capability_.imageInput ? std::string(TuiI18n::instance().t("picker.image")) : "",
            capability_.audioInput ? std::string(TuiI18n::instance().t("picker.audio")) : "",
            capability_.videoInput ? std::string(TuiI18n::instance().t("picker.video")) : ""
        )
    );

    Elements content;

    // 若跨设备, 顶部渲染 Tab 切换按钮
    if (isCrossDevice) {
        auto localBtn = text(std::string(TuiI18n::instance().t("picker.tab_local")));
        if (activeTab_ == PickerTab::Local) {
            localBtn = localBtn | bold | bgcolor(theme.buttonActiveBgColor)
                       | color(theme.buttonActiveTextColor);
        } else {
            localBtn = localBtn | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        localBtn = hits_.add(std::move(localBtn), std::string{kLocalTabHitId});

        auto serverBtn = text(std::string(TuiI18n::instance().t("picker.tab_server")));
        if (activeTab_ == PickerTab::Server) {
            serverBtn = serverBtn | bold | bgcolor(theme.buttonActiveBgColor)
                        | color(theme.buttonActiveTextColor);
        } else {
            serverBtn = serverBtn | bgcolor(theme.buttonBgColor) | color(theme.buttonTextColor);
        }
        serverBtn = hits_.add(std::move(serverBtn), std::string{kServerTabHitId});

        content.push_back(hbox({
            localBtn,
            text(" "),
            serverBtn,
            filler(),
        }));
        content.push_back(tuiSurfaceGapRow(theme.surfaceColor));
    }

    // 路径行
    auto pathLine = hbox({
        text(std::string(TuiI18n::instance().t("picker.path"))) | color(theme.hintColor),
        paragraph(curTab.currentDir) | color(theme.normalColor) | xflex_shrink,
    });
    content.push_back(pathLine);
    content.push_back(tuiSurfaceGapRow(theme.surfaceColor));

    // 文件列表区
    Elements  items;
    const int maxVisible  = std::max(5, ctx_.terminalSize().dimy / 2);
    const int scrollStart = std::max(0, curTab.selectedIndex - maxVisible + 2);
    const int scrollEnd
        = std::min(static_cast<int>(curTab.entries.size()), scrollStart + maxVisible);

    if (activeTab_ == PickerTab::Server && curTab.loading) {
        items.push_back(text(std::string(TuiI18n::instance().t("picker.server_loading"))) | dim);
    } else if (activeTab_ == PickerTab::Server && !curTab.error.empty()) {
        items.push_back(text(trf("picker.server_error", curTab.error)) | color(theme.errorColor));
    } else {
        const std::string parentNameStr = std::string(TuiI18n::instance().t("picker.parent"));
        for (int i = scrollStart; i < scrollEnd; ++i) {
            const auto& entry    = curTab.entries[i];
            const bool  selected = (i == curTab.selectedIndex);

            Elements rowItems;
            if (entry.isDir) {
                std::string displayName;
                if (entry.name == parentNameStr) {
                    displayName = "📁 " + entry.name;
                } else {
                    displayName = "📁 " + entry.name + "/";
                }
                rowItems.push_back(text(displayName) | color(theme.normalColor));
            } else if (entry.supported) {
                std::string displayName = fmt::format(
                    "{} {}",
                    agentxx::agent::MediaAttachment::mediaTypeIcon(entry.mediaType),
                    entry.name
                );
                rowItems.push_back(text(displayName) | color(theme.normalColor));
                rowItems.push_back(filler());
                rowItems.push_back(
                    text(fmt::format("( {} ) ", agentxx::util::formatSize(entry.sizeBytes))) | dim
                );
            } else {
                std::string displayName = fmt::format(
                    "{} {}",
                    agentxx::agent::MediaAttachment::mediaTypeIcon(entry.mediaType),
                    entry.name
                );
                const char* labelKey
                    = entry.mediaType == agentxx::agent::MediaType::Image   ? "picker.image"
                      : entry.mediaType == agentxx::agent::MediaType::Audio ? "picker.audio"
                                                                            : "picker.video";
                rowItems.push_back(text(displayName) | dim);
                rowItems.push_back(filler());
                rowItems.push_back(
                    text(trf("picker.unsupported", std::string(TuiI18n::instance().t(labelKey))))
                    | dim
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
            // 命中登记 (id 含 tab 归属与条目下标): 仅本帧渲染出来的条目可命中
            items.push_back(hits_.add(std::move(row), itemHitId(activeTab_, static_cast<size_t>(i)))
            );
        }

        if (curTab.entries.empty()) {
            items.push_back(text(std::string(TuiI18n::instance().t("picker.empty"))) | dim);
        }
    }

    const int dimX     = ctx_.terminalSize().dimx;
    const int dimY     = ctx_.terminalSize().dimy;
    const int overlayW = std::max(40, dimX * 3 / 5);
    const int overlayH = std::max(12, dimY * 4 / 5);

    const auto style = TuiSurfaceStyle::fromTheme(theme);
    content.push_back(vbox(std::move(items)) | flex | size(HEIGHT, LESS_THAN, overlayH - 10));

    const std::string hintText = isCrossDevice
                                     ? std::string(TuiI18n::instance().t("picker.hint_tabs"))
                                     : std::string(TuiI18n::instance().t("picker.hint"));

    return tuiSurfacePopup(style, titleText, vbox(std::move(content)) | flex, hintText)
           | size(WIDTH, EQUAL, overlayW) | size(HEIGHT, LESS_THAN, overlayH) | center;
}

bool FilePickerOverlay::OnEvent(Event event) {
    const bool isCrossDevice = ctx_.isServerDifferentDevice();
    auto&      curTab        = currentTab();

    if (event.is_mouse()) {
        const auto& mouse = event.mouse();
        // 命中查表: 标签页按钮 / 目录条目 (未渲染的项不在表中, 不会命中)
        if (const auto* hit = hits_.findClick(mouse)) {
            const std::string& hitId = hit->payload.id;
            if (hitId == kLocalTabHitId) {
                switchTab(PickerTab::Local);
                return true;
            }
            if (hitId == kServerTabHitId) {
                switchTab(PickerTab::Server);
                return true;
            }
            if (const int index = itemIndexOfHitId(activeTab_, hitId); index >= 0) {
                curTab.selectedIndex = index;
                confirmSelection();
                return true;
            }
        }
        if (mouse.button == Mouse::WheelUp && curTab.selectedIndex > 0) {
            --curTab.selectedIndex;
            ctx_.postRedraw();
            return true;
        }
        if (mouse.button == Mouse::WheelDown
            && curTab.selectedIndex + 1 < static_cast<int>(curTab.entries.size())) {
            ++curTab.selectedIndex;
            ctx_.postRedraw();
            return true;
        }
        // 模态是遮挡层: 未命中的鼠标事件同样不再下发
        // (否则滚轮会滚动被遮挡的消息列表)
        return true;
    }

    if (event == Event::Tab && isCrossDevice) {
        switchTab(activeTab_ == PickerTab::Local ? PickerTab::Server : PickerTab::Local);
        return true;
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
        if (curTab.selectedIndex > 0) {
            --curTab.selectedIndex;
            ctx_.postRedraw();
        }
        return true;
    }

    if (event == Event::ArrowDown) {
        if (curTab.selectedIndex < static_cast<int>(curTab.entries.size()) - 1) {
            ++curTab.selectedIndex;
            ctx_.postRedraw();
        }
        return true;
    }

    if (event == Event::Backspace) {
        // 退格键: 寻找上级目录条目并进入
        const std::string parentNameStr = std::string(TuiI18n::instance().t("picker.parent"));
        for (const auto& e : curTab.entries) {
            if (e.isDir && e.name == parentNameStr) {
                if (activeTab_ == PickerTab::Local) {
                    navigateToLocal(e.fullPath);
                } else {
                    navigateToServer(e.fullPath);
                }
                ctx_.postRedraw();
                return true;
            }
        }
    }

    // 模态是遮挡层: 其余事件一律吞掉 (输入字符不得落到被遮挡的输入框)
    return true;
}

} // namespace agentxx::client
