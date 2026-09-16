target_sources_ifdef(CONFIG_HOOK_4G_ENABLE app PRIVATE
	hook_4g.c
	hook_at.c
	hook_cloud_proto.c
)
