target_sources_ifdef(CONFIG_HOOK_OTA_ENABLE app PRIVATE
	hook_ymodem.c
	hook_ota.c
)
