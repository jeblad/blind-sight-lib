// SPDX-License-Identifier: GPL-3.0-only
//
// external_change_guard.hpp — shared by every "Details" dialog's Save
// handler (project/session/image — project_dialogs.cpp/session_dialogs.cpp/
// image_dialogs.cpp), which is why this lives in its own file rather than
// inside any one of them.

#pragma once

#include <giomm/file.h>
#include <giomm/filemonitor.h>
#include <glibmm/refptr.h>

#include <filesystem>
#include <string>

// Watches `path` for external changes from construction until destroyed —
// a last-moment check against overwriting a concurrent change made while
// a dialog was open (e.g. a hand-edit to protocol.md in a text editor).
// Gio::FileMonitor is the primary signal (inotify where available; GLib
// itself degrades to polling elsewhere rather than silently claiming
// success the way an unsupported flock() can on some filesystems — NFS
// without lockd, older SMB/FAT); comparing the file's own mtime at
// has_changed()-check time is the fallback, in case monitor_file() itself
// failed or missed the event. Neither mechanism tries to *prevent* the
// write a dialog's Save performs while open elsewhere — only to detect,
// after the fact, that one happened.
class ExternalChangeGuard {
public:
    explicit ExternalChangeGuard(const std::string& path)
        : path_(path), baseline_mtime_(mtime_of(path)) {
        try {
            monitor_ = Gio::File::create_for_path(path)->monitor_file(
                Gio::FileMonitorFlags::NONE);
            monitor_->signal_changed().connect(
                [this](const Glib::RefPtr<Gio::File>&, const Glib::RefPtr<Gio::File>&,
                        Gio::FileMonitor::Event) { changed_ = true; });
        } catch (const Glib::Error&) {
            // No monitor available on this backend — mtime_of()'s own
            // comparison in has_changed() is the fallback.
        }
    }

    bool has_changed() const { return changed_ || mtime_of(path_) != baseline_mtime_; }

private:
    static std::filesystem::file_time_type mtime_of(const std::string& path) {
        std::error_code ec;
        auto result = std::filesystem::last_write_time(path, ec);
        return ec ? std::filesystem::file_time_type{} : result;
    }

    std::string path_;
    std::filesystem::file_time_type baseline_mtime_;
    Glib::RefPtr<Gio::FileMonitor> monitor_;
    bool changed_ = false;
};
