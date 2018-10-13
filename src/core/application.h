/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, David Sansome <me@davidsansome.com>
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

#ifndef APPLICATION_H_
#define APPLICATION_H_

#include "config.h"

#include <memory>
#include <stdbool.h>

#include <QObject>
#include <QThread>
#include <QList>
#include <QString>

#include "settings/settingsdialog.h"

class TaskManager;
class ApplicationImpl;
class TagReaderClient;
class Database;
class EngineDevice;
class Player;
class Appearance;
class SCollection;
class CollectionBackend;
class CollectionModel;
class PlaylistBackend;
class PlaylistManager;
#ifndef Q_OS_WIN
class DeviceManager;
#endif
class CoverProviders;
class AlbumCoverLoader;
class CurrentArtLoader;
class LyricsProviders;
class InternetModel;
class TidalSearch;
class DeezerSearch;

class Application : public QObject {
  Q_OBJECT

 public:
  static bool kIsPortable;

  explicit Application(QObject *parent = nullptr);
  ~Application();

  TagReaderClient *tag_reader_client() const;
  Database *database() const;
  Appearance *appearance() const;
  TaskManager *task_manager() const;
  Player *player() const;
  EngineDevice *enginedevice() const;
#ifndef Q_OS_WIN
  DeviceManager *device_manager() const;
#endif

  SCollection *collection() const;
  CollectionBackend *collection_backend() const;
  CollectionModel *collection_model() const;

  PlaylistBackend *playlist_backend() const;
  PlaylistManager *playlist_manager() const;

  CoverProviders *cover_providers() const;
  AlbumCoverLoader *album_cover_loader() const;
  CurrentArtLoader *current_art_loader() const;

  LyricsProviders *lyrics_providers() const;

  InternetModel *internet_model() const;
  TidalSearch *tidal_search() const;
  DeezerSearch *deezer_search() const;

  void MoveToNewThread(QObject *object);
  void MoveToThread(QObject *object, QThread *thread);

 public slots:
  void AddError(const QString &message);
  void ReloadSettings();
  void OpenSettingsDialogAtPage(SettingsDialog::Page page);

signals:
  void ErrorAdded(const QString &message);
  void SettingsChanged();
  void SettingsDialogRequested(SettingsDialog::Page page);

 private:
  std::unique_ptr<ApplicationImpl> p_;
  QList<QThread*> threads_;

};

#endif  // APPLICATION_H_
