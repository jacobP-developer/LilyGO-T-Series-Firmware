cmake_minimum_required(VERSION 3.16)

# Allow multiple definitions so our override of
# ieee80211_raw_frame_sanity_check in deauth.c can replace the one
# inside the pre-compiled libnet80211.a.
add_link_options(-Wl,--allow-multiple-definition)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m4nt1s-firmware)