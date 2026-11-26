#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta_schema.h"
#include "nvbufsurface.h"
#ifndef PLATFORM_TEGRA
  #include "gst-nvmessage.h"
#endif

#include <cuda_runtime_api.h>
#include <cuda.h>

// Detections are published to Redis for the web UI backend
#include <hiredis/hiredis.h>

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cmath>
#include <climits>
#include <cstdlib>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

using namespace std;
using namespace std::chrono;

#define SOURCE_PATH "inputsources.txt"

#define PERF_INTERVAL 2

#define MAX_DISPLAY_LEN 64

// Network Compute Mode
#define COMPUTE_MODE "fp32"

// Muxer Resolution
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout in microseconds. Should be about one frame
 * interval of the fastest live source: 40 ms suits 25-30 fps cameras.
 * (This was 4000000 = 4 s, which stalled the whole batch whenever one source lagged.) */
#define MUXER_BATCH_TIMEOUT_USEC 40000

// Tiles Resolution
#define TILED_OUTPUT_WIDTH 1920
#define TILED_OUTPUT_HEIGHT 1080

// RTSP output (re-streamed to browsers as WebRTC by MediaMTX, see mediamtx.yml)
#define DEFAULT_RTSP_PORT 8555
#define RTSP_MOUNT "/hermes"
#define RTSP_UDP_PORT 5400
#define RTSP_BITRATE 4000000

// Redis channel/keys shared with backend/hermes_backend.py
#define REDIS_DETECTIONS_CHANNEL "hermes:detections"
#define REDIS_PIPELINE_KEY "hermes:pipeline"

/* NVIDIA Decoder source pad memory feature. This feature signifies that source
 * pads having this capability will push GstBuffers containing cuda buffers. */
#define GST_CAPS_FEATURES_NVMM "memory:NVMM"

#define CHECK_ERROR(error) \
    if (error) { \
        g_printerr ("Error while parsing config file: %s\n", error->message); \
        goto done; \
    }

#define CONFIG_GROUP_TRACKER "tracker"
#define CONFIG_GROUP_TRACKER_WIDTH "tracker-width"
#define CONFIG_GROUP_TRACKER_HEIGHT "tracker-height"
#define CONFIG_GROUP_TRACKER_LL_CONFIG_FILE "ll-config-file"
#define CONFIG_GROUP_TRACKER_LL_LIB_FILE "ll-lib-file"
#define CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS "enable-batch-process"
#define CONFIG_GPU_ID "gpu-id"

enum PGIE_CLASS {FIRE = 0};

int num_sources = 0;

namespace WildFireDetection {
  class Hermes {
    private:
      inline static const char *class_names[1] = {"fire"};

      struct fps_calculator {
        system_clock::time_point fps_timer;
        system_clock::time_point display_timer;
        gint rolling_fps;
        gint display_fps;
      };

      inline static fps_calculator fps[16];

      inline static char *PGIE_YOLO_DETECTOR_CONFIG_FILE_PATH;

      inline static char *TRACKER_CONFIG_FILE;

      // Redis publisher (non-fatal: the pipeline keeps running if Redis is down)
      inline static redisContext *redis = NULL;
      inline static system_clock::time_point redis_retry_at;

    public:
      gboolean display_off;
      gboolean rtsp_off;
      gint rtsp_port;
      gchar *redis_host;
      gint redis_port;

      inline static std::string redis_host_s = "127.0.0.1";
      inline static int redis_port_i = 6379;

      std::vector<std::string> source_uris;

      GOptionEntry entries[6] = {
        {"no-display", 0, 0, G_OPTION_ARG_NONE, &display_off, "Disable the local display window", NULL},
        {"no-rtsp", 0, 0, G_OPTION_ARG_NONE, &rtsp_off, "Disable the RTSP output used by the web UI", NULL},
        {"rtsp-port", 0, 0, G_OPTION_ARG_INT, &rtsp_port, "RTSP output port (default 8555)", "PORT"},
        {"redis-host", 0, 0, G_OPTION_ARG_STRING, &redis_host, "Redis host for detections (default 127.0.0.1)", "HOST"},
        {"redis-port", 0, 0, G_OPTION_ARG_INT, &redis_port, "Redis port (default 6379)", "PORT"},
        {NULL}
      };

      std::string PGIE_YOLO_ENGINE_PATH;

      static void
      update_fps (gint id);

      int
      create_input_sources (gpointer pipe, gpointer mux, guint num_sources);

      static void
      changeBBoxColor (gpointer obj_meta_data, int has_bg_color, float red, float green,
                      float blue, float alpha);

      static void
      addDisplayMeta (gpointer batch_meta_data, gpointer frame_meta_data);

      static gboolean
      redis_ready ();

      static void
      publish_frame (NvDsFrameMeta *frame_meta);

      void
      publish_pipeline_info (guint rows, guint columns);

      static GstPadProbeReturn
      tiler_src_pad_buffer_probe (GstPad * pad, GstPadProbeInfo * info,
          gpointer u_data);

      static gboolean
      bus_call (GstBus * bus, GstMessage * msg, gpointer data);

      static void
      cb_newpad (GstElement * decodebin, GstPad * decoder_src_pad, gpointer data);

      static void
      decodebin_child_added (GstChildProxy *child_proxy, GObject *object,
                            gchar *name, gpointer user_data);

      static GstElement *
      create_source_bin (guint index, gchar * uri);

      static gchar *
      get_absolute_file_path (gchar *cfg_file_path, gchar *file_path);

      static gboolean
      set_tracker_properties (GstElement *nvtracker);

      static void
      set_if_exists (GstElement *element, const gchar *property, guint value);

      int
      configure_element_properties(int num_sources, GstElement *streammux, GstElement *pgie_yolo_detector,
                           GstElement *nvtracker, GstElement *sink, GstElement *tiler,
                           guint *rows, guint *columns);

      gboolean
      start_rtsp_server ();

      void setPaths(guint num_sources);

      Hermes() {

        int counter;
        for (counter = 0; counter < 16; counter++) {
          fps[counter].fps_timer = system_clock::now();
          fps[counter].display_timer = system_clock::now();
          fps[counter].rolling_fps = 0;
          fps[counter].display_fps = 0;
        }

        display_off = false;
        rtsp_off = false;
        rtsp_port = DEFAULT_RTSP_PORT;
        redis_host = NULL;
        redis_port = 6379;
        redis_retry_at = system_clock::now();
      }
      ~Hermes() {
        if (redis) {
          redisFree(redis);
          redis = NULL;
        }
      }
  };
}
