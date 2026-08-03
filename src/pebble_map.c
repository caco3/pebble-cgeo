#include <pebble.h>

#define KEY_COMMAND      0
#define KEY_VALUE        1
#define KEY_CHUNK_INDEX  2
#define KEY_CHUNK_TOTAL  3
#define KEY_CHUNK_DATA   4
#define KEY_FRAME_ID     5

#define CMD_REFRESH      0
#define CMD_ZOOM_IN      1
#define CMD_ZOOM_OUT     2
#define CMD_SET_INTERVAL 3
#define CMD_SET_ZOOM     4

#define MAP_WIDTH        200
#define MAP_HEIGHT       228
#define MAP_BYTES_PER_ROW MAP_WIDTH
#define MAP_SIZE         (MAP_HEIGHT * MAP_BYTES_PER_ROW)
#define CHUNK_SIZE       1000
#define DATA_WATCHDOG_MS 10000

static Window *s_window;
static BitmapLayer *s_bitmap_layer;
static GBitmap *s_front_bmp = NULL;
static GBitmap *s_back_bmp = NULL;
static GBitmap *s_fallback_bitmap = NULL;
static uint8_t *s_front = NULL;
static uint8_t *s_back = NULL;

static int s_chunks_total = -1;
static uint64_t s_received_mask = 0;
static bool s_map_received = false;
static bool s_has_image = false;
static int s_frame_id = -1;
static AppTimer *s_refresh_timer = NULL;
static AppTimer *s_data_watchdog = NULL;
static int s_refresh_interval_ms = 10000;
static bool s_auto_refresh = true;
static Layer *s_progress_layer = NULL;
static TextLayer *s_status_layer = NULL;

static void send_command(const uint32_t command, const int32_t value) {
    DictionaryIterator *iter;
    AppMessageResult result = app_message_outbox_begin(&iter);
    if (result != APP_MSG_OK || iter == NULL) {
        return;
    }
    dict_write_int32(iter, KEY_COMMAND, (int32_t) command);
    dict_write_int32(iter, KEY_VALUE, value);
    app_message_outbox_send();
}

static int count_bits(uint64_t v);

static void draw_progress(Layer *layer, GContext *ctx) {
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

static void update_progress() {
    if (s_progress_layer == NULL) {
        return;
    }
    int width = 0;
    if (s_chunks_total > 0) {
        width = (MAP_WIDTH * count_bits(s_received_mask)) / s_chunks_total;
    }
    layer_set_frame(s_progress_layer, GRect(0, 0, width, 4));
    layer_mark_dirty(s_progress_layer);
}

static void update_map_display() {
    if (s_bitmap_layer == NULL) {
        return;
    }
    if (s_has_image && s_front_bmp != NULL) {
        bitmap_layer_set_bitmap(s_bitmap_layer, s_front_bmp);
    } else if (s_fallback_bitmap != NULL) {
        bitmap_layer_set_bitmap(s_bitmap_layer, s_fallback_bitmap);
    }
    layer_mark_dirty(bitmap_layer_get_layer(s_bitmap_layer));
    if (s_status_layer != NULL) {
        layer_set_hidden(text_layer_get_layer(s_status_layer), s_has_image);
    }
}

static void handle_refresh_timer(void *data) {
    send_command(CMD_REFRESH, 0);
    if (s_auto_refresh) {
        s_refresh_timer = app_timer_register(s_refresh_interval_ms, handle_refresh_timer, NULL);
    }
}

static void data_watchdog_handler(void *data) {
    s_data_watchdog = NULL;
    s_has_image = false;
    update_map_display();
}

static void start_refresh_timer() {
    if (s_refresh_timer != NULL) {
        app_timer_cancel(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_auto_refresh && s_refresh_interval_ms > 0) {
        s_refresh_timer = app_timer_register(s_refresh_interval_ms, handle_refresh_timer, NULL);
    }
}

static int count_bits(uint64_t v) {
    int c = 0;
    for (; v; v &= v - 1) {
        c++;
    }
    return c;
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
    Tuple *index_tuple = dict_find(iter, KEY_CHUNK_INDEX);
    Tuple *total_tuple = dict_find(iter, KEY_CHUNK_TOTAL);
    Tuple *data_tuple = dict_find(iter, KEY_CHUNK_DATA);
    Tuple *frame_tuple = dict_find(iter, KEY_FRAME_ID);

    if (total_tuple != NULL) {
        s_chunks_total = (int) total_tuple->value->int32;
    }
    if (frame_tuple != NULL) {
        int new_frame = (int) frame_tuple->value->int32;
        if (s_frame_id == -1 || new_frame > s_frame_id) {
            s_frame_id = new_frame;
            s_received_mask = 0;
            s_map_received = false;
            // clear the receiving buffer, not the displayed front buffer
            if (s_back != NULL && s_back != s_front) {
                memset(s_back, GColorBlackARGB8, MAP_SIZE);
            }
            update_progress();
        } else if (new_frame < s_frame_id) {
            // stale chunk from a previous frame, ignore it
            return;
        }
    }
    if (s_frame_id == -1 || index_tuple == NULL || data_tuple == NULL) {
        return;
    }

    const int index = (int) index_tuple->value->int32;
    const uint8_t *data = data_tuple->value->data;
    const int len = data_tuple->length;

    int offset = index * CHUNK_SIZE;
    if (s_back != NULL) {
        for (int i = 0; i < len && (offset + i) < MAP_SIZE; i++) {
            s_back[offset + i] = data[i];
        }
    }

    if (s_data_watchdog != NULL) {
        app_timer_cancel(s_data_watchdog);
    }
    s_data_watchdog = app_timer_register(DATA_WATCHDOG_MS, data_watchdog_handler, NULL);

    s_received_mask |= (1ULL << index);
    update_progress();

    if (s_chunks_total > 0 && count_bits(s_received_mask) >= s_chunks_total) {
        s_map_received = true;
        s_has_image = true;
        // swap the completed back bitmap to the front for display
        if (s_back_bmp != s_front_bmp) {
            GBitmap *tmp_bmp = s_front_bmp;
            s_front_bmp = s_back_bmp;
            s_back_bmp = tmp_bmp;
            s_front = gbitmap_get_data(s_front_bmp);
            s_back = gbitmap_get_data(s_back_bmp);
        }
        update_map_display();
    }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_DEBUG, "chunk dropped (reason %d)", (int) reason);
    s_chunks_total = -1;
    s_received_mask = 0;
    s_map_received = false;
    update_progress();
    send_command(CMD_REFRESH, 0);
}

static void outbox_sent_callback(DictionaryIterator *iter, void *context) {
}

static void outbox_failed_callback(DictionaryIterator *iter, AppMessageResult reason, void *context) {
}

static void select_single_click_handler(ClickRecognizerRef recognizer, void *context) {
    send_command(CMD_REFRESH, 0);
}

static void up_single_click_handler(ClickRecognizerRef recognizer, void *context) {
    send_command(CMD_ZOOM_IN, 0);
}

static void down_single_click_handler(ClickRecognizerRef recognizer, void *context) {
    send_command(CMD_ZOOM_OUT, 0);
}

static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_refresh_interval_ms == 10000) {
        s_refresh_interval_ms = 0;
        s_auto_refresh = false;
    } else {
        s_refresh_interval_ms = 10000;
        s_auto_refresh = true;
    }
    send_command(CMD_SET_INTERVAL, s_refresh_interval_ms / 1000);
    start_refresh_timer();
}

static void click_config_provider(void *context) {
    window_single_click_subscribe(BUTTON_ID_SELECT, select_single_click_handler);
    window_single_click_subscribe(BUTTON_ID_UP, up_single_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN, down_single_click_handler);
    window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_click_handler, NULL);
}

static void window_load(Window *window) {
    window_set_background_color(window, GColorBlack);
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_front_bmp = gbitmap_create_blank(GSize(MAP_WIDTH, MAP_HEIGHT), GBitmapFormat8Bit);
    s_back_bmp = gbitmap_create_blank(GSize(MAP_WIDTH, MAP_HEIGHT), GBitmapFormat8Bit);
    if (s_back_bmp == NULL) {
        s_back_bmp = s_front_bmp;
    }
    if (s_front_bmp != NULL) {
        s_front = gbitmap_get_data(s_front_bmp);
        memset(s_front, GColorBlackARGB8, MAP_SIZE);
    }
    if (s_back_bmp != NULL && s_back_bmp != s_front_bmp) {
        s_back = gbitmap_get_data(s_back_bmp);
        memset(s_back, GColorBlackARGB8, MAP_SIZE);
    } else if (s_front_bmp != NULL) {
        s_back = s_front;
    }

    s_fallback_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CGEO_LOGO);

    s_data_watchdog = app_timer_register(DATA_WATCHDOG_MS, data_watchdog_handler, NULL);

    s_bitmap_layer = bitmap_layer_create(bounds);
    if (s_fallback_bitmap != NULL) {
        bitmap_layer_set_bitmap(s_bitmap_layer, s_fallback_bitmap);
    }
    bitmap_layer_set_alignment(s_bitmap_layer, GAlignTop);
    layer_add_child(window_layer, bitmap_layer_get_layer(s_bitmap_layer));

    s_progress_layer = layer_create(GRect(0, 0, 0, 4));
    layer_set_update_proc(s_progress_layer, draw_progress);
    layer_add_child(window_layer, s_progress_layer);

    s_status_layer = text_layer_create(GRect(0, 140, MAP_WIDTH, 60));
    text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
    text_layer_set_overflow_mode(s_status_layer, GTextOverflowModeWordWrap);
    text_layer_set_text_color(s_status_layer, GColorWhite);
    text_layer_set_background_color(s_status_layer, GColorBlack);
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_text(s_status_layer, "Waiting for data from c:geo...");
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

    update_map_display();
}

static void window_unload(Window *window) {
    if (s_back_bmp != NULL && s_back_bmp != s_front_bmp) {
        gbitmap_destroy(s_back_bmp);
        s_back_bmp = NULL;
    }
    if (s_front_bmp != NULL) {
        gbitmap_destroy(s_front_bmp);
        s_front_bmp = NULL;
    }
    if (s_fallback_bitmap != NULL) {
        gbitmap_destroy(s_fallback_bitmap);
        s_fallback_bitmap = NULL;
    }
    if (s_bitmap_layer != NULL) {
        bitmap_layer_destroy(s_bitmap_layer);
        s_bitmap_layer = NULL;
    }
    if (s_progress_layer != NULL) {
        layer_destroy(s_progress_layer);
        s_progress_layer = NULL;
    }
    if (s_status_layer != NULL) {
        text_layer_destroy(s_status_layer);
        s_status_layer = NULL;
    }
}

static void init(void) {
    s_window = window_create();
    window_set_click_config_provider(s_window, click_config_provider);
    window_set_window_handlers(s_window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload
    });
    window_stack_push(s_window, true);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_register_outbox_sent(outbox_sent_callback);
    app_message_register_outbox_failed(outbox_failed_callback);
    app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

    send_command(CMD_REFRESH, 0);
    start_refresh_timer();
}

static void deinit(void) {
    if (s_refresh_timer != NULL) {
        app_timer_cancel(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_data_watchdog != NULL) {
        app_timer_cancel(s_data_watchdog);
        s_data_watchdog = NULL;
    }
    app_message_deregister_callbacks();
    window_destroy(s_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
