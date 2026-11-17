// LAN and direct (peer-to-peer) co-op.
// The game already has local Wi-Fi co-op (CO-OP MULTIPLAYER > LOCAL WI-FI): its "Bonjour_Tcp" network setup finds
// games with Marmalade's s3eZeroConf extension, then plays over TCP. The Android release ships without s3eZeroConf,
// so the port provides it:
//   - a hosting phone announces its game with a UDP broadcast every second (port LAN_DISCOVERY_PORT);
//   - a joining phone broadcasts a query when it starts looking, and also asks the addresses typed in for direct
//     play (debug.boz64.peers, files/boz64_peers.txt, peers.txt next to the PC exe or in the phone's
//     Android/data/<package>/files: IPs or host names, separated by commas, spaces or lines);
//   - broadcasts go to every network the device is on, VPN adapters included, so VPNs that act as a LAN (Radmin
//     VPN, Hamachi, ZeroTier) work like home Wi-Fi; VPNs without broadcast (Tailscale, WireGuard) use the list;
//   - games heard are remembered from the start, so the game's short search finds a far host at once;
//   - the game itself then connects over TCP. Its listening port is fixed at LAN_GAME_PORT instead of a random
//     one, so a host on the internet can forward the two ports (UDP 47710 and TCP 47711) for direct play.
#pragma once
#include <string>

#include "common.h"

constexpr u16 LAN_DISCOVERY_PORT = 47710;  // UDP
constexpr u16 LAN_GAME_PORT = 47711;       // TCP

void lan_init(const std::string& files_dir);  // where the direct-play address list lives
// Called by the guest's connect() for an IPv4 address (network byte order). The game learns its own address by
// "connecting" a UDP socket to 99.99.99.99 and reading the local end, which picks the adapter of the default route
// (home Wi-Fi), not a VPN's. This turns that probe toward the other player, so the system picks the adapter that
// actually reaches them (Radmin VPN, Hamachi, ZeroTier, Tailscale or the LAN). A connection to a host's game port
// notes that host as the other player. Port in network byte order; returns true when it changed the address.
bool lan_redirect_probe(u32& ip, u16 port);
void lan_note_peer(u32 ip);  // the guest's accept() took a connection from this IPv4 address: a player joining
void lan_set_image(u32 image_lo);  // the game image is loaded (addresses are relative to it)
void lan_frame();                  // once per frame (any thread): installs the extension
// Reports games found and lost to the game. It calls back into game code that makes s3e calls, so it must run in
// game code on the app fibre (the network update hook), never from a GL or other JNI thunk.
void lan_poll();
