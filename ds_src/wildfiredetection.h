#include <gst/gst.h>
#include <glib.h>

#include "gstnvdsmeta.h"
#include "nvdsmeta_schema.h"
#include "nvbufsurface.h"
// #include "gstnvstreammeta.h"
#ifndef PLATFORM_TEGRA
  #include "gst-nvmessage.h"
#endif

#include <cuda_runtime_api.h>
#include <cuda.h>

// OpenCV for image operations
#include <opencv2/opencv.hpp>

// Asynchronous calls and timers
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <fstream>
#include <array>
#include <map>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <iostream>

#include <curl/curl.h>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

using namespace std;
using namespace std::chrono;
using namespace cv;

#define SOURCE_PATH "inputsources.txt"

#define PERF_INTERVAL 2

#define MAX_DISPLAY_LEN 64

// Network Compute Mode
#define COMPUTE_MODE "fp32"

#define MAX_TRACKING_ID_LEN 16

// Muxer Resolution
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 4000000

// Tiles Resolution
#define TILED_OUTPUT_WIDTH 1920
#define TILED_OUTPUT_HEIGHT 1080

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

enum GIE_UID {FIRE_DETECTOR = 1};

int num_sources = 0;

namespace WildFireDetection {
  class Hermes {
    private:
      gchar pgie_yolo_classes_str[1][10] = {
          "Fire"
        };

      struct fps_calculator {
        system_clock::time_point fps_timer;
        system_clock::time_point display_timer;
        gint rolling_fps;
        gint display_fps;
      };

      inline static fps_calculator fps[16];

      inline static char *PGIE_YOLO_DETECTOR_CONFIG_FILE_PATH;

      inline static char *TRACKER_CONFIG_FILE;

    public:
      // To save the frames
      gint frame_number;

      gboolean display_off;

      GOptionEntry entries[2] = {
        {"no-display", 0, 0, G_OPTION_ARG_NONE, &display_off, "Disable display", NULL},
        {NULL}
      };

      std::string PGIE_YOLO_ENGINE_PATH;

      static void
      update_fps (gint id);

      static int
      create_input_sources (gpointer pipe, gpointer mux, guint num_sources);

      size_t
      WriteCallback (char *contents, size_t size, size_t nmemb, void *userp);

      static void
      changeBBoxColor (gpointer obj_meta_data, int has_bg_color, float red, float green,
                      float blue, float alpha);

      static void
      addDisplayMeta (gpointer batch_meta_data, gpointer frame_meta_data);

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

      int
      configure_element_properties(int num_sources, GstElement *streammux, GstElement *pgie_yolo_detector,
                           GstElement *nvtracker, GstElement *sink, GstElement *tiler);

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
        frame_number = 0;
      }
      ~Hermes() {}
  };
}