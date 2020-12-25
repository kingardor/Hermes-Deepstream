#include "wildfiredetection.h"

namespace WildFireDetection {
  void
  Hermes::update_fps(gint id) {

    gdouble current_fps = duration_cast<std::chrono::milliseconds>(system_clock::now() - fps[id].fps_timer).count();
    current_fps /= 1000;
    current_fps = 1 / current_fps;
    fps[id].rolling_fps = (gint)(fps[id].rolling_fps * 0.7 + current_fps * 0.3);
    auto timer = duration_cast<std::chrono::seconds>(system_clock::now() - fps[id].display_timer).count();
    if (timer > PERF_INTERVAL) {
      fps[id].display_fps = fps[id].rolling_fps;
      fps[id].display_timer = system_clock::now();
    }
    fps[id].fps_timer = system_clock::now();
  }

  int
  Hermes::create_input_sources(gpointer pipe, gpointer mux, guint num_sources) {

    GstElement *pipeline = (GstElement *)pipe;
    GstElement *streammux = (GstElement *)mux;

    std::ifstream infile(SOURCE_PATH);
    std::string source;

    if (infile.is_open()) {
      while (getline(infile, source)) {
        GstPad *sinkpad, *srcpad;
        GstElement *source_bin = NULL;
        gchar pad_name[16] = {};

        source_bin = create_source_bin(num_sources, (gchar *)source.c_str());

        if (!source_bin) {
          g_printerr("Failed to create source bin. Exiting.\n");
          return -1;
        }

        gst_bin_add(GST_BIN(pipeline), source_bin);

        g_snprintf(pad_name, 15, "sink_%u", num_sources);
        sinkpad = gst_element_get_request_pad(streammux, pad_name);
        if (!sinkpad) {
          g_printerr("Streammux request sink pad failed. Exiting.\n");
          return -1;
        }

        srcpad = gst_element_get_static_pad(source_bin, "src");
        if (!srcpad) {
          g_printerr("Failed to get src pad of source bin. Exiting.\n");
          return -1;
        }

        if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
          g_printerr("Failed to link source bin to stream muxer. Exiting.\n");
          return -1;
        }

        gst_object_unref(srcpad);
        gst_object_unref(sinkpad);
        num_sources++;
      }
    }
    infile.close();
    return num_sources;
  }

  void
  Hermes::changeBBoxColor(gpointer obj_meta_data, int has_bg_color, float red, float green,
                  float blue, float alpha) {

    NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)obj_meta_data;
    #ifndef PLATFORM_TEGRA
      obj_meta->rect_params.has_bg_color = has_bg_color;
      obj_meta->rect_params.bg_color.red = red;
      obj_meta->rect_params.bg_color.green = green;
      obj_meta->rect_params.bg_color.blue = blue;
      obj_meta->rect_params.bg_color.alpha = alpha;
    #endif
    obj_meta->rect_params.border_color.red = red;
    obj_meta->rect_params.border_color.green = green;
    obj_meta->rect_params.border_color.blue = blue;
    obj_meta->rect_params.border_color.alpha = alpha;
    obj_meta->text_params.font_params.font_size = 14;
  }

  void
  Hermes::addDisplayMeta(gpointer batch_meta_data, gpointer frame_meta_data) {

    NvDsBatchMeta *batch_meta = (NvDsBatchMeta *)batch_meta_data;
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)frame_meta_data;

    // To access the data that will be used to draw
    NvDsDisplayMeta *display_meta = NULL;
    NvOSD_TextParams *txt_params = NULL;
    NvOSD_LineParams *line_params = NULL;

    int offset = 0;
    display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
    txt_params = display_meta->text_params;
    line_params = display_meta->line_params;
    display_meta->num_labels = 1;

    // if (txt_params->display_text)
    //   g_free (txt_params->display_text);
    txt_params->display_text = (char *)g_malloc0(MAX_DISPLAY_LEN);

    update_fps(frame_meta->source_id);

    offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Source: %d | FPS: %d | ",
                      frame_meta->source_id, fps[frame_meta->source_id].display_fps);

    /* Now set the offsets where the string should appear */
    txt_params->x_offset = 10;
    txt_params->y_offset = 12;

    /* Font , font-color and font-size */
    txt_params->font_params.font_name = (char *)"Serif";
    txt_params->font_params.font_size = 14;
    txt_params->font_params.font_color.red = 1.0;
    txt_params->font_params.font_color.green = 1.0;
    txt_params->font_params.font_color.blue = 1.0;
    txt_params->font_params.font_color.alpha = 1.0;

    /* Text background color */
    txt_params->set_bg_clr = 1;
    txt_params->text_bg_clr.red = 0.0;
    txt_params->text_bg_clr.green = 0.0;
    txt_params->text_bg_clr.blue = 0.0;
    txt_params->text_bg_clr.alpha = 1.0;

    nvds_add_display_meta_to_frame(frame_meta, display_meta);
  }

  GstPadProbeReturn
  Hermes::tiler_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info,
                            gpointer u_data) {
    GstBuffer *buf = (GstBuffer *)info->data;

    // To access the entire batch data
    NvDsBatchMeta *batch_meta = NULL;

    NvDsObjectMeta *obj_meta = NULL;
    NvDsFrameMeta *frame_meta = NULL;

    // To access the frame
    NvBufSurface *surface = NULL;
    // TO generate message meta
    NvDsEventMsgMeta *msg_meta = NULL;

    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;

    // Get original raw data
    GstMapInfo in_map_info;
    char *src_data = NULL;

    if (!gst_buffer_map(buf, &in_map_info, GST_MAP_READ)) {
      g_print("Error: Failed to map gst buffer\n");
      gst_buffer_unmap(buf, &in_map_info);
      return GST_PAD_PROBE_OK;
    }

    batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    if (!batch_meta) {
      return GST_PAD_PROBE_OK;
    }

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
        l_frame = l_frame->next) {
      frame_meta = (NvDsFrameMeta *)(l_frame->data);

      if (frame_meta == NULL) {
        // Ignore Null frame meta.
        continue;
      }

      guint person_count = 0;

      for (l_obj = frame_meta->obj_meta_list; l_obj != NULL;
          l_obj = l_obj->next) {

        obj_meta = (NvDsObjectMeta *)(l_obj->data);

        if (obj_meta == NULL) {
          // Ignore Null object.
          continue;
        }

        gint class_index = obj_meta->class_id;

        if(class_index == FIRE) {
          changeBBoxColor(obj_meta, 1, 1.0, 0.0, 0.0, 0.25);
        }

      }
      // Add Information to every stream
      addDisplayMeta(batch_meta, frame_meta);
    }
    gst_buffer_unmap(buf, &in_map_info);
    return GST_PAD_PROBE_OK;
  }

  gboolean
  Hermes::bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      g_print("End of stream\n");
      g_main_loop_quit(loop);
      break;
    case GST_MESSAGE_WARNING: {
      gchar *debug;
      GError *error;
      gst_message_parse_warning(msg, &error, &debug);
      g_printerr("WARNING from element %s: %s\n",
                GST_OBJECT_NAME(msg->src), error->message);
      g_free(debug);
      g_printerr("Warning: %s\n", error->message);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      gchar *debug;
      GError *error;
      gst_message_parse_error(msg, &error, &debug);
      g_printerr("ERROR from element %s: %s\n",
                GST_OBJECT_NAME(msg->src), error->message);
      if (debug)
        g_printerr("Error details: %s\n", debug);
      g_free(debug);
      g_error_free(error);
      g_main_loop_quit(loop);
      break;
    }
    #ifndef PLATFORM_TEGRA
      case GST_MESSAGE_ELEMENT: {
        if (gst_nvmessage_is_stream_eos(msg)) {
          guint stream_id;
          if (gst_nvmessage_parse_stream_eos(msg, &stream_id)) {
            g_print("Got EOS from stream %d\n", stream_id);
          }
        }
        break;
      }
    #endif
      default:
        break;
      }
      return TRUE;
  }

  void
  Hermes::cb_newpad(GstElement *decodebin, GstPad *decoder_src_pad, gpointer data) {
    g_print("In cb_newpad\n");
    GstCaps *caps = gst_pad_get_current_caps(decoder_src_pad);
    const GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);
    GstElement *source_bin = (GstElement *)data;
    GstCapsFeatures *features = gst_caps_get_features(caps, 0);

    /* Need to check if the pad created by the decodebin is for video and not
    * audio. */
    if (!strncmp(name, "video", 5)) {
      /* Link the decodebin pad only if decodebin has picked nvidia
      * decoder plugin nvdec_*. We do this by checking if the pad caps contain
      * NVMM memory features. */
      if (gst_caps_features_contains(features, GST_CAPS_FEATURES_NVMM)) {
        /* Get the source bin ghost pad */
        GstPad *bin_ghost_pad = gst_element_get_static_pad(source_bin, "src");
        if (!gst_ghost_pad_set_target(GST_GHOST_PAD(bin_ghost_pad),
                                      decoder_src_pad)) {
          g_printerr("Failed to link decoder src pad to source bin ghost pad\n");
        }
        gst_object_unref(bin_ghost_pad);
      }
      else {
        g_printerr("Error: Decodebin did not pick nvidia decoder plugin.\n");
      }
    }
  }

  void
  Hermes::decodebin_child_added(GstChildProxy *child_proxy, GObject *object, gchar *name, gpointer user_data) {
    g_print("Decodebin child added: %s\n", name);
    if (g_strrstr(name, "decodebin") == name) {
      g_signal_connect(G_OBJECT(object), "child-added",
                      G_CALLBACK(decodebin_child_added), user_data);
    }
    if (g_strstr_len(name, -1, "nvv4l2decoder") == name) {
      g_print("Seting bufapi_version\n");
      g_object_set(object, "bufapi-version", TRUE, NULL);
    }
  }

  GstElement *
  Hermes::create_source_bin(guint index, gchar *uri) {
    GstElement *bin = NULL, *uri_decode_bin = NULL;
    gchar bin_name[16] = {};

    g_snprintf(bin_name, 15, "source-bin-%02d", index);
    /* Create a source GstBin to abstract this bin's content from the rest of the
    * pipeline */
    bin = gst_bin_new(bin_name);

    /* Source element for reading from the uri.
    * We will use decodebin and let it figure out the container format of the
    * stream and the codec and plug the appropriate demux and decode plugins. */
    uri_decode_bin = gst_element_factory_make("uridecodebin", "uri-decode-bin");

    if (!bin || !uri_decode_bin) {
      g_printerr("One element in source bin could not be created.\n");
      return NULL;
    }

    /* We set the input uri to the source element */
    g_object_set(G_OBJECT(uri_decode_bin), "uri", uri, NULL);

    /* Connect to the "pad-added" signal of the decodebin which generates a
    * callback once a new pad for raw data has beed created by the decodebin */
    g_signal_connect(G_OBJECT(uri_decode_bin), "pad-added",
                    G_CALLBACK(cb_newpad), bin);
    g_signal_connect(G_OBJECT(uri_decode_bin), "child-added",
                    G_CALLBACK(decodebin_child_added), bin);

    gst_bin_add(GST_BIN(bin), uri_decode_bin);

    /* We need to create a ghost pad for the source bin which will act as a proxy
    * for the video decoder src pad. The ghost pad will not have a target right
    * now. Once the decode bin creates the video decoder and generates the
    * cb_newpad callback, we will set the ghost pad target to the video decoder
    * src pad. */
    if (!gst_element_add_pad(bin, gst_ghost_pad_new_no_target("src",
                                                              GST_PAD_SRC))) {
      g_printerr("Failed to add ghost pad in source bin\n");
      return NULL;
    }
    return bin;
  }

  gchar *
  Hermes::get_absolute_file_path(gchar *cfg_file_path, gchar *file_path) {
    gchar abs_cfg_path[PATH_MAX + 1];
    gchar *abs_file_path;
    gchar *delim;

    if (file_path && file_path[0] == '/') {
      return file_path;
    }

    if (!realpath(cfg_file_path, abs_cfg_path)) {
      g_free(file_path);
      return NULL;
    }

    // Return absolute path of config file if file_path is NULL.
    if (!file_path) {
      abs_file_path = g_strdup(abs_cfg_path);
      return abs_file_path;
    }

    delim = g_strrstr(abs_cfg_path, "/");
    *(delim + 1) = '\0';

    abs_file_path = g_strconcat(abs_cfg_path, file_path, NULL);
    g_free(file_path);

    return abs_file_path;
  }

  gboolean
  Hermes::set_tracker_properties(GstElement *nvtracker) {
    gboolean ret = FALSE;
    GError *error = NULL;
    gchar **keys = NULL;
    gchar **key = NULL;
    GKeyFile *key_file = g_key_file_new();

    if (!g_key_file_load_from_file(key_file, TRACKER_CONFIG_FILE, G_KEY_FILE_NONE,
                                  &error)) {
      g_printerr("Failed to load config file: %s\n", error->message);
      return FALSE;
    }

    keys = g_key_file_get_keys(key_file, CONFIG_GROUP_TRACKER, NULL, &error);
    CHECK_ERROR(error);

    for (key = keys; *key; key++) {
      if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_WIDTH)) {
        gint width =
            g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                  CONFIG_GROUP_TRACKER_WIDTH, &error);
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "tracker-width", width, NULL);
      }
      else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_HEIGHT)) {
        gint height =
            g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                  CONFIG_GROUP_TRACKER_HEIGHT, &error);
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "tracker-height", height, NULL);
      }
      else if (!g_strcmp0(*key, CONFIG_GPU_ID)) {
        guint gpu_id =
            g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                  CONFIG_GPU_ID, &error);
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "gpu_id", gpu_id, NULL);
      }
      else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_LL_CONFIG_FILE)) {
        char *ll_config_file = get_absolute_file_path(TRACKER_CONFIG_FILE,
                                                      g_key_file_get_string(key_file,
                                                                            CONFIG_GROUP_TRACKER,
                                                                            CONFIG_GROUP_TRACKER_LL_CONFIG_FILE, &error));
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "ll-config-file", ll_config_file, NULL);
      }
      else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_LL_LIB_FILE)) {
        char *ll_lib_file = get_absolute_file_path(TRACKER_CONFIG_FILE,
                                                  g_key_file_get_string(key_file,
                                                                        CONFIG_GROUP_TRACKER,
                                                                        CONFIG_GROUP_TRACKER_LL_LIB_FILE, &error));
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "ll-lib-file", ll_lib_file, NULL);
      }
      else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS)) {
        gboolean enable_batch_process =
            g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                  CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS, &error);
        CHECK_ERROR(error);
        g_object_set(G_OBJECT(nvtracker), "enable_batch_process",
                    enable_batch_process, NULL);
      }
      else {
        g_printerr("Unknown key '%s' for group [%s]", *key,
                  CONFIG_GROUP_TRACKER);
      }
    }

    ret = TRUE;

    done:
      if (error) {
        g_error_free(error);
      }
      if (keys) {
        g_strfreev(keys);
      }
      if (!ret) {
        g_printerr("%s failed", __func__);
      }
    return ret;
  }

  int
  Hermes::configure_element_properties(int num_sources, GstElement *streammux, GstElement *pgie_yolo_detector,
                               GstElement *nvtracker, GstElement *sink, GstElement *tiler) {

    guint tiler_rows, tiler_columns;

    g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH,
               "height", MUXER_OUTPUT_HEIGHT, "batch-size", num_sources,
               "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC,
               "live-source", FALSE, NULL);

    // Set all important properties of pgie_yolo_detector
    g_object_set(G_OBJECT(pgie_yolo_detector),
                "config-file-path", PGIE_YOLO_DETECTOR_CONFIG_FILE_PATH, NULL);

    // Override batch-size of pgie_yolo_detector
    g_object_set(G_OBJECT(pgie_yolo_detector), "batch-size", num_sources, NULL);

    // Check if Engine Exists
    if (boost::filesystem::exists(boost::filesystem::path(
        WildFireDetection::Hermes::PGIE_YOLO_ENGINE_PATH))) {

      g_object_set(G_OBJECT(pgie_yolo_detector),
                  "model-engine-file", WildFireDetection::Hermes::PGIE_YOLO_ENGINE_PATH.c_str(), NULL);
    }
    else {
      cout << str(boost::format("YOLO Engine for batch-size: %d and compute-mode: %s not found.")
              % num_sources % COMPUTE_MODE) << endl;
      return EXIT_FAILURE;
    }

    // Set necessary properties of the tracker element
    if (!WildFireDetection::Hermes::set_tracker_properties(nvtracker)) {
      g_printerr("Failed to set tracker properties. Exiting.\n");
      return -1;
    }

    g_object_set(G_OBJECT(sink),
                "sync", TRUE, NULL);

    tiler_rows = (guint)sqrt(num_sources);
    tiler_columns = (guint)ceil(1.0 * num_sources / tiler_rows);
    // Tiler Properties
    g_object_set(G_OBJECT(tiler), "rows", tiler_rows, "columns", tiler_columns,
                "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);

    return EXIT_SUCCESS;
  }

  void Hermes::setPaths(guint num_sources) {

    // Config Paths
    PGIE_YOLO_DETECTOR_CONFIG_FILE_PATH =
    strdup("models/YOLOv3WildFires/config_infer_primary_yolov3.txt");

    TRACKER_CONFIG_FILE =
    strdup("models/Trackers/DCF/ds_tracker_config.txt");

    // Engine Paths
    PGIE_YOLO_ENGINE_PATH =
    str(boost::format("model_b%d_gpu0_%s.engine") % num_sources % COMPUTE_MODE);
  }
}

int main(int argc, char *argv[]) {

  WildFireDetection::Hermes hermes;
  GMainLoop *loop = NULL;
  GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL,
             *pgie_yolo_detector = NULL, *nvtracker = NULL,
             *nvvidconv = NULL, *nvosd = NULL, *tiler;

  #ifdef PLATFORM_TEGRA
    GstElement *transform = NULL;
  #endif

  GstBus *bus = NULL;
  guint bus_watch_id = 0;
  GstPad *tiler_src_pad = NULL;
  guint i;

  // Standard GStreamer initialization
  gst_init(&argc, &argv);
  loop = g_main_loop_new(NULL, FALSE);

  GOptionContext *ctx = NULL;
  GOptionGroup *group = NULL;
  GError *error = NULL;

  ctx = g_option_context_new("Hermes DeepStream App");
  group = g_option_group_new("Hermes", NULL, NULL, NULL, NULL);
  g_option_group_add_entries(group, hermes.entries);

  g_option_context_set_main_group(ctx, group);
  g_option_context_add_group(ctx, gst_init_get_option_group());

  if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
    g_option_context_free(ctx);
    g_printerr("%s", error->message);
    return -1;
  }
  g_option_context_free(ctx);

  /* Create gstreamer elements */
  // Create Pipeline element to connect all elements
  pipeline = gst_pipeline_new("dsirisretail-pipeline");

  // Stream Multiplexer for input
  streammux = gst_element_factory_make("nvstreammux", "stream-muxer");

  if (!pipeline || !streammux) {
    g_printerr("One element could not be created. Exiting.\n");
    return -1;
  }
  gst_bin_add(GST_BIN(pipeline), streammux);

  gint sources = hermes.create_input_sources(pipeline, streammux, num_sources);
  if (sources == -1) {
    return -1;
  }
  else {
    num_sources = sources;
  }

  hermes.setPaths(num_sources);

  // Primary GPU Inference Engine
  pgie_yolo_detector = gst_element_factory_make("nvinfer", "primary-yolo-nvinference-engine");

  if (!pgie_yolo_detector) {
    g_printerr("PGIE YOLO Detector could not be created.\n");
    return -1;
  }

  // Initialize Tracker
  nvtracker = gst_element_factory_make("nvtracker", "tracker");

  if (!nvtracker ) {
    g_printerr("NVTRACKER could not be created.\n");
    return -1;
  }

  // Compose all the sources into one 2D tiled window
  tiler = gst_element_factory_make("nvmultistreamtiler", "nvtiler");

  if (!tiler) {
    g_printerr("SINK could not be created.\n");
    return -1;
  }

  // Use convertor to convert from NV12 to RGBA as required by nvosd
  nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");

  if (!nvvidconv) {
    g_printerr("NVVIDCONV could not be created.\n");
    return -1;
  }

  // Create OSD to draw on the converted RGBA buffer
  nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");

  if (!nvosd) {
    g_printerr("NVOSD could not be created.\n");
    return -1;
  }
  /* Redner OSD Output */
  #ifdef PLATFORM_TEGRA
    transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
  #endif

  if (hermes.display_off) {
    sink = gst_element_factory_make("fakesink", "nvvideo-renderer");
  }
  else {
    sink = gst_element_factory_make("nveglglessink", "nvvideo-renderer");
  }

  if (!sink) {
    g_printerr("SINK could not be created.\n");
    return -1;
  }

  #ifdef PLATFORM_TEGRA
    if (!transform) {
      g_printerr("Tegra element TRANSFORM could not be created. Exiting.\n");
      return -1;
    }
  #endif

  int fail_safe = hermes.configure_element_properties(num_sources, streammux, pgie_yolo_detector,
                                                      nvtracker, sink, tiler);

  if(fail_safe == -1) {
    return -1;
  }
  // Message Handler
  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_watch_id = gst_bus_add_watch(bus, hermes.bus_call, loop);
  gst_object_unref(bus);

  /* Set up the pipeline */
  #ifdef PLATFORM_TEGRA
    if (!hermes.display_off) {
      gst_bin_add_many(GST_BIN(pipeline),
                      pgie_yolo_detector, nvtracker, tiler, nvvidconv, nvosd, transform, sink, NULL);

      if (!gst_element_link_many(streammux, pgie_yolo_detector, nvtracker,
                                 tiler, nvvidconv, nvosd, transform, sink, NULL)) {
        g_printerr("Elements could not be linked. Exiting.\n");
        return -1;
      }
    }
    else {
      gst_bin_add_many(GST_BIN(pipeline),
                      pgie_yolo_detector, nvtracker, tiler, nvvidconv, nvosd, sink, NULL);

      if (!gst_element_link_many(streammux, pgie_yolo_detector, nvtracker,
                                 tiler, nvvidconv, nvosd, sink, NULL)) {
        g_printerr("Elements could not be linked. Exiting.\n");
        return -1;
      }
    }

  #else
    gst_bin_add_many(GST_BIN(pipeline),
                    pgie_yolo_detector, nvtracker, tiler, nvvidconv, nvosd, sink, NULL);

    if (!gst_element_link_many(streammux, pgie_yolo_detector, nvtracker,
                               tiler, nvvidconv, nvosd, sink, NULL)) {
      g_printerr("Elements could not be linked. Exiting.\n");
      return -1;
    }
  #endif

  /* Lets add probe to get informed of the meta data generated, we add probe to
   * the sink pad of the osd element, since by that time, the buffer would have
   * had got all the metadata. */
  tiler_src_pad = gst_element_get_static_pad(tiler, "sink");
  if (!tiler_src_pad) {
    g_print("Unable to get sink pad\n");
  }
  else {
    gst_pad_add_probe(tiler_src_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      hermes.tiler_src_pad_buffer_probe, NULL, NULL);
  }
  /* Set the pipeline to "playing" state */
  cout << "Now playing:" << endl;
  std::ifstream infile(SOURCE_PATH);
  std::string source;
  if (infile.is_open()) {
    while (getline(infile, source)) {
      cout << source << endl;
    }
  }
  infile.close();

  gst_element_set_state(pipeline, GST_STATE_PLAYING);

  /* Wait till pipeline encounters an error or EOS */
  g_print("Running...\n");
  g_main_loop_run(loop);

  /* Out of the main loop, clean up nicely */
  g_print("Returned, stopping playback\n");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  g_print("Deleting pipeline\n");
  gst_object_unref(GST_OBJECT(pipeline));
  g_source_remove(bus_watch_id);
  g_main_loop_unref(loop);
  return 0;
}