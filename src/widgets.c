#include "widgets.h"
#include "candybar.h"

static struct widget **widgets_active = NULL;
static size_t widgets_len = 0;

pthread_mutex_t web_view_ready_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t update_cond = PTHREAD_COND_INITIALIZER;

static void
init_widget_js_obj (void *context, struct widget *widget) {
	char classname[64];
	snprintf(classname, 64, "widget_%s", widget->type);
	const JSClassDefinition widget_js_def = {
		.className = classname,
		.staticFunctions = widget->js_staticfuncs,
	};

	JSClassRef class_def = JSClassCreate(&widget_js_def);
	JSObjectRef class_obj = JSObjectMake(context, class_def, widget);
	JSObjectRef global_obj = JSContextGetGlobalObject(context);
	JSStringRef str_name = JSStringCreateWithUTF8CString(classname);
	JSObjectSetProperty(context, global_obj, str_name, class_obj, kJSPropertyAttributeNone, NULL);
	JSStringRelease(str_name);

	widget->js_context = context;
	widget->js_object = class_obj;
}

static struct widget*
spawn_widget (struct bar *bar, void *context, json_t *config, const char *name) {
	widget_main_t widget_main;
	widget_type_t widget_type;
	char libname[64];
	snprintf(libname, 64, "libwidget_%s", name);
	gchar *libpath = g_module_build_path(LIBDIR, libname);
	GModule *lib = g_module_open(libpath, G_MODULE_BIND_LOCAL);
	pthread_t return_thread;
	struct widget *widget = calloc(1, sizeof(struct widget));

	if (lib == NULL) {
		LOG_WARN("loading of '%s' (%s) failed", libpath, name);

		goto error;
	}

	if (!g_module_symbol(lib, "widget_main", (void*)&widget_main)) {
		LOG_WARN("loading of '%s' (%s) failed: unable to load module symbol", libpath, name);

		goto error;
	}

	JSStaticFunction *js_staticfuncs = calloc(1, sizeof(JSStaticFunction));
	if (g_module_symbol(lib, "widget_js_staticfuncs", (void*)&js_staticfuncs)) {
		widget->js_staticfuncs = js_staticfuncs;
	}
	else {
		free(js_staticfuncs);
	}

	widget->bar = bar;
	widget->config = config;
	widget->name = strdup(name);

	pthread_mutex_init(&widget->exit_mutex, NULL);
	pthread_cond_init(&widget->exit_cond, NULL);

	if (g_module_symbol(lib, "widget_type", (void*)&widget_type)) {
		widget->type = widget_type();
	}
	else {
		widget->type = widget->name;
	}

	init_widget_js_obj(context, widget);

	if (pthread_create(&return_thread, NULL, (void*(*)(void*))widget_main, widget) != 0) {
		LOG_ERR("failed to start widget %s: %s", name, strerror(errno));

		goto error;
	}

	widget->thread = return_thread;

	return widget;

error:
	if (widget->name != NULL) {
		free(widget->name);
	}
	if (widget->js_staticfuncs != NULL) {
		free(widget->js_staticfuncs);
	}
	free(widget);

	return 0;
}

void
join_widget_threads (struct bar *bar) {
	unsigned short i;
	struct timespec timeout;

	if (widgets_active && (widgets_len > 0)) {
		LOG_DEBUG("gracefully shutting down widget threads...");
		for (i = 0; i < widgets_len; i++) {
			/* make all threads wait until we're ready to receive
			   the cond signal below */
			pthread_mutex_lock(&widgets_active[i]->exit_mutex);
		}

		/* send exit signal */
		eventfd_write(bar->efd, 1);
		for (i = 0; i < widgets_len; i++) {
			/* update cond timeout */
			clock_gettime(CLOCK_REALTIME, &timeout);
			timeout.tv_sec += 2;

			/* wait until thread times out or sends an exit
			   confirmation signal */
			int ret = pthread_cond_timedwait(&widgets_active[i]->exit_cond, &widgets_active[i]->exit_mutex, &timeout);

			if (ret == ETIMEDOUT) {
				LOG_WARN("timed out waiting for widget %s to exit", widgets_active[i]->name);
				pthread_cancel(widgets_active[i]->thread);
			}
			else {
				pthread_join(widgets_active[i]->thread, NULL);
			}
		}

		/* read any data from the efd so it blocks on epoll_wait */
		eventfd_read(bar->efd, NULL);
		free(widgets_active);
	}
	else {
		LOG_DEBUG("no widget threads have been spawned");
	}
}

bool
web_view_callback (struct js_callback_data *data) {
	unsigned short i;

	JSValueRef js_args[data->args_len];
	for (i = 0; i < data->args_len; i++) {
		switch (data->args[i].type) {
		case kJSTypeBoolean:
			js_args[i] = JSValueMakeBoolean(data->widget->js_context, data->args[i].value.boolean);
			break;
		case kJSTypeNull:
			js_args[i] = JSValueMakeNull(data->widget->js_context);
			break;
		case kJSTypeNumber:
			js_args[i] = JSValueMakeNumber(data->widget->js_context, data->args[i].value.number);
			break;
		case kJSTypeObject:
			js_args[i] = data->args[i].value.object;
			break;
		case kJSTypeString: {
			JSStringRef str = JSStringCreateWithUTF8CString(data->args[i].value.string);
			js_args[i] = JSValueMakeString(data->widget->js_context, str);
			JSStringRelease(str);
			break;
		}
		case kJSTypeUndefined:
			js_args[i] = JSValueMakeUndefined(data->widget->js_context);
			break;
		}
	}

	if (!data->widget->js_context || !data->widget->js_object) {
		LOG_ERR("missing JS context or object!");

		return false;
	}

	JSStringRef str_ondatachanged = JSStringCreateWithUTF8CString("onDataChanged");
	JSValueRef func = JSObjectGetProperty(data->widget->js_context, data->widget->js_object, str_ondatachanged, NULL);
	JSObjectRef function = JSValueToObject(data->widget->js_context, func, NULL);
	JSStringRelease(str_ondatachanged);

	/* let the thread know we're done with the data so it can cleanup */
	pthread_cond_signal(&update_cond);

	if (!JSObjectIsFunction(data->widget->js_context, function)) {
		LOG_DEBUG("onDataChanged callback for 'widget_%s' with type '%s' is not a function or is not set", data->widget->name, data->widget->type);

		return false; /* only run once */
	}

	JSObjectCallAsFunction(data->widget->js_context, function, NULL, data->args_len, js_args, NULL);

	return false; /* only run once */
}

void
wk_load_status_cb (GObject *object, GParamSpec *pspec, gpointer data) {
	WebKitWebView *web_view = WEBKIT_WEB_VIEW(object);
	WebKitLoadStatus status = webkit_web_view_get_load_status(web_view);

	if (status == WEBKIT_LOAD_FINISHED) {
		LOG_DEBUG("webkit: load finished");
		pthread_mutex_unlock(&web_view_ready_mutex);
	}
}

void
wk_window_object_cleared_cb (WebKitWebView *web_view, GParamSpec *pspec, void *context, void *window_object, void *user_data) {
	LOG_DEBUG("webkit: window object cleared");
	struct bar *bar = user_data;

	json_t *widget;
	json_t *widgets = json_object_get(bar->config, "widgets");
	size_t index;

	widgets_len = json_array_size(widgets);
	widgets_active = calloc(widgets_len, sizeof(struct widget));

	LOG_DEBUG("starting %i widget threads", widgets_len);
	json_array_foreach(widgets, index, widget) {
		widgets_active[index] = spawn_widget(bar,
		                                     context,
		                                     json_object_get(widget, "config"),
		                                     json_string_value(json_object_get(widget, "module")));
	}

	/* lock mutex until the web page has been loaded completely */
	pthread_mutex_lock(&web_view_ready_mutex);
}
