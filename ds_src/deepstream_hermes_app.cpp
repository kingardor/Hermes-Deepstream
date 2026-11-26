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
        // Skip blank lines and comments so the file can be annotated
        if (source.empty() || source[0] == '#') {
          continue;
        }

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
        source_uris.push_back(source);
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

    display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
    txt_params = display_meta->text_params;
    display_meta->num_labels = 1;

    txt_params->display_text = (char *)g_malloc0(MAX_DISPLAY_LEN);

    snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Source: %d | FPS: %d",
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

  /* ---------------------------------------------------------------------------
   * Redis publishing for the web UI.
   * Every frame of every source publishes one JSON message, even with no
   * detections, so the backend can also tell the pipeline is alive.
   * ------------------------------------------------------------------------- */
  gboolean
  Hermes::redis_ready() {
    if (redis && !redis->err) {
      return TRUE;
    }
    // Retry at most every 5 s so a missing Redis never slows the pipeline down
    if (system_clock::now() < redis_retry_at) {
      return FALSE;
    }
    redis_retry_at = system_clock::now() + std::chrono::seconds(5);
    if (redis) {
      redisFree(redis);
      redis = NULL;
    }
    struct timeval timeout = {0, 200000};  // 200 ms
    redis = redisConnectWithTimeout(redis_host_s.c_str(), redis_port_i, timeout);
    if (!redis || redis->err) {
      g_printerr("Redis unavailable at %s:%d (%s); web UI will not get detections. Retrying.\n",
                 redis_host_s.c_str(), redis_port_i, redis ? redis->errstr : "allocation failed");
      if (redis) {
        redisFree(redis);
        redis = NULL;
      }
      return FALSE;
    }
    redisSetTimeout(redis, timeout);
    g_print("Publishing detections to redis://%s:%d %s\n", redis_host_s.c_str(), redis_port_i,
            REDIS_DETECTIONS_CHANNEL);
    return TRUE;
  }

  void
  Hermes::publish_frame(NvDsFrameMeta *frame_meta) {
    if (!redis_ready()) {
      return;
    }

    gint64 now_ms = duration_cast<std::chrono::milliseconds>(
        system_clock::now().time_since_epoch()).count();

    std::ostringstream json;
    json << "{\"t\":" << now_ms
         << ",\"src\":" << frame_meta->source_id
         << ",\"frame\":" << frame_meta->frame_num
         << ",\"fps\":" << fps[frame_meta->source_id].display_fps
         << ",\"w\":" << MUXER_OUTPUT_WIDTH
         << ",\"h\":" << MUXER_OUTPUT_HEIGHT
         << ",\"objs\":[";

    bool first = true;
    for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj = (NvDsObjectMeta *)l_obj->data;
      if (obj == NULL || obj->class_id != FIRE) {
        continue;
      }
      // Rect is in muxer coordinates (MUXER_OUTPUT_WIDTH x MUXER_OUTPUT_HEIGHT) for this source
      json << (first ? "" : ",")
           << "{\"id\":" << obj->object_id
           << ",\"cls\":\"" << class_names[FIRE] << "\""
           << ",\"conf\":" << obj->confidence
           << ",\"x\":" << (gint)obj->rect_params.left
           << ",\"y\":" << (gint)obj->rect_params.top
           << ",\"bw\":" << (gint)obj->rect_params.width
           << ",\"bh\":" << (gint)obj->rect_params.height << "}";
      first = false;
    }
    json << "]}";

    redisReply *reply = (redisReply *)redisCommand(redis, "PUBLISH %s %s",
                                                   REDIS_DETECTIONS_CHANNEL, json.str().c_str());
    if (reply) {
      freeReplyObject(reply);
    }
    else {
      // Connection dropped; redis_ready() will reconnect later
      redis_retry_at = system_clock::now() + std::chrono::seconds(5);
    }
  }

  void
  Hermes::publish_pipeline_info(guint rows, guint columns) {
    if (!redis_ready()) {
      return;
    }
    std::ostringstream sources;
    sources << "[";
    for (size_t i = 0; i < source_uris.size(); i++) {
      std::string uri = source_uris[i];
      std::string escaped;
      for (char c : uri) {
        if (c == '"' || c == '\\') escaped += '\\';
        escaped += c;
      }
      sources << (i ? "," : "") << "\"" << escaped << "\"";
    }
    sources << "]";

    gint64 now_ms = duration_cast<std::chrono::milliseconds>(
        system_clock::now().time_since_epoch()).count();
    std::string rtsp = rtsp_off ? std::string("") :
        str(boost::format("rtsp://127.0.0.1:%d%s") % rtsp_port % RTSP_MOUNT);

    redisReply *reply = (redisReply *)redisCommand(redis,
        "HSET %s sources %s rows %u columns %u width %d height %d tile_width %d tile_height %d started %lld rtsp %s",
        REDIS_PIPELINE_KEY, sources.str().c_str(), rows, columns,
        MUXER_OUTPUT_WIDTH, MUXER_OUTPUT_HEIGHT, TILED_OUTPUT_WIDTH, TILED_OUTPUT_HEIGHT,
        (long long)now_ms, rtsp.c_str());
    if (reply) {
      freeReplyObject(reply);
    }
  }

  /* The probe sits on the tiler's sink pad, so object coordinates are still
   * per source (muxer resolution) and every source's frame meta is present. */
  GstPadProbeReturn
  Hermes::tiler_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info,
                            gpointer u_data) {
    GstBuffer *buf = (GstBuffer *)info->data;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    if (!batch_meta) {
      return GST_PAD_PROBE_OK;
    }

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL;
        l_frame = l_frame->next) {
      NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

      if (frame_meta == NULL) {
        continue;
      }

      update_fps(frame_meta->source_id);

      for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL;
          l_obj = l_obj->next) {

        NvDsObjectMeta *obj_meta = (NvDsObjectMeta *)(l_obj->data);

        if (obj_meta == NULL) {
          continue;
        }

        if (obj_meta->class_id == FIRE) {
          changeBBoxColor(obj_meta, 1, 1.0, 0.0, 0.0, 0.25);
        }
      }
      // Source / FPS label on every stream, then hand the frame to the web UI
      addDisplayMeta(batch_meta, frame_meta);
      publish_frame(frame_meta);
    }
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
    gst_caps_unref(caps);
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

  // Encoder properties differ between Jetson and dGPU builds; only set what exists.
  void
  Hermes::set_if_exists(GstElement *element, const gchar *property, guint value) {
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), property)) {
      g_object_set(G_OBJECT(element), property, value, NULL);
    }
  }

  int
  Hermes::configure_element_properties(int num_sources, GstElement *streammux, GstElement *pgie_yolo_detector,
                               GstElement *nvtracker, GstElement *sink, GstElement *tiler,
                               guint *rows, guint *columns) {

    guint tiler_rows, tiler_columns;

    g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH,
               "height", MUXER_OUTPUT_HEIGHT, "batch-size", num_sources,
               "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC,
               "live-source", TRUE, NULL);

    // Set all important properties of pgie_yolo_detector
    g_object_set(G_OBJECT(pgie_yolo_detector),
                "config-file-path", PGIE_YOLO_DETECTOR_CONFIG_FILE_PATH, NULL);

    // Override batch-size of pgie_yolo_detector
    g_object_set(G_OBJECT(pgie_yolo_detector), "batch-size", num_sources, NULL);

    // Check if Engine Exists
    if (boost::filesystem::exists(boost::filesystem::path(PGIE_YOLO_ENGINE_PATH))) {

      g_object_set(G_OBJECT(pgie_yolo_detector),
                  "model-engine-file", PGIE_YOLO_ENGINE_PATH.c_str(), NULL);
    }
    else {
      // nvinfer builds the engine from the cfg/weights in this case (first run takes a few minutes)
      cout << str(boost::format("YOLO engine for batch-size %d / %s not found (%s); nvinfer will build it.")
              % num_sources % COMPUTE_MODE % PGIE_YOLO_ENGINE_PATH) << endl;
    }

    // Set necessary properties of the tracker element
    if (!set_tracker_properties(nvtracker)) {
      g_printerr("Failed to set tracker properties. Exiting.\n");
      return -1;
    }

    g_object_set(G_OBJECT(sink),
                "sync", FALSE, NULL);

    tiler_rows = (guint)sqrt(num_sources);
    tiler_columns = (guint)ceil(1.0 * num_sources / tiler_rows);
    // Tiler Properties
    g_object_set(G_OBJECT(tiler), "rows", tiler_rows, "columns", tiler_columns,
                "width", TILED_OUTPUT_WIDTH, "height", TILED_OUTPUT_HEIGHT, NULL);

    *rows = tiler_rows;
    *columns = tiler_columns;
    return EXIT_SUCCESS;
  }

  /* Serve the encoded tiled output over RTSP. The encoder branch sends RTP to a
   * local UDP port; this RTSP server wraps it (same approach as deepstream-app). */
  gboolean
  Hermes::start_rtsp_server() {
    GstRTSPServer *server = gst_rtsp_server_new();
    gchar port_str[16];
    g_snprintf(port_str, sizeof(port_str), "%d", rtsp_port);
    g_object_set(server, "service", port_str, NULL);

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    gchar *launch = g_strdup_printf(
        "( udpsrc name=pay0 port=%d buffer-size=524288 "
        "caps=\"application/x-rtp, media=video, clock-rate=90000, "
        "encoding-name=(string)H264, payload=96 \" )", RTSP_UDP_PORT);
    gst_rtsp_media_factory_set_launch(factory, launch);
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    gst_rtsp_mount_points_add_factory(mounts, RTSP_MOUNT, factory);
    g_object_unref(mounts);
    g_free(launch);

    if (gst_rtsp_server_attach(server, NULL) == 0) {
      g_printerr("Failed to start the RTSP server on port %d\n", rtsp_port);
      return FALSE;
    }
    g_print("RTSP output: rtsp://<this-device>:%d%s\n", rtsp_port, RTSP_MOUNT);
    return TRUE;
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

/* Link `tee` -> queue -> ...elements, returning FALSE on failure. */
static gboolean
link_tee_branch(GstElement *tee, GstElement *first) {
  GstPad *tee_pad = gst_element_get_request_pad(tee, "src_%u");
  GstPad *sink_pad = gst_element_get_static_pad(first, "sink");
  gboolean ok = tee_pad && sink_pad && gst_pad_link(tee_pad, sink_pad) == GST_PAD_LINK_OK;
  if (sink_pad) gst_object_unref(sink_pad);
  if (tee_pad) gst_object_unref(tee_pad);
  return ok;
}

int main(int argc, char *argv[]) {

  WildFireDetection::Hermes hermes;
  GMainLoop *loop = NULL;
  GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL,
             *pgie_yolo_detector = NULL, *nvtracker = NULL,
             *nvvidconv = NULL, *nvosd = NULL, *tiler = NULL, *tee = NULL,
             *display_queue = NULL;

  #ifdef PLATFORM_TEGRA
    GstElement *transform = NULL;
  #endif

  GstBus *bus = NULL;
  guint bus_watch_id = 0;
  GstPad *tiler_sink_pad = NULL;
  guint tiler_rows = 1, tiler_columns = 1;

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

  if (hermes.redis_host) {
    WildFireDetection::Hermes::redis_host_s = hermes.redis_host;
  }
  WildFireDetection::Hermes::redis_port_i = hermes.redis_port;

  /* Create gstreamer elements */
  pipeline = gst_pipeline_new("hermes-pipeline");

  // Stream Multiplexer for input
  streammux = gst_element_factory_make("nvstreammux", "stream-muxer");

  if (!pipeline || !streammux) {
    g_printerr("One element could not be created. Exiting.\n");
    return -1;
  }
  gst_bin_add(GST_BIN(pipeline), streammux);

  gint sources = hermes.create_input_sources(pipeline, streammux, num_sources);
  if (sources <= 0) {
    g_printerr("No input sources found in %s. Exiting.\n", SOURCE_PATH);
    return -1;
  }
  num_sources = sources;

  hermes.setPaths(num_sources);

  // Primary GPU Inference Engine
  pgie_yolo_detector = gst_element_factory_make("nvinfer", "primary-yolo-nvinference-engine");
  // Tracker
  nvtracker = gst_element_factory_make("nvtracker", "tracker");
  // Compose all the sources into one 2D tiled frame
  tiler = gst_element_factory_make("nvmultistreamtiler", "nvtiler");
  // Use convertor to convert from NV12 to RGBA as required by nvosd
  nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");
  // Create OSD to draw on the converted RGBA buffer
  nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");
  // Split the annotated video between the local display and the RTSP output
  tee = gst_element_factory_make("tee", "output-tee");
  display_queue = gst_element_factory_make("queue", "display-queue");

  if (!pgie_yolo_detector || !nvtracker || !tiler || !nvvidconv || !nvosd || !tee || !display_queue) {
    g_printerr("One of the DeepStream elements could not be created. Exiting.\n");
    return -1;
  }

  #ifdef PLATFORM_TEGRA
    if (!hermes.display_off) {
      transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
      if (!transform) {
        g_printerr("Tegra element TRANSFORM could not be created. Exiting.\n");
        return -1;
      }
    }
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

  int fail_safe = hermes.configure_element_properties(num_sources, streammux, pgie_yolo_detector,
                                                      nvtracker, sink, tiler, &tiler_rows, &tiler_columns);

  if (fail_safe != EXIT_SUCCESS) {
    return -1;
  }
  // Message Handler
  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_watch_id = gst_bus_add_watch(bus, hermes.bus_call, loop);
  gst_object_unref(bus);

  /* Main chain: mux -> detector -> tracker -> tiler -> convert -> OSD -> tee */
  gst_bin_add_many(GST_BIN(pipeline),
                   pgie_yolo_detector, nvtracker, tiler, nvvidconv, nvosd, tee, NULL);
  if (!gst_element_link_many(streammux, pgie_yolo_detector, nvtracker,
                             tiler, nvvidconv, nvosd, tee, NULL)) {
    g_printerr("Elements could not be linked. Exiting.\n");
    return -1;
  }

  /* Branch 1: local display (or a fakesink when --no-display) */
  gst_bin_add_many(GST_BIN(pipeline), display_queue, sink, NULL);
  #ifdef PLATFORM_TEGRA
    if (transform) {
      gst_bin_add(GST_BIN(pipeline), transform);
      if (!gst_element_link_many(display_queue, transform, sink, NULL)) {
        g_printerr("Display branch could not be linked. Exiting.\n");
        return -1;
      }
    }
    else if (!gst_element_link(display_queue, sink)) {
      g_printerr("Display branch could not be linked. Exiting.\n");
      return -1;
    }
  #else
    if (!gst_element_link(display_queue, sink)) {
      g_printerr("Display branch could not be linked. Exiting.\n");
      return -1;
    }
  #endif
  if (!link_tee_branch(tee, display_queue)) {
    g_printerr("Could not link the display branch to the tee. Exiting.\n");
    return -1;
  }

  /* Branch 2: H.264 over RTSP for the web UI */
  if (!hermes.rtsp_off) {
    GstElement *rtsp_queue = gst_element_factory_make("queue", "rtsp-queue");
    GstElement *rtsp_conv = gst_element_factory_make("nvvideoconvert", "rtsp-converter");
    GstElement *rtsp_caps = gst_element_factory_make("capsfilter", "rtsp-caps");
    GstElement *encoder = gst_element_factory_make("nvv4l2h264enc", "rtsp-encoder");
    GstElement *parser = gst_element_factory_make("h264parse", "rtsp-parser");
    GstElement *payloader = gst_element_factory_make("rtph264pay", "rtsp-payloader");
    GstElement *udpsink = gst_element_factory_make("udpsink", "rtsp-udpsink");

    if (!rtsp_queue || !rtsp_conv || !rtsp_caps || !encoder || !parser || !payloader || !udpsink) {
      g_printerr("RTSP output elements could not be created. Exiting.\n");
      return -1;
    }

    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=I420");
    g_object_set(G_OBJECT(rtsp_caps), "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(G_OBJECT(encoder), "bitrate", RTSP_BITRATE, NULL);
    WildFireDetection::Hermes::set_if_exists(encoder, "iframeinterval", 30);
    WildFireDetection::Hermes::set_if_exists(encoder, "insert-sps-pps", 1);
    WildFireDetection::Hermes::set_if_exists(encoder, "preset-level", 1);
    #ifdef PLATFORM_TEGRA
      WildFireDetection::Hermes::set_if_exists(encoder, "bufapi-version", 1);
    #endif
    g_object_set(G_OBJECT(payloader), "config-interval", 1, NULL);
    g_object_set(G_OBJECT(udpsink), "host", "127.0.0.1", "port", RTSP_UDP_PORT,
                 "async", FALSE, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), rtsp_queue, rtsp_conv, rtsp_caps, encoder,
                     parser, payloader, udpsink, NULL);
    if (!gst_element_link_many(rtsp_queue, rtsp_conv, rtsp_caps, encoder, parser,
                               payloader, udpsink, NULL) ||
        !link_tee_branch(tee, rtsp_queue)) {
      g_printerr("RTSP output branch could not be linked. Exiting.\n");
      return -1;
    }
    if (!hermes.start_rtsp_server()) {
      return -1;
    }
  }

  /* Probe on the tiler's sink pad: by then the buffer carries all detection
   * and tracking metadata, still in per-source coordinates. */
  tiler_sink_pad = gst_element_get_static_pad(tiler, "sink");
  if (!tiler_sink_pad) {
    g_print("Unable to get sink pad\n");
  }
  else {
    gst_pad_add_probe(tiler_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                      hermes.tiler_src_pad_buffer_probe, NULL, NULL);
    gst_object_unref(tiler_sink_pad);
  }

  // Tell the web UI backend which sources are in which tile
  hermes.publish_pipeline_info(tiler_rows, tiler_columns);

  cout << "Now playing:" << endl;
  for (const std::string &uri : hermes.source_uris) {
    cout << uri << endl;
  }

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
