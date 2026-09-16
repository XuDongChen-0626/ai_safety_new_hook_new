target_sources_ifdef(CONFIG_HOOK_GPS_ENABLE app PRIVATE
	hook_gps.c
	hook_rtc.c
)
