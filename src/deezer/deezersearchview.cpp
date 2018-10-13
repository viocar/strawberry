/*
 * Strawberry Music Player
 * This code was part of Clementine (GlobalSearch)
 * Copyright 2012, David Sansome <me@davidsansome.com>
 * Copyright 2018, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <functional>

#include <QtGlobal>
#include <QWidget>
#include <QTimer>
#include <QList>
#include <QString>
#include <QStringList>
#include <QPixmap>
#include <QPalette>
#include <QColor>
#include <QFont>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QStandardItem>
#include <QSettings>
#include <QAction>
#include <QtEvents>

#include "core/application.h"
#include "core/logging.h"
#include "core/mimedata.h"
#include "core/timeconstants.h"
#include "core/iconloader.h"
#include "internet/internetsongmimedata.h"
#include "collection/collectionfilterwidget.h"
#include "collection/collectionmodel.h"
#include "collection/groupbydialog.h"
#include "playlist/songmimedata.h"
#include "deezersearch.h"
#include "deezersearchitemdelegate.h"
#include "deezersearchmodel.h"
#include "deezersearchsortmodel.h"
#include "deezersearchview.h"
#include "ui_deezersearchview.h"
#include "settings/deezersettingspage.h"

using std::placeholders::_1;
using std::placeholders::_2;
using std::swap;

const int DeezerSearchView::kSwapModelsTimeoutMsec = 250;

DeezerSearchView::DeezerSearchView(Application *app, QWidget *parent)
    : QWidget(parent),
      app_(app),
      engine_(app_->deezer_search()),
      ui_(new Ui_DeezerSearchView),
      context_menu_(nullptr),
      last_search_id_(0),
      front_model_(new DeezerSearchModel(engine_, this)),
      back_model_(new DeezerSearchModel(engine_, this)),
      current_model_(front_model_),
      front_proxy_(new DeezerSearchSortModel(this)),
      back_proxy_(new DeezerSearchSortModel(this)),
      current_proxy_(front_proxy_),
      swap_models_timer_(new QTimer(this)),
      search_icon_(IconLoader::Load("search")),
      warning_icon_(IconLoader::Load("dialog-warning")),
      error_(false) {

  ui_->setupUi(this);
  ui_->progressbar->hide();
  ui_->progressbar->reset();

  front_model_->set_proxy(front_proxy_);
  back_model_->set_proxy(back_proxy_);

  ui_->search->installEventFilter(this);
  ui_->results_stack->installEventFilter(this);

  ui_->settings->setIcon(IconLoader::Load("configure"));

  // Must be a queued connection to ensure the DeezerSearch handles it first.
  connect(app_, SIGNAL(SettingsChanged()), SLOT(ReloadSettings()), Qt::QueuedConnection);

  connect(ui_->search, SIGNAL(textChanged(QString)), SLOT(TextEdited(QString)));
  connect(ui_->results, SIGNAL(AddToPlaylistSignal(QMimeData*)), SIGNAL(AddToPlaylist(QMimeData*)));
  connect(ui_->results, SIGNAL(FocusOnFilterSignal(QKeyEvent*)), SLOT(FocusOnFilter(QKeyEvent*)));

  // Set the appearance of the results list
  ui_->results->setItemDelegate(new DeezerSearchItemDelegate(this));
  ui_->results->setAttribute(Qt::WA_MacShowFocusRect, false);
  ui_->results->setStyleSheet("QTreeView::item{padding-top:1px;}");

  // Show the help page initially
  ui_->results_stack->setCurrentWidget(ui_->help_page);
  ui_->help_frame->setBackgroundRole(QPalette::Base);

  // Set the colour of the help text to the disabled window text colour
  QPalette help_palette = ui_->label_helptext->palette();
  const QColor help_color = help_palette.color(QPalette::Disabled, QPalette::WindowText);
  help_palette.setColor(QPalette::Normal, QPalette::WindowText, help_color);
  help_palette.setColor(QPalette::Inactive, QPalette::WindowText, help_color);
  ui_->label_helptext->setPalette(help_palette);

  // Make it bold
  QFont help_font = ui_->label_helptext->font();
  help_font.setBold(true);
  ui_->label_helptext->setFont(help_font);

  // Set up the sorting proxy model
  front_proxy_->setSourceModel(front_model_);
  front_proxy_->setDynamicSortFilter(true);
  front_proxy_->sort(0);

  back_proxy_->setSourceModel(back_model_);
  back_proxy_->setDynamicSortFilter(true);
  back_proxy_->sort(0);

  swap_models_timer_->setSingleShot(true);
  swap_models_timer_->setInterval(kSwapModelsTimeoutMsec);
  connect(swap_models_timer_, SIGNAL(timeout()), SLOT(SwapModels()));

  // Add actions to the settings menu
  group_by_actions_ = CollectionFilterWidget::CreateGroupByActions(this);
  QMenu *settings_menu = new QMenu(this);
  settings_menu->addActions(group_by_actions_->actions());
  settings_menu->addSeparator();
  settings_menu->addAction(IconLoader::Load("configure"), tr("Configure Deezer..."), this, SLOT(OpenSettingsDialog()));
  ui_->settings->setMenu(settings_menu);

  connect(ui_->radiobutton_searchbyalbums, SIGNAL(clicked(bool)), SLOT(SearchByAlbumsClicked(bool)));
  connect(ui_->radiobutton_searchbysongs, SIGNAL(clicked(bool)), SLOT(SearchBySongsClicked(bool)));

  connect(group_by_actions_, SIGNAL(triggered(QAction*)), SLOT(GroupByClicked(QAction*)));

  // These have to be queued connections because they may get emitted before our call to Search() (or whatever) returns and we add the ID to the map.

  connect(engine_, SIGNAL(UpdateStatus(QString)), SLOT(UpdateStatus(QString)));
  connect(engine_, SIGNAL(ProgressSetMaximum(int)), SLOT(ProgressSetMaximum(int)), Qt::QueuedConnection);
  connect(engine_, SIGNAL(UpdateProgress(int)), SLOT(UpdateProgress(int)), Qt::QueuedConnection);

  connect(engine_, SIGNAL(AddResults(int, DeezerSearch::ResultList)), SLOT(AddResults(int, DeezerSearch::ResultList)), Qt::QueuedConnection);
  connect(engine_, SIGNAL(SearchError(int, QString)), SLOT(SearchError(int, QString)), Qt::QueuedConnection);
  connect(engine_, SIGNAL(ArtLoaded(int, QPixmap)), SLOT(ArtLoaded(int, QPixmap)), Qt::QueuedConnection);

  ReloadSettings();

}

DeezerSearchView::~DeezerSearchView() { delete ui_; }

void DeezerSearchView::ReloadSettings() {

  QSettings s;

  // Collection settings

  s.beginGroup(DeezerSettingsPage::kSettingsGroup);
  const bool pretty = s.value("pretty_covers", true).toBool();
  front_model_->set_use_pretty_covers(pretty);
  back_model_->set_use_pretty_covers(pretty);
  s.endGroup();

  // Deezer search settings

  s.beginGroup(DeezerSettingsPage::kSettingsGroup);
  searchby_ = DeezerSettingsPage::SearchBy(s.value("searchby", int(DeezerSettingsPage::SearchBy_Songs)).toInt());
  switch (searchby_) {
    case DeezerSettingsPage::SearchBy_Songs:
      ui_->radiobutton_searchbysongs->setChecked(true);
      break;
    case DeezerSettingsPage::SearchBy_Albums:
      ui_->radiobutton_searchbyalbums->setChecked(true);
      break;
  }

  SetGroupBy(CollectionModel::Grouping(
      CollectionModel::GroupBy(s.value("group_by1", int(CollectionModel::GroupBy_Artist)).toInt()),
      CollectionModel::GroupBy(s.value("group_by2", int(CollectionModel::GroupBy_Album)).toInt()),
      CollectionModel::GroupBy(s.value("group_by3", int(CollectionModel::GroupBy_None)).toInt())));
  s.endGroup();

}

void DeezerSearchView::StartSearch(const QString &query) {

  ui_->search->setText(query);
  TextEdited(query);

  // Swap models immediately
  swap_models_timer_->stop();
  SwapModels();

}

void DeezerSearchView::TextEdited(const QString &text) {

  const QString trimmed(text.trimmed());

  error_ = false;

  // Add results to the back model, switch models after some delay.
  back_model_->Clear();
  current_model_ = back_model_;
  current_proxy_ = back_proxy_;
  swap_models_timer_->start();

  // Cancel the last search (if any) and start the new one.
  engine_->CancelSearch(last_search_id_);
  // If text query is empty, don't start a new search
  if (trimmed.isEmpty()) {
    last_search_id_ = -1;
    ui_->label_helptext->setText("Enter search terms above to find music");
    ui_->label_status->clear();
    ui_->progressbar->hide();
    ui_->progressbar->reset();
  }
  else {
    ui_->progressbar->reset();
    last_search_id_ = engine_->SearchAsync(trimmed, searchby_);
  }

}

void DeezerSearchView::AddResults(int id, const DeezerSearch::ResultList &results) {
  if (id != last_search_id_) return;
  if (results.isEmpty()) return;
  ui_->label_status->clear();
  ui_->progressbar->reset();
  ui_->progressbar->hide();
  current_model_->AddResults(results);
}

void DeezerSearchView::SearchError(const int id, const QString error) {
  error_ = true;
  ui_->label_helptext->setText(error);
  ui_->label_status->clear();
  ui_->progressbar->reset();
  ui_->progressbar->hide();
  ui_->results_stack->setCurrentWidget(ui_->help_page);
}

void DeezerSearchView::SwapModels() {

  art_requests_.clear();

  std::swap(front_model_, back_model_);
  std::swap(front_proxy_, back_proxy_);

  ui_->results->setModel(front_proxy_);

  if (ui_->search->text().trimmed().isEmpty() || error_) {
    ui_->results_stack->setCurrentWidget(ui_->help_page);
  }
  else {
    ui_->results_stack->setCurrentWidget(ui_->results_page);
  }

}

void DeezerSearchView::LazyLoadArt(const QModelIndex &proxy_index) {

  if (!proxy_index.isValid() || proxy_index.model() != front_proxy_) {
    return;
  }

  // Already loading art for this item?
  if (proxy_index.data(DeezerSearchModel::Role_LazyLoadingArt).isValid()) {
    return;
  }

  // Should we even load art at all?
  if (!app_->collection_model()->use_pretty_covers()) {
    return;
  }

  // Is this an album?
  const CollectionModel::GroupBy container_type = CollectionModel::GroupBy(proxy_index.data(CollectionModel::Role_ContainerType).toInt());
  if (container_type != CollectionModel::GroupBy_Album &&
      container_type != CollectionModel::GroupBy_AlbumArtist &&
      container_type != CollectionModel::GroupBy_YearAlbum &&
      container_type != CollectionModel::GroupBy_OriginalYearAlbum) {
    return;
  }

  // Mark the item as loading art
  const QModelIndex source_index = front_proxy_->mapToSource(proxy_index);
  QStandardItem *item = front_model_->itemFromIndex(source_index);
  item->setData(true, DeezerSearchModel::Role_LazyLoadingArt);

  // Walk down the item's children until we find a track
  while (item->rowCount()) {
    item = item->child(0);
  }

  // Get the track's Result
  const DeezerSearch::Result result = item->data(DeezerSearchModel::Role_Result).value<DeezerSearch::Result>();

  // Load the art.
  int id = engine_->LoadArtAsync(result);
  art_requests_[id] = source_index;

}

void DeezerSearchView::ArtLoaded(int id, const QPixmap &pixmap) {

  if (!art_requests_.contains(id)) return;
  QModelIndex index = art_requests_.take(id);

  if (!pixmap.isNull()) {
    front_model_->itemFromIndex(index)->setData(pixmap, Qt::DecorationRole);
  }

}

MimeData *DeezerSearchView::SelectedMimeData() {

  if (!ui_->results->selectionModel()) return nullptr;

  // Get all selected model indexes
  QModelIndexList indexes = ui_->results->selectionModel()->selectedRows();
  if (indexes.isEmpty()) {
    // There's nothing selected - take the first thing in the model that isn't a divider.
    for (int i = 0; i < front_proxy_->rowCount(); ++i) {
      QModelIndex index = front_proxy_->index(i, 0);
      if (!index.data(CollectionModel::Role_IsDivider).toBool()) {
        indexes << index;
        ui_->results->setCurrentIndex(index);
        break;
      }
    }
  }

  // Still got nothing?  Give up.
  if (indexes.isEmpty()) {
    return nullptr;
  }

  // Get items for these indexes
  QList<QStandardItem*> items;
  for (const QModelIndex &index : indexes) {
    items << (front_model_->itemFromIndex(front_proxy_->mapToSource(index)));
  }

  // Get a MimeData for these items
  return engine_->LoadTracks(front_model_->GetChildResults(items));

}

bool DeezerSearchView::eventFilter(QObject *object, QEvent *event) {

  if (object == ui_->search && event->type() == QEvent::KeyRelease) {
    if (SearchKeyEvent(static_cast<QKeyEvent*>(event))) {
      return true;
    }
  }
  else if (object == ui_->results_stack && event->type() == QEvent::ContextMenu) {
    if (ResultsContextMenuEvent(static_cast<QContextMenuEvent*>(event))) {
      return true;
    }
  }

  return QWidget::eventFilter(object, event);

}

bool DeezerSearchView::SearchKeyEvent(QKeyEvent *event) {

  switch (event->key()) {
    case Qt::Key_Up:
      ui_->results->UpAndFocus();
      break;

    case Qt::Key_Down:
      ui_->results->DownAndFocus();
      break;

    case Qt::Key_Escape:
      ui_->search->clear();
      break;

    case Qt::Key_Return:
      TextEdited(ui_->search->text());
      break;

    default:
      return false;
  }

  event->accept();
  return true;

}

bool DeezerSearchView::ResultsContextMenuEvent(QContextMenuEvent *event) {

  context_menu_ = new QMenu(this);
  context_actions_ << context_menu_->addAction( IconLoader::Load("media-play"), tr("Append to current playlist"), this, SLOT(AddSelectedToPlaylist()));
  context_actions_ << context_menu_->addAction( IconLoader::Load("media-play"), tr("Replace current playlist"), this, SLOT(LoadSelected()));
  context_actions_ << context_menu_->addAction( IconLoader::Load("document-new"), tr("Open in new playlist"), this, SLOT(OpenSelectedInNewPlaylist()));

  context_menu_->addSeparator();
  context_actions_ << context_menu_->addAction(IconLoader::Load("go-next"), tr("Queue track"), this, SLOT(AddSelectedToPlaylistEnqueue()));

  context_menu_->addSeparator();

  if (ui_->results->selectionModel() && ui_->results->selectionModel()->selectedRows().length() == 1) {
    context_actions_ << context_menu_->addAction(IconLoader::Load("search"), tr("Search for this"), this, SLOT(SearchForThis()));
  }

  context_menu_->addSeparator();
  context_menu_->addMenu(tr("Group by"))->addActions(group_by_actions_->actions());
  context_menu_->addAction(IconLoader::Load("configure"), tr("Configure Deezer..."), this, SLOT(OpenSettingsDialog()));

  const bool enable_context_actions = ui_->results->selectionModel() && ui_->results->selectionModel()->hasSelection();

  for (QAction *action : context_actions_) {
    action->setEnabled(enable_context_actions);
  }

  context_menu_->popup(event->globalPos());

  return true;

}

void DeezerSearchView::AddSelectedToPlaylist() {
  emit AddToPlaylist(SelectedMimeData());
}

void DeezerSearchView::LoadSelected() {
  MimeData *data = SelectedMimeData();
  if (!data) return;

  data->clear_first_ = true;
  emit AddToPlaylist(data);
}

void DeezerSearchView::AddSelectedToPlaylistEnqueue() {
  MimeData *data = SelectedMimeData();
  if (!data) return;

  data->enqueue_now_ = true;
  emit AddToPlaylist(data);
}

void DeezerSearchView::OpenSelectedInNewPlaylist() {
  MimeData *data = SelectedMimeData();
  if (!data) return;

  data->open_in_new_playlist_ = true;
  emit AddToPlaylist(data);
}

void DeezerSearchView::SearchForThis() {
  StartSearch(ui_->results->selectionModel()->selectedRows().first().data().toString());
}

void DeezerSearchView::showEvent(QShowEvent *e) {
  QWidget::showEvent(e);
  FocusSearchField();
}

void DeezerSearchView::FocusSearchField() {
  ui_->search->setFocus();
  ui_->search->selectAll();
}

void DeezerSearchView::hideEvent(QHideEvent *e) {
  QWidget::hideEvent(e);
}

void DeezerSearchView::FocusOnFilter(QKeyEvent *event) {
  ui_->search->setFocus();
  QApplication::sendEvent(ui_->search, event);
}

void DeezerSearchView::OpenSettingsDialog() {
  app_->OpenSettingsDialogAtPage(SettingsDialog::Page_Deezer);
}

void DeezerSearchView::GroupByClicked(QAction *action) {

  if (action->property("group_by").isNull()) {
    if (!group_by_dialog_) {
      group_by_dialog_.reset(new GroupByDialog);
      connect(group_by_dialog_.data(), SIGNAL(Accepted(CollectionModel::Grouping)), SLOT(SetGroupBy(CollectionModel::Grouping)));
    }

    group_by_dialog_->show();
    return;
  }

  SetGroupBy(action->property("group_by").value<CollectionModel::Grouping>());

}

void DeezerSearchView::SetGroupBy(const CollectionModel::Grouping &g) {

  // Clear requests: changing "group by" on the models will cause all the items to be removed/added again,
  // so all the QModelIndex here will become invalid. New requests will be created for those
  // songs when they will be displayed again anyway (when DeezerSearchItemDelegate::paint will call LazyLoadArt)
  art_requests_.clear();
  // Update the models
  front_model_->SetGroupBy(g, true);
  back_model_->SetGroupBy(g, false);

  // Save the setting
  QSettings s;
  s.beginGroup(DeezerSettingsPage::kSettingsGroup);
  s.setValue("group_by1", int(g.first));
  s.setValue("group_by2", int(g.second));
  s.setValue("group_by3", int(g.third));
  s.endGroup();

  // Make sure the correct action is checked.
  for (QAction *action : group_by_actions_->actions()) {
    if (action->property("group_by").isNull()) continue;

    if (g == action->property("group_by").value<CollectionModel::Grouping>()) {
      action->setChecked(true);
      return;
    }
  }

  // Check the advanced action
  group_by_actions_->actions().last()->setChecked(true);

}

void DeezerSearchView::SearchBySongsClicked(bool checked) {
  SetSearchBy(DeezerSettingsPage::SearchBy_Songs);
}

void DeezerSearchView::SearchByAlbumsClicked(bool checked) {
  SetSearchBy(DeezerSettingsPage::SearchBy_Albums);
}

void DeezerSearchView::SetSearchBy(DeezerSettingsPage::SearchBy searchby) {
  searchby_ = searchby;
  QSettings s;
  s.beginGroup(DeezerSettingsPage::kSettingsGroup);
  s.setValue("searchby", int(searchby));
  s.endGroup();
  TextEdited(ui_->search->text());
}

void DeezerSearchView::UpdateStatus(QString text) {
  ui_->progressbar->show();
  ui_->label_status->setText(text);
}

void DeezerSearchView::ProgressSetMaximum(int max) {
  ui_->progressbar->setMaximum(max);
}

void DeezerSearchView::UpdateProgress(int progress) {
  ui_->progressbar->setValue(progress);
}
