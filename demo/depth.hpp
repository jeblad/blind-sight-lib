// SPDX-License-Identifier: GPL-3.0-only
//
// depth.hpp — depth-map generation lifecycle: the Depth-Anything-V2 venv
// and model cache, availability checks, ArUco detection, and reading a
// generated .depth.exr file back in.
//
// Scope note: compute_prod_distance_m()/session_has_depth()/
// distance_to_delay_ms() stay in main.cpp — they operate directly on
// RecordingSession and PanoramaProjection, both core app types with no
// header of their own yet. This module is just the model/file lifecycle
// underneath them.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <string>
#include <vector>

std::string depth_venv_dir();
std::string depth_file_path_for(const std::string& image_path);
bool has_depth_file(const std::string& image_path);
bool depth_generation_available();
bool depth_model_cached(const std::string& model_size);
bool aruco_detection_available();

// Reads the "Z" channel written by demo/depth/generate_depth.py into
// `out` (row-major, `out_width` x `out_height`). False (leaving
// out/out_width/out_height untouched) on any read failure.
bool load_depth_exr(const std::string& path, std::vector<float>& out,
                     int& out_width, int& out_height);

// Prefetches the Depth-Anything-V2 model for `model_size` right now,
// synced with an explicit user action (the Preferences toggle). Async;
// reverts `row` and leaves the setting off on failure.
void start_depth_model_prefetch(Gtk::Window* parent,
                                 const Glib::RefPtr<Gio::Settings>& settings,
                                 AdwToastOverlay* toast_overlay, AdwSwitchRow* row,
                                 const std::string& model_size);
