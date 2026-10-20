idf_component_register(
    SRCS "airplay_rtsp_codec.c" "airplay_rtsp_pairing.c" "airplay_bplist.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_pairing
)
