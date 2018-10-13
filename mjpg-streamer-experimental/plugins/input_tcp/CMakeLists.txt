
check_include_files(sys/inotify.h HAVE_SYS_INOTIFY_H)

MJPG_STREAMER_PLUGIN_OPTION(input_tcp "File input plugin" ONLYIF HAVE_SYS_INOTIFY_H)
MJPG_STREAMER_PLUGIN_COMPILE(input_tcp input_tcp.c)


