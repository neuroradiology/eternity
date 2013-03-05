//----------------------------------------------------------------------------
//
// Copyright(C) 2005 Simon Howard, James Haley
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//--------------------------------------------------------------------------
//
// Key Bindings
//
//----------------------------------------------------------------------------

#ifndef G_BIND_H__
#define G_BIND_H__

void G_InitKeyBindings(void);
int  G_KeyResponder(event_t *ev, int bclass);

void G_ClearKeyStates(void);

typedef void (*binding_handler)(event_t *ev);

void G_EditBinding(const char *action);
const char *G_BoundKeys(const char *action);
const char *G_FirstBoundKey(const char *action);

// default file loading

void G_LoadDefaults(void);
void G_SaveDefaults(void);

void G_Bind_AddCommands(void);

// haleyjd 07/03/04: key binding classes
enum keyactionclass
{
   kac_game,            // game bindings -- handled by G_BuildTiccmd
   kac_menu,            // menu bindings -- handled by MN_Responder
   kac_map,             // map  bindings -- handled by AM_Responder
   kac_console,         // con. bindings -- handled by C_Repsonder
   kac_hud,             // hud  bindings -- handled by HU_Responder
   kac_cmd,             // command
   NUMKEYACTIONCLASSES
};

enum keyaction_e
{
   ka_nothing,
   ka_forward,
   ka_backward,
   ka_left,      
   ka_right,     
   ka_moveleft,  
   ka_moveright, 
   ka_use,       
   ka_strafe,    
   ka_attack,    
   ka_flip,
   ka_speed,
   ka_jump,
   ka_autorun,
   ka_mlook,     
   ka_lookup,    
   ka_lookdown,  
   ka_center,
   ka_flyup,
   ka_flydown,
   ka_flycenter,
   ka_weapon1,   
   ka_weapon2,   
   ka_weapon3,   
   ka_weapon4,   
   ka_weapon5,   
   ka_weapon6,   
   ka_weapon7,   
   ka_weapon8,   
   ka_weapon9,   
   ka_nextweapon,
   ka_weaponup,
   ka_weapondown,
   ka_frags,   
   ka_menu_toggle,   
   ka_menu_help,     
   ka_menu_setup,    
   ka_menu_up,       
   ka_menu_down,     
   ka_menu_confirm,  
   ka_menu_previous, 
   ka_menu_left,     
   ka_menu_right,    
   ka_menu_pageup,   
   ka_menu_pagedown,
   ka_menu_contents,
   ka_map_right,   
   ka_map_left,    
   ka_map_up,      
   ka_map_down,    
   ka_map_zoomin,  
   ka_map_zoomout, 
   ka_map_toggle,  
   ka_map_gobig,   
   ka_map_follow,  
   ka_map_mark,    
   ka_map_clear,   
   ka_map_grid,
   ka_console_pageup,
   ka_console_pagedown,
   ka_console_toggle,
   ka_console_tab,
   ka_console_enter,
   ka_console_up,
   ka_console_down,
   ka_console_backspace,
   NUMKEYACTIONS
};

#endif

// EOF
