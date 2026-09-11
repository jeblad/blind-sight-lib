// SPDX-License-Identifier: GPL-3.0-only
//
// main.cpp — blind-sight-lib demo: minimal app window (splash image + label).
//
// First milestone (see notes.md, "Demo architecture"): a plain window that
// opens with a splash image and a centered explanatory label, and closes
// cleanly via the window's own close button. No header bar, no file loading,
// no library link yet.
//
// GTK4 is used through gtkmm; libadwaita has no official C++ binding, so its
// application/window objects are created through the C API and wrapped into
// gtkmm objects with Glib::wrap() where that is safe. Content is set through
// adw_application_window_set_content() rather than Gtk::Window::set_child(),
// since AdwApplicationWindow does not support the generic GtkWindow child API
// for its content. Keep this interop confined to this file's app/window setup
// so a future switch to libadwaitamm (if it matures) stays local — see
// notes.md, "Demo stack".

#include <adwaita.h>
#include <gtkmm.h>
#include <glib/gi18n.h>

#include <clocale>
#include <string>

namespace {

constexpr auto kAppId = "io.github.jeblad.blind-sight-demo";
constexpr auto kGettextDomain = "blind-sight-demo";
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr auto kSplashFile = "E16,_Aurland.jpeg";

std::string resource_path(const std::string& filename) {
    return std::string(BLIND_SIGHT_RESOURCE_DIR) + "/" + filename;
}

// .osd gives a translucent dark backing but no control over how translucent,
// no rounding, and no padding worth the name — and .dim-label (tried first
// for the copyright line) sets *opacity* on the label itself, which is
// exactly the "transparent text" effect that should never happen. A small
// dedicated stylesheet instead: opaque text always, tunable background
// alpha, explicit padding between the card edge and the text.
void load_app_css() {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(
        ".blind-sight-splash-caption {"
        "  background-color: rgba(0, 0, 0, 0.75);"
        "  border-radius: 12px;"
        "  padding: 16px 24px;"
        "  color: rgb(255, 255, 255);"
        "}"
        ".blind-sight-splash-copyright {"
        "  color: rgb(200, 200, 200);"
        "}");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// All user-facing strings go through _() (gettext) so the app is actually
// translatable, not just written in one language. English is the source
// (msgid) language, matching the rest of the repo; Norwegian and others are
// .po translations (see po/, extracted into po/blind-sight-demo.pot via
// po/POTFILES.in, compiled to locale/<lang>/LC_MESSAGES/*.mo via msgfmt —
// no CMake-automated pipeline yet, same manual approach as
// ~/spaces/gnome-agent-proxy). _() is a safe no-op for any locale without a
// catalog.
Gtk::Widget* build_splash() {
    auto* overlay = Gtk::make_managed<Gtk::Overlay>();

    auto* picture = Gtk::make_managed<Gtk::Picture>();
    picture->set_filename(resource_path(kSplashFile));
    picture->set_content_fit(Gtk::ContentFit::COVER);
    picture->set_can_shrink(true);
    overlay->set_child(*picture);

    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    text->set_halign(Gtk::Align::CENTER);
    text->set_valign(Gtk::Align::CENTER);
    text->add_css_class("blind-sight-splash-caption");
    // Margin: gap between the card and the window/overlay edge. Padding
    // (inside the card, between its edge and the text) is separate and
    // lives in the CSS class above — the two were conflated before.
    text->set_margin(16);

    auto* title = Gtk::make_managed<Gtk::Label>(_("Blind Sight"));
    title->add_css_class("title-1");

    auto* tagline = Gtk::make_managed<Gtk::Label>(
        _("Echolocation for blind and visually impaired people"));

    auto* copyright = Gtk::make_managed<Gtk::Label>(
        _("© John Erling Blad, 2026"));
    copyright->add_css_class("caption");
    copyright->add_css_class("blind-sight-splash-copyright");

    for (auto* line : {title, tagline, copyright}) {
        line->set_halign(Gtk::Align::CENTER);
        line->set_justify(Gtk::Justification::CENTER);
        text->append(*line);
    }

    overlay->add_overlay(*text);
    return overlay;
}

// The "essential" dialog per John (2026-09-11): contributors for the code,
// translators for the translated text. Developers is a plain name list here
// (solo project so far); translator_credits uses the gettext convention —
// the msgid is literally "translator-credits", and each .po file's
// *translation* of it is that language's translator name(s) (po/nb.po).
void show_about(Gtk::Window* parent) {
    auto* about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(about, _("Blind Sight"));
    adw_about_dialog_set_application_icon(about, "audio-speakers-symbolic");
    adw_about_dialog_set_version(about, "0.0.0");
    adw_about_dialog_set_developer_name(about, "John Erling Blad");

    const char* developers[] = {"John Erling Blad", nullptr};
    adw_about_dialog_set_developers(about, developers);

    adw_about_dialog_set_translator_credits(about, _("translator-credits"));
    adw_about_dialog_set_copyright(about, "© 2026 John Erling Blad");

    // GTK_LICENSE_GPL_3_0 only labels the license family — the enum has no
    // -only vs -or-later distinction. That distinction is LICENSE (verbatim,
    // unmodified) and the SPDX headers' job, not this dialog's; see
    // notes.md / README "License". This is the standard recognizable
    // badge + "Read License" link, not the authoritative statement.
    adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_website(about, "https://github.com/jeblad/blind-sight-lib");
    adw_about_dialog_set_issue_url(
        about, "https://github.com/jeblad/blind-sight-lib/issues");

    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(parent->gobj()));
}

// Empty for now, as agreed — pages land here later (render scale,
// depth-estimation override, max-measured-depth). AdwPreferencesDialog is
// the current idiom (AdwPreferencesWindow's replacement).
void show_preferences(Gtk::Window* parent) {
    auto* prefs = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(parent->gobj()));
}

// Also empty for now. GtkShortcutsWindow (and GtkShortcutsSection/Group/
// Shortcut) is deprecated as of GTK 4.18 — this system has 4.22 — with no
// single official replacement widget yet; GNOME's current guidance is
// roughly "build your own shortcuts presentation." Not worth building
// against a widget family on its way out for what's meant to be an empty
// stub, so this reuses the same empty-AdwPreferencesDialog placeholder as
// Preferences. Replace with real (translated) shortcut content once there
// are app shortcuts worth documenting and a non-deprecated pattern to show
// them in.
void show_shortcuts(Gtk::Window* parent) {
    auto* shortcuts = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    adw_dialog_present(ADW_DIALOG(shortcuts), GTK_WIDGET(parent->gobj()));
}

void on_activate(const Glib::RefPtr<Gtk::Application>& app) {
    auto* adw_win = ADW_APPLICATION_WINDOW(
        adw_application_window_new(app->gobj()));

    // AdwApplicationWindow rejects both Gtk::Window::set_child() and
    // set_titlebar() (the latter aborts with a hard Adwaita-ERROR, not just
    // a warning). The supported structure is an AdwToolbarView as the
    // window's content, holding an AdwHeaderBar as its top bar — this is
    // also where the header-bar controls from notes.md (Open, source,
    // hamburger menu) will live, so it's the right foundation, not a
    // workaround. GNOME/Mutter draws no server-side decorations under
    // Wayland, so without this the window would have no close button at all.
    auto* header_bar = adw_header_bar_new();

    // Primary/hamburger menu, right of the window controls. John flagged
    // this pattern as slightly against current GNOME HIG guidance but
    // common enough in real apps to use here too.
    auto menu = Gio::Menu::create();
    menu->append(_("Preferences"), "app.preferences");
    menu->append(_("Keyboard Shortcuts"), "app.shortcuts");
    menu->append(_("About Blind Sight"), "app.about");

    auto* menu_button = Gtk::make_managed<Gtk::MenuButton>();
    menu_button->set_icon_name("open-menu-symbolic");
    menu_button->set_menu_model(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(menu_button->gobj()));

    auto* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar_view, header_bar);
    adw_toolbar_view_set_content(toolbar_view, build_splash()->gobj());

    adw_application_window_set_content(adw_win, GTK_WIDGET(toolbar_view));

    app->add_action("about", [app]() { show_about(app->get_active_window()); });
    app->add_action("preferences",
                     [app]() { show_preferences(app->get_active_window()); });
    app->add_action("shortcuts",
                     [app]() { show_shortcuts(app->get_active_window()); });

    auto* window = Glib::wrap(GTK_WINDOW(adw_win), false);
    window->set_title(_("Blind Sight"));
    window->set_default_size(kDefaultWidth, kDefaultHeight);
    window->present();

    // No background threads yet, so the default Gtk::Application shutdown
    // (last window closes -> run() returns -> process exits) is sufficient.
    // Revisit when the audio render thread lands: it must be stopped and
    // joined on the window's close-request / the app's shutdown signal, or
    // it becomes exactly the dangling process this milestone is meant to
    // rule out.
}

}  // namespace

int main(int argc, char** argv) {
    // Glib::wrap() needs gtkmm's per-module wrap tables registered before it
    // can resolve a GObject to a C++ wrapper, and that registration only
    // happens as a side effect of constructing a real gtkmm object through
    // gtkmm's own factory. Wrapping a raw AdwApplication* here — before any
    // such object existed — crashed with "wrap_func_table != nullptr". Fix:
    // create the application through Gtk::Application::create() (a genuine
    // gtkmm object) and initialize libadwaita explicitly instead of routing
    // through AdwApplication. AdwApplicationWindow does not require the
    // *application* to be an AdwApplication, only a GtkApplication, so this
    // loses nothing. The window further down still uses the C API +
    // Glib::wrap() — safe now, since gtkmm's tables exist by then.
    gtk_init();
    adw_init();

    // Gettext bootstrap — catalogs are in locale/<lang>/LC_MESSAGES/, built
    // from po/*.po via msgfmt (see po/POTFILES.in).
    setlocale(LC_ALL, "");
    bindtextdomain(kGettextDomain, BLIND_SIGHT_LOCALE_DIR);
    bind_textdomain_codeset(kGettextDomain, "UTF-8");
    textdomain(kGettextDomain);

    auto app = Gtk::Application::create(kAppId);
    app->signal_activate().connect([app]() {
        load_app_css();
        on_activate(app);
    });

    return app->run(argc, argv);
}
