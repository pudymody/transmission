// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <errno.h>
#include <stdio.h>
#include <map>
#include <memory>

#include <glibmm.h>
#include <glibmm/i18n.h>

#include <fmt/core.h>

#include <libtransmission/transmission.h>
#include <libtransmission/log.h>

#include "Actions.h"
#include "MessageLogWindow.h"
#include "Prefs.h"
#include "PrefsDialog.h"
#include "Session.h"
#include "Utils.h"

class MessageLogColumnsModel : public Gtk::TreeModelColumnRecord
{
public:
    MessageLogColumnsModel()
    {
        add(sequence);
        add(name);
        add(message);
        add(tr_msg);
    }

    Gtk::TreeModelColumn<unsigned int> sequence;
    Gtk::TreeModelColumn<Glib::ustring> name;
    Gtk::TreeModelColumn<Glib::ustring> message;
    Gtk::TreeModelColumn<tr_log_message*> tr_msg;
};

MessageLogColumnsModel const message_log_cols;

class MessageLogWindow::Impl
{
public:
    Impl(MessageLogWindow& window, Glib::RefPtr<Gtk::Builder> const& builder, Glib::RefPtr<Session> const& core);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

private:
    bool onRefresh();

    void onSaveRequest();
    void onSaveDialogResponse(std::shared_ptr<Gtk::FileChooserDialog>& d, int response);
    void doSave(Gtk::Window& parent, Glib::ustring const& filename);

    void onClearRequest();
    void onPauseToggled(Gio::SimpleAction& action);

    void scroll_to_bottom();
    void level_combo_changed_cb(Gtk::ComboBox* combo_box);
    void level_combo_init(Gtk::ComboBox* level_combo) const;

    bool is_pinned_to_new() const;
    bool isRowVisible(Gtk::TreeModel::const_iterator const& iter) const;

private:
    MessageLogWindow& window_;
    Glib::RefPtr<Session> const core_;

    Gtk::TreeView* view_ = nullptr;
    Glib::RefPtr<Gtk::ListStore> store_;
    Glib::RefPtr<Gtk::TreeModelFilter> filter_;
    Glib::RefPtr<Gtk::TreeModelSort> sort_;
    tr_log_level maxLevel_ = TR_LOG_INFO;
    bool isPaused_ = false;
    sigc::connection refresh_tag_;
    std::map<tr_log_level, char const*> const level_names_;
};

namespace
{

tr_log_message* myTail = nullptr;
tr_log_message* myHead = nullptr;

} // namespace

/****
*****
****/

/* is the user looking at the latest messages? */
bool MessageLogWindow::Impl::is_pinned_to_new() const
{
    bool pinned_to_new = false;

    if (view_ == nullptr)
    {
        pinned_to_new = true;
    }
    else
    {
        Gtk::TreeModel::Path first_visible;
        Gtk::TreeModel::Path last_visible;

        if (view_->get_visible_range(first_visible, last_visible))
        {
            auto const row_count = sort_->children().size();

            if (auto const iter = sort_->children()[row_count - 1]; iter)
            {
                pinned_to_new = last_visible == sort_->get_path(TR_GTK_TREE_MODEL_CHILD_ITER(iter));
            }
        }
    }

    return pinned_to_new;
}

void MessageLogWindow::Impl::scroll_to_bottom()
{
    if (sort_ != nullptr)
    {
        auto const row_count = sort_->children().size();

        if (auto const iter = sort_->children()[row_count - 1]; iter)
        {
            view_->scroll_to_row(sort_->get_path(TR_GTK_TREE_MODEL_CHILD_ITER(iter)), 1);
        }
    }
}

/****
*****
****/

void MessageLogWindow::Impl::level_combo_init(Gtk::ComboBox* level_combo) const
{
    auto items = std::vector<std::pair<Glib::ustring, int>>{};
    for (auto const& [level, name] : level_names_)
    {
        items.emplace_back(name, level);
    }
    gtr_combo_box_set_enum(*level_combo, items);
    gtr_combo_box_set_active_enum(*level_combo, gtr_pref_int_get(TR_KEY_message_level));
}

void MessageLogWindow::Impl::level_combo_changed_cb(Gtk::ComboBox* combo_box)
{
    auto const level = static_cast<tr_log_level>(gtr_combo_box_get_active_enum(*combo_box));
    bool const pinned_to_new = is_pinned_to_new();

    tr_logSetLevel(level);
    core_->set_pref(TR_KEY_message_level, level);
    maxLevel_ = level;
    filter_->refilter();

    if (pinned_to_new)
    {
        scroll_to_bottom();
    }
}

namespace
{

/* similar to asctime, but is utf8-clean */
Glib::ustring gtr_asctime(time_t t)
{
    return Glib::DateTime::create_now_local(t).format("%a %b %e %T %Y"); /* ctime equiv */
}

} // namespace

void MessageLogWindow::Impl::doSave(Gtk::Window& parent, Glib::ustring const& filename)
{
    auto* fp = fopen(filename.c_str(), "w+");

    if (fp == nullptr)
    {
        auto const errcode = errno;
        auto w = std::make_shared<Gtk::MessageDialog>(
            parent,
            fmt::format(
                _("Couldn't save '{path}': {error} ({error_code})"),
                fmt::arg("path", filename),
                fmt::arg("error", g_strerror(errcode)),
                fmt::arg("error_code", errcode)),
            false,
            TR_GTK_MESSAGE_TYPE(ERROR),
            TR_GTK_BUTTONS_TYPE(CLOSE));
        w->set_secondary_text(Glib::strerror(errno));
        w->signal_response().connect([w](int /*response*/) mutable { w.reset(); });
        w->show();
    }
    else
    {
        for (auto const& row : store_->children())
        {
            auto const* const node = row.get_value(message_log_cols.tr_msg);
            auto const date = gtr_asctime(node->when);

            auto const it = level_names_.find(node->level);
            auto const* const level_str = it != std::end(level_names_) ? it->second : "???";

            fprintf(fp, "%s\t%s\t%s\t%s\n", date.c_str(), level_str, node->name.c_str(), node->message.c_str());
        }

        fclose(fp);
    }
}

void MessageLogWindow::Impl::onSaveDialogResponse(std::shared_ptr<Gtk::FileChooserDialog>& d, int response)
{
    if (response == TR_GTK_RESPONSE_TYPE(ACCEPT))
    {
        doSave(*d, d->get_file()->get_path());
    }

    d.reset();
}

void MessageLogWindow::Impl::onSaveRequest()
{
    auto d = std::make_shared<Gtk::FileChooserDialog>(window_, _("Save Log"), TR_GTK_FILE_CHOOSER_ACTION(SAVE));
    d->add_button(_("_Cancel"), TR_GTK_RESPONSE_TYPE(CANCEL));
    d->add_button(_("_Save"), TR_GTK_RESPONSE_TYPE(ACCEPT));

    d->signal_response().connect([this, d](int response) mutable { onSaveDialogResponse(d, response); });
    d->show();
}

void MessageLogWindow::Impl::onClearRequest()
{
    store_->clear();
    tr_logFreeQueue(myHead);
    myHead = myTail = nullptr;
}

void MessageLogWindow::Impl::onPauseToggled(Gio::SimpleAction& action)
{
    bool value = false;
    action.get_state(value);

    action.set_state(Glib::Variant<bool>::create(!value));

    isPaused_ = !value;
}

namespace
{

void setForegroundColor(Gtk::CellRendererText* renderer, tr_log_level level)
{
    switch (level)
    {
    case TR_LOG_CRITICAL:
    case TR_LOG_ERROR:
    case TR_LOG_WARN:
        renderer->property_foreground() = "red";
        break;

    case TR_LOG_DEBUG:
    case TR_LOG_TRACE:
        renderer->property_foreground() = "forestgreen";
        break;

    default:
        renderer->property_foreground_set() = false;
        break;
    }
}

void renderText(
    Gtk::CellRendererText* renderer,
    Gtk::TreeModel::const_iterator const& iter,
    Gtk::TreeModelColumn<Glib::ustring> const& col)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = iter->get_value(col);
    renderer->property_ellipsize() = TR_PANGO_ELLIPSIZE_MODE(END);
    setForegroundColor(renderer, node->level);
}

void renderTime(Gtk::CellRendererText* renderer, Gtk::TreeModel::const_iterator const& iter)
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    renderer->property_text() = Glib::DateTime::create_now_local(node->when).format("%T");
    setForegroundColor(renderer, node->level);
}

void appendColumn(Gtk::TreeView* view, Gtk::TreeModelColumnBase const& col)
{
    Gtk::TreeViewColumn* c;

    if (col == message_log_cols.name)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Name"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.name); });
        c->set_sizing(TR_GTK_TREE_VIEW_COLUMN_SIZING(FIXED));
        c->set_fixed_width(200);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.message)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Message"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderText(r, iter, message_log_cols.message); });
        c->set_sizing(TR_GTK_TREE_VIEW_COLUMN_SIZING(FIXED));
        c->set_fixed_width(500);
        c->set_resizable(true);
    }
    else if (col == message_log_cols.sequence)
    {
        auto* r = Gtk::make_managed<Gtk::CellRendererText>();
        c = Gtk::make_managed<Gtk::TreeViewColumn>(_("Time"), *r);
        c->set_cell_data_func(*r, [r](auto* /*renderer*/, auto const& iter) { renderTime(r, iter); });
        c->set_resizable(true);
    }
    else
    {
        g_assert_not_reached();
    }

    view->append_column(*c);
}

} // namespace

bool MessageLogWindow::Impl::isRowVisible(Gtk::TreeModel::const_iterator const& iter) const
{
    auto const* const node = iter->get_value(message_log_cols.tr_msg);
    return node != nullptr && node->level <= maxLevel_;
}

MessageLogWindow::Impl::~Impl()
{
    refresh_tag_.disconnect();
}

namespace
{

tr_log_message* addMessages(Glib::RefPtr<Gtk::ListStore> const& store, tr_log_message* head)
{
    tr_log_message* i;
    static unsigned int sequence = 0;
    auto const default_name = Glib::get_application_name();

    for (i = head; i != nullptr && i->next != nullptr; i = i->next)
    {
        char const* name = !std::empty(i->name) ? i->name.c_str() : default_name.c_str();

        auto row_it = store->prepend();
        auto& row = *row_it;
        row[message_log_cols.tr_msg] = i;
        row[message_log_cols.name] = name;
        row[message_log_cols.message] = i->message;
        row[message_log_cols.sequence] = ++sequence;

        /* if it's an error message, dump it to the terminal too */
        if (i->level == TR_LOG_ERROR)
        {
            auto gstr = fmt::format("{}:{} {}", i->file, i->line, i->message);

            if (!std::empty(i->name))
            {
                gstr += fmt::format(" ({})", i->name.c_str());
            }

            g_warning("%s", gstr.c_str());
        }
    }

    return i; /* tail */
}

} // namespace

bool MessageLogWindow::Impl::onRefresh()
{
    bool const pinned_to_new = is_pinned_to_new();

    if (!isPaused_)
    {
        if (auto* msgs = tr_logGetQueue(); msgs != nullptr)
        {
            /* add the new messages and append them to the end of
             * our persistent list */
            tr_log_message* tail = addMessages(store_, msgs);

            if (myTail != nullptr)
            {
                myTail->next = msgs;
            }
            else
            {
                myHead = msgs;
            }

            myTail = tail;
        }

        if (pinned_to_new)
        {
            scroll_to_bottom();
        }
    }

    return true;
}

/**
***  Public Functions
**/

std::unique_ptr<MessageLogWindow> MessageLogWindow::create(Gtk::Window& parent, Glib::RefPtr<Session> const& core)
{
    auto const builder = Gtk::Builder::create_from_resource(gtr_get_full_resource_path("MessageLogWindow.ui"));
    return std::unique_ptr<MessageLogWindow>(
        gtr_get_widget_derived<MessageLogWindow>(builder, "MessageLogWindow", parent, core));
}

MessageLogWindow::MessageLogWindow(
    BaseObjectType* cast_item,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Gtk::Window& parent,
    Glib::RefPtr<Session> const& core)
    : Gtk::Window(cast_item)
    , impl_(std::make_unique<Impl>(*this, builder, core))
{
    set_transient_for(parent);
}

MessageLogWindow::~MessageLogWindow() = default;

MessageLogWindow::Impl::Impl(
    MessageLogWindow& window,
    Glib::RefPtr<Gtk::Builder> const& builder,
    Glib::RefPtr<Session> const& core)
    : window_(window)
    , core_(core)
    , view_(gtr_get_widget<Gtk::TreeView>(builder, "messages_view"))
    , level_names_{ { { TR_LOG_CRITICAL, _("Critical") },
                      { TR_LOG_ERROR, _("Error") },
                      { TR_LOG_WARN, _("Warning") },
                      { TR_LOG_INFO, _("Information") },
                      { TR_LOG_DEBUG, _("Debug") },
                      { TR_LOG_TRACE, _("Trace") } } }
{
    /**
    ***  toolbar
    **/

    auto const action_group = Gio::SimpleActionGroup::create();

    auto const save_action = Gio::SimpleAction::create("save-message-log");
    save_action->signal_activate().connect([this](auto const& /*value*/) { onSaveRequest(); });
    action_group->add_action(save_action);

    auto const clear_action = Gio::SimpleAction::create("clear-message-log");
    clear_action->signal_activate().connect([this](auto const& /*value*/) { onClearRequest(); });
    action_group->add_action(clear_action);

    auto const pause_action = Gio::SimpleAction::create_bool("pause-message-log");
    pause_action->signal_activate().connect([this, &action = *pause_action.get()](auto const& /*value*/)
                                            { onPauseToggled(action); });
    action_group->add_action(pause_action);

    auto* const level_combo = gtr_get_widget<Gtk::ComboBox>(builder, "level_combo");
    level_combo_init(level_combo);
    level_combo->signal_changed().connect([this, level_combo]() { level_combo_changed_cb(level_combo); });

    window_.insert_action_group("win", action_group);

    /**
    ***  messages
    **/

    store_ = Gtk::ListStore::create(message_log_cols);

    addMessages(store_, myHead);
    onRefresh(); /* much faster to populate *before* it has listeners */

    filter_ = Gtk::TreeModelFilter::create(store_);
    sort_ = Gtk::TreeModelSort::create(filter_);
    sort_->set_sort_column(message_log_cols.sequence, TR_GTK_SORT_TYPE(ASCENDING));
    maxLevel_ = static_cast<tr_log_level>(gtr_pref_int_get(TR_KEY_message_level));
    filter_->set_visible_func(sigc::mem_fun(*this, &Impl::isRowVisible));

    view_->set_model(sort_);
    setup_tree_view_button_event_handling(
        *view_,
        {},
        [this](double view_x, double view_y) { return on_tree_view_button_released(*view_, view_x, view_y); });
    appendColumn(view_, message_log_cols.sequence);
    appendColumn(view_, message_log_cols.name);
    appendColumn(view_, message_log_cols.message);

    refresh_tag_ = Glib::signal_timeout().connect_seconds(
        sigc::mem_fun(*this, &Impl::onRefresh),
        SECONDARY_WINDOW_REFRESH_INTERVAL_SECONDS);

    scroll_to_bottom();
}
