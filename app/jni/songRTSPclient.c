/*
 * Copyright (C) 2021 FishSemi Inc. All rights reserved.

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#include <string.h>
#include <stdint.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/sdp/gstsdpmessage.h>
#include <pthread.h>
#include <sys/system_properties.h>

#define TAG "SongRTSPClientJNI"
#define GTAG "SongRTSPClientJNIG"

#define alogv(...)    __android_log_print (ANDROID_LOG_VERBOSE, TAG, __VA_ARGS__)
#define alogi(...)    __android_log_print (ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define alogd(...)    __android_log_print (ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define alogw(...)    __android_log_print (ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define aloge(...)    __android_log_print (ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

GST_DEBUG_CATEGORY_STATIC (SongRTSPClientJNIG_category);
#define GST_CAT_DEFAULT SongRTSPClientJNIG_category

/*
 * These macros provide a way to store the native pointer to CustomData, which might be 32 or 64 bits, into
 * a jlong, which is always 64 bits, without warnings.
 */
#if GLIB_SIZEOF_VOID_P == 8
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)data)
#else
# define GET_CUSTOM_DATA(env, thiz, fieldID) (CustomData *)(jint)(*env)->GetLongField (env, thiz, fieldID)
# define SET_CUSTOM_DATA(env, thiz, fieldID, data) (*env)->SetLongField (env, thiz, fieldID, (jlong)(jint)data)
#endif

#define USR_MESSAGE_PUSH_RTMP_SHUTDOWN   "0: push rtmp branch shutdown"
#define USR_MESSAGE_PUSH_RTSP_SHUTDOWN   "1: push rtsp branch shutdown"
#define USR_MESSAGE_FETCH_EOS_RESTART    "3: fetch eos, pipline restart"
#define USR_MESSAGE_RTSP_SRC_ERR_RESTART "4: rtsp src err, pipline restart "

#define BRANCH_DISABLE     0
#define BRANCH_ENABLE      1
#define BRANCH_DISABLE_ING 2
#define BRANCH_ENABLE_ING  3

#define RESET_REQUEST_NULL    0x00
#define RESET_REQUEST_DISPLAY 0x01
#define RESET_REQUEST_PRTMP   0x02
#define RESET_REQUEST_PRTSP   0x04
#define RESET_REQUEST_PIPELINE 0x07

/* Structure to contain all our information, so we can pass it to callbacks */
typedef struct _CustomData {
  GMainContext *context;
  pthread_t gst_app_thread;
  jobject app;

  GstElement *pipeline;         /* The running pipeline */
  gint pipeline_ref;
  GstElement *rtspsrc;
  GstElement **rtspsrc_elements;
  GstPad *tee_sinkpad;
  GstPad *tee_srcpad_display;
  GstPad *tee_srcpad_push_rtmp;
  GstPad *tee_srcpad_push_rtsp;
  GstPad *tee_srcpad_recording;
  gboolean rtspsrc_linked;
  gchar *rtspsrc_url;

  GstElement **display_elements;
  GstPad *display_queue_sinkpad;
  gchar display_enabled;
  gboolean display_requst;
  ANativeWindow *native_window;

  GstElement **push_rtmp_elements;
  GstPad *push_rtmp_queue_sinkpad;
  gboolean push_rtmp_enabled;
  gboolean push_rtmp_request;
  gchar *push_rtmp_url;

  GstElement **push_rtsp_elements;
  GstPad *push_rtsp_queue_sinkpad;
  gchar push_rtsp_enabled;
  gboolean push_rtsp_request;
  gchar *push_rtsp_url;
  GCond push_rtsp_cond_eos;

  GstElement **recording_elements;
  GstPad *recording_queue_sinkpad;
  GstPad *filesink_sinkpad;
  gboolean recording_enabled;
  gchar *recording_dir;

  GAsyncQueue *worker_cmd_queue;
  GMutex mutex_branch;
  pthread_t gst_worker_thread;
  gboolean worker_run;
  guchar reset_request;
  gboolean pipeline_restarting;

  GMainLoop *main_loop;         /* GLib main loop */

} CustomData;

typedef struct _element_node {
  gchar *factoryname;
  gchar *name;
} element_node;

/*
 *
 *                                           .--> queue -> flvmux -> rtmpsink
 *                                          |
 *                                           .--> queue -> flvmux -> filesink
 *                                          |
 * rtspsrc -> rtph264depay -> h264parse -> tee -> queue -> flvmux -> fakesink
 *                                          |
 *                                           .--> queue -> h264parse -> amcviddec-omxarmvideov5xxdecoder -> queue -> autovideosink
 *                                          |
 *                                           .--> queue -> rtspclientsink
 * */
#define FK_H264DEPAY 0
#define FK_H264PARSE 1
#define FK_TEE       2
#define FK_QUEUE     3
#define FK_FLVMUX    4
#define FK_FAKESINK  5

const static element_node fakesink_vector[] = {
  {"rtph264depay", "f0-rtph264depay"},
  {"h264parse", "f1-h264parse"},
  {"tee", "f2-tee"},
  {"queue", "f3-queue"},
  {"flvmux", "f4-flvmux"},
  {"fakesink", "f5-fakesink"},
  {NULL, NULL}
};

#define DP_QUEUE0    0
#define DP_H264PARSE 1
#define DP_AMCVIDEO  2
#define DP_QUEUE1    3
#define DP_VIDEOSINK 4

const static element_node display_vector[] = {
  {"queue", "v0-queue"},
  {"h264parse", "v1-264parse"},
  {"amcviddec-omxarmvideov5xxdecoder", "v2-amcviddec"},
  {"queue", "v3-queue"},
  {"autovideosink", "v4-autovideosink"},
  {NULL, NULL}
};

#define PU_RTMP_QUEUE   0
#define PU_RTMP_FLVMUX  1
#define PU_RTMPSINK     2

const static element_node push_rtmp_vector[] = {
  {"queue", "prtmp0-queue"},
  {"flvmux", "prtmp1-flvmux"},
  {"rtmpsink", "prtmp2-rtmpsink"},
  {NULL, NULL},
};

#define PU_RTSP_QUEUE   0
#define PU_RTSPSINK     1

const static element_node push_rtsp_vector[] = {
  {"queue", "prtsp0-queue"},
  {"rtspclientsink", "prtsp1-rtspclientsink"},
  {NULL, NULL},
};

#define RC_QUEUE     0
#define RC_MP4MUX    1
#define RC_FILESINK  2

const static element_node recording_vector[] = {
  {"queue", "r0-queue"},
  {"flvmux", "r2-flvmux"},
  {"filesink", "r3-filesink"},
  {NULL, NULL},
};

typedef struct {
  guint index;
  gchar *comment;
} _worker_cmd;

#define WORKER_CMD          0
#define WORKER_CMD_START_DISPLAY   1
#define WORKER_CMD_STOP_DISPLAY    2
#define WORKER_CMD_START_PUSH_RTSP 3
#define WORKER_CMD_STOP_PUSH_RTSP  4
#define WORKER_CMD_START_PUSH_RTMP 5
#define WORKER_CMD_STOP_PUSH_RTMP  6
#define WORKER_CMD_RESET_PIPELINE  7

const static _worker_cmd worke_cmd[] = {
  {0, ""},
  {1, "start display"},
  {2, "stop display"},
  {3, "start push rtsp"},
  {4, "stop push rtsp"},
  {5, "start push rtmp"},
  {6, "stop push rtmp"},
  {7, "reset pipeline"},
};

static GstStateChangeReturn gst_elements_set_state_v (GstElement **el_v, GstState state) {
  GstStateChangeReturn ret;
  int i = 0;

  while (el_v[i] != NULL) {
    ret = gst_element_set_state (el_v[i], state);
    gst_element_get_state (el_v[i], NULL, NULL, 2000);
    i++;
  }
  return ret;
}

static GstStateChangeReturn gst_elements_get_state_v (GstElement **el_v) {
  GstState state, state_pending;
  GstStateChangeReturn ret;
  int i = 0;

  while (el_v[i] != NULL) {
    gst_element_get_state (el_v[i], &state, &state_pending, 0);
    i++;
  }
  return ret;
}

static gboolean gst_elements_set_locked_state_v (GstElement **el_v, gboolean locked_state) {
  gboolean ret = TRUE;
  int i = 0;

  while (el_v[i] != NULL) {
    ret = gst_element_set_locked_state (el_v[i], locked_state);
    i++;
  }

  return TRUE;
}

static gboolean gst_element_sync_state_with_parent_v (GstElement **el_v) {
  gboolean ret = TRUE;
  int i = 0;

  while (el_v[i] != NULL) {
    ret = gst_element_sync_state_with_parent (el_v[i]);
    i++;
  }

  return TRUE;
}

static void set_usr_message (const gchar *message, CustomData *data);
static gboolean launch_restart_process(CustomData *data, guchar reset_request);
static GstPadProbeReturn probe_eos_cb (GstPad *pad, GstPadProbeInfo *info, gpointer user_data) {
  CustomData *data = (CustomData *)user_data;
  GstEvent *event = gst_pad_probe_info_get_event(info);

  if (GST_EVENT_TYPE(event) != GST_EVENT_EOS)
    return GST_PAD_PROBE_OK;

  if (data == NULL)
    return GST_PAD_PROBE_OK;

  if (pad != data->tee_sinkpad)
    return GST_PAD_PROBE_OK;

  alogi("Probe EOS CB: tee_sinkpad receive eos.");
  set_usr_message (USR_MESSAGE_FETCH_EOS_RESTART, data);
  launch_restart_process (data, RESET_REQUEST_PIPELINE);

  return GST_PAD_PROBE_DROP;
}

static void probe_rtspsrc_pad_added_cb (GstElement* element, GstPad* pad, gpointer _data) {
  CustomData *data;
  GstCaps *caps;
  const GstStructure *s;
  const gchar *encoding_name;
  gchar *name, *description;

  data = (CustomData *)_data;
  name = gst_pad_get_name(pad);
  caps = gst_pad_get_current_caps (pad);
  if (!caps) {
    aloge ("probe_rtspsrc_pad_added_cb: failed to get pad caps %s", name);
    return;
  }

  description = gst_caps_to_string (caps);
  alogi ("probe_rtspsrc_pad_added_cb: name(%d):%s caps:%s", data->rtspsrc_linked, name, description);
  g_free (name);
  g_free (description);

  if (data->rtspsrc_linked) {
    alogi ("probe_rtspsrc_pad_added_cb: rtspsrc has been linked");
    gst_caps_unref (caps);
    return;
  }

  s = gst_caps_get_structure (caps, 0);
  if (!s) {
    aloge ("probe_rtspsrc_pad_added_cb: failed to get pad caps structure");
    gst_caps_unref (caps);
    return;
  }

  encoding_name = gst_structure_get_string (s, "encoding-name");
  if (!g_strcmp0 (encoding_name, "H264")) {
    if (gst_element_link_pads(element, name, data->rtspsrc_elements[FK_H264DEPAY], NULL))
      data->rtspsrc_linked = TRUE;
    else
      aloge ("probe_rtspsrc_pad_added_cb: failed to link elements");
  } else {
    aloge ("probe_rtspsrc_pad_added_cb: unsupported codec type %s", encoding_name);
  }

  gst_caps_unref (caps);
}

static void probe_rtspsrc_pad_removed_cb (GstElement* element, GstPad* pad, gpointer _data) {
  CustomData *data;
  gchar *name, *description;

  data = (CustomData *)_data;
  name = gst_pad_get_name (pad);
  description = gst_caps_to_string (gst_pad_get_pad_template_caps (pad));

  alogi ("probe_rtspsrc_pad_removed_cb: %s, pad name:%s", description, name);
  data->rtspsrc_linked = FALSE;

  g_free (name);
  g_free (description);
}

static void cleanup_elements (GstElement *pipeline, GstElement **elements, int count) {
  for (int i = 0; (i < count) && (elements[i] != NULL); i++) {

    if ((i + 1) < count)
      gst_element_unlink (elements[i], elements[i + 1]);

    alogi ("cleanup element %s", gst_element_get_name (elements[i]));
    gst_element_set_locked_state (elements[i], FALSE);
    gst_bin_remove (GST_BIN (pipeline), elements[i]);
    gst_object_unref (elements[i]);
    elements[i] = NULL;
  }
}

static gboolean setup_elements (GstElement *pipeline, GstElement **elements, const element_node *vector) {
  int i;

  for (i = 0; vector[i].name != NULL; i++) {
    elements[i] = gst_element_factory_make (vector[i].factoryname, vector[i].name);
    if (!elements[i]) {
      aloge ("Unable to create %s", vector[i].name);
      break;
    }
    gst_object_ref (elements[i]);

    alogi ("setup element create %s", vector[i].name);
    gst_bin_add (GST_BIN (pipeline), elements[i]);
    gst_element_set_locked_state (elements[i], TRUE);

    if (i)
      gst_element_link (elements[i - 1], elements[i]);
  }

  if ( vector[i].name == NULL)
    return TRUE;

  i--;
  cleanup_elements (pipeline, elements, i);

  return FALSE;
}

static void cleanup_rtspsrc_elements (CustomData *data) {
  int count;

  if (!(data && data->pipeline && data->rtspsrc))
    return;

  if (data->rtspsrc_linked)
    gst_element_unlink (data->rtspsrc, data->rtspsrc_elements[FK_H264DEPAY]);

  gst_element_release_request_pad (data->rtspsrc_elements[FK_TEE],
          data->tee_srcpad_display);
  gst_element_release_request_pad (data->rtspsrc_elements[FK_TEE],
          data->tee_srcpad_push_rtmp);
  gst_element_release_request_pad (data->rtspsrc_elements[FK_TEE],
          data->tee_srcpad_push_rtsp);
  gst_element_release_request_pad (data->rtspsrc_elements[FK_TEE],
          data->tee_srcpad_recording);

  count = sizeof (fakesink_vector) / sizeof (element_node) - 1;
  cleanup_elements (data->pipeline, data->rtspsrc_elements, count);
  g_free (data->rtspsrc_elements);

  gst_bin_remove (GST_BIN(data->pipeline), data->rtspsrc);

  data->tee_srcpad_display = NULL;
  data->tee_srcpad_push_rtmp = NULL;
  data->tee_srcpad_push_rtsp = NULL;
  data->tee_srcpad_recording = NULL;
  data->rtspsrc = NULL;
}

static gboolean setup_rtspsrc_elements (CustomData *data) {
  GstElement *pipeline, *rtspsrc, **elements;
  GstPad *tee_sinkpad, *tee_srcpad[5];
  int count, i;

  if (!data) {
    aloge ("setup_rtspsrc_elements: Parameter error!");
    return FALSE;
  }

  pipeline = data->pipeline;
  if (!pipeline) {
    aloge ("setup_rtspsrc_elements: get pipeline failed!");
    return FALSE;
  }

  rtspsrc = gst_element_factory_make ("rtspsrc", "rtspsrc");
  if (!rtspsrc) {
    aloge ("setup_rtspsrc_elements: create rtspsrc !");
    return FALSE;
  }

  count = sizeof (fakesink_vector) / sizeof (element_node);
  elements = (GstElement **)g_malloc0 (sizeof(GstElement*) * count);
  if (!elements) {
    aloge ("setup_rtspsrc_elements: alloc elements failed !");
    return FALSE;
  }

  if (!setup_elements (pipeline, elements, fakesink_vector)) {
    aloge ("setup_rtspsrc_elements:setup elements failed!");
    g_free (elements);
    return FALSE;
  }
  gst_elements_set_locked_state_v (elements, FALSE);

  tee_sinkpad = gst_element_get_static_pad(elements[FK_TEE], "sink");
  if (!tee_sinkpad) {
    aloge ("setup_rtspsrc_elements: get tee_sinkpad failed!");
    cleanup_elements (pipeline, elements, count - 1);
    g_free (elements);
    return FALSE;
  }

  for (i = 0; i < 4; i++) {
    tee_srcpad[i] = gst_element_get_request_pad (elements[FK_TEE], "src_%u");
    if (!tee_srcpad[i]) {
      aloge ("setup_rtspsrc_elements: get tee_srcpad[%d] failed!", i);
      break;
    }
  }

  if (i != 4) {
    for (--i; i > 0; i--) {
      gst_element_release_request_pad (elements[FK_TEE], tee_srcpad[i]);
      gst_object_unref (tee_srcpad[i]);
    }

    cleanup_elements (pipeline, elements, count - 1);
    g_free (elements);
    return FALSE;
  }

  gst_bin_add (GST_BIN (pipeline), rtspsrc);
  g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(probe_rtspsrc_pad_added_cb), data);
  //g_signal_connect(rtspsrc, "pad-removed", G_CALLBACK(probe_rtspsrc_pad_removed_cb), data);
  g_object_set(G_OBJECT(rtspsrc), "latency", 41, "udp-reconnect",(gboolean) TRUE,
      "timeout", (guint64) 0, "do-retransmission", (gboolean) FALSE, NULL);

  // drop eos of autovideosink branch
  gst_pad_add_probe(tee_sinkpad, GST_PAD_PROBE_TYPE_EVENT_BOTH, probe_eos_cb, data, NULL);

  g_object_set (G_OBJECT(elements[FK_H264PARSE]), "config-interval", -1, NULL);
  g_object_set (G_OBJECT(elements[FK_FLVMUX]), "streamable", TRUE, NULL);
  //g_object_set (G_OBJECT(tee), "allow-not-linked", (gboolean) TRUE, NULL);

  data->rtspsrc = rtspsrc;
  data->rtspsrc_elements = elements;
  data->tee_sinkpad = tee_sinkpad;
  data->tee_srcpad_display = tee_srcpad[0];
  data->tee_srcpad_push_rtmp = tee_srcpad[1];
  data->tee_srcpad_push_rtsp = tee_srcpad[2];
  data->tee_srcpad_recording = tee_srcpad[3];
  data->rtspsrc_linked = FALSE;

  return TRUE;
}

static void cleanup_display_elements (CustomData *data) {
  int count;

  if (!(data && data->display_elements))
    return;

  count = sizeof (display_vector) / sizeof (element_node) - 1;
  cleanup_elements (data->pipeline, data->display_elements, count);
  g_free (data->display_elements);

  data->display_elements = NULL;
  data->display_queue_sinkpad = NULL;

}

static gboolean setup_display_elements (CustomData *data) {
  GstElement **elements;
  GstPad *display_queue_sinkpad;
  int count;

  if (!data || !data->pipeline) {
    aloge ("setup_display_elements: Parameter error!");
    return FALSE;
  }

  count = sizeof (display_vector) / sizeof (element_node);
  elements = (GstElement **)g_malloc0 (sizeof(GstElement*) * count);
  if (!elements) {
    aloge ("setup_display_elements: alloc elements failed!");
    return FALSE;
  }

  if (!setup_elements (data->pipeline, elements, display_vector)) {
    aloge ("setup_display_elements: setup elements failed!");
    g_free (elements);
    return FALSE;
  }

  display_queue_sinkpad = gst_element_get_static_pad(elements[DP_QUEUE0], "sink");
  if (!display_queue_sinkpad) {
    aloge ("setup_display_elements: get display queue sinkpad failed !");
    cleanup_elements (data->pipeline, elements, count - 1);
    g_free (elements);
    return FALSE;
  }

  g_object_set (G_OBJECT(elements[DP_VIDEOSINK]), "sync", (gboolean) FALSE,
          "message-forward", (gboolean) TRUE, "async-handling", (gboolean) TRUE, NULL);

  data->display_elements = elements;
  data->display_queue_sinkpad = display_queue_sinkpad;

  return TRUE;
}

static void cleanup_push_rtmp_elements (CustomData *data) {
  int count;

  if (!(data && data->push_rtmp_elements))
    return;

  count = sizeof (push_rtmp_vector) / sizeof (element_node) - 1;
  cleanup_elements (data->pipeline, data->push_rtmp_elements, count);
  g_free (data->push_rtmp_elements);

  data->push_rtmp_queue_sinkpad = NULL;
  data->push_rtmp_elements = NULL;
}

static gboolean setup_push_rtmp_elements (CustomData *data) {
  GstElement **elements;
  GstPad *push_rtmp_queue_sinkpad;
  int count;

  if (!data || !data->pipeline) {
    aloge ("setup_push_rtmp_elements: Parameter error!");
    return FALSE;
  }

  count = sizeof (push_rtmp_vector) / sizeof (element_node);
  elements = (GstElement **)g_malloc0 (sizeof(GstElement*) * count);
  if (!elements) {
    aloge ("setup_push_rtmp_elements: alloc elements failed!");
    return FALSE;
  }

  if (!setup_elements (data->pipeline, elements, push_rtmp_vector)) {
    aloge ("setup_push_rtmp_elements: setup elements failed!");
    g_free (elements);
    return FALSE;
  }

  push_rtmp_queue_sinkpad = gst_element_get_static_pad(elements[PU_RTMP_QUEUE], "sink");
  if (!push_rtmp_queue_sinkpad) {
    aloge ("setup_push_rtmp_elements: get pushing queue sinkpad failed !");
    cleanup_elements (data->pipeline, elements, count - 1);
    g_free (elements);
    return FALSE;
  }

  g_object_set (G_OBJECT(elements[PU_RTMP_QUEUE]), "max-size-buffers", 0, NULL);
  g_object_set (G_OBJECT(elements[PU_RTMP_QUEUE]), "max-size-bytes", 0, NULL);
  //g_object_set (G_OBJECT(elements[PU_RTMP_QUEUE]), "max-size-time", 0, NULL);
  g_object_set (G_OBJECT(elements[PU_RTMP_QUEUE]), "flush-on-eos", TRUE, NULL);

  g_object_set (G_OBJECT(elements[PU_RTMP_FLVMUX]), "streamable", ( (gboolean) TRUE), NULL);
  g_object_set (G_OBJECT(elements[PU_RTMPSINK]), "sync", ( (gboolean) FALSE), NULL);

  data->push_rtmp_queue_sinkpad = push_rtmp_queue_sinkpad;
  data->push_rtmp_elements = elements;

  return TRUE;
}

static void cleanup_push_rtsp_elements (CustomData *data) {
  int count;

  if (!(data && data->push_rtsp_elements))
    return;

  gst_object_unref (data->push_rtsp_queue_sinkpad);
  data->push_rtsp_queue_sinkpad = NULL;

  count = sizeof (push_rtsp_vector) / sizeof (element_node);
  cleanup_elements (data->pipeline, data->push_rtsp_elements, count);

  g_cond_clear (&data->push_rtsp_cond_eos);
  g_free (data->push_rtsp_elements);
  data->push_rtsp_elements = NULL;

}

static gboolean setup_push_rtsp_elements (CustomData *data) {
  GstElement **elements;
  GstPad *push_rtsp_queue_sinkpad;
  int count;

  if (!data || !data->pipeline) {
    aloge ("setup_push_rtsp_elements: Parameter error!");
    return FALSE;
  }

  count = sizeof (push_rtsp_vector) / sizeof (element_node);
  elements = (GstElement **)g_malloc0 (sizeof(GstElement*) * count);
  if (!elements) {
    aloge ("setup_push_rtsp_elements: alloc elements failed!");
    return FALSE;
  }

  if (!setup_elements (data->pipeline, elements, push_rtsp_vector)) {
    aloge ("setup_push_rtsp_elements: setup elements failed!");
    g_free (elements);
    return FALSE;
  }

  push_rtsp_queue_sinkpad = gst_element_get_static_pad(elements[PU_RTSP_QUEUE], "sink");
  if (!push_rtsp_queue_sinkpad) {
    aloge ("setup_push_rtsp_elements: get pushing queue sinkpad failed !");
    cleanup_elements (data->pipeline, elements, count - 1);
    g_free (elements);
    return FALSE;
  }

  g_object_set (G_OBJECT(elements[PU_RTSP_QUEUE]), "max-size-buffers", 0, NULL);
  g_object_set (G_OBJECT(elements[PU_RTSP_QUEUE]), "max-size-bytes", 0, NULL);
  g_object_set (G_OBJECT(elements[PU_RTSP_QUEUE]), "max-size-time", 0, NULL);
  g_object_set (G_OBJECT(elements[PU_RTSP_QUEUE]), "flush-on-eos", TRUE, NULL);
  //g_object_set (G_OBJECT(elements[PU_RTSP_QUEUE]), "leaky", 2, NULL);
  g_object_set (G_OBJECT(elements[PU_RTSPSINK]), "protocols", GST_RTSP_LOWER_TRANS_TCP, "latency", 10000, NULL);
  //g_object_set (G_OBJECT(elements[PU_RTSPSINK]), "debug", TRUE, NULL);

  g_cond_init (&data->push_rtsp_cond_eos);
  data->push_rtsp_queue_sinkpad = push_rtsp_queue_sinkpad;
  data->push_rtsp_elements = elements;

  return TRUE;
}

static void cleanup_recording_elements (CustomData *data) {
  int count;

  if (!(data && data->recording_elements))
    return;

  count = sizeof (recording_vector) / sizeof (element_node) - 1;
  cleanup_elements (data->pipeline, data->recording_elements, count);
  g_free (data->recording_elements);

  if (data->recording_dir) {
    g_free (data->recording_dir);
    data->recording_dir = NULL;
  }

  data->filesink_sinkpad = NULL;
  data->recording_queue_sinkpad = NULL;
  data->recording_elements = NULL;
  data->recording_enabled = FALSE;
}

static gboolean setup_recording_elements (CustomData *data) {
  GstElement **elements;
  GstPad *recording_queue_sinkpad;
  GstPad *filesink_sinkpad;
  int count;

  if (!data || !data->pipeline) {
    aloge ("setup_recording_elements: Parameter error!");
    return FALSE;
  }

  count = sizeof (recording_vector) / sizeof (element_node);
  elements = (GstElement **)g_malloc0 (sizeof(GstElement*) * count);
  if (!elements) {
    aloge ("setup_recording_elements: alloc elements failed!");
    return FALSE;
  }

  if (!setup_elements (data->pipeline, elements, recording_vector)) {
    aloge ("setup_recording_elements: setup elements failed!");
    g_free (elements);
    return FALSE;
  }
  data->recording_elements = elements;

  recording_queue_sinkpad = gst_element_get_static_pad(elements[RC_QUEUE], "sink");
  if (!recording_queue_sinkpad) {
    aloge ("setup_recording_elements: get recording queue sink pad failed !");
    cleanup_elements (data->pipeline, elements, count - 1);
    g_free (elements);
  }

  filesink_sinkpad = gst_element_get_static_pad(elements[RC_FILESINK], "sink");
  if (!filesink_sinkpad) {
    aloge ("setup_recording_elements: get recording file sink pad failed !");
    cleanup_elements (data->pipeline, elements, count - 1);
    g_free (elements);
  }

  data->filesink_sinkpad = filesink_sinkpad;
  data->recording_queue_sinkpad = recording_queue_sinkpad;
  data->recording_enabled = FALSE;
  data->recording_dir = NULL;

  return TRUE;
}

static gchar *make_filesink_dir(gchar *dir) {
  GDateTime *date;
  gchar *date_str;
  gchar *filesink_dir;

  date = g_date_time_new_now_utc ();
  date_str = g_date_time_format (date, "%Y-%m-%d-%H-%M-%S-utc");
  g_date_time_unref (date);

  filesink_dir = g_strdup_printf ("%s/VideoRecording-%s.flv", dir, date_str);
  g_free(date_str);

  return filesink_dir;
}

static void notify_worker_update_pipeline (CustomData *data, guint cmd) {
  g_async_queue_push (data->worker_cmd_queue, (gpointer)&worke_cmd[cmd]);
}

static gboolean display_start (CustomData *data) {
  GstElement *overlay_sink;
  gboolean ret = FALSE;

  alogi ("display start (ref:%d)!", data->pipeline_ref);
  do {
    g_mutex_lock (&data->mutex_branch);

    if (!data->native_window)
      break;

    if (data->display_enabled != BRANCH_DISABLE)
      break;

    if (data->pipeline_restarting)
      break;

    if (data->pipeline_ref == 0) {
      if (!setup_rtspsrc_elements (data))
        break;
      g_object_set (G_OBJECT(data->rtspsrc), "location", data->rtspsrc_url, NULL);
    }

    if (!setup_display_elements (data)) {
      cleanup_rtspsrc_elements (data);
      break;
    }

    gst_pad_link (data->tee_srcpad_display, data->display_queue_sinkpad);
    gst_elements_set_locked_state_v (data->display_elements, FALSE);

    if (data->pipeline_ref == 0) {
      gst_element_set_state (data->pipeline, GST_STATE_READY);
      gst_element_get_state (data->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    } else {
      gst_elements_set_state_v (data->display_elements, GST_STATE_READY);
      gst_element_get_state (data->display_elements[DP_VIDEOSINK], NULL, NULL, GST_CLOCK_TIME_NONE);
    }

    gst_element_get_state (data->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    overlay_sink = gst_bin_get_by_interface (GST_BIN(data->pipeline),
            GST_TYPE_VIDEO_OVERLAY);
    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (overlay_sink),
            (guintptr) data->native_window);
    gst_object_unref (overlay_sink);

    if (data->pipeline_ref == 0)
      gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    else
      gst_element_sync_state_with_parent_v (data->display_elements);

    data->display_enabled = BRANCH_ENABLE;
    data->pipeline_ref++;
    ret = TRUE;
  } while(0);

  alogi ("display start (ref:%d)! end", data->pipeline_ref);
  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static gboolean display_stop (CustomData *data) {
  gboolean ret = FALSE;
  alogi ("display stop (ref:%d)!", data->pipeline_ref);

  do {
    g_mutex_lock (&data->mutex_branch);

    if (data->display_enabled != BRANCH_ENABLE)
      break;

    data->display_enabled = BRANCH_DISABLE_ING;
    if (data->pipeline_ref == 1) {
      gst_element_set_state (data->pipeline, GST_STATE_NULL);
      gst_element_get_state (data->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    } else {
      gst_elements_set_locked_state_v (data->display_elements, TRUE);
      gst_elements_set_state_v (data->display_elements, GST_STATE_NULL);
      gst_element_get_state (data->display_elements[DP_VIDEOSINK], NULL, NULL, GST_CLOCK_TIME_NONE);
    }

    gst_pad_unlink (data->tee_srcpad_display, data->display_queue_sinkpad);
    cleanup_display_elements (data);

    if (data->pipeline_ref == 1)
      cleanup_rtspsrc_elements (data);

    data->display_enabled = BRANCH_DISABLE;
    data->pipeline_ref--;
    ret = TRUE;
  } while(0);

  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static gboolean push_rtmp_start (CustomData *data) {
  gboolean ret = FALSE;
  alogi ("push rtmp start (ref:%d)!", data->pipeline_ref);

  do {
    g_mutex_lock (&data->mutex_branch);

    if (data->push_rtmp_enabled != BRANCH_DISABLE)
      break;

    if (data->pipeline_restarting)
      break;

    if (data->pipeline_ref == 0) {
      if (!setup_rtspsrc_elements (data))
        break;
      g_object_set (G_OBJECT(data->rtspsrc), "location", data->rtspsrc_url, NULL);
    }

    if (!setup_push_rtmp_elements (data)) {
      cleanup_rtspsrc_elements (data);
      break;
    }

    aloge ("%s", data->push_rtmp_url);
    g_object_set (G_OBJECT(data->push_rtmp_elements[PU_RTMPSINK]),
                "location", data->push_rtmp_url, NULL);
    gst_pad_link (data->tee_srcpad_push_rtmp, data->push_rtmp_queue_sinkpad);
    gst_elements_set_locked_state_v (data->push_rtmp_elements, FALSE);

    if (data->pipeline_ref == 0)
      gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    else
      gst_element_sync_state_with_parent_v (data->push_rtmp_elements);

    data->push_rtmp_enabled = BRANCH_ENABLE;
    data->pipeline_ref++;
    ret = TRUE;
  } while (0);

  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static gboolean push_rtmp_stop (CustomData *data) {
  gboolean ret = FALSE;

  alogi ("push rtmp stop (ref:%d)!", data->pipeline_ref);
  do {
    g_mutex_lock (&data->mutex_branch);

    if (data->push_rtmp_enabled != BRANCH_ENABLE)
      break;

    data->push_rtmp_enabled = BRANCH_DISABLE_ING;
    if (data->pipeline_ref == 1) {
      gst_element_set_state (data->pipeline, GST_STATE_NULL);
      gst_element_get_state (data->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
    } else {
      gst_elements_set_locked_state_v (data->push_rtmp_elements, TRUE);
      gst_elements_set_state_v (data->push_rtmp_elements, GST_STATE_NULL);
    }

    gst_pad_unlink (data->tee_srcpad_push_rtmp, data->push_rtmp_queue_sinkpad);
    cleanup_push_rtmp_elements (data);

    if (data->pipeline_ref == 1)
      cleanup_rtspsrc_elements (data);

    data->push_rtmp_enabled = BRANCH_DISABLE;
    data->pipeline_ref--;
    ret = TRUE;
  } while (0);

  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static gboolean push_rtsp_start (CustomData *data) {
  gboolean ret = FALSE;

  alogi ("push rtsp start (ref:%d)!", data->pipeline_ref);
  do {
    g_mutex_lock (&data->mutex_branch);

    if (data->push_rtsp_enabled != BRANCH_DISABLE)
      break;

    if (data->pipeline_restarting)
      break;

    if (data->pipeline_ref == 0) {
      if(!setup_rtspsrc_elements (data))
        break;
      g_object_set (G_OBJECT(data->rtspsrc), "location", data->rtspsrc_url, NULL);
    }

    if (!setup_push_rtsp_elements (data)) {
      cleanup_rtspsrc_elements (data);
      break;
    }

    g_object_set (G_OBJECT(data->push_rtsp_elements[PU_RTSPSINK]),
            "location", data->push_rtsp_url, NULL);
    gst_pad_link (data->tee_srcpad_push_rtsp, data->push_rtsp_queue_sinkpad);
    gst_elements_set_locked_state_v (data->push_rtsp_elements, FALSE);

    if (data->pipeline_ref == 0)
      gst_element_set_state (data->pipeline, GST_STATE_PLAYING);
    else
      gst_element_sync_state_with_parent_v (data->push_rtsp_elements);

    data->push_rtsp_enabled = BRANCH_ENABLE;
    data->pipeline_ref++;
    ret = TRUE;
  } while (0);

  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static GstPadProbeReturn probe_push_rtsp_stop (GstPad *pad, GstPadProbeInfo *info, gpointer _data) {
  CustomData *data = (CustomData *)_data;

  alogi ("probe_push_rtsp_stop");
  gst_pad_unlink (data->tee_srcpad_push_rtsp, data->push_rtsp_queue_sinkpad);
  gst_elements_set_locked_state_v (data->push_rtsp_elements, TRUE);
  gst_pad_send_event(data->push_rtsp_queue_sinkpad, gst_event_new_eos());

  return GST_PAD_PROBE_REMOVE;
}

static gboolean push_rtsp_stop (CustomData *data) {
  gboolean ret = FALSE;
  gboolean signal;
  gint64 end_time;

  alogi ("push_rtsp_stop (ref:%d)!", data->pipeline_ref);
  do {
    g_mutex_lock (&data->mutex_branch);

    if (data->push_rtsp_enabled != BRANCH_ENABLE)
      break;

    data->push_rtsp_enabled = BRANCH_DISABLE_ING;
    gst_pad_add_probe (data->tee_srcpad_push_rtsp, GST_PAD_PROBE_TYPE_IDLE,
            probe_push_rtsp_stop, data, NULL);

    end_time = g_get_monotonic_time () + 900 * G_TIME_SPAN_MILLISECOND;
    signal = g_cond_wait_until (&data->push_rtsp_cond_eos, &data->mutex_branch, end_time);
    if(!signal)
      gst_pad_unlink (data->tee_srcpad_push_rtsp, data->push_rtsp_queue_sinkpad);

    if (data->pipeline_ref == 1) {
      if (signal)
        gst_elements_set_locked_state_v (data->push_rtsp_elements, TRUE);
      gst_element_set_state (data->pipeline, GST_STATE_NULL);
      gst_element_get_state (data->pipeline, NULL, NULL, GST_CLOCK_TIME_NONE);
      cleanup_push_rtsp_elements (data);
      cleanup_rtspsrc_elements (data);
    } else {
      gst_elements_set_locked_state_v (data->push_rtsp_elements, TRUE);
      if (signal) {
        gst_elements_set_state_v (data->push_rtsp_elements, GST_STATE_NULL);
        gst_element_get_state (data->push_rtsp_elements[PU_RTSPSINK], NULL, NULL, GST_CLOCK_TIME_NONE);
      }
      cleanup_push_rtsp_elements (data);
    }

    data->push_rtsp_enabled = BRANCH_DISABLE;
    data->pipeline_ref--;
    ret = TRUE;
  } while(0);

  alogi ("push_rtsp_stop (ref:%d)! end", data->pipeline_ref);
  g_mutex_unlock (&data->mutex_branch);
  return ret;
}

static gboolean recording_start (CustomData *data, const gchar *recording_dir) {
  gboolean str_equ;
  gchar *filesink_dir;

  alogi ("recording start (ref:%d)!", data->pipeline_ref);
  if (!recording_dir)
    return FALSE;

  str_equ = g_strcmp0 (data->recording_dir, recording_dir) == 0;
  if (str_equ && data->recording_enabled) {
    return FALSE;
  }

  alogi ("recording start!!");
  if (data->recording_dir) {
    g_free (data->recording_dir);
    data->recording_dir = NULL;
  }
  data->recording_dir = g_strdup (recording_dir);

  if (data->recording_enabled) {
    gst_elements_set_locked_state_v (data->recording_elements, TRUE);
    gst_elements_set_state_v (data->recording_elements, GST_STATE_NULL);
  } else {
    data->pipeline_ref++;
    data->recording_enabled = TRUE;
    gst_pad_link (data->tee_srcpad_recording, data->recording_queue_sinkpad);
  }

  filesink_dir = make_filesink_dir (data->recording_dir);
  g_object_set (G_OBJECT(data->recording_elements[RC_FILESINK]),
      "location", filesink_dir, NULL);
  g_free (filesink_dir);
  gst_elements_set_locked_state_v (data->recording_elements, FALSE);

  if (data->pipeline_ref == 1)
    ;//pipeline_enable (data, TRUE);
  else
    gst_element_sync_state_with_parent_v (data->recording_elements);

  return TRUE;
}

static void recording_stop (CustomData *data) {
  alogi ("recording stop (ref:%d)!", data->pipeline_ref);

  if (data->recording_enabled) {
    data->recording_enabled = FALSE;
    data->pipeline_ref--;
    gst_elements_set_locked_state_v (data->recording_elements, TRUE);

    if (data->pipeline_ref < 1)
      ;//pipeline_enable (data, FALSE);
    else
      gst_elements_set_state_v (data->recording_elements, GST_STATE_NULL);

    gst_pad_unlink (data->tee_srcpad_recording, data->recording_queue_sinkpad);
  }
}

static void display_update_native_surface (CustomData *data, ANativeWindow *native_window) {
  if (!data->display_requst) {
    if (data->native_window)
      ANativeWindow_release (data->native_window);
    data->native_window = native_window;
    return;
  }

  if (!native_window) {
    if (data->native_window)
      ANativeWindow_release (data->native_window);
    display_stop (data);
    data->native_window = native_window;
    return;
  }

  g_mutex_lock (&data->mutex_branch);
  if (data->display_enabled == BRANCH_ENABLE) {
    if (data->native_window == native_window) {
      ANativeWindow_release (data->native_window);
      gst_video_overlay_expose (GST_VIDEO_OVERLAY (data->pipeline));
      gst_video_overlay_expose (GST_VIDEO_OVERLAY (data->pipeline));
      g_mutex_unlock (&data->mutex_branch);
      return;
    }

    g_mutex_unlock (&data->mutex_branch);
    display_stop (data);

    g_mutex_lock (&data->mutex_branch);
    ANativeWindow_release (data->native_window);
  }

  data->native_window = native_window;
  g_mutex_unlock (&data->mutex_branch);

  if (data->native_window)
    display_start (data);
}

static gboolean launch_restart_process (CustomData *data, guchar reset_request) {

  if ((data->reset_request & reset_request) == reset_request)
    return FALSE;

  data->reset_request |= reset_request;
  notify_worker_update_pipeline (data, WORKER_CMD_RESET_PIPELINE);

  return TRUE;
}

static void check_media_size (CustomData *data);
static void message_state_changed_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstState old_state, new_state, pending_state;

  gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
  //alogv("message_state_changed_cb: %s: %s", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)),
  //        gst_element_state_get_name(new_state));

  if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline)) {
    alogi("message_state_changed_cb: pipeline: %s->%s",
            gst_element_state_get_name(old_state), gst_element_state_get_name(new_state));
    if (old_state == GST_STATE_PAUSED && new_state == GST_STATE_PLAYING)
      check_media_size (data);
  }
}

static void message_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;


  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_WARNING) {
    gst_message_parse_warning (msg, &err, &debug_info);
    alogi ("message_cb: %s: %s : %s: %s", GST_OBJECT_NAME (msg->src),
            GST_MESSAGE_TYPE_NAME (msg), err->message, debug_info);

    g_clear_error (&err);
    g_free (debug_info);
  } else if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ELEMENT) {
    const GstStructure *s;
    gchar *ss;

    s = gst_message_get_structure (msg);
    ss = gst_structure_to_string (s);

    alogi ("message_cb: %s: %s :%s", GST_OBJECT_NAME (msg->src),
            GST_MESSAGE_TYPE_NAME (msg), ss);
    g_free (ss);

    GstMessage *orig;
    gst_structure_get (s, "message", GST_TYPE_MESSAGE, &orig, NULL);
    s = gst_message_get_structure (orig);
    ss = gst_structure_to_string (s);

     alogi ("message_cb: %s: %s :%s", GST_OBJECT_NAME (orig->src),
             GST_MESSAGE_TYPE_NAME (orig), ss);
     g_free (ss);
  } else
    alogi ("message_cb: %s: %s", GST_OBJECT_NAME (msg->src), GST_MESSAGE_TYPE_NAME (msg));

}

static void message_error_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GError *err;
  gchar *debug_info;

  gst_message_parse_error (msg, &err, &debug_info);
  aloge ("message_error_cb: %s: %s %s", GST_OBJECT_NAME (msg->src), err->message, debug_info);

  do {
    if (!g_strcmp0 (GST_OBJECT_NAME (msg->src), "prtmp2-rtmpsink")) {
      if (g_strcmp0 (err->message, "Could not open resource for writing.") == 0) {
        aloge("message_error_cb: shutdown push rtmp");
        set_usr_message (USR_MESSAGE_PUSH_RTMP_SHUTDOWN, data);
        notify_worker_update_pipeline (data, WORKER_CMD_STOP_PUSH_RTMP);
        break;
      }
    } else if (!g_strcmp0 (GST_OBJECT_NAME (msg->src), "prtsp1-rtspclientsink")) {
      aloge("message_error_cb: shutdown push rtsp");
      set_usr_message (USR_MESSAGE_PUSH_RTSP_SHUTDOWN, data);
      data->push_rtsp_request = FALSE;
      notify_worker_update_pipeline (data, WORKER_CMD_STOP_PUSH_RTSP);
      break;
    } else if (!g_strcmp0 (GST_OBJECT_NAME (msg->src), "rtspsrc")) {
      if ((data->pipeline_ref == 1) && (data->display_enabled == BRANCH_DISABLE_ING)) {
        if (!g_strcmp0 (err->message, "Unhandled error") ||
            !g_strcmp0 (err->message, "Could not write to resource.")) {
          alogi ("message_error_cb: pipeline in stoping state, ignore this!(cause by PAUSE/TEARDOWN)");
          break;
        }
      }
    }

    if (launch_restart_process (data, RESET_REQUEST_PIPELINE))
      set_usr_message (USR_MESSAGE_RTSP_SRC_ERR_RESTART, data);
    else
      aloge("message_error_cb: alreadly in restart pipeline process");
  } while (0);


  g_clear_error (&err);
  g_free (debug_info);
}

static void message_element_cb (GstBus *bus, GstMessage *msg, CustomData *data) {
  GstMessage *orig;
  const GstStructure *s;
  gchar *ss;

  s = gst_message_get_structure (msg);
  if (gst_structure_has_name (s, "GstBinForwarded")) {

    gst_structure_get (s, "message", GST_TYPE_MESSAGE, &orig, NULL);
    if (g_strcmp0 (GST_OBJECT_NAME (orig->src), push_rtsp_vector[PU_RTSPSINK].name) == 0) {

      //handle RTSPSINK message
      if (GST_MESSAGE_TYPE (orig) == GST_MESSAGE_EOS) {
        //EOS message
        s = gst_message_get_structure (orig);
        ss = gst_structure_to_string (gst_message_get_structure (orig));
        g_free (ss);
        alogi ("message_element_cb push_rtsp_cond_eos ");
        g_cond_signal (&data->push_rtsp_cond_eos);
      } //end of EOS message
    } //end of handle  RTSPSINK message

  } //end of GstBinForwarded
}

static void cleanup_main_loop (CustomData *data) {
  g_mutex_clear (&data->mutex_branch);

  if (data->main_loop) {
    g_main_loop_unref (data->main_loop);
    data->main_loop = NULL;
  }
}

//this will print all message on the bus
static gboolean my_bus_callback (GstBus *bus, GstMessage *msg, gpointer data) {

  alogi ("My bus callback Got %s message:%s\n", GST_MESSAGE_TYPE_NAME (msg),
          GST_OBJECT_NAME (msg->src));

  return TRUE;
}


static gboolean setup_main_loop (CustomData *data) {
  GstElement *pipeline = NULL;
  GstBus *bus = NULL;
  GSource *bus_source = NULL;

  if (!data) {
    aloge ("setup_main_loop: Parameter error!");
    return FALSE;
  }

  pipeline = gst_pipeline_new ("rtspclient-pipline");
  if (!pipeline) {
    aloge ("setup_main_loop: create pipeline failed!");
    return FALSE;
  }
  g_object_set (pipeline, "message-forward", TRUE, NULL);

  bus = gst_element_get_bus (pipeline);
  bus_source = gst_bus_create_watch (bus);

  //g_source_set_priority (bus_source, G_PRIORITY_HIGH);
  g_source_set_callback (bus_source, (GSourceFunc) gst_bus_async_signal_func, NULL, NULL);
  g_source_attach (bus_source, data->context);
  g_source_unref (bus_source);

  //gst_bus_add_watch (G_OBJECT (bus), my_bus_callback, NULL);

  //g_signal_connect (G_OBJECT (bus), "message", (GCallback)message_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)message_error_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::state-changed", (GCallback)message_state_changed_cb, data);
  g_signal_connect (G_OBJECT (bus), "message::element", (GCallback)message_element_cb, data);
  gst_object_unref (bus);

  g_mutex_init (&data->mutex_branch);

  data->main_loop = g_main_loop_new (data->context, FALSE);
  if (!data->main_loop) {
    aloge ("setup_main_loop: new a main loop failed!");
    gst_object_unref (pipeline);
    return FALSE;
  }

  data->pipeline = pipeline;
  data->pipeline_restarting = FALSE;
  data->pipeline_ref = 0;

  return TRUE;
}

/* These global variables cache values which are not changing during execution */
static pthread_t gst_app_thread;
static pthread_key_t current_jni_env;
static JavaVM *java_vm;
static jfieldID custom_data_field_id;
static jmethodID set_message_method_id;
static jmethodID on_gstreamer_initialized_method_id;
static jmethodID on_media_size_changed_method_id;

/*
 * Private methods
 */

/* Register this thread with the VM */
static JNIEnv *attach_current_thread (void) {
  JNIEnv *env;
  JavaVMAttachArgs args;

  alogi("Attaching thread %p", g_thread_self ());
  args.version = JNI_VERSION_1_4;
  args.name = NULL;
  args.group = NULL;

  if ((*java_vm)->AttachCurrentThread (java_vm, &env, &args) < 0) {
    aloge ("Failed to attach current thread");
    return NULL;
  }

  return env;
}

/* Unregister this thread from the VM */
static void detach_current_thread (void *env) {
  alogi ("Detaching thread %p", g_thread_self ());
  (*java_vm)->DetachCurrentThread (java_vm);
}

/* Retrieve the JNI environment for this thread */
static JNIEnv *get_jni_env (void) {
  JNIEnv *env;

  if ((env = pthread_getspecific (current_jni_env)) == NULL) {
    env = attach_current_thread ();
    pthread_setspecific (current_jni_env, env);
  }

  return env;
}

static void gst_element_unref_v (GstElement **el_v) {
  int i = 0;

  while (el_v[i] != NULL) {
    gst_object_unref (el_v[i]);
  }
}

/* message to USR */
static void set_usr_message (const gchar *message, CustomData *data) {
  JNIEnv *env = get_jni_env ();
  alogd ("Setting message to: %s", message);

  jstring jmessage = (*env)->NewStringUTF(env, message);
  (*env)->CallVoidMethod (env, data->app, set_message_method_id, jmessage);

  if ((*env)->ExceptionCheck (env)) {
    aloge ("Failed to call Java method");
    (*env)->ExceptionClear (env);
  }

  (*env)->DeleteLocalRef (env, jmessage);
}

/* Retrieve the video sink's Caps and tell the application about the media size */
static void check_media_size (CustomData *data) {
  JNIEnv *env = get_jni_env ();
  GstElement *video_overlay_sink;
  GstPad *video_sink_pad;
  GstCaps *caps;
  GstVideoInfo info;

  /* Retrieve the Caps at the entrance of the video sink */
  video_overlay_sink = gst_bin_get_by_interface (GST_BIN(data->pipeline), GST_TYPE_VIDEO_OVERLAY);
  video_sink_pad = gst_element_get_static_pad (video_overlay_sink, "sink");
  gst_object_unref (video_overlay_sink);
  caps = gst_pad_get_current_caps (video_sink_pad);

  if (gst_video_info_from_caps (&info, caps)) {
    info.width = info.width * info.par_n / info.par_d;
    alogi ("Media size is %dx%d, notifying application", info.width, info.height);

    (*env)->CallVoidMethod (env, data->app, on_media_size_changed_method_id, (jint)info.width, (jint)info.height);
    if ((*env)->ExceptionCheck (env)) {
      aloge ("Failed to call Java method");
      (*env)->ExceptionClear (env);
    }
  }

  gst_caps_unref (caps);
  gst_object_unref (video_sink_pad);
}

static gint64 reset_latency_calculation(void) {
  static gint64 old = 0;
  static guint64 count = 0;
  gint64 new;

  new = g_get_monotonic_time ();
  if ((new - old) < G_TIME_SPAN_SECOND)
    count ++;
  else
    count = 0;

  if (count > 4)
      count = 4;
  old = new;

  return G_TIME_SPAN_MILLISECOND * 500 * (1 < count);

}

static void *worker_function (void *userdata) {
  CustomData *data = (CustomData *) userdata;
  guchar reset_request;
  guchar do_reset_request;
  gint64 end_time;
  const _worker_cmd *cmd = NULL;

  do {
    while (!cmd)
      cmd = (const _worker_cmd *)g_async_queue_pop (data->worker_cmd_queue);

    if (!data->worker_run)
      break;

    switch (cmd->index) {
      case WORKER_CMD_START_DISPLAY:
        data->display_requst = TRUE;
        cmd = NULL;
        break;
      case WORKER_CMD_STOP_DISPLAY:
        data->display_requst = FALSE;
        cmd = NULL;
        break;
      case WORKER_CMD_START_PUSH_RTSP:
        data->push_rtsp_request = TRUE;
        cmd = NULL;
        break;
      case WORKER_CMD_STOP_PUSH_RTSP:
        data->push_rtsp_request = FALSE;
        cmd = NULL;
        break;
      case WORKER_CMD_START_PUSH_RTMP:
        data->push_rtmp_request = TRUE;
        cmd = NULL;
        break;
      case WORKER_CMD_STOP_PUSH_RTMP:
        data->push_rtmp_request = FALSE;
        cmd = NULL;
        break;
      case WORKER_CMD_RESET_PIPELINE:
        reset_request = 0;
        data->pipeline_restarting = TRUE;
        do {
          do_reset_request = data->reset_request & (~reset_request);
          reset_request = data->reset_request;
          if (!reset_request)
              break;

          if (do_reset_request & RESET_REQUEST_DISPLAY)
            display_stop (data);

          if (do_reset_request & RESET_REQUEST_PRTMP)
            push_rtmp_stop (data);

          if (do_reset_request & RESET_REQUEST_PRTSP)
            push_rtsp_stop (data);

          if (!data->worker_run)
            break;

          end_time = reset_latency_calculation();
          cmd = g_async_queue_timeout_pop (data->worker_cmd_queue, end_time);
        } while(cmd && cmd->index == WORKER_CMD_RESET_PIPELINE);
        data->pipeline_restarting = FALSE;
        data->reset_request = RESET_REQUEST_NULL;
        break;
      default:
        break;
    }

    if (!data->worker_run)
      break;

    if (!data->display_requst && (data->display_enabled == BRANCH_ENABLE))
      display_stop (data);

    if (!data->push_rtmp_request && (data->push_rtmp_enabled == BRANCH_ENABLE))
      push_rtmp_stop (data);

    if (!data->push_rtsp_request && (data->push_rtsp_enabled == BRANCH_ENABLE))
      push_rtsp_stop (data);

    if (data->display_requst && (data->display_enabled == BRANCH_DISABLE))
      display_start (data);

    if (data->push_rtmp_request && (data->push_rtmp_enabled == BRANCH_DISABLE))
      push_rtmp_start (data);

    if (data->push_rtsp_request && (data->push_rtsp_enabled == BRANCH_DISABLE))
      push_rtsp_start (data);

  } while (data->worker_run);

  return NULL;
}

/* Main method for the native code. This is executed on its own thread. */
static void *app_function (void *userdata) {
  JNIEnv *env = NULL;
  CustomData *data = NULL;
  GMainContext *context = NULL;

  env = get_jni_env ();
  data = (CustomData *)userdata;

  /* Create our own GLib Main Context and make it the default one */
  context = g_main_context_new ();
  g_main_context_push_thread_default (context);

  data->worker_cmd_queue = g_async_queue_new ();
  data->native_window = NULL;
  data->rtspsrc_url = NULL;
  data->display_requst = FALSE;
  data->display_enabled = BRANCH_DISABLE;
  data->push_rtmp_request = FALSE;
  data->push_rtmp_enabled = BRANCH_DISABLE;
  data->push_rtmp_url = NULL;
  data->push_rtsp_request = FALSE;
  data->push_rtsp_enabled = BRANCH_DISABLE;
  data->push_rtsp_url = NULL;
  data->reset_request = RESET_REQUEST_NULL;
  data->worker_run = TRUE;

  data->context = context;
  setup_main_loop (data);

  pthread_create (&(data->gst_worker_thread), NULL, &worker_function, data);

  alogi ("app func: Entering main loop... (CustomData:%p)", data);
  g_main_loop_run (data->main_loop);
  alogi ("app func: Exited main loop");

  data->worker_run = FALSE;
  notify_worker_update_pipeline (data, WORKER_CMD);
  pthread_join (data->gst_worker_thread, NULL);
  g_async_queue_unref (data->worker_cmd_queue);

  /* Free resources */
  //cleanup_recording_elements (data);
  cleanup_push_rtsp_elements (data);
  cleanup_push_rtmp_elements (data);
  cleanup_display_elements (data);

  if (data->rtspsrc_url)
    g_free (data->rtspsrc_url);

  if (data->push_rtmp_url)
    g_free (data->push_rtmp_url);

  if (data->push_rtsp_url)
    g_free (data->push_rtsp_url);

  cleanup_main_loop (data);

  if (context) {
    g_main_context_pop_thread_default (context);
    g_main_context_unref (context);
  }

  return NULL;
}

/*
 * Java Bindings
 */

static char GST_LEVEL_STRING[GST_LEVEL_COUNT][PROP_NAME_MAX] = {
    "GST_LEVEL_NONE",
    "GST_LEVEL_ERROR",
    "GST_LEVEL_WARNING",
    "GST_LEVEL_FIXME",
    "GST_LEVEL_INFO",
    "GST_LEVEL_DEBUG",
    "GST_LEVEL_LOG",
    "GST_LEVEL_TRACE",
    "GST_LEVEL_MEMDUMP",
};

static void set_gst_debuglevel_from_prop(GstDebugLevel level) {
  char prop[PROP_VALUE_MAX];
  gchar **str, **str1;
  int len;

  if (level >= GST_LEVEL_COUNT)
      return;

  memset (prop, 0, PROP_VALUE_MAX);
  if (__system_property_get(GST_LEVEL_STRING[level], prop) < 1)
      return;

  str = g_strsplit (prop, ",", 10);
  for (str1 = str; *str1; str1++) {
    gst_debug_set_threshold_for_name (*str1, level);
    alogi ("set %s in %s", *str1, GST_LEVEL_STRING[level]);
  }

  if (str)
      g_strfreev (str);
}

/* Instruct the native code to create its internal data structure, pipeline and thread */
static jboolean gst_native_init (JNIEnv* env, jobject thiz) {
  char prop[PROP_VALUE_MAX];
  int len;

  CustomData *data = g_new0 (CustomData, 1);
  if (!data) {
      aloge ("gst_native_init alloc custdata failed");
      return JNI_FALSE;
  }
  memset (data, 0, sizeof(CustomData));

  //GST_LEVEL_DEFAULT
  memset (prop, 0, PROP_VALUE_MAX);
  len = __system_property_get("persist.gst.debug.level", prop);
  if (len > 0) {
    if (!g_strcmp0 ("ERROR", prop)) {
      gst_debug_set_default_threshold (GST_LEVEL_ERROR);
    } else if (!g_strcmp0 ("WARNING", prop)){
      gst_debug_set_default_threshold (GST_LEVEL_WARNING);
    } else if (!g_strcmp0 ("INFO", prop)) {
      gst_debug_set_default_threshold (GST_LEVEL_INFO);
    } else if (!g_strcmp0 ("DEBUG", prop)) {
      gst_debug_set_default_threshold (GST_LEVEL_DEBUG);
    } else if (!g_strcmp0 ("TRACE", prop))
      gst_debug_set_default_threshold (GST_LEVEL_TRACE);
  }

  GST_DEBUG_CATEGORY_INIT (SongRTSPClientJNIG_category, GTAG, 0, "Android FishSemi RTSP Client");
  gst_debug_set_threshold_for_name (GTAG, GST_LEVEL_INFO);

  set_gst_debuglevel_from_prop (GST_LEVEL_ERROR);
  set_gst_debuglevel_from_prop (GST_LEVEL_WARNING);
  set_gst_debuglevel_from_prop (GST_LEVEL_INFO);
  set_gst_debuglevel_from_prop (GST_LEVEL_DEBUG);
  set_gst_debuglevel_from_prop (GST_LEVEL_TRACE);

  //gst_debug_set_threshold_for_name ("rtspsrc", GST_LEVEL_DEBUG);
  //gst_debug_set_threshold_for_name ("rtmpsink", GST_LEVEL_TRACE);
  //gst_debug_set_threshold_for_name ("flvmux", GST_LEVEL_LOG);
  //gst_debug_set_threshold_for_name ("queue", GST_LEVEL_TRACE);
  //gst_debug_set_threshold_for_name ("glimagesink", GST_LEVEL_TRACE);
  //gst_debug_set_threshold_for_name ("rtspclientsink", GST_LEVEL_DEBUG);

  data->app = (*env)->NewGlobalRef (env, thiz);
  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, data);
  pthread_create (&(data->gst_app_thread), NULL, &app_function, data);

  return JNI_TRUE;
}

static void gst_native_surface_finalize (JNIEnv *env, jobject thiz);

/* Quit the main loop, remove the native thread and free resources */
static void gst_native_finalize (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);

  alogi ("Quitting main loop...iiiiiii");
  if (!data) return;

  gst_native_surface_finalize (env, thiz);

  alogi ("Quitting main loop...");
  g_main_loop_quit (data->main_loop);

  alogi ("Waiting for thread to finish...");
  pthread_join (data->gst_app_thread, NULL);

  alogi ("Deleting GlobalRef for app object at %p", data->app);
  (*env)->DeleteGlobalRef (env, data->app);

  if (data->native_window)
    ANativeWindow_release (data->native_window);

  alogi ("Freeing CustomData at %p", data);
  g_free (data);

  SET_CUSTOM_DATA (env, thiz, custom_data_field_id, NULL);
  alogi ("Done finalizing");

}

static jboolean gst_native_play (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);

  alogi ("gst_native_play");
  if (!data)
    return JNI_FALSE;

  if (!data->rtspsrc_url)
      return JNI_FALSE;

  if (!data->native_window)
    return JNI_FALSE;

  notify_worker_update_pipeline (data, WORKER_CMD_START_DISPLAY);

  return JNI_TRUE;
}

static void gst_native_stop (JNIEnv* env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  GstElement *video_overlay_sink;

  alogi ("gst_native_stop");
  if (!data)
      return;

  notify_worker_update_pipeline (data, WORKER_CMD_STOP_DISPLAY);
}

static void gst_native_surface_init (JNIEnv *env, jobject thiz, jobject surface) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  ANativeWindow *new_native_window = ANativeWindow_fromSurface(env, surface);

  if (!data)
    return;

  alogi ("Received surface %p (native window %p)", surface, new_native_window);
  display_update_native_surface (data, new_native_window);
}

static void gst_native_surface_finalize (JNIEnv *env, jobject thiz) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);

  if (!data)
    return;

  alogi ("finalize surface");
  display_update_native_surface (data, NULL);
}

static jboolean gst_native_recording (JNIEnv* env, jobject thiz,
        jboolean enable, jstring dir) {
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  const gchar *recording_dir = (*env)->GetStringUTFChars (env, dir, NULL);

//  if (!data || !data->pipeline) {
//      aloge ("Recording : data or pipeline is null");
//      return JNI_FALSE;
//  }
//
//  if (enable == JNI_TRUE && (recording_dir != NULL) &&
//          (data->rtspsrc_url != NULL)) {
//    alogi ("start recording video stream to %s", recording_dir);
//    recording_start (data, recording_dir);
//    return JNI_TRUE;
//  } else if (enable == JNI_FALSE) {
//    recording_stop (data);
//    return JNI_TRUE;
//  }
//

  aloge ("Recording do not support now!");

  return JNI_FALSE;
}

static void gst_native_set_rtsp_url (JNIEnv* env, jobject thiz, jstring media_url) {
  CustomData *data;
  const gchar *_media_url;
  gboolean str_cmp;

  data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  _media_url = (*env)->GetStringUTFChars (env, media_url, NULL);

  if (!g_strcmp0 (data->rtspsrc_url, _media_url))
      return;

  if (data->rtspsrc_url) {
      g_free (data->rtspsrc_url);
      data->rtspsrc_url = NULL;
  }
  data->rtspsrc_url = g_strdup (_media_url);
}

static void gst_native_set_rtmp_url (JNIEnv* env, jobject thiz, jstring media_url) {
  CustomData *data;
  const gchar *_media_url;
  gboolean str_cmp;

  data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  _media_url = (*env)->GetStringUTFChars (env, media_url, NULL);
  str_cmp = g_strcmp0 (data->push_rtmp_url, _media_url) == 0;

  if (str_cmp)
      return;

  if (data->push_rtmp_url) {
      g_free (data->push_rtmp_url);
      data->push_rtmp_url = NULL;
  }

  data->push_rtmp_url = g_strdup (_media_url);
}

static jboolean gst_native_push_stream (JNIEnv* env, jobject thiz,
        jboolean enable, jstring url) {
  guint cmd;
  CustomData *data = GET_CUSTOM_DATA (env, thiz, custom_data_field_id);
  const gchar *stream_url = (*env)->GetStringUTFChars (env, url, NULL);

  if (!data || !data->pipeline) {
      aloge ("Push Stream : data or pipeline is null");
      return JNI_FALSE;
  }


  if (!data->rtspsrc_url) {
    alogi ("Push RTSP Stream: failed, rtsp (src) url is NULL");
    return JNI_FALSE;
  }

  if (!stream_url) {
    alogi ("Push Stream: failed, Push url is NULL");
    return JNI_FALSE;
  }

  if (g_str_has_prefix (stream_url, "rtmp")) {
    if (g_strcmp0 (data->push_rtmp_url, stream_url)) {
      if (data->push_rtmp_url)
        g_free (data->push_rtmp_url);
      data->push_rtmp_url = g_strdup (stream_url);
    }

    if (enable)
      cmd = WORKER_CMD_START_PUSH_RTMP;
    else
      cmd = WORKER_CMD_STOP_PUSH_RTMP;

  } else if (g_str_has_prefix (stream_url, "rtsp")) {
    if (g_strcmp0 (data->push_rtsp_url, stream_url)) {
      if (data->push_rtsp_url)
        g_free (data->push_rtsp_url);
      data->push_rtsp_url = g_strdup (stream_url);
    }

    if (enable)
      cmd = WORKER_CMD_START_PUSH_RTSP;
    else
      cmd = WORKER_CMD_STOP_PUSH_RTSP;
  }

  notify_worker_update_pipeline (data, cmd);

  return JNI_TRUE;
}


/* Static class initializer: retrieve method and field IDs */
static jboolean gst_native_class_init (JNIEnv* env, jclass klass) {
  custom_data_field_id = (*env)->GetFieldID (env, klass, "native_custom_data", "J");
  set_message_method_id = (*env)->GetMethodID (env, klass, "setMessage", "(Ljava/lang/String;)V");
  on_gstreamer_initialized_method_id = (*env)->GetMethodID (env, klass, "onGStreamerInitialized", "()V");
  on_media_size_changed_method_id = (*env)->GetMethodID (env, klass, "onMediaSizeChanged", "(II)V");

  if (!custom_data_field_id || !set_message_method_id || !on_gstreamer_initialized_method_id ||
      !on_media_size_changed_method_id ) {
    aloge ("The calling class does not implement all necessary interface methods");
    return JNI_FALSE;
  }
  return JNI_TRUE;
}

/* List of implemented native methods */
static JNINativeMethod native_methods[] = {
  { "nativeInit", "()Z", (void *) gst_native_init},
  { "nativeFinalize", "()V", (void *) gst_native_finalize},
  { "nativePlay", "()Z", (void *) gst_native_play},
  { "nativeStop", "()V", (void *) gst_native_stop},
  { "nativeSurfaceInit", "(Ljava/lang/Object;)V", (void *) gst_native_surface_init},
  { "nativeSurfaceFinalize", "()V", (void *) gst_native_surface_finalize},
  { "nativeRecording", "(ZLjava/lang/String;)Z", (void *) gst_native_recording},
  { "nativePushStream", "(ZLjava/lang/String;)Z", (void *) gst_native_push_stream},
  { "nativeSetRTSPURL", "(Ljava/lang/String;)V", (void *) gst_native_set_rtsp_url},
  { "nativeSetRTMPURL", "(Ljava/lang/String;)V", (void *) gst_native_set_rtmp_url},
  { "nativeClassInit", "()Z", (void *) gst_native_class_init}
};

/* Library initializer */
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  JNIEnv *env = NULL;
  java_vm = vm;

  if ((*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_4) != JNI_OK) {
    aloge ("Could not retrieve JNIEnv");
    return 0;
  }

  jclass klass = (*env)->FindClass (env, "com/fishsemi/sdk/aircontrol/VideoStream");
  (*env)->RegisterNatives (env, klass, native_methods, G_N_ELEMENTS(native_methods));

  pthread_key_create (&current_jni_env, detach_current_thread);

  return JNI_VERSION_1_4;
}
