/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2011, David Sansome <me@davidsansome.com>
 * Copyright 2018-2024, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef STREAMSONGMIMEDATA_H
#define STREAMSONGMIMEDATA_H

#include "includes/shared_ptr.h"
#include "mimedata/mimedata.h"
#include "core/song.h"

class StreamingService;

class StreamSongMimeData : public MimeData {
  Q_OBJECT

 public:
  explicit StreamSongMimeData(SharedPtr<StreamingService> _service, QObject *parent = nullptr);

  SharedPtr<StreamingService> service;
  SongList songs;
};

#endif  // STREAMSONGMIMEDATA_H
