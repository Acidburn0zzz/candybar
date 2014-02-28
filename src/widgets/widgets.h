#include <glib.h>
#include <jansson.h>
#include <xcb/xcb_ewmh.h>

typedef struct thread_data_t {
	int screen_nbr;
	xcb_ewmh_connection_t *ewmh;
} thread_data_t;

typedef struct widget_data_t {
	char *widget;
	char *data;
} widget_data_t;

extern thread_data_t thread_data;

void *widget_desktops();
void *widget_external_ip();
void *widget_now_playing_mpd();
void *widget_volume();
void *widget_weather();

gboolean update_widget (widget_data_t *widget_data);

static const void *wkline_widgets[] = {
#ifndef DISABLE_WIDGET_DESKTOPS
	widget_desktops,
#endif
#ifndef DISABLE_WIDGET_EXTERNAL_IP
	widget_external_ip,
#endif
#ifndef DISABLE_WIDGET_NOW_PLAYING_MPD
	widget_now_playing_mpd,
#endif
#ifndef DISABLE_WIDGET_VOLUME
	widget_volume,
#endif
#ifndef DISABLE_WIDGET_WEATHER
	widget_weather,
#endif
};