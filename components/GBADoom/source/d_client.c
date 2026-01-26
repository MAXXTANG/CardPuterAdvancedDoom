/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *    Network client. Passes information to/from server, staying
 *    synchronised.
 *    Contains the main wait loop, waiting for network input or
 *    time before doing the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *    M5 Cardputer UART Multiplayer: Lock-step input synchronization
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"
#include "p_mobj.h"

#include "protocol.h"
#include "i_network.h"
#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "lprintf.h"

#include "global_data.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#define NET_TAG "DOOM_NET"

// External declarations for serial network functions - use int for C compatibility
extern int is_multiplayer;
extern int is_master;
extern bool serial_net_is_ready(void);
extern int Serial_NetSend(int tic, const void *ticcmd_data);
extern int Serial_WaitForTiccmd(int expected_tic, void *ticcmd_data, int timeout_ms);
extern int Serial_GetLastRemoteTic(void);
extern int Serial_GetDesyncCount(void);
extern void Serial_FlushBuffers(void);
extern int Serial_SendChecksum(int tic, int player0_x, int player0_y);
extern int Serial_CheckChecksum(int *out_tic, int *out_x, int *out_y);
extern int desync_detected;
extern esp_err_t Serial_CheckExitLevel(int *out_secret_exit, int *out_next_episode, int *out_next_map);
extern esp_err_t Serial_CheckHandshakePackets(void);

// Flag to tell G_DoCompleted not to overwrite wminfo (defined in g_game.c)
extern int wminfo_set_by_network;

// Network sync state
static int net_timeout_count = 0;
static int net_sync_tics = 0;
static int consecutive_timeouts = 0;  // Track consecutive failures
static int level_transition_timeouts = 0; // Track timeouts for level transition detection
static int last_sent_tic = -1;
static int last_recv_tic = -1;

// Checksum interval for desync detection (every 64 tics)
#define CHECKSUM_INTERVAL 64

// Debug: print heap every N tics
#define HEAP_CHECK_INTERVAL 350  // Every 10 seconds at 35 tics/sec

// Watchdog feeding for single player/demo mode
static int64_t last_watchdog_feed_time = 0;
#define WATCHDOG_FEED_INTERVAL_US 500000  // Feed every 500ms (half the typical 1s timeout)
#endif


void D_InitNetGame (void)
{
    // Initialize player indices
    _g->consoleplayer = 0;  // Default to player 0
    _g->displayplayer = 0;
    _g->netgame = false;    // Default to single player
    
    // Player 0 is always in game
    _g->playeringame[0] = true;
    _g->playeringame[1] = false;
    
#ifdef ESP_PLATFORM
    if (is_multiplayer) {
        // Enable netgame mode - this flag tells the engine it's multiplayer
        _g->netgame = true;
        
        // In multiplayer, both players are active
        _g->playeringame[1] = true;
        
        // CRITICAL: Enable deathmatch for player-to-player damage
        _g->deathmatch = 1;
        _g->nomonsters = 0;  // Ensure monsters are present unless explicitly set
        
        // Slave plays as player 1, Master plays as player 0
        if (!is_master) {
            _g->consoleplayer = 1;
            _g->displayplayer = 1;
        }
        
        // Initialize health for all players in multiplayer (both master and slave)
        for (int i = 0; i < MAXPLAYERS; i++) {
            if (_g->playeringame[i]) {
                _g->players[i].health = 100; // Use explicit value for clarity
            }
        }
        
        lprintf(LO_INFO, "D_InitNetGame: Multiplayer mode (%s) - consoleplayer=%d, netgame=%d, deathmatch=%d\n", 
                is_master ? "MASTER" : "SLAVE", _g->consoleplayer, _g->netgame, _g->deathmatch);
        lprintf(LO_INFO, "D_InitNetGame: Player 0 health=%d, Player 1 health=%d\n",
                _g->players[0].health, _g->players[1].health);
        
        // Log initial heap state
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        lprintf(LO_INFO, "Initial Gameplay Heap: %lu bytes (max block: %lu)\n", 
                free_heap, max_block);
    }
#endif
}

void D_BuildNewTiccmds(void)
{
    int newtics = I_GetTime() - _g->lastmadetic;
    _g->lastmadetic += newtics;

    while (newtics--)
    {
        I_StartTic();
        if (_g->maketic - _g->gametic > 3)
            break;

        // Build command for local player (consoleplayer)
        G_BuildTiccmd(&_g->netcmds[_g->consoleplayer]);
        _g->maketic++;
    }
}

#ifdef ESP_PLATFORM
/**
 * @brief Exchange ticcmds with remote player (STRICT BLOCKING LOCK-STEP)
 * 
 * Master/Slave protocol:
 * - Master sends first, then BLOCKS until slave response arrives
 * - Slave BLOCKS until master's packet arrives, then sends response
 * 
 * STRICT LOCK-STEP: The engine WILL NOT advance until BOTH devices have
 * the same tic. Game will stutter/freeze rather than desync.
 * 
 * NO PREDICTION: If packet not received, we wait - never use cached/empty cmd.
 * 
 * @param local_cmd Our local ticcmd to send
 * @param remote_cmd Buffer to receive remote ticcmd
 * @return true if exchange successful, false on timeout (game should halt)
 */
static boolean NetExchangeTiccmds(ticcmd_t *local_cmd, ticcmd_t *remote_cmd)
{
    if (!serial_net_is_ready()) {
        return false;
    }
    
    int current_tic = _g->gametic;
    esp_err_t ret;
    
    // DETECT LEVEL TRANSITION: Check for EXIT_LEVEL packet from remote player
    // Either player can trigger exit, so both need to check
    int last_remote = Serial_GetLastRemoteTic();
    
    // Fallback for slave: Master tic reset to low number while slave is at high tic
    if (!is_master && last_remote >= 0 && last_remote < 100 && current_tic > 1000) {
        printf("SLAVE: Master tic reset detected (remote tic=%d, local tic=%d). Triggering level transition.\n", 
               last_remote, current_tic);
        _g->gameaction = ga_completed;
        memset(remote_cmd, 0, sizeof(ticcmd_t));
        return true;
    }
    
    // STRICT SEQUENCE NUMBER CHECK: We only accept the exact tic we expect
    // If we receive a packet with wrong tic ID, we discard it and keep waiting
    if (last_remote >= 0 && last_remote != current_tic && last_remote != current_tic - 1) {
        printf("TIC SEQUENCE BREAK: local=%d, remote_last=%d - DISCARDING\n",
               current_tic, last_remote);
        // We'll still try to get the right tic via blocking wait
    }
    
        if (is_master) {
            // MASTER: Send our tic first
            ret = Serial_NetSend(current_tic, local_cmd);
            if (ret != ESP_OK) {
                printf("MASTER SEND FAIL: tic %d\n", current_tic);
                return false;
            }
            last_sent_tic = current_tic;
            
            // IRON CURTAIN: Strict blocking wait for slave's exact tic
            // Game WILL FREEZE if packet is missing
            ret = Serial_WaitForTiccmd(current_tic, remote_cmd, 0);  // timeout ignored, uses strict 2s
            if (ret == ESP_OK) {
                consecutive_timeouts = 0;
                level_transition_timeouts = 0;
                last_recv_tic = current_tic;
                return true;
            } else {
                // CRITICAL: Timeout - DO NOT ADVANCE WITHOUT SLAVE'S INPUT
                consecutive_timeouts++;
                printf("MASTER IRON CURTAIN: tic %d (timeout #%d) - GAME FROZEN\n", 
                       current_tic, consecutive_timeouts);
                return false;  // Return false - game halts until packet arrives
            }
            
        } else {
            // SLAVE: Check for handshake packets (master might be trying to resync level)
            // Skip this check for the first 50 tics after level load to avoid detecting
            // stale handshake packets that arrived during level loading
            if (current_tic > 50 && Serial_CheckHandshakePackets() == ESP_OK) {
                printf("SLAVE: Received handshake packet during gameplay at tic %d - triggering level reload\n", current_tic);
                _g->gameaction = ga_loadlevel;
                memset(remote_cmd, 0, sizeof(ticcmd_t));
                return true;
            }
            
            // SLAVE: Wait for master's packet first (STRICT BLOCKING)
            ret = Serial_WaitForTiccmd(current_tic, remote_cmd, 0);
            if (ret == ESP_OK) {
                consecutive_timeouts = 0;
                level_transition_timeouts = 0;
                last_recv_tic = current_tic;
            } else {
                // TIMEOUT: Master is not responding
                printf("SLAVE TIMEOUT: current_tic=%d, gametic=%d\n", current_tic, _g->gametic);
                if (current_tic == 0 || current_tic == 1 || _g->gametic <= 10 || current_tic <= 10) {
                    printf("SLAVE: Timeout at low tic %d - forcing level reload to re-sync with master\n", current_tic);
                    _g->gameaction = ga_loadlevel;
                    memset(remote_cmd, 0, sizeof(ticcmd_t));
                    return true;
                }
                
        // Level transition detection: if we've had multiple consecutive timeouts
        // and we're at a high tic count, assume master has moved to next level
        level_transition_timeouts++;
        
        // Emergency recovery: If we have too many timeouts even at low tics,
        // assume master is dead and reload the level to attempt re-sync
        if (level_transition_timeouts >= 5) {
            printf("SLAVE: EMERGENCY - Too many timeouts (%d) at tic %d - reloading level to re-sync\n",
                   level_transition_timeouts, current_tic);
            _g->gameaction = ga_loadlevel;
            memset(remote_cmd, 0, sizeof(ticcmd_t));
            return true;
        }
                
                if (level_transition_timeouts >= 3 && current_tic > 500) {
                    printf("SLAVE: Multiple timeouts (%d) at high tic %d - assuming master level transition\n", 
                           level_transition_timeouts, current_tic);
                    _g->secretexit = 0; // Assume normal exit
                    _g->gameaction = ga_completed;
                    memset(remote_cmd, 0, sizeof(ticcmd_t));
                    return true;
                }
                
                // Fallback: Master tic reset detection even when no packets received
                // If we're at high tic and master's last known tic is low (or unknown)
                // and we're timing out, assume master has reset for new level
                if (current_tic > 1000 && (last_remote < 100 || last_remote == -1)) {
                    printf("SLAVE: Master tic reset detected (remote tic=%d, local tic=%d). Triggering level transition.\n", 
                           last_remote, current_tic);
                    _g->secretexit = 0;
                    _g->gameaction = ga_completed;
                    memset(remote_cmd, 0, sizeof(ticcmd_t));
                    return true;
                }
                
                // CRITICAL: Cannot proceed without master's input
                consecutive_timeouts++;
                printf("SLAVE IRON CURTAIN: tic %d (timeout #%d) - GAME FROZEN\n",
                       current_tic, consecutive_timeouts);
                return false;  // Do NOT advance
            }
            
            // Send our response (only after receiving master's command)
            ret = Serial_NetSend(current_tic, local_cmd);
            if (ret == ESP_OK) {
                last_sent_tic = current_tic;
            } else {
                printf("SLAVE SEND FAIL: tic %d\n", current_tic);
                // Master will timeout and both will freeze - that's correct behavior
            }
            
            return true;
        }
}
#endif

void TryRunTics (void)
{
    int runtics;
    int entertime = I_GetTime();
    static int debug_counter = 0;
    static int call_counter = 0;
    
    const int POSITION_TOLERANCE = 196608;  // 3 units in fixed-point (65536*3)
    
    call_counter++;
    if (call_counter % 100 == 0) {
        printf("TryRunTics ENTRY #%d\n", call_counter);
    }

    // Wait for tics to run
    while (1)
    {
        D_BuildNewTiccmds();

        runtics = (_g->maketic) - _g->gametic;
        
        // Debug: print tic state every 35 frames
        if (++debug_counter >= 35) {
            debug_counter = 0;
            printf("TryRunTics: maketic=%d gametic=%d runtics=%d gamestate=%d\n",
                   _g->maketic, _g->gametic, runtics, _g->gamestate);
        }
        
        if (runtics <= 0)
        {
            if (I_GetTime() - entertime > 10)
            {
                M_Ticker();
                return;
            }
            // Yield to prevent watchdog reset in single player/demo mode
            // 1ms delay is negligible for 35Hz game (28.5ms per frame)
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        else
            break;
    }

    while (runtics-- > 0)
    {
#ifdef ESP_PLATFORM
        // Multiplayer: STRICT LOCK-STEP - Exchange inputs before advancing
        // CRITICAL: Only sync during actual gameplay (GS_LEVEL), NOT during intro/menu screens
        // This prevents the slave from timing out waiting for tic 0 while still at intro screen
        if (is_multiplayer && serial_net_is_ready() && _g->gamestate == GS_LEVEL)
        {
        // ================================================================
        // EARLY LEVEL MISMATCH CHECK (SLAVE ONLY) - Check BEFORE blocking on ticcmd
        // This allows slave to detect when master has moved to a new level
        // and load it immediately without getting stuck in ticcmd wait
        // ================================================================
        if (!is_master) {
            int target_episode = 0, target_map = 0;
            if (Serial_CheckLevelMismatch(_g->gameepisode, _g->gamemap, 
                                          &target_episode, &target_map) == ESP_OK) {
                printf("*** SLAVE LEVEL MISMATCH - LOADING CORRECT LEVEL ***\n");
                printf("Current: E%dM%d, Target: E%dM%d\n",
                       _g->gameepisode, _g->gamemap, target_episode, target_map);
                
                // Set flag to skip handshake (master already loaded, won't respond)
                extern int skip_level_handshake;
                skip_level_handshake = 1;
                
                // Set the correct level and trigger load
                _g->gameepisode = target_episode;
                _g->gamemap = target_map;
                _g->gameaction = ga_loadlevel;
                Serial_ClearLevelMismatch();
                return;  // Exit to trigger level load
            }
        }
        
        // Determine local and remote player indices
        int local_player = _g->consoleplayer;
        int remote_player = 1 - local_player;  // 0->1 or 1->0
        
        // HEARTBEAT DURING DEATH: Ensure we always send ticcmds even when dead
        // Check if local player is dead but still needs to send inputs for respawn
        if (_g->players[local_player].playerstate == PST_DEAD) {
            // Player is dead, but we MUST still send network packets
            // Build ticcmd to capture potential respawn inputs (BT_USE, BT_ATTACK)
            G_BuildTiccmd(&_g->netcmds[local_player]);
            printf("HEARTBEAT: Dead player %d still sending ticcmd (buttons=0x%02X)\n", 
                   local_player, _g->netcmds[local_player].buttons);
        }
        
        // Exchange ticcmds: send our cmd, receive theirs
        // STRICT: If exchange fails, do NOT advance game state!
        ticcmd_t remote_cmd;
        if (!NetExchangeTiccmds(&_g->netcmds[local_player], &remote_cmd)) {
            // SYNC FAILURE: Do not advance game, retry next frame
            // This causes a stutter but prevents desync
            printf("SYNC STALL: Waiting for remote tic %d\n", _g->gametic);
            return;  // Exit TryRunTics, will retry on next call
        }
            
            // Check if gameaction was set during network exchange (level transition)
            if (_g->gameaction == ga_completed) {
                // Level transition detected - store empty remote command and continue
                memset(&remote_cmd, 0, sizeof(ticcmd_t));
            }
            
            // Store remote player's command (only if exchange succeeded)
            memcpy(&_g->netcmds[remote_player], &remote_cmd, sizeof(ticcmd_t));
            
            // DEBUG: Log attack buttons (to diagnose enemy desync)
            if ((remote_cmd.buttons & 1) || (_g->netcmds[local_player].buttons & 1)) {
                printf("Tic %d: P%d attack=%d, P%d attack=%d\n",
                       _g->gametic,
                       local_player, (_g->netcmds[local_player].buttons & 1) ? 1 : 0,
                       remote_player, (remote_cmd.buttons & 1) ? 1 : 0);
            }
            
            net_sync_tics++;
            
            // ================================================================
            // ZERO-TOLERANCE PLAYER SYNC: Position/angle/health checksum every 16 tics
            // Both players' full state must match exactly for player-to-player damage
            // NEW: Also includes episode/map for automatic level correction!
            // ================================================================
            if ((_g->gametic % 16) == 0 && _g->gametic > 0) {
                // Get both players' full state
                mobj_t *p0_mo = _g->players[0].mo;
                mobj_t *p1_mo = _g->players[1].mo;
                
                if (p0_mo && p1_mo) {
                    if (is_master) {
                        // MASTER: Send extended checksum with both players' full state + level info
                        // Ensure player 1 health is not 0 (workaround for initialization issue)
                        int p0_health = _g->players[0].health;
                        int p1_health = _g->players[1].health;
                        if (p1_health == 0) {
                            printf("WARNING: Master player 1 health is 0, correcting to 100\n");
                            p1_health = 100;
                            _g->players[1].health = p1_health;
                        }
                        printf("MASTER EXTENDED_CHKSUM: tic=%d E%dM%d, P0.health=%d, P1.health=%d\n",
                               _g->gametic, _g->gameepisode, _g->gamemap, p0_health, p1_health);
                        Serial_SendExtendedChecksum(_g->gametic, _g->gameepisode, _g->gamemap,
                            p0_mo->x, p0_mo->y, p0_mo->angle, p0_health,
                            p1_mo->x, p1_mo->y, p1_mo->angle, p1_health);
                    } else {
                        // SLAVE: Check for incoming checksum and verify
                        int chk_tic, chk_episode, chk_map;
                        int chk_p0_x, chk_p0_y, chk_p0_angle, chk_p0_health;
                        int chk_p1_x, chk_p1_y, chk_p1_angle, chk_p1_health;
                        
                        if (Serial_CheckExtendedChecksum(&chk_tic, &chk_episode, &chk_map,
                            &chk_p0_x, &chk_p0_y, &chk_p0_angle, &chk_p0_health,
                            &chk_p1_x, &chk_p1_y, &chk_p1_angle, &chk_p1_health) == ESP_OK) {
                            
                            // NOTE: Level mismatch is now checked EVERY tic at start of loop
                            // via Serial_CheckLevelMismatch(), so we don't need to check here
                            
                            // Only process checksum if it's for the current tic (or very close) to avoid stale data
                            if (abs(chk_tic - _g->gametic) <= 2) {
                                // Compare player 0 state - update if different (remote player on slave)
                                if (chk_p0_x != p0_mo->x || chk_p0_y != p0_mo->y || 
                                    chk_p0_angle != p0_mo->angle) {
                                    printf("SLAVE POSITION SYNC UPDATE: Tic %d - P0 (remote) pos/angle Master=(%d,%d,%d) Local=(%d,%d,%d) -> updating\n",
                                           _g->gametic, chk_p0_x, chk_p0_y, chk_p0_angle, p0_mo->x, p0_mo->y, p0_mo->angle);
                                    p0_mo->x = chk_p0_x;
                                    p0_mo->y = chk_p0_y;
                                    p0_mo->angle = chk_p0_angle;
                                }
                            
                            // Compare player 1 state - local player on slave
                            // Use tolerance for small differences, update if within threshold
                            int dx = chk_p1_x - p1_mo->x;
                            int dy = chk_p1_y - p1_mo->y;
                            if (dx < 0) dx = -dx;
                            if (dy < 0) dy = -dy;
                            if (dx <= POSITION_TOLERANCE && dy <= POSITION_TOLERANCE) {
                                // Small position drift, correct it
                                if (chk_p1_x != p1_mo->x || chk_p1_y != p1_mo->y) {
                                    printf("SLAVE POSITION CORRECTION (small drift): Tic %d - P1 (local) pos Master=(%d,%d) Local=(%d,%d) diff=(%d,%d) -> updating\n",
                                           _g->gametic, chk_p1_x, chk_p1_y, p1_mo->x, p1_mo->y, dx, dy);
                                    p1_mo->x = chk_p1_x;
                                    p1_mo->y = chk_p1_y;
                                }
                                // Also update angle if different
                                if (chk_p1_angle != p1_mo->angle) {
                                    printf("SLAVE ANGLE CORRECTION: Tic %d - P1 (local) angle Master=%d Local=%d -> updating\n",
                                           _g->gametic, chk_p1_angle, p1_mo->angle);
                                    p1_mo->angle = chk_p1_angle;
                                }
                            } else if (_g->gametic < 100) {
                                // For the first 100 tics, ignore position mismatch for local player (initialization issue)
                                printf("SLAVE POSITION IGNORE (early game): Tic %d - P1 (local) pos/angle mismatch, ignoring\n", _g->gametic);
                            } else {
                                printf("SLAVE POSITION DESYNC CRITICAL: Tic %d - P1 (local) pos/angle mismatch Master=(%d,%d,%d) Local=(%d,%d,%d)\n",
                                       _g->gametic, chk_p1_x, chk_p1_y, chk_p1_angle, p1_mo->x, p1_mo->y, p1_mo->angle);
                                desync_detected = true;
                                return;
                            }
                            
                            // HEALTH DESYNC HANDLING
                            // For local player: halt on mismatch (critical desync)
                            // For remote player: update health to match master (keep in sync)
                            if (is_master) {
                                // Master: player 0 is local, player 1 is remote
                                if (chk_p0_health != _g->players[0].health) {
                                    printf("HEALTH DESYNC CRITICAL: Tic %d - P0 (local) health mismatch Master=%d Local=%d\n", 
                                           _g->gametic, chk_p0_health, _g->players[0].health);
                                    desync_detected = true;
                                    return;
                                }
                                // Remote player 1: update if different
                                if (chk_p1_health != _g->players[1].health) {
                                    printf("HEALTH SYNC UPDATE: Tic %d - P1 (remote) health Master=%d Local=%d -> updating\n",
                                           _g->gametic, chk_p1_health, _g->players[1].health);
                                    _g->players[1].health = chk_p1_health;
                                }
                            } else {
                                // Slave: player 1 is local, player 0 is remote
                                if (chk_p1_health != _g->players[1].health) {
                                    // During the first 100 tics of the game, ignore health mismatch for local player (initialization issue)
                                    if (_g->gametic < 100) {
                                        printf("HEALTH INIT IGNORE (early game): Tic %d - P1 (local) health Master=%d Local=%d (ignoring mismatch)\n", 
                                               _g->gametic, chk_p1_health, _g->players[1].health);
                                        // Do nothing, keep our health at 100
                                    } else {
                                        printf("HEALTH DESYNC CRITICAL: Tic %d - P1 (local) health mismatch Master=%d Local=%d\n",
                                               _g->gametic, chk_p1_health, _g->players[1].health);
                                        desync_detected = true;
                                        return;
                                    }
                                }
                                // Remote player 0: update if different
                                if (chk_p0_health != _g->players[0].health) {
                                    printf("HEALTH SYNC UPDATE: Tic %d - P0 (remote) health Master=%d Local=%d -> updating\n",
                                           _g->gametic, chk_p0_health, _g->players[0].health);
                                    _g->players[0].health = chk_p0_health;
                                }
                            }
                        }
                    }
                }
            }
            
            // Check if desync was detected - halt game
            if (desync_detected) {
                printf("GAME HALTED: Desync detected\n");
                return;
            }
            
            // Periodic heap check during gameplay
            if ((net_sync_tics % HEAP_CHECK_INTERVAL) == 0) {
                uint32_t free_heap = esp_get_free_heap_size();
                ESP_LOGI(NET_TAG, "Tic %d: Heap=%lu bytes, Timeouts=%d", 
                         _g->gametic, free_heap, net_timeout_count);
            }
            }
        }
#endif

        if (_g->advancedemo)
            D_DoAdvanceDemo ();

        M_Ticker ();
        G_Ticker ();
        _g->gametic++;
    }
}
