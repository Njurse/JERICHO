#ifndef JER_NET_H
#define JER_NET_H

/*
 * jer_net.h -- the JERICHO addon network bridge.
 *
 * A tiny, transport-agnostic way for ANY module to put data on the wire that
 * its peers need but that is NOT derivable locally -- host-spawned entities,
 * event logs, RNG outcomes a module author owns, per-mod gameplay state. The
 * multiplayer module (mp) owns the sockets; this API just multiplexes named,
 * reliable-or-latest-wins channels over the active session.
 *
 * Model:
 *   - A channel is a short name (<= 15 chars) + a reliability policy.
 *   - The HOST relays: jer_net_send() from the host is broadcast to every
 *     client; from a client it goes to the host only (the host may relay).
 *   - Inbound payloads arrive via JER_EVENT_NET_RECV (JER_ARGS_NET_RECV),
 *     carrying the channel name, the sending player id and the bytes.
 *
 * With no active session these are safe no-ops (jer_net_is_active() == 0),
 * so a module can call them unconditionally.
 *
 * A channel whose data affects the simulation should be RELIABLE and is
 * tagged with the frame it belongs to (so host and clients consume it on the
 * same step); a purely cosmetic channel can be latest-wins.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Reliability policy for a channel. */
enum
{
	JER_NET_UNRELIABLE = 0,		/* latest-wins; a dropped message is fine */
	JER_NET_RELIABLE = 1		/* ordered + retransmitted; for sim-affecting data */
};

/* Register a channel (idempotent; returns a channel slot >= 0 or -1 on error).
 * Call once from the module entry. `name` must be <= 15 chars. */
int jer_net_register_channel(const char* name, int reliability);

/* Send `len` bytes on `name`. Returns 1 if it was queued, 0 if there is no
 * session or the channel is unknown (data/length are not retained). */
int jer_net_send(const char* name, const void* data, int len);

/* Is a multiplayer session live (host or connected client)? */
int jer_net_is_active(void);

/* Is this instance the host? */
int jer_net_is_host(void);

/* Number of peers currently connected (host: clients; client: 1 when up). */
int jer_net_peer_count(void);

/* This machine's player id (0 = host). -1 when not in a session. */
int jer_net_local_player(void);

/* Is our own car a real CHOICE yet (a pick, or -mpcar), rather than what mp assigned a
 * player who has not picked? A carhacks-style module must not publish an assignment:
 * the session's import set would take a resident slot nobody drives. */
int jer_net_local_car_chosen(void);

/* Is player `id` still in the session? (0 = the host.) A module that holds per-player
 * state - carhacks holds a resident slot per player's car - needs this to notice a
 * player LEAVING: the session's car table is what a machine learns about its peers
 * from, and a player who has gone is simply no longer in it. Returns 1 while the
 * player is present and 0 with no session at all, so it is safe to call always. */
int jer_net_player_present(int id);

#ifdef __cplusplus
}
#endif

#endif /* JER_NET_H */
