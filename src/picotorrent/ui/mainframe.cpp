#include "mainframe.hpp"

#include <filesystem>
#include <regex>

#include <boost/log/trivial.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <wx/persist.h>
#include <wx/persist/toplevel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/taskbarbutton.h>

#include "../bittorrent/addparams.hpp"
#include "../bittorrent/session.hpp"
#include "../bittorrent/sessionstatistics.hpp"
#include "../bittorrent/torrenthandle.hpp"
#include "../bittorrent/torrentstatistics.hpp"
#include "../bittorrent/torrentstatus.hpp"
#include "../core/configuration.hpp"
#include "../core/database.hpp"
#include "../core/environment.hpp"
#include "../core/utils.hpp"
#include "../ipc/server.hpp"
#include "dialogs/aboutdialog.hpp"
#include "dialogs/addmagnetlinkdialog.hpp"
#include "dialogs/addtorrentdialog.hpp"
#include "dialogs/createtorrentdialog.hpp"
#include "dialogs/preferencesdialog.hpp"
#include "ids.hpp"
#include "models/torrentlistmodel.hpp"
#include "statusbar.hpp"
#include "taskbaricon.hpp"
#include "torrentcontextmenu.hpp"
#include "torrentdetailsview.hpp"
#include "torrentlistview.hpp"
#include "translator.hpp"

#ifdef _WIN32
#include "win32/openfiledialog.hpp"
#else
#include <wx/filedlg.h>
#endif


namespace fs = std::filesystem;
using pt::UI::MainFrame;

const char* WindowTitle = "PicoTorrent";

MainFrame::MainFrame(std::shared_ptr<Core::Environment> env, std::shared_ptr<Core::Database> db, std::shared_ptr<Core::Configuration> cfg)
    : wxFrame(nullptr, wxID_ANY, WindowTitle, wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE, "MainFrame"),
    m_env(env),
    m_db(db),
    m_cfg(cfg),
    m_session(new BitTorrent::Session(this, db, cfg, env)),
    m_splitter(new wxSplitterWindow(this, ptID_MAIN_SPLITTER)),
    m_statusBar(new StatusBar(this)),
    m_taskBarIcon(new TaskBarIcon(this)),
    m_torrentDetails(new TorrentDetailsView(m_splitter, ptID_MAIN_TORRENT_DETAILS, cfg)),
    m_torrentListModel(new Models::TorrentListModel()),
    m_torrentList(new TorrentListView(m_splitter, ptID_MAIN_TORRENT_LIST, m_torrentListModel)),
    m_torrentsCount(0),
    m_menuItemFilters(nullptr),
    m_ipc(std::make_unique<IPC::Server>(this))
{
    m_splitter->SetWindowStyleFlag(
        m_splitter->GetWindowStyleFlag() | wxSP_LIVE_UPDATE);
    m_splitter->SetMinimumPaneSize(10);
    m_splitter->SetSashGravity(0.5);
    m_splitter->SplitHorizontally(
        m_torrentList,
        m_torrentDetails);

    m_torrentListModel->SetBackgroundColorEnabled(
        m_cfg->Get<bool>("use_label_as_list_bgcolor").value());

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_splitter, 1, wxEXPAND, 0);
    sizer->SetSizeHints(this);

    // Keyboard accelerators
    std::vector<wxAcceleratorEntry> entries =
    {
        wxAcceleratorEntry(wxACCEL_CTRL,   int('A'),   ptID_KEY_SELECT_ALL),
        wxAcceleratorEntry(wxACCEL_CTRL,   int('U'),   ptID_KEY_ADD_MAGNET_LINK),
        wxAcceleratorEntry(wxACCEL_CTRL,   int('O'),   ptID_KEY_ADD_TORRENT),
        wxAcceleratorEntry(wxACCEL_NORMAL, WXK_DELETE, ptID_KEY_DELETE),
        wxAcceleratorEntry(wxACCEL_SHIFT,  WXK_DELETE, ptID_KEY_DELETE_FILES),
    };

    this->SetAcceleratorTable(wxAcceleratorTable(static_cast<int>(entries.size()), entries.data()));
#ifdef _WIN32
    this->SetIcon(wxICON(AppIcon));
#endif
    this->SetMenuBar(this->CreateMainMenu());
    this->SetSizerAndFit(sizer);
    this->SetStatusBar(m_statusBar);

    this->CreateLabelMenuItems();
    this->UpdateLabels();

    // Set checked on menu items
    m_menuItemDetailsPanel->SetCheckable(true);
    m_menuItemDetailsPanel->Check(m_cfg->Get<bool>("ui.show_details_panel").value());
    m_menuItemStatusBar->SetCheckable(true);
    m_menuItemStatusBar->Check(m_cfg->Get<bool>("ui.show_status_bar").value());

    if (!m_cfg->Get<bool>("ui.show_details_panel").value()) { m_splitter->Unsplit(); }
    if (!m_cfg->Get<bool>("ui.show_status_bar").value()) { m_statusBar->Hide(); }

    if (!wxPersistenceManager::Get().RegisterAndRestore(this))
    {
        this->SetSize(FromDIP(wxSize(450, 400)));
    }

    // Session events
    this->Bind(ptEVT_SESSION_STATISTICS, [this](pt::BitTorrent::SessionStatisticsEvent& evt)
        {
            bool dhtEnabled = m_cfg->Get<bool>("libtorrent.enable_dht").value();
            m_statusBar->UpdateDhtNodesCount(dhtEnabled ? evt.GetData().dhtNodes : -1);
        });

    this->Bind(ptEVT_TORRENT_ADDED, [this](wxCommandEvent& evt)
        {
            m_torrentsCount++;
            m_statusBar->UpdateTorrentCount(m_torrentsCount);
            m_torrentListModel->AddTorrent(static_cast<BitTorrent::TorrentHandle*>(evt.GetClientData()));
        });

#ifdef _WIN32
    this->Bind(ptEVT_TORRENT_FINISHED, [this](wxCommandEvent& evt)
        {
            auto torrent = static_cast<BitTorrent::TorrentHandle*>(evt.GetClientData());
            m_taskBarIcon->ShowBalloon(
                i18n("torrent_finished"),
                Utils::toStdWString(torrent->Status().name));
        });
#endif

    this->Bind(ptEVT_TORRENT_REMOVED, [this](pt::BitTorrent::InfoHashEvent& evt)
        {
            m_torrentsCount--;
            m_statusBar->UpdateTorrentCount(m_torrentsCount);
            m_torrentListModel->RemoveTorrent(evt.GetData());

            // If this torrent is in our selection, remove it and clear the details view
            auto iter = m_selection.find(evt.GetData());

            if (iter != m_selection.end())
            {
                m_selection.erase(iter);
                m_torrentDetails->Reset();
            }
        });

    this->Bind(ptEVT_TORRENT_STATISTICS, [this](pt::BitTorrent::TorrentStatisticsEvent& evt)
        {
            auto stats = evt.GetData();

            m_statusBar->UpdateTransferRates(
                stats.totalPayloadDownloadRate,
                stats.totalPayloadUploadRate);

#ifdef _WIN32
            if (wxTaskBarButton* tbb = MSWGetTaskBarButton())
            {
                if (stats.isDownloadingAny && stats.totalWanted > 0)
                {
                    float totalProgress = stats.totalWantedDone / static_cast<float>(stats.totalWanted);

                    tbb->SetProgressState(wxTaskBarButtonState::wxTASKBAR_BUTTON_NORMAL);
                    tbb->SetProgressRange(static_cast<int>(100));
                    tbb->SetProgressValue(static_cast<int>(totalProgress * 100));
                }
                else
                {
                    tbb->SetProgressState(wxTaskBarButtonState::wxTASKBAR_BUTTON_NO_PROGRESS);
                }
            }
#endif
        });

    this->Bind(ptEVT_TORRENTS_UPDATED, [this](pt::BitTorrent::TorrentsUpdatedEvent& evt)
        {
            auto torrents = evt.GetData();
            m_torrentListModel->UpdateTorrents(torrents);

            std::map<lt::info_hash_t, pt::BitTorrent::TorrentHandle*> selectedUpdated;

            for (auto torrent : torrents)
            {
                auto infoHash = torrent->InfoHash();

                if (m_selection.find(infoHash) != m_selection.end())
                {
                    selectedUpdated.insert({ infoHash, torrent });
                }
            }

            if (selectedUpdated.size() > 0)
            {
                m_torrentDetails->Refresh(selectedUpdated);
            }

            this->CheckDiskSpace(torrents);
        });

    this->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxCommandEvent&)
        {
            for (auto const& sel : m_selection)
            {
                auto status = sel.second->Status();
                fs::path p = fs::path(status.savePath) / status.name;
                if (fs::exists(p)) { Utils::openAndSelect(p); }
            }
        }, ptID_MAIN_TORRENT_LIST);

    this->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU, &MainFrame::ShowTorrentContextMenu, this, ptID_MAIN_TORRENT_LIST);

    this->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
        [this](wxCommandEvent&)
        {
            wxDataViewItemArray items;
            m_torrentList->GetSelections(items);

            m_selection.clear();

            if (items.IsEmpty())
            {
                m_torrentDetails->Reset();
                return;
            }

            for (wxDataViewItem& item : items)
            {
                auto torrent = m_torrentListModel->GetTorrentFromItem(item);
                m_selection.insert({ torrent->InfoHash(), torrent });
            }

            m_torrentDetails->Refresh(m_selection);
        }, ptID_MAIN_TORRENT_LIST);

    // connect events
    this->Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this, wxID_ANY);
    this->Bind(wxEVT_ICONIZE, &MainFrame::OnIconize, this, wxID_ANY);
    this->Bind(wxEVT_MENU, &MainFrame::OnFileAddTorrent, this, ptID_EVT_ADD_TORRENT);
    this->Bind(wxEVT_MENU, &MainFrame::OnFileAddMagnetLink, this, ptID_EVT_ADD_MAGNET_LINK);
    this->Bind(wxEVT_MENU, &MainFrame::OnFileCreateTorrent, this, ptID_EVT_CREATE_TORRENT);
    this->Bind(wxEVT_MENU, [this](wxCommandEvent&) { this->Close(true); }, ptID_EVT_EXIT);
    this->Bind(wxEVT_MENU, &MainFrame::OnViewPreferences, this, ptID_EVT_VIEW_PREFERENCES);
    this->Bind(wxEVT_MENU, &MainFrame::OnHelpAbout, this, ptID_EVT_ABOUT);

    // Keyboard shortcuts
    this->Bind(wxEVT_MENU, &MainFrame::OnFileAddTorrent, this, ptID_KEY_ADD_TORRENT);
    this->Bind(wxEVT_MENU, &MainFrame::OnFileAddMagnetLink, this, ptID_KEY_ADD_MAGNET_LINK);

    this->Bind(
        wxEVT_MENU,
        [&](wxCommandEvent&)
        {
            wxDataViewItemArray items;
            m_torrentList->GetSelections(items);

            if (items.IsEmpty()) { return; }

            for (wxDataViewItem& item : items)
            {
                m_torrentListModel->GetTorrentFromItem(item)->Remove();
            }
        },
        ptID_KEY_DELETE);

    this->Bind(
        wxEVT_MENU,
        [&](wxCommandEvent&)
        {
            wxDataViewItemArray items;
            m_torrentList->GetSelections(items);

            if (items.IsEmpty()) { return; }

            if (wxMessageBox(
                i18n("confirm_remove_description"),
                i18n("confirm_remove"),
                wxOK | wxCANCEL | wxICON_INFORMATION,
                m_parent) != wxOK) {
                return;
            }

            for (wxDataViewItem& item : items)
            {
                m_torrentListModel->GetTorrentFromItem(item)->RemoveFiles();
            }
        },
        ptID_KEY_DELETE_FILES);

    this->Bind(
        wxEVT_MENU,
        [&](wxCommandEvent&)
        {
            m_torrentList->SelectAll();
            wxPostEvent(
                this,
                wxCommandEvent(wxEVT_DATAVIEW_SELECTION_CHANGED, ptID_MAIN_TORRENT_LIST));
        },
        ptID_KEY_SELECT_ALL);

    m_taskBarIcon->Bind(wxEVT_MENU, &MainFrame::OnFileAddTorrent, this, ptID_EVT_ADD_TORRENT);
    m_taskBarIcon->Bind(wxEVT_MENU, &MainFrame::OnFileAddMagnetLink, this, ptID_EVT_ADD_MAGNET_LINK);
    m_taskBarIcon->Bind(wxEVT_MENU, [this](wxCommandEvent&) { this->Close(true); }, ptID_EVT_EXIT);
    m_taskBarIcon->Bind(wxEVT_MENU, &MainFrame::OnViewPreferences, this, ptID_EVT_VIEW_PREFERENCES);
    m_taskBarIcon->Bind(wxEVT_TASKBAR_LEFT_DOWN, &MainFrame::OnTaskBarLeftDown, this, wxID_ANY);

    this->Bind(
        wxEVT_MENU,
        [this](wxCommandEvent&)
        {
            m_cfg->Set("ui.show_details_panel", m_menuItemDetailsPanel->IsChecked());

            if (m_menuItemDetailsPanel->IsChecked())
            {
                m_splitter->SplitHorizontally(
                    m_torrentList,
                    m_torrentDetails);
            }
            else
            {
                m_splitter->Unsplit();
            }
        }, ptID_EVT_SHOW_DETAILS);

    this->Bind(
        wxEVT_MENU,
        [this](wxCommandEvent&)
        {
            m_cfg->Set("ui.show_status_bar", m_menuItemStatusBar->IsChecked());

            if (m_menuItemStatusBar->IsChecked()) { m_statusBar->Show(); }
            else { m_statusBar->Hide(); }
            this->SendSizeEvent();
        }, ptID_EVT_SHOW_STATUS_BAR);

    this->Bind(
        ptEVT_TORRENT_METADATA_FOUND,
        [this](pt::BitTorrent::MetadataFoundEvent& evt)
        {
            for (auto dlg : m_addDialogs)
            {
                wxPostEvent(dlg, evt);
            }
        });

    // Update status bar
    m_statusBar->UpdateDhtNodesCount(m_cfg->Get<bool>("libtorrent.enable_dht").value() ? 0 : -1);
    m_statusBar->UpdateTorrentCount(m_torrentsCount);

    // Show taskbar icon
    if (m_cfg->Get<bool>("show_in_notification_area").value())
    {
        m_taskBarIcon->Show();
    }
}

MainFrame::~MainFrame()
{
    m_taskBarIcon->Hide();
    delete m_taskBarIcon;
}

void MainFrame::AddFilter(wxString const& name, std::function<bool(BitTorrent::TorrentHandle*)> const& filter)
{
    if (m_menuItemFilters == nullptr)
    {
        m_filtersMenu = new wxMenu();
        m_filtersMenu->AppendRadioItem(ptID_EVT_FILTERS_NONE, i18n("amp_none"));
        m_filtersMenu->Bind(
            wxEVT_MENU,
            [this](wxCommandEvent& evt)
            {
                auto filter = m_filters.find(evt.GetId());
                if (filter == m_filters.end())
                {
                    m_torrentListModel->ClearFilter();
                }
                else
                {
                    m_torrentListModel->SetFilter(filter->second);
                }
            });

        m_viewMenu->InsertSeparator(0);
        m_menuItemFilters = m_viewMenu->Insert(0, wxID_ANY, i18n("amp_filter"), m_filtersMenu);
    }

    m_filters.insert({ ptID_EVT_FILTERS_USER + m_filtersMenu->GetMenuItemCount(), filter });

    m_filtersMenu->AppendRadioItem(
        ptID_EVT_FILTERS_USER + m_filtersMenu->GetMenuItemCount(),
        name);
}

void MainFrame::AddTorrents(std::vector<lt::add_torrent_params>& params)
{
    if (params.empty())
    {
        return;
    }

    std::vector<lt::info_hash_t> hashes;

    // Set up default values for all params
    // Also match labels

    auto labels = m_cfg->GetLabels();

    for (lt::add_torrent_params& p : params)
    {
        auto our = new BitTorrent::AddParams();

        p.flags |= lt::torrent_flags::duplicate_is_error;
        p.save_path = m_cfg->Get<std::string>("default_save_path").value();
        p.userdata = lt::client_data_t(our);

        // If we have a param with an info hash and no torrent info,
        // let the session find metadata for us

        if (
            ((p.info_hashes.has_v1() && !p.info_hashes.v1.is_all_zeros())
                || (p.info_hashes.has_v2() && !p.info_hashes.v2.is_all_zeros()))
            && !p.ti)
        {
            hashes.push_back(p.info_hashes);
        }

        // match any label that has an apply filter
        for (auto const& label : labels)
        {
            if (!label.applyFilterEnabled
                || label.applyFilter.empty())
            {
                continue;
            }

            std::string name;
            if (auto ti = p.ti) { name = ti->name(); }
            if (p.name.size() > 0) { name = p.name; }

            if (name.empty())
            {
                continue;
            }

            std::regex re(label.applyFilter, std::regex_constants::ECMAScript | std::regex_constants::icase);
            
            if (std::regex_search(name, re))
            {
                // we have a match
                our->labelId = label.id;

                if (label.savePath.size() > 0
                    && label.savePathEnabled)
                {
                    p.save_path = label.savePath;
                }

                break;
            }
        }
    }

    if (m_cfg->Get<bool>("skip_add_torrent_dialog").value())
    {
        for (lt::add_torrent_params& p : params)
        {
            m_session->AddTorrent(p);
        }

        return;
    }

    for (auto& param : params)
    {
        auto dlg = new Dialogs::AddTorrentDialog(this, wxID_ANY, param, m_db, m_cfg, m_session);
        dlg->Bind(
            wxEVT_CLOSE_WINDOW,
            [this, dlg](wxCloseEvent& evt)
            {
                evt.Skip();
                m_addDialogs.erase(dlg);
            });
        dlg->Show();

        m_addDialogs.insert(dlg);
    }

    m_session->AddMetadataSearch(hashes);
}

void MainFrame::HandleParams(std::vector<std::string> const& files, std::vector<std::string> const& magnets)
{
    std::vector<lt::add_torrent_params> params;

    if (!files.empty())
    {
        ParseTorrentFiles(params, files);
    }

    if (!magnets.empty())
    {
        for (std::string const& magnet : magnets)
        {
            lt::error_code ec;
            lt::add_torrent_params tp = lt::parse_magnet_uri(magnet, ec);

            if (ec)
            {
                BOOST_LOG_TRIVIAL(warning) << "Failed to parse magnet uri: " << magnet << ", error: " << ec;
                continue;
            }

            params.push_back(tp);
        }
    }

    AddTorrents(params);
}

void MainFrame::CheckDiskSpace(std::vector<pt::BitTorrent::TorrentHandle*> const& torrents)
{
    bool shouldCheck = m_cfg->Get<bool>("pause_on_low_disk_space").value();
    int limit = m_cfg->Get<int>("pause_on_low_disk_space_limit").value();

    if (!shouldCheck)
    {
        return;
    }
#ifdef _WIN32
    for (auto torrent : torrents)
    {
        auto status = torrent->Status();

        ULARGE_INTEGER freeBytesAvailableToCaller;
        ULARGE_INTEGER totalNumberOfBytes;
        ULARGE_INTEGER totalNumberOfFreeBytes;

        BOOL res = GetDiskFreeSpaceEx(
            Utils::toStdWString(status.savePath).c_str(),
            &freeBytesAvailableToCaller,
            &totalNumberOfBytes,
            &totalNumberOfFreeBytes);

        if (res)
        {
            float diskSpaceAvailable = static_cast<float>(freeBytesAvailableToCaller.QuadPart) / static_cast<float>(totalNumberOfBytes.QuadPart);
            float diskSpaceLimit = limit / 100.0f;

            if (diskSpaceAvailable < diskSpaceLimit)
            {
                BOOST_LOG_TRIVIAL(info) << "Pausing torrent "
                    << status.infoHash << " due to disk space too low (avail: "
                    << diskSpaceAvailable << ", limit: "
                    << diskSpaceLimit << ")";

                torrent->Pause();

                m_taskBarIcon->ShowBalloon(
                    i18n("pause_on_low_disk_space_alert"),
                    status.name);
            }
        }
    }
#endif
}

void MainFrame::CreateLabelMenuItems()
{
    for (int i = static_cast<int>(m_labelsMenu->GetMenuItemCount()) - 1; i >= 0; i--)
    {
        wxMenuItem* item = m_labelsMenu->FindItemByPosition(i);
        if (item->GetId() <= ptID_EVT_LABELS_USER) { continue; }
        m_labelsMenu->Delete(item);
    }

    for (auto const& label : m_cfg->GetLabels())
    {
        m_labelsMenu->AppendRadioItem(ptID_EVT_LABELS_USER + label.id, Utils::toStdWString(label.name));
    }
}

wxMenuBar* MainFrame::CreateMainMenu()
{
    auto fileMenu = new wxMenu();
    fileMenu->Append(ptID_EVT_ADD_TORRENT, i18n("amp_add_torrent"));
    fileMenu->Append(ptID_EVT_ADD_MAGNET_LINK, i18n("amp_add_magnet_link_s"));
    fileMenu->AppendSeparator();
    fileMenu->Append(ptID_EVT_CREATE_TORRENT, i18n("amp_create_torrent"));
    fileMenu->AppendSeparator();
    fileMenu->Append(ptID_EVT_EXIT, i18n("amp_exit"));

    m_viewMenu = new wxMenu();
    m_labelsMenu = new wxMenu();
    m_labelsMenu->AppendRadioItem(ptID_EVT_LABELS_NONE, i18n("none"));
    m_labelsMenu->Bind(
        wxEVT_MENU,
        [this](wxCommandEvent& evt)
        {
            if (evt.GetId() > ptID_EVT_LABELS_USER)
            {
                int labelId = evt.GetId() - ptID_EVT_LABELS_USER;
                m_torrentListModel->SetLabelFilter(labelId);
            }
            else if (evt.GetId() == ptID_EVT_LABELS_NONE)
            {
                m_torrentListModel->ClearLabelFilter();
            }
        });

    m_menuItemLabels = m_viewMenu->AppendSubMenu(m_labelsMenu, i18n("labels"));
    m_viewMenu->AppendSeparator();

    m_menuItemDetailsPanel = m_viewMenu->Append(ptID_EVT_SHOW_DETAILS, i18n("amp_details_panel"));
    m_menuItemStatusBar = m_viewMenu->Append(ptID_EVT_SHOW_STATUS_BAR, i18n("amp_status_bar"));
    m_viewMenu->AppendSeparator();
    m_viewMenu->Append(ptID_EVT_VIEW_PREFERENCES, i18n("amp_preferences"));

    auto helpMenu = new wxMenu();
    helpMenu->Append(ptID_EVT_ABOUT, i18n("amp_about"));

    auto mainMenu = new wxMenuBar();
    mainMenu->Append(fileMenu, i18n("amp_file"));
    mainMenu->Append(m_viewMenu, i18n("amp_view"));
    mainMenu->Append(helpMenu, i18n("amp_help"));

    return mainMenu;
}

void MainFrame::OnClose(wxCloseEvent& evt)
{
    if (evt.CanVeto()
        && m_cfg->Get<bool>("show_in_notification_area").value()
        && m_cfg->Get<bool>("close_to_notification_area").value())
    {
        Hide();
#ifdef _WIN32
        MSWGetTaskBarButton()->Hide();
#endif
    }
    else
    {
        // We hide early while closing to not occupy the
        // screen more than necessary. Otherwise the window
        // would be visible (and unresponsive) for a few seconds
        // before being destroyed.
        Hide();

        evt.Skip();
    }
}

void MainFrame::OnFileAddMagnetLink(wxCommandEvent&)
{
    Dialogs::AddMagnetLinkDialog dlg(this, wxID_ANY);

    if (dlg.ShowModal() == wxID_OK)
    {
        auto params = dlg.GetParams();
        this->AddTorrents(params);
    }
}

void MainFrame::OnFileAddTorrent(wxCommandEvent&)
{   
    #ifdef _WIN32
    Win32::OpenFileDialog ofd;

    ofd.SetFileTypes({
        std::make_tuple(L"Torrent files", L"*.torrent"),
        std::make_tuple(L"All files (*.*)", L"*.*")
    });

    ofd.SetOption(Win32::OpenFileDialog::Option::Multi);
    ofd.SetTitle(i18n("add_torrent_s"));
    ofd.Show(this);

    std::vector<std::string> files;
    ofd.GetFiles(files);

    if (files.empty())
    {
        return;
    }

    std::vector<lt::add_torrent_params> params;
    this->ParseTorrentFiles(params, files);
    this->AddTorrents(params);
    #else
    wxFileDialog* OpenDialog = new wxFileDialog(this, _(i18n("add_torrent_s")), wxEmptyString, wxEmptyString, _("Torrent files|*.torrent|All files (*.*)|*.*"), wxFD_OPEN|wxFD_MULTIPLE, wxDefaultPosition);
    if (OpenDialog->ShowModal() == wxID_OK){
		wxArrayString wxfiles;
		OpenDialog -> GetPaths(wxfiles);
		std::vector<std::string> files;
        for (auto const& f : wxfiles){
            files.push_back(f.ToStdString());
        }
		std::vector<lt::add_torrent_params> params;
        this->ParseTorrentFiles(params, files);
        this->AddTorrents(params);
	}
	OpenDialog->Destroy();
    #endif
}

void MainFrame::OnFileCreateTorrent(wxCommandEvent&)
{
    auto dlg = new Dialogs::CreateTorrentDialog(this, wxID_ANY, m_session);
    dlg->Show();
    dlg->Bind(wxEVT_CLOSE_WINDOW,
        [dlg](wxCloseEvent&)
        {
            dlg->Destroy();
        });
}

void MainFrame::OnHelpAbout(wxCommandEvent&)
{
    Dialogs::AboutDialog dlg(this, wxID_ANY);
    dlg.ShowModal();
}

void MainFrame::OnIconize(wxIconizeEvent& ev)
{
    if (ev.IsIconized()
        && m_cfg->Get<bool>("show_in_notification_area").value()
        && m_cfg->Get<bool>("minimize_to_notification_area").value())
    {
#ifdef _WIN32
        MSWGetTaskBarButton()->Hide();
#endif
    }
}

void MainFrame::OnTaskBarLeftDown(wxTaskBarIconEvent&)
{
#ifdef _WIN32
    this->MSWGetTaskBarButton()->Show();
#endif
    this->Restore();
    this->Raise();
    this->Show();
    this->SendSizeEvent();
}

void MainFrame::OnViewPreferences(wxCommandEvent&)
{
    Dialogs::PreferencesDialog dlg(this, m_cfg);
    
    if (dlg.ShowModal() == wxID_OK)
    {
        if (dlg.WantsRestart())
        {
            /*TCHAR path[MAX_PATH];
            GetModuleFileName(NULL, path, ARRAYSIZE(path));

            std::wstringstream proc;
            proc << path << L" --wait-for-pid=" << std::to_wstring(GetCurrentProcessId());

            wxExecute(proc.str(), wxEXEC_ASYNC);
            Close(true);*/

            return;
        }

        // Reload settings
        m_session->ReloadSettings();

        if (m_cfg->Get<bool>("show_in_notification_area").value() && !m_taskBarIcon->IsIconInstalled())
        {
            m_taskBarIcon->Show();
        }
        else if (!m_cfg->Get<bool>("show_in_notification_area").value() && m_taskBarIcon->IsIconInstalled())
        {
            m_taskBarIcon->Hide();
        }

        m_torrentDetails->ReloadConfiguration();
        m_torrentListModel->SetBackgroundColorEnabled(
            m_cfg->Get<bool>("use_label_as_list_bgcolor").value());

        this->CreateLabelMenuItems();
        this->UpdateLabels();
    }
}

void MainFrame::ParseTorrentFiles(std::vector<lt::add_torrent_params>& params, std::vector<std::string> const& paths)
{
    for (std::string const& path : paths)
    {
        lt::error_code ec;
        lt::add_torrent_params param;

        param.ti = std::make_shared<lt::torrent_info>(path, ec);

        if (ec)
        {
            BOOST_LOG_TRIVIAL(error) << "Failed to parse torrent file: " << ec;
            continue;
        }

        params.push_back(param);
    }
}

void MainFrame::ShowTorrentContextMenu(wxCommandEvent&)
{
    wxDataViewItemArray items;
    m_torrentList->GetSelections(items);

    if (items.IsEmpty())
    {
        return;
    }

    std::vector<BitTorrent::TorrentHandle*> selectedTorrents;

    for (wxDataViewItem& item : items)
    {
        selectedTorrents.push_back(
            m_torrentListModel->GetTorrentFromItem(item));
    }

    TorrentContextMenu menu(this, m_cfg, selectedTorrents);
    PopupMenu(&menu);
}

void MainFrame::UpdateLabels()
{
    std::map<int, std::tuple<std::string, std::string>> labels;
    for (auto const& label : m_cfg->GetLabels())
    {
        labels.insert(
            std::pair<int, std::tuple<std::string, std::string>>(label.id, { label.name, label.color }));
    }
    m_torrentListModel->UpdateLabels(labels);
}
