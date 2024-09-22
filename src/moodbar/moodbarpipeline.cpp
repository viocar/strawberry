/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2012, David Sansome <me@davidsansome.com>
 * Copyright 2019-2024, Jonas Kvinge <jonas@jkvinge.net>
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

#include "moodbarpipeline.h"

#include <cstdlib>

#include <memory>

#include <glib.h>
#include <glib-object.h>
#include <gst/gst.h>

#include <QObject>
#include <QCoreApplication>
#include <QThread>
#include <QString>
#include <QUrl>

#include "core/logging.h"
#include "core/signalchecker.h"
#include "utilities/threadutils.h"
#include "moodbar/moodbarbuilder.h"

#include "gstfastspectrum.h"

using namespace Qt::StringLiterals;
using std::make_unique;

namespace {
constexpr int kBands = 128;
}

MoodbarPipeline::MoodbarPipeline(const QUrl &url, QObject *parent)
    : QObject(parent),
      url_(url),
      pipeline_(nullptr),
      convert_element_(nullptr),
      success_(false),
      running_(false) {}

MoodbarPipeline::~MoodbarPipeline() { Cleanup(); }

GstElement *MoodbarPipeline::CreateElement(const QString &factory_name) {

  GstElement *ret = gst_element_factory_make(factory_name.toLatin1().constData(), nullptr);

  if (ret) {
    gst_bin_add(GST_BIN(pipeline_), ret);
  }
  else {
    qLog(Warning) << "Unable to create gstreamer element" << factory_name;
  }

  return ret;

}

QByteArray MoodbarPipeline::ToGstUrl(const QUrl &url) {

  if (url.isLocalFile() && !url.host().isEmpty()) {
    QString str = "file:////"_L1 + url.host() + url.path();
    return str.toUtf8();
  }

  return url.toEncoded();

}

void MoodbarPipeline::Start() {

  Q_ASSERT(QThread::currentThread() != qApp->thread());

  Utilities::SetThreadIOPriority(Utilities::IoPriority::IOPRIO_CLASS_IDLE);

  if (pipeline_) {
    return;
  }

  pipeline_ = gst_pipeline_new("moodbar-pipeline");

  GstElement *decodebin = CreateElement(QStringLiteral("uridecodebin"));
  convert_element_ = CreateElement(QStringLiteral("audioconvert"));
  GstElement *spectrum = CreateElement(QStringLiteral("spectrum"));
  GstElement *fakesink = CreateElement(QStringLiteral("fakesink"));

  if (!decodebin || !convert_element_ || !spectrum || !fakesink) {
    gst_object_unref(GST_OBJECT(pipeline_));
    pipeline_ = nullptr;
    Q_EMIT Finished(false);
    return;
  }

  // Join them together
  if (!gst_element_link_many(convert_element_, spectrum, fakesink, nullptr)) {
    qLog(Error) << "Failed to link elements";
    gst_object_unref(GST_OBJECT(pipeline_));
    pipeline_ = nullptr;
    Q_EMIT Finished(false);
    return;
  }

  //builder_ = make_unique<MoodbarBuilder>();

  // Set properties

  QByteArray gst_url = ToGstUrl(url_);
  g_object_set(decodebin, "uri", gst_url.constData(), nullptr);
  g_object_set(spectrum, "bands", kBands, nullptr);

  //GstStrawberryFastSpectrum *fastspectrum = reinterpret_cast<GstStrawberryFastSpectrum*>(spectrum);
  //fastspectrum->output_callback = [this](double *magnitudes, const int size) { builder_->AddFrame(magnitudes, size); };

  {
    GstPad *pad = gst_element_get_static_pad(fakesink, "src");
    if (pad) {
      buffer_probe_cb_id_ = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, BufferProbeCallback, this, nullptr);
      gst_object_unref(pad);
    }
  }

  // Connect signals
  CHECKED_GCONNECT(decodebin, "pad-added", &NewPadCallback, this);
  GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  if (bus) {
    gst_bus_set_sync_handler(bus, BusCallbackSync, this, nullptr);
    gst_object_unref(bus);
  }

  // Start playing
  running_ = true;
  gst_element_set_state(pipeline_, GST_STATE_PLAYING);

}

void MoodbarPipeline::ReportError(GstMessage *msg) {

  GError *error = nullptr;
  gchar *debugs = nullptr;

  gst_message_parse_error(msg, &error, &debugs);
  QString message = QString::fromLocal8Bit(error->message);

  g_error_free(error);
  g_free(debugs);

  qLog(Error) << "Error processing" << url_ << ":" << message;

}

void MoodbarPipeline::NewPadCallback(GstElement *element, GstPad *pad, gpointer self) {

  Q_UNUSED(element)

  MoodbarPipeline *instance = reinterpret_cast<MoodbarPipeline*>(self);

  if (!instance->running_) {
    qLog(Warning) << "Received gstreamer callback after pipeline has stopped.";
    return;
  }

  GstPad *const audiopad = gst_element_get_static_pad(instance->convert_element_, "sink");
  if (!audiopad) return;

  if (GST_PAD_IS_LINKED(audiopad)) {
    qLog(Warning) << "audiopad is already linked, unlinking old pad";
    gst_pad_unlink(audiopad, GST_PAD_PEER(audiopad));
  }

  gst_pad_link(pad, audiopad);
  gst_object_unref(audiopad);

  int rate = 0;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  if (caps) {
    GstStructure *structure = gst_caps_get_structure(caps, 0);
    if (structure) {
      gst_structure_get_int(structure, "rate", &rate);
    }
    gst_caps_unref(caps);
  }

  if (instance->builder_) {
    instance->builder_->Init(kBands, rate);
  }
  else {
    qLog(Error) << "Builder does not exist";
  }

}

GstBusSyncReply MoodbarPipeline::BusCallbackSync(GstBus *bus, GstMessage *message, gpointer self) {

  Q_UNUSED(bus)

  MoodbarPipeline *instance = reinterpret_cast<MoodbarPipeline*>(self);

  switch (GST_MESSAGE_TYPE(message)) {
#if 0
    case GST_MESSAGE_ELEMENT:{
      const GstStructure *structure = gst_message_get_structure(message);
      const gchar *name = gst_structure_get_name(structure);
      if (strcmp(name, "spectrum") == 0) {
        const GValue *magnitudes = gst_structure_get_value(structure, "magnitude");
        if (instance->builder_) {
          instance->builder_->AddFrame(magnitudes, kBands);
        }
      }
      break;
    }
#endif

    case GST_MESSAGE_EOS:
      instance->Stop(true);
      break;

    case GST_MESSAGE_ERROR:
      instance->ReportError(message);
      instance->Stop(false);
      break;

    default:
      break;
  }

  return GST_BUS_PASS;

}

GstPadProbeReturn MoodbarPipeline::BufferProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer self) {

  Q_UNUSED(pad)

  MoodbarPipeline *instance = reinterpret_cast<MoodbarPipeline*>(self);

  GstBuffer *buffer = gst_pad_probe_info_get_buffer(info);

  GstMapInfo map;
  gst_buffer_map(buffer, &map, GST_MAP_READ);
  instance->data_.append(reinterpret_cast<const char*>(map.data), map.size);
  gst_buffer_unmap(buffer, &map);
  gst_buffer_unref(buffer);

  return GST_PAD_PROBE_OK;

}

void MoodbarPipeline::Stop(const bool success) {

  success_ = success;
  running_ = false;
  if (builder_) {
    data_ = builder_->Finish(1000);
    builder_.reset();
  }

  Q_EMIT Finished(success);

}

void MoodbarPipeline::Cleanup() {

  Q_ASSERT(QThread::currentThread() == thread());
  Q_ASSERT(QThread::currentThread() != qApp->thread());

  running_ = false;
  if (pipeline_) {
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    if (bus) {
      gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);
      gst_object_unref(bus);
    }

    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }

}
