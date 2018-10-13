
check_include_files(sys/inotify.h HAVE_SYS_INOTIFY_H)

MJPG_STREAMER_PLUGIN_OPTION(input_udp "File input plugin" ONLYIF HAVE_SYS_INOTIFY_H)
MJPG_STREAMER_PLUGIN_COMPILE(input_udp input_udp.c)


