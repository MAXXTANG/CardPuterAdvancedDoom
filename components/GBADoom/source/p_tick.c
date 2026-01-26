/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000,2002 by
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
 *      Thinker, Ticker.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "p_user.h"
#include "p_spec.h"
#include "p_tick.h"
#include "p_map.h"

#include "global_data.h"

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "../../main/i_net_serial.h"
#endif

//
// THINKERS
// All thinkers should be allocated by Z_Malloc
// so they can be operated on uniformly.
// The actual structures will vary in size,
// but the first element must be thinker_t.
//



//
// P_InitThinkers
//

void P_InitThinkers(void)
{
  thinkercap.prev = thinkercap.next  = &thinkercap;
}

//
// P_AddThinker
// Adds a new thinker at the end of the list.
//

void P_AddThinker(thinker_t* thinker)
{
  thinkercap.prev->next = thinker;
  thinker->next = &thinkercap;
  thinker->prev = thinkercap.prev;
  thinkercap.prev = thinker;
  thinker->self_before_load = thinker->self;
  thinker->self = thinker;
}

//
// killough 11/98:
//
// Make currentthinker external, so that P_RemoveThinkerDelayed
// can adjust currentthinker when thinkers self-remove.


//
// P_RemoveThinkerDelayed()
//
// Called automatically as part of the thinker loop in P_RunThinkers(),
// on nodes which are pending deletion.
//
// If this thinker has no more pointers referencing it indirectly,
// remove it, and set currentthinker to one node preceeding it, so
// that the next step in P_RunThinkers() will get its successor.
//

void P_RemoveThinkerDelayed(thinker_t *thinker)
{

    thinker_t *next = thinker->next;
    /* Note that currentthinker is guaranteed to point to us,
         * and since we're freeing our memory, we had better change that. So
         * point it to thinker->prev, so the iterator will correctly move on to
         * thinker->prev->next = thinker->next */
    (next->prev = thinker->prev)->next = next;

    Z_Free(thinker);
}

void P_RemoveThingDelayed(thinker_t *thinker)
{

    thinker_t *next = thinker->next;
    /* Note that currentthinker is guaranteed to point to us,
         * and since we're freeing our memory, we had better change that. So
         * point it to thinker->prev, so the iterator will correctly move on to
         * thinker->prev->next = thinker->next */
    (next->prev = thinker->prev)->next = next;

    mobj_t* thing = (mobj_t*)thinker;

    if(thing->flags & MF_POOLED)
        thing->type = MT_NOTHING;
    else
        Z_Free(thinker);
}

//
// P_RemoveThinker
//
// Deallocation is lazy -- it will not actually be freed
// until its thinking turn comes up.
//
// killough 4/25/98:
//
// Instead of marking the function with -1 value cast to a function pointer,
// set the function to P_RemoveThinkerDelayed(), so that later, it will be
// removed automatically as part of the thinker process.
//

void P_RemoveThinker(thinker_t *thinker)
{
  thinker->function = P_RemoveThinkerDelayed;
}

void P_RemoveThing(mobj_t *thing)
{
  thing->thinker.function = P_RemoveThingDelayed;
}


/* cph 2002/01/13 - iterator for thinker list
 * WARNING: Do not modify thinkers between calls to this functin
 */
thinker_t* P_NextThinker(thinker_t* th)
{
  thinker_t* top = &_g->thinkerclasscap[th_all];
  if (!th) th = top;
  th = th->next;
  return th == top ? NULL : th;
}

/*
 * P_SetTarget
 *
 * This function is used to keep track of pointer references to mobj thinkers.
 * In Doom, objects such as lost souls could sometimes be removed despite
 * their still being referenced. In Boom, 'target' mobj fields were tested
 * during each gametic, and any objects pointed to by them would be prevented
 * from being removed. But this was incomplete, and was slow (every mobj was
 * checked during every gametic). Now, we keep a count of the number of
 * references, and delay removal until the count is 0.
 */

void P_SetTarget(mobj_t **mop, mobj_t *targ)
{
    *mop = targ;    // Set new target and if non-NULL, increase its counter
}


void P_Ticker (void)
{
  int i;

#ifdef ESP_PLATFORM
  extern int is_multiplayer;
  if (is_multiplayer) {
    _g->prndindex = (0xED + _g->leveltime) & 0xff;
  }
  if (is_multiplayer && _g->gamestate == GS_LEVEL && _g->leveltime > 50) {
    int remote_secret_exit = 0;
    int remote_next_episode = 0;
    int remote_next_map = 0;
    if (Serial_CheckExitLevel(&remote_secret_exit, &remote_next_episode, &remote_next_map) == ESP_OK) {
      printf("NET: Remote player triggered exit (secret=%d, next=E%dM%d) at tic %d\n", 
             remote_secret_exit, remote_next_episode, remote_next_map, _g->leveltime);
      _g->secretexit = remote_secret_exit;
      // Store next level info for G_DoWorldDone
      if (remote_next_episode > 0 && remote_next_map > 0) {
        _g->wminfo.epsd = remote_next_episode - 1;
        _g->wminfo.next = remote_next_map - 1;
      }
      _g->gameaction = ga_completed;
    }
  }
#endif

  /* pause if in menu and at least one tic has been run
   *
   * killough 9/29/98: note that this ties in with basetic,
   * since G_Ticker does the pausing during recording or
   * playback, and compenates by incrementing basetic.
   *
   * All of this complicated mess is used to preserve demo sync.
   */

  if (_g->menuactive && !_g->demoplayback && _g->players[_g->displayplayer].viewz != 1)
    return;

  P_MapStart();
               // not if this is an intermission screen
  if(_g->gamestate==GS_LEVEL)
  {
    // Think for all players in game
    for (i = 0; i < MAXPLAYERS; i++) {
      if (_g->playeringame[i])
        P_PlayerThink(&_g->players[i]);
    }
  }

  P_RunThinkers();
  P_UpdateSpecials();
  P_RespawnSpecials();
  P_MapEnd();
  _g->leveltime++;                       // for par times
}
