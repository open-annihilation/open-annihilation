# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The sockets of oa-netgame (socket_host.cpp), network play's and the
# streams' (stream_socket.hpp), the one source that calls the system's
# socket functions. The system library they need on Windows is linked where
# oa-netgame is defined, as every link of a target is.
target_sources(oa-netgame PRIVATE socket_host.cpp overlay_route.cpp)
