// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 2013 James Haley et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:  
//   Inventory
//
//-----------------------------------------------------------------------------

#define NEED_EDF_DEFINITIONS

#include "z_zone.h"
#include "i_system.h"

#include "Confuse/confuse.h"
#include "e_edf.h"
#include "e_hash.h"
#include "e_inventory.h"
#include "e_lib.h"
#include "e_sprite.h"

#include "autopalette.h"
#include "am_map.h"
#include "c_runcmd.h"
#include "d_dehtbl.h"
#include "d_gi.h"
#include "d_player.h"
#include "doomstat.h"
#include "g_game.h"
#include "info.h"
#include "m_collection.h"
#include "metaapi.h"
#include "p_mobj.h"
#include "p_skin.h"
#include "s_sound.h"
#include "v_video.h"

//=============================================================================
//
// Effect Classes
//

// Item Effect Type names
static const char *e_ItemEffectTypeNames[NUMITEMFX] =
{
   "None",
   "Health",
   "Armor",
   "Ammo",
   "Power",
   "WeaponGiver",
   "Artifact"
};

//
// E_EffectTypeForName
//
// Look up an effect type index based on its name. ITEMFX_NONE is returned
// if an invalid name is provided.
//
itemeffecttype_t E_EffectTypeForName(const char *name)
{
   itemeffecttype_t fx;
   
   if((fx = E_StrToNumLinear(e_ItemEffectTypeNames, NUMITEMFX, name)) == NUMITEMFX)
      fx = ITEMFX_NONE;

   return fx;
}

//=============================================================================
//
// Effects
//
// An effect applies when an item is collected, used, or dropped.
//

// The effects table contains as properties metatables for each effect.
static MetaTable e_effectsTable;

//
// E_addItemEffect
//
// Add an item effect from a cfg_t definition.
//
static itemeffect_t *E_addItemEffect(cfg_t *cfg)
{
   itemeffect_t *table;
   const char   *name = cfg_title(cfg);

   if(!(table = E_ItemEffectForName(name)))
      e_effectsTable.addObject((table = new itemeffect_t(name)));

   E_MetaTableFromCfg(cfg, table);

   return table;
}

//
// E_ItemEffectForName
//
// Find an item effect by name.
//
itemeffect_t *E_ItemEffectForName(const char *name)
{
   return runtime_cast<itemeffect_t *>(e_effectsTable.getObject(name));
}

//
// E_GetItemEffects
//
// Get the whole effects table, for the few places it is needed externally
// (mainly for console debugging features).
//
MetaTable *E_GetItemEffects()
{
   return &e_effectsTable;
}

//=============================================================================
//
// Effect Processing
//

// metakey vocabulary
#define KEY_ADDITIVETIME   "additivetime"
#define KEY_ALWAYSPICKUP   "alwayspickup"
#define KEY_AMMO           "ammo"
#define KEY_AMOUNT         "amount"
#define KEY_ARTIFACTTYPE   "artifacttype"
#define KEY_BACKPACKAMOUNT "ammo.backpackamount"
#define KEY_BACKPACKMAXAMT "ammo.backpackmaxamount"
#define KEY_AMMOCOOPSTAY   "ammo.coopstay"
#define KEY_AMMODMSTAY     "ammo.dmstay"
#define KEY_AMMODROPPED    "ammo.dropped"
#define KEY_AMMOGIVE       "ammo.give"
#define KEY_BONUS          "bonus"
#define KEY_CLASS          "class"
#define KEY_CLASSNAME      "classname"
#define KEY_DROPAMOUNT     "dropamount"
#define KEY_DURATION       "duration"
#define KEY_FULLAMOUNTONLY "fullamountonly"
#define KEY_ICON           "icon"
#define KEY_IGNORESKILL    "ignoreskill"
#define KEY_INTERHUBAMOUNT "interhubamount"
#define KEY_INVBAR         "invbar"
#define KEY_ITEMID         "itemid"
#define KEY_KEEPDEPLETED   "keepdepleted"
#define KEY_LOWMESSAGE     "lowmessage"
#define KEY_MAXAMOUNT      "maxamount"
#define KEY_MAXSAVEAMOUNT  "maxsaveamount"
#define KEY_SAVEAMOUNT     "saveamount"
#define KEY_SAVEDIVISOR    "savedivisor"
#define KEY_SAVEFACTOR     "savefactor"
#define KEY_SETHEALTH      "sethealth"
#define KEY_SORTORDER      "sortorder"
#define KEY_TYPE           "type"
#define KEY_UNDROPPABLE    "undroppable"
#define KEY_USEEFFECT      "useeffect"
#define KEY_USESOUND       "usesound"
#define KEY_WEAPON         "weapon"

// Interned metatable keys
static MetaKeyIndex keyAmount        (KEY_AMOUNT        );
static MetaKeyIndex keyArtifactType  (KEY_ARTIFACTTYPE  );
static MetaKeyIndex keyBackpackAmount(KEY_BACKPACKAMOUNT);
static MetaKeyIndex keyClass         (KEY_CLASS         );
static MetaKeyIndex keyClassName     (KEY_CLASSNAME     );
static MetaKeyIndex keyFullAmountOnly(KEY_FULLAMOUNTONLY);
static MetaKeyIndex keyInterHubAmount(KEY_INTERHUBAMOUNT);
static MetaKeyIndex keyItemID        (KEY_ITEMID        );
static MetaKeyIndex keyKeepDepleted  (KEY_KEEPDEPLETED  );
static MetaKeyIndex keyMaxAmount     (KEY_MAXAMOUNT     );
static MetaKeyIndex keyBackpackMaxAmt(KEY_BACKPACKMAXAMT);
static MetaKeyIndex keySortOrder     (KEY_SORTORDER     );

// Keys for specially treated artifact types
static MetaKeyIndex keyBackpackItem  (ARTI_BACKPACKITEM );

// Health fields
cfg_opt_t edf_healthfx_opts[] =
{
   CFG_INT(KEY_AMOUNT,     0,  CFGF_NONE), // amount to recover
   CFG_INT(KEY_MAXAMOUNT,  0,  CFGF_NONE), // max that can be recovered
   CFG_STR(KEY_LOWMESSAGE, "", CFGF_NONE), // message if health < amount
   
   CFG_FLAG(KEY_ALWAYSPICKUP, 0, CFGF_SIGNPREFIX), // if +, always pick up
   CFG_FLAG(KEY_SETHEALTH,    0, CFGF_SIGNPREFIX), // if +, sets health  
   
   CFG_END()
};

// Armor fields
cfg_opt_t edf_armorfx_opts[] =
{
   CFG_INT(KEY_SAVEAMOUNT,    0,  CFGF_NONE), // amount of armor given
   CFG_INT(KEY_SAVEFACTOR,    1,  CFGF_NONE), // numerator of save percentage
   CFG_INT(KEY_SAVEDIVISOR,   3,  CFGF_NONE), // denominator of save percentage
   CFG_INT(KEY_MAXSAVEAMOUNT, 0,  CFGF_NONE), // max save amount, for bonuses
   
   CFG_FLAG(KEY_ALWAYSPICKUP, 0, CFGF_SIGNPREFIX), // if +, always pick up
   CFG_FLAG(KEY_BONUS,        0, CFGF_SIGNPREFIX), // if +, is a bonus (adds to current armor type)

   CFG_END()
};

// Ammo giver fields
cfg_opt_t edf_ammofx_opts[] =
{
   CFG_STR(KEY_AMMO,       "", CFGF_NONE), // name of ammo type artifact to give
   CFG_INT(KEY_AMOUNT,      0, CFGF_NONE), // amount of ammo given
   CFG_INT(KEY_DROPAMOUNT,  0, CFGF_NONE), // amount of ammo given when item is dropped

   CFG_FLAG(KEY_IGNORESKILL, 0, CFGF_SIGNPREFIX), // if +, does not double on skills that double ammo

   CFG_END()
};

// Powerup effect fields
cfg_opt_t edf_powerfx_opts[] =
{
   CFG_INT(KEY_DURATION,  -1, CFGF_NONE), // length of time to last
   CFG_STR(KEY_TYPE,      "", CFGF_NONE), // name of powerup effect to give

   CFG_FLAG(KEY_ADDITIVETIME, 0, CFGF_SIGNPREFIX), // if +, adds to current duration

   // TODO: support HUBPOWER and PERSISTENTPOWER properties, etc.

   CFG_END()
};

// Weapon Giver effect fields
cfg_opt_t edf_weapgfx_opts[] =
{
   CFG_STR(KEY_WEAPON,       "", CFGF_NONE), // name of weapon to give
   CFG_INT(KEY_AMMODMSTAY,    0, CFGF_NONE), // amount of ammo given in DM w/weapons stay
   CFG_INT(KEY_AMMOCOOPSTAY,  0, CFGF_NONE), // amount of ammo given in coop w/weapon stay
   CFG_INT(KEY_AMMOGIVE,      0, CFGF_NONE), // amount of ammo given normally
   CFG_INT(KEY_AMMODROPPED,   0, CFGF_NONE), // amount of ammo given when dropped
   CFG_END()
};

// Artifact subtype names
static const char *artiTypeNames[NUMARTITYPES] =
{
   "NORMAL",   // an ordinary artifact
   "AMMO",     // ammo type
   "KEY",      // key
   "PUZZLE",   // puzzle item
   "POWER",    // powerup token
   "WEAPON",   // weapon token
   "QUEST"     // quest token
};

//
// E_artiTypeCB
//
// Value parsing callback for artifact type
//
static int E_artiTypeCB(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result)
{
   int res;

   if((res = E_StrToNumLinear(artiTypeNames, NUMARTITYPES, value)) == NUMARTITYPES)
      res = ARTI_NORMAL;

   *(int *)result = res;

   return 0;
}

// Artifact fields
cfg_opt_t edf_artifact_opts[] =
{
   CFG_INT(KEY_AMOUNT,          1, CFGF_NONE), // amount gained with one pickup
   CFG_INT(KEY_MAXAMOUNT,       1, CFGF_NONE), // max amount that can be carried in inventory
   CFG_INT(KEY_INTERHUBAMOUNT,  0, CFGF_NONE), // amount carryable between hubs (or levels)
   CFG_INT(KEY_SORTORDER,       0, CFGF_NONE), // relative ordering within inventory
   CFG_STR(KEY_ICON,           "", CFGF_NONE), // icon used on inventory bars
   CFG_STR(KEY_USESOUND,       "", CFGF_NONE), // sound to play when used
   CFG_STR(KEY_USEEFFECT,      "", CFGF_NONE), // effect to activate when used

   CFG_FLAG(KEY_UNDROPPABLE,    0, CFGF_SIGNPREFIX), // if +, cannot be dropped
   CFG_FLAG(KEY_INVBAR,         0, CFGF_SIGNPREFIX), // if +, appears in inventory bar
   CFG_FLAG(KEY_KEEPDEPLETED,   0, CFGF_SIGNPREFIX), // if +, remains in inventory if amount is 0
   CFG_FLAG(KEY_FULLAMOUNTONLY, 0, CFGF_SIGNPREFIX), // if +, pick up for full amount only

   CFG_INT_CB(KEY_ARTIFACTTYPE, ARTI_NORMAL, CFGF_NONE, E_artiTypeCB), // artifact sub-type

   // Sub-Type Specific Fields
   // These only have meaning if the value of artifacttype is the expected value.
   // You can set the keys on other artifacts, but they'll have no effect.

   // Ammo sub-type
   CFG_INT(KEY_BACKPACKAMOUNT, 0, CFGF_NONE),
   CFG_INT(KEY_BACKPACKMAXAMT, 0, CFGF_NONE),
   
   CFG_END()
};

static const char *e_ItemSectionNames[NUMITEMFX] =
{
   "",
   EDF_SEC_HEALTHFX,
   EDF_SEC_ARMORFX,
   EDF_SEC_AMMOFX,
   EDF_SEC_POWERFX,
   EDF_SEC_WEAPGFX,
   EDF_SEC_ARTIFACT
};

//
// E_processItemEffects
//
static void E_processItemEffects(cfg_t *cfg)
{
   // process each type of effect section
   for(int i = ITEMFX_HEALTH; i < NUMITEMFX; i++)
   {
      const char   *cfgSecName  = e_ItemSectionNames[i];
      const char   *className   = e_ItemEffectTypeNames[i];
      unsigned int  numSections = cfg_size(cfg, cfgSecName);

      E_EDFLogPrintf("\t* Processing %s item effects (%u defined)\n", 
                     className, numSections);

      // process each section of the current type
      for(unsigned int secNum = 0; secNum < numSections; secNum++)
      {
         auto newEffect = E_addItemEffect(cfg_getnsec(cfg, cfgSecName, secNum));

         // add the item effect type and name as properties
         newEffect->setInt(keyClass, i);
         newEffect->setConstString(keyClassName, className);

         E_EDFLogPrintf("\t\t* Processed item '%s'\n", newEffect->getKey());
      }
   }
}

//=============================================================================
//
// Ammo Types
//
// Solely for efficiency, a collection of all ammo type artifacts is kept.
// This allows iterating over all ammo types without having hard-coded sets
// of metakeys or scanning the entire artifacts table.
//

// The ammo types lookup provides fast lookup of every artifact type that is of
// subtype ARTI_AMMO. This is for benefit of effects like the backpack, and
// cheats that give all ammo.
static PODCollection<itemeffect_t *> e_ammoTypesLookup;

//
// E_GetNumAmmoTypes
//
// Returns the total number of ammo types defined.
//
size_t E_GetNumAmmoTypes()
{
   return e_ammoTypesLookup.getLength();
}

//
// E_AmmoTypeForIndex
//
// Get an ammo type for its index in the ammotypes lookup table.
// There is no extra bounds check here, so an illegal request will exit the 
// game engine. Use E_GetNumAmmoTypes to get the upper array bound.
//
itemeffect_t *E_AmmoTypeForIndex(size_t idx)
{
   return e_ammoTypesLookup[idx];
}

//
// E_collectAmmoTypes
//
// Scan the effects table for all effects that are of type ARTI_AMMO and add
// them to the ammo types lookup.
//
static void E_collectAmmoTypes()
{
   e_ammoTypesLookup.makeEmpty();

   itemeffect_t *itr = NULL;

   while((itr = runtime_cast<itemeffect_t *>(e_effectsTable.tableIterator(itr))))
   {
      if(itr->getInt(keyArtifactType, ARTI_NORMAL) == ARTI_AMMO)
         e_ammoTypesLookup.add(itr);
   }
}

//
// E_GiveAllAmmo
//
// Function to give the player a certain amount of all ammo types; the amount 
// given can be controlled using enumeration values in e_inventory.h
//
void E_GiveAllAmmo(player_t *player, giveallammo_e op, int amount)
{
   size_t numAmmo = E_GetNumAmmoTypes();

   for(size_t i = 0; i < numAmmo; i++)
   {
      int  giveamount = 0;
      auto ammoType   = E_AmmoTypeForIndex(i);

      switch(op)
      {
         // ioanch 20151225: removed GAA_BACKPACKAMOUNT because backpack really 
         // does more than populate the inventory.
      case GAA_MAXAMOUNT:
         giveamount = E_GetMaxAmountForArtifact(player, ammoType);
         break;
      case GAA_CUSTOM:
         giveamount = amount;
         break;
      default:
         break;
      }

      E_GiveInventoryItem(player, ammoType, giveamount);
   }
}

//=============================================================================
//
// Keys
//
// For similar reasons as for ammo, we keep a direct lookup table for artifacts
// of "key" type, so we can look them all up quickly when checking for locks,
// etc.
//

static PODCollection<itemeffect_t *> e_keysLookup;

//
// E_GetNumKeyItems
//
// Returns the total number of keys defined.
//
size_t E_GetNumKeyItems()
{
   return e_keysLookup.getLength();
}

//
// E_KeyItemForIndex
//
// Get a key type for its index in the ammotypes lookup table.
// There is no extra bounds check here, so an illegal request will exit the 
// game engine. Use E_GetNumKeyItems to get the upper array bound.
//
itemeffect_t *E_KeyItemForIndex(size_t idx)
{
   return e_keysLookup[idx];
}

//
// E_collectKeyItems
//
// Scan the effects table for all effects that are of type ARTI_KEY and add
// them to the keys lookup.
//
static void E_collectKeyItems()
{
   e_keysLookup.makeEmpty();

   itemeffect_t *itr = NULL;

   while((itr = runtime_cast<itemeffect_t *>(e_effectsTable.tableIterator(itr))))
   {
      if(itr->getInt(keyArtifactType, ARTI_NORMAL) == ARTI_KEY)
         e_keysLookup.add(itr);
   }
}

//=============================================================================
//
// Lockdefs
//

// lockdef color types
enum lockdefcolor_e
{
   LOCKDEF_COLOR_CONSTANT, // use a constant value
   LOCKDEF_COLOR_VARIABLE  // use a console variable
};

struct anykey_t
{
   unsigned int numKeys;
   itemeffect_t **keys;
};

struct lockdef_t
{
   DLListItem<lockdef_t> links; // list links
   int id;                      // lock ID, for ACS and internal use

   // Keys which are required to open the lock. If this list is empty, and
   // there are no "any" keys, then possession of any artifact of the "Key"
   // subtype is sufficient to open the lock.
   unsigned int numRequiredKeys;
   itemeffect_t **requiredKeys;

   // Groups of keys in which at least one key is needed in each group to
   // open the lock - this forms a two-dimensional grid of lists.
   unsigned int numAnyLists; // number of anykey_t structures
   unsigned int numAnyKeys;  // total number of keys in the structures
   anykey_t *anyKeys;

   char *message;        // message to give if attempt to open fails
   char *remoteMessage;  // message to give if remote attempt to open fails
   char *lockedSound;    // name of sound to play on failure

   // Lock color data
   lockdefcolor_e colorType; // either constant or variable
   int  color;               // constant color, if colorType == LOCKDEF_COLOR_CONSTANT
   int *colorVar;            // cvar color, if colorType == LOCKDEF_COLOR_VARIABLE    
};

// Lockdefs hash, by ID number
static EHashTable<lockdef_t, EIntHashKey, &lockdef_t::id, &lockdef_t::links> e_lockDefs;

#define ITEM_LOCKDEF_REQUIRE  "require"
#define ITEM_LOCKDEF_ANY      "any"
#define ITEM_LOCKDEF_MESSAGE  "message"
#define ITEM_LOCKDEF_REMOTE   "remotemessage"
#define ITEM_LOCKDEF_ANY_KEYS "keys"
#define ITEM_LOCKDEF_LOCKSND  "lockedsound"
#define ITEM_LOCKDEF_MAPCOLOR "mapcolor"

// "any" section options
static cfg_opt_t any_opts[] =
{
   CFG_STR(ITEM_LOCKDEF_ANY_KEYS, "", CFGF_LIST),
   CFG_END()
};

// Lockdef section options
cfg_opt_t edf_lockdef_opts[] =
{
   CFG_STR(ITEM_LOCKDEF_REQUIRE,  "",       CFGF_MULTI),
   CFG_SEC(ITEM_LOCKDEF_ANY,      any_opts, CFGF_MULTI),
   CFG_STR(ITEM_LOCKDEF_MESSAGE,  NULL,     CFGF_NONE ),
   CFG_STR(ITEM_LOCKDEF_REMOTE,   NULL,     CFGF_NONE ),
   CFG_STR(ITEM_LOCKDEF_LOCKSND,  NULL,     CFGF_NONE ),
   CFG_STR(ITEM_LOCKDEF_MAPCOLOR, NULL,     CFGF_NONE ),
   CFG_END()
};

//
// E_LockDefForID
//
// Look up a lockdef by its id number.
//
static lockdef_t *E_LockDefForID(int id)
{
   return e_lockDefs.objectForKey(id);
}

//
// E_freeLockDefData
//
// Free the data in a lockdef and return it to its default state.
//
static void E_freeLockDefData(lockdef_t *lockdef)
{
   if(lockdef->requiredKeys)
   {
      efree(lockdef->requiredKeys);
      lockdef->requiredKeys = NULL;
   }
   lockdef->numRequiredKeys = 0;

   if(lockdef->anyKeys)
   {
      for(unsigned int i = 0; i < lockdef->numAnyLists; i++)
      {
         anykey_t *any = &lockdef->anyKeys[i];

         if(any->keys)
            efree(any->keys);
      }
      efree(lockdef->anyKeys);
      lockdef->anyKeys = NULL;
   }
   lockdef->numAnyLists = 0;
   lockdef->numAnyKeys  = 0;

   if(lockdef->message)
   {
      efree(lockdef->message);
      lockdef->message = NULL;
   }
   if(lockdef->remoteMessage)
   {
      efree(lockdef->remoteMessage);
      lockdef->remoteMessage = NULL;
   }
   if(lockdef->lockedSound)
   {
      efree(lockdef->lockedSound);
      lockdef->lockedSound = NULL;
   }
}

//
// E_processKeyList
//
// Resolve a list of key names into a set of itemeffect pointers.
//
static void E_processKeyList(itemeffect_t **effects, unsigned int numKeys,
                             cfg_t *sec, const char *fieldName)
{
   for(unsigned int i = 0; i < numKeys; i++)
   {
      const char   *name = cfg_getnstr(sec, fieldName, i);
      itemeffect_t *fx   = E_ItemEffectForName(name);

      if(!fx || fx->getInt(keyClass, ITEMFX_NONE) != ITEMFX_ARTIFACT)
         E_EDFLoggedWarning(2, "Warning: lockdef key '%s' is not an artifact\n", name);

      effects[i] = fx;
   }
}

//
// E_processLockDefColor
//
// Process a lockdef color field.
//
static void E_processLockDefColor(lockdef_t *lock, const char *value)
{
   // Default behavior - act like a closed door.
   lock->colorType = LOCKDEF_COLOR_VARIABLE;
   lock->colorVar  = &mapcolor_clsd;

   if(!value || !*value)
      return;
   
   AutoPalette pal(wGlobalDir);
   long        lresult = 0;
   command_t  *cmd     = NULL;

   switch(*value)
   {
   case '$':
      // cvar value
      if((cmd = C_GetCmdForName(value + 1)) &&
         cmd->type == ct_variable &&
         cmd->variable->type == vt_int &&
         cmd->variable->min == 0 &&
         cmd->variable->max == 255)
      {
         lock->colorType = LOCKDEF_COLOR_VARIABLE;
         lock->colorVar  = (int *)(cmd->variable->variable);
      }
      break;
   case '#':
      // hex constant
      lresult = strtol(value + 1, NULL, 16);
      lock->colorType = LOCKDEF_COLOR_CONSTANT;
      lock->color = V_FindBestColor(pal.get(),
                       (int)((lresult >> 16) & 0xff),
                       (int)((lresult >>  8) & 0xff),
                       (int)( lresult        & 0xff));
      break;
   default:
      // decimal palette index
      lresult = strtol(value, NULL, 10);
      if(lresult >= 0 && lresult <= 255)
      {
         lock->colorType = LOCKDEF_COLOR_CONSTANT;
         lock->color     = (int)lresult;
      }
      break;
   }
}

//
// E_processLockDef
//
// Process a single lock definition.
//
static void E_processLockDef(cfg_t *lock)
{
   lockdef_t *lockdef = NULL;

   // the ID of the lockdef is the title of the section; it will be interpreted
   // as an integer
   int id = atoi(cfg_title(lock));

   // ID must be greater than 0
   if(id <= 0)
   {
      E_EDFLoggedWarning(2, "Warning: lockdef with invalid ID %d has been ignored\n", id);
      return;
   }

   // do we have this lock already?
   if((lockdef = E_LockDefForID(id)))
   {
      // free the existing data for this lock
      E_freeLockDefData(lockdef);
   }
   else
   {
      // create a new lockdef and hash it by key
      lockdef = estructalloc(lockdef_t, 1);
      lockdef->id = id;
      e_lockDefs.addObject(lockdef);
   }

   // process required key definitions
   if((lockdef->numRequiredKeys = cfg_size(lock, ITEM_LOCKDEF_REQUIRE)))
   {
      lockdef->requiredKeys = estructalloc(itemeffect_t *, lockdef->numRequiredKeys);
      E_processKeyList(lockdef->requiredKeys, lockdef->numRequiredKeys, 
                       lock, ITEM_LOCKDEF_REQUIRE);
   }

   // process "any" key lists
   if((lockdef->numAnyLists = cfg_size(lock, ITEM_LOCKDEF_ANY)))
   {
      lockdef->anyKeys = estructalloc(anykey_t, lockdef->numAnyLists);
      for(unsigned int i = 0; i < lockdef->numAnyLists; i++)
      {
         cfg_t    *anySec    = cfg_getnsec(lock, ITEM_LOCKDEF_ANY, i);
         anykey_t *curAnyKey = &lockdef->anyKeys[i];

         if((curAnyKey->numKeys = cfg_size(anySec, ITEM_LOCKDEF_ANY_KEYS)))
         {
            curAnyKey->keys = estructalloc(itemeffect_t *, curAnyKey->numKeys);
            E_processKeyList(curAnyKey->keys, curAnyKey->numKeys, 
                             anySec, ITEM_LOCKDEF_ANY_KEYS);
            lockdef->numAnyKeys += curAnyKey->numKeys;
         }
      }
   }

   // process messages and sounds
   const char *tempstr;
   if((tempstr = cfg_getstr(lock, ITEM_LOCKDEF_MESSAGE))) // message
      lockdef->message = estrdup(tempstr);
   if((tempstr = cfg_getstr(lock, ITEM_LOCKDEF_REMOTE)))  // remote message
      lockdef->remoteMessage = estrdup(tempstr);
   if((tempstr = cfg_getstr(lock, ITEM_LOCKDEF_LOCKSND))) // locked sound
      lockdef->lockedSound = estrdup(tempstr);

   // process map color
   E_processLockDefColor(lockdef, cfg_getstr(lock, ITEM_LOCKDEF_MAPCOLOR));

   E_EDFLogPrintf("\t\tDefined lockdef %d\n", lockdef->id);
}

//
// E_processLockDefs
//
// Process all lock definitions.
//
static void E_processLockDefs(cfg_t *cfg)
{
   unsigned int numLockDefs = cfg_size(cfg, EDF_SEC_LOCKDEF);

   E_EDFLogPrintf("\t* Processing lockdefs (%u defined)\n", numLockDefs);

   for(unsigned int i = 0; i < numLockDefs; i++)
      E_processLockDef(cfg_getnsec(cfg, EDF_SEC_LOCKDEF, i));
}

//
// E_failPlayerUnlock
//
// Routine to call when unlocking a lock has failed.
//
static void E_failPlayerUnlock(player_t *player, lockdef_t *lock, bool remote)
{
   const char *msg = NULL;

   if(remote && lock->remoteMessage)
   {
      // if remote and have a remote message, give remote message
      msg = lock->remoteMessage;
      if(msg[0] == '$')
         msg = DEH_String(msg + 1);
   }
   else if(lock->message)
   {
      // otherwise, give normal message
      msg = lock->message;
      if(msg[0] == '$')
         msg = DEH_String(msg + 1);
   }
   if(msg)
      player_printf(player, "%s", msg);

   // play sound if specified; if not, use skin default
   if(lock->lockedSound)
      S_StartSoundName(player->mo, lock->lockedSound);
   else
      S_StartSound(player->mo, GameModeInfo->playerSounds[sk_oof]);
}

//
// E_PlayerCanUnlock
//
// Check if a player has the keys necessary to unlock an object that is
// protected by a lock with the given ID.
//
bool E_PlayerCanUnlock(player_t *player, int lockID, bool remote)
{
   lockdef_t *lock;

   if(!(lock = E_LockDefForID(lockID)))
      return true; // there's no such lock, so you can open it.

   // does this lock have required keys?
   if(lock->numRequiredKeys > 0)
   {
      unsigned int numRequiredHave = 0;

      for(unsigned int i = 0; i < lock->numRequiredKeys; i++)
      {
         itemeffect_t *key = lock->requiredKeys[i];
         if(E_GetItemOwnedAmount(player, key) > 0)
            ++numRequiredHave;
      }

      // check that the full number of required keys is present
      if(numRequiredHave < lock->numRequiredKeys)
      {
         E_failPlayerUnlock(player, lock, remote);
         return false;
      }
   }

   // does this lock have "any" key sets?
   if(lock->numAnyKeys > 0)
   {
      unsigned int numAnyHave = 0;

      for(unsigned int i = 0; i < lock->numAnyLists; i++)
      {
         anykey_t *any = &lock->anyKeys[i];

         if(!any->numKeys)
            continue;

         // we need at least one key in this set
         for(unsigned int keynum = 0; keynum < any->numKeys; keynum++)
         {
            itemeffect_t *key = any->keys[keynum];
            if(E_GetItemOwnedAmount(player, key) > 0)
            {
               numAnyHave += any->numKeys; // credit for full set
               break; // can break out of inner loop, player has a key in this set
            }
         }
      }

      // missing one or more categories of "any" list keys?
      if(numAnyHave < lock->numAnyKeys)
      {
         E_failPlayerUnlock(player, lock, remote);
         return false;
      }
   }

   // if a lockdef has neither required nor "any" keys, then it opens if the
   // player possesses at least one item of class "Key"
   if(!lock->numRequiredKeys && !lock->numAnyKeys)
   {
      int numKeys = 0;

      for(size_t i = 0; i < E_GetNumKeyItems(); i++)
      {
         itemeffect_t *key = E_KeyItemForIndex(i);
         if(E_GetItemOwnedAmount(player, key) > 0)
            ++numKeys;
      }

      // if no keys are possessed, fail the lock
      if(!numKeys)
      {
         E_failPlayerUnlock(player, lock, remote);
         return false;
      }
   }

   // you can unlock it!
   return true;
}

//
// E_GetLockDefColor
//
// Get the automap color for a lockdef.
//
int E_GetLockDefColor(int lockID)
{
   int color = 0;
   const lockdef_t *lock;

   if((lock = E_LockDefForID(lockID)))
   {
      switch(lock->colorType)
      {
      case LOCKDEF_COLOR_CONSTANT:
         color = lock->color;
         break;
      case LOCKDEF_COLOR_VARIABLE:
         color = *lock->colorVar;
         break;
      default:
         break;
      }
   }

   return color;
}

//
// E_GiveAllKeys
//
// Give a player every artifact type that is considered a key and is not
// already owned. Returns the number of keys given.
//
int E_GiveAllKeys(player_t *player)
{
   size_t numKeys = E_GetNumKeyItems();
   int keysGiven  = 0;

   for(size_t i = 0; i < numKeys; i++)
   {
      itemeffect_t *key = E_KeyItemForIndex(i);
      if(!E_GetItemOwnedAmount(player, key))
      {
         if(E_GiveInventoryItem(player, key))
            ++keysGiven;
      }
   }

   return keysGiven;
}

// 
// E_TakeAllKeys
//
// Take away every artifact a player has that is of "key" type.
// Returns the number of keys taken away.
//
int E_TakeAllKeys(player_t *player)
{
   size_t numKeys = E_GetNumKeyItems();
   int keysTaken  = 0;

   for(size_t i = 0; i < numKeys; i++)
   {
      if(E_RemoveInventoryItem(player, E_KeyItemForIndex(i), -1) != INV_NOTREMOVED)
         ++keysTaken;
   }

   return keysTaken;
}

//=============================================================================
//
// Effect Bindings
//
// Effects can be bound to sprites.
//

// Sprite pick-up effects
#define ITEM_PICKUP_FX       "effect"
#define ITEM_PICKUP_MSG      "message"
#define ITEM_PICKUP_SOUND    "sound"

// sprite-based pickup items
cfg_opt_t edf_pickup_opts[] =
{
   CFG_STR(ITEM_PICKUP_FX,    "PFX_NONE", CFGF_NONE),
   CFG_STR(ITEM_PICKUP_MSG,   NULL,       CFGF_NONE),
   CFG_STR(ITEM_PICKUP_SOUND, NULL,       CFGF_NONE),

   CFG_END()
};

// pickup variables

// pickup effect names (these are currently searched linearly)
// matching enum values are defined in e_edf.h

// INVENTORY_FIXME: make this a "compatibility name" for the effect
const char *pickupnames[PFX_NUMFX] =
{
   "PFX_NONE",
   "PFX_GREENARMOR",
   "PFX_BLUEARMOR",
   "PFX_POTION",
   "PFX_ARMORBONUS",
   "PFX_SOULSPHERE",
   "PFX_MEGASPHERE",
   "PFX_BLUEKEY",
   "PFX_YELLOWKEY",
   "PFX_REDKEY",
   "PFX_BLUESKULL",
   "PFX_YELLOWSKULL",
   "PFX_REDSKULL",
   "PFX_STIMPACK",
   "PFX_MEDIKIT",
   "PFX_INVULNSPHERE",
   "PFX_BERZERKBOX",
   "PFX_INVISISPHERE",
   "PFX_RADSUIT",
   "PFX_ALLMAP",
   "PFX_LIGHTAMP",
   "PFX_CLIP",
   "PFX_CLIPBOX",
   "PFX_ROCKET",
   "PFX_ROCKETBOX",
   "PFX_CELL",
   "PFX_CELLPACK",
   "PFX_SHELL",
   "PFX_SHELLBOX",
   "PFX_BACKPACK",
   "PFX_BFG",
   "PFX_CHAINGUN",
   "PFX_CHAINSAW",
   "PFX_LAUNCHER",
   "PFX_PLASMA",
   "PFX_SHOTGUN",
   "PFX_SSG",
   "PFX_HGREENKEY",
   "PFX_HBLUEKEY",
   "PFX_HYELLOWKEY",
   "PFX_HPOTION",
   "PFX_SILVERSHIELD",
   "PFX_ENCHANTEDSHIELD",
   "PFX_BAGOFHOLDING",
   "PFX_HMAP",
   "PFX_GWNDWIMPY",
   "PFX_GWNDHEFTY",
   "PFX_MACEWIMPY",
   "PFX_MACEHEFTY",
   "PFX_CBOWWIMPY",
   "PFX_CBOWHEFTY",
   "PFX_BLSRWIMPY",
   "PFX_BLSRHEFTY",
   "PFX_PHRDWIMPY",
   "PFX_PHRDHEFTY",
   "PFX_SKRDWIMPY",
   "PFX_SKRDHEFTY",
   "PFX_TOTALINVIS",
};

// pickupfx lookup table used in P_TouchSpecialThing (is allocated
// with size NUMSPRITES)
e_pickupfx_t *pickupfx = NULL;

//
// E_processPickupItems
//
// Allocates the pickupfx array used in P_TouchSpecialThing, and loads all 
// pickupitem definitions, using the sprite hash table to resolve what sprite
// owns the specified effect.
//
static void E_processPickupItems(cfg_t *cfg)
{
   static int oldnumsprites;
   int i, numnew, numpickups;

   E_EDFLogPuts("\t* Processing pickup items\n");

   // allocate and initialize pickup effects array
   // haleyjd 11/21/11: allow multiple runs
   numnew = NUMSPRITES - oldnumsprites;
   if(numnew > 0)
   {
      pickupfx = erealloc(e_pickupfx_t *, pickupfx, NUMSPRITES * sizeof(*pickupfx));
      for(i = oldnumsprites; i < NUMSPRITES; i++)
         memset(&pickupfx[i], 0, sizeof(e_pickupfx_t));
      oldnumsprites = NUMSPRITES;
   }

   // sanity check
   if(!pickupfx)
      E_EDFLoggedErr(2, "E_ProcessItems: no sprites defined!?\n");
   
   // load pickupfx
   numpickups = cfg_size(cfg, EDF_SEC_PICKUPFX);
   E_EDFLogPrintf("\t\t%d pickup item(s) defined\n", numpickups);
   for(i = 0; i < numpickups; ++i)
   {
      int fxnum, sprnum;
      cfg_t *sec = cfg_getnsec(cfg, EDF_SEC_PICKUPFX, i);
      const char *title = cfg_title(sec);
      const char *str   = cfg_getstr(sec, ITEM_PICKUP_FX);

      // validate the sprite name given in the section title and
      // resolve to a sprite number (hashed)
      sprnum = E_SpriteNumForName(title);

      if(sprnum == -1)
      {
         // haleyjd 05/31/06: downgraded to warning, substitute blanksprite
         E_EDFLoggedWarning(2,
            "Warning: invalid sprite mnemonic for pickup item: '%s'\n",
            title);
         continue;
      }

      // INVENTORY_FIXME: old names need to become a compat feature only;
      //   "effect" should start referring to an itemeffect_t.
      // find the proper pickup effect number (linear search)
      fxnum = E_StrToNumLinear(pickupnames, PFX_NUMFX, str);
      if(fxnum == PFX_NUMFX)
      {
         E_EDFLoggedWarning(2, "Warning: invalid pickup effect: '%s'\n", str);
         continue;
      }
      
      E_EDFLogPrintf("\t\tSet sprite %s(#%d) to pickup effect %s(#%d)\n",
                     title, sprnum, str, fxnum);

      e_pickupfx_t &pfx = pickupfx[sprnum];

      // INVENTORY_FIXME: replace with effect pointer
      pfx.tempeffect = fxnum;

      // free any strings that might have been previously set
      if(pfx.message)
      {
         efree(pfx.message);
         pfx.message = NULL;
      }
      if(pfx.sound)
      {
         efree(pfx.sound);
         pfx.sound = NULL;
      }

      // process effect properties

      // message
      if((str = cfg_getstr(sec, ITEM_PICKUP_MSG)))
         pfx.message = estrdup(str);

      // sound
      if((str = cfg_getstr(sec, ITEM_PICKUP_SOUND)))
         pfx.sound = estrdup(str);
   }
}

//=============================================================================
//
// Inventory Items
//
// Inventory items represent a holdable item that can take up a slot in an 
// inventory.
//

// Lookup table of inventory item types by assigned ID number
static PODCollection<itemeffect_t *> e_InventoryItemsByID;
static inventoryitemid_t e_maxitemid;

//
// E_allocateInventoryItemIDs
//
// First, if the item ID table has already been built, we need to wipe it out.
// Then, regardless, build the artifact ID table by scanning the effects table
// for items of classes which enter the inventory, and assign each one a unique
// artifact ID. The ID will be added to the object.
//
static void E_allocateInventoryItemIDs()
{
   itemeffect_t *item = NULL;

   // empty the table if it was already created before
   e_InventoryItemsByID.clear();
   e_maxitemid = 0;

   // scan the effects table and add artifacts to the table
   while((item = runtime_cast<itemeffect_t *>(e_effectsTable.tableIterator(item))))
   {
      itemeffecttype_t fxtype = item->getInt(keyClass, ITEMFX_NONE);
      
      // only interested in effects that are recorded in the inventory
      if(fxtype == ITEMFX_ARTIFACT)
      {
         // add it to the table
         e_InventoryItemsByID.add(item);

         // add the ID to the artifact definition
         item->setInt(keyItemID, e_maxitemid++);
      }
   }
}

//
// E_allocatePlayerInventories
//
// Allocate inventory arrays for the player_t structures.
//
static void E_allocatePlayerInventories()
{
   for(int i = 0; i < MAXPLAYERS; i++)
   {
      if(players[i].inventory)
         efree(players[i].inventory);

      players[i].inventory = estructalloc(inventoryslot_t, e_maxitemid);

      for(inventoryindex_t idx = 0; idx < e_maxitemid; idx++)
         players[i].inventory[idx].item = -1;
   }
}

//
// E_EffectForInventoryItemID
//
// Return the effect definition associated with an inventory item ID.
//
itemeffect_t *E_EffectForInventoryItemID(inventoryitemid_t id)
{
   return (id >= 0 && id < e_maxitemid) ? e_InventoryItemsByID[id] : NULL;
}

//
// E_EffectForInventoryIndex
//
// Get the item effect for a particular index in a given player's inventory.
//
itemeffect_t *E_EffectForInventoryIndex(player_t *player, inventoryindex_t idx)
{
   return (idx >= 0 && idx < e_maxitemid) ? 
      E_EffectForInventoryItemID(player->inventory[idx].item) : NULL;
}

//
// E_InventorySlotForItemID
//
// Find the slot being used by an item in the player's inventory, if one exists.
// NULL is returned if the item is not in the player's inventory.
//
inventoryslot_t *E_InventorySlotForItemID(player_t *player, inventoryitemid_t id)
{
   inventory_t inventory = player->inventory;

   for(inventoryindex_t idx = 0; idx < e_maxitemid; idx++)
   {
      if(inventory[idx].item == id)
         return &inventory[idx];
   }

   return NULL;
}

//
// E_InventorySlotForItem
//
// Find the slot being used by an item in the player's inventory, by pointer,
// if one exists. NULL is returned if the item is not in the player's 
// inventory.
//
inventoryslot_t *E_InventorySlotForItem(player_t *player, itemeffect_t *effect)
{
   inventoryitemid_t id;

   if(effect && (id = effect->getInt(keyItemID, -1)) >= 0)
      return E_InventorySlotForItemID(player, id);
   else
      return NULL;
}

//
// E_InventorySlotForItemName
//
// Find the slot being used by an item in the player's inventory, by name,
// if one exists. NULL is returned if the item is not in the player's 
// inventory.
//
inventoryslot_t *E_InventorySlotForItemName(player_t *player, const char *name)
{
   return E_InventorySlotForItem(player, E_ItemEffectForName(name));
}

//
// E_findInventorySlot
//
// Finds the first unused inventory slot index.
//
static inventoryindex_t E_findInventorySlot(inventory_t inventory)
{
   // find the first unused slot and return it
   for(inventoryindex_t idx = 0; idx < e_maxitemid; idx++)
   {
      if(inventory[idx].item == -1)
         return idx;
   }

   // should be unreachable
   return -1;
}

//
// E_sortInventory
//
// After a new slot is added to the inventory, it needs to be placed into its
// proper sorted position based on the item effects' sortorder fields.
//
static void E_sortInventory(player_t *player, inventoryindex_t newIndex, int sortorder)
{
   inventory_t     inventory = player->inventory;
   inventoryslot_t tempSlot  = inventory[newIndex];

   for(inventoryindex_t idx = 0; idx < newIndex; idx++)
   {
      itemeffect_t *effect;

      if((effect = E_EffectForInventoryIndex(player, idx)))
      {
         int thatorder = effect->getInt(keySortOrder, 0);
         if(thatorder < sortorder)
            continue;
         else
         {
            // shift everything up
            for(inventoryindex_t up = newIndex; up > idx; up--)
               inventory[up] = inventory[up - 1];

            // put the saved slot into its proper place
            inventory[idx] = tempSlot;
            return;
         }
      }
   }
}

//
// E_PlayerHasBackpack
//
// Special lookup function to test if the player has a backpack.
//
bool E_PlayerHasBackpack(player_t *player)
{
   auto backpackItem = runtime_cast<itemeffect_t *>(e_effectsTable.getObject(keyBackpackItem));
   return (E_GetItemOwnedAmount(player, backpackItem) > 0);
}

//
// E_GiveBackpack
//
// Special function to give a backpack.
//
bool E_GiveBackpack(player_t *player)
{
   auto backpackItem = runtime_cast<itemeffect_t *>(e_effectsTable.getObject(keyBackpackItem));

   return E_GiveInventoryItem(player, backpackItem);
}

//
// E_RemoveBackpack
//
// Special function to remove a backpack.
//
bool E_RemoveBackpack(player_t *player)
{
   auto backpackItem = runtime_cast<itemeffect_t *>(e_effectsTable.getObject(keyBackpackItem));
   bool removed = false;
   itemremoved_e code;

   if((code = E_RemoveInventoryItem(player, backpackItem, -1)) != INV_NOTREMOVED)
   {
      removed = true;

      // cut all ammo types to their normal max amount
      size_t numAmmo = E_GetNumAmmoTypes();

      for(size_t i = 0; i < numAmmo; i++)
      {
         auto ammo      = E_AmmoTypeForIndex(i);
         int  maxamount = ammo->getInt(keyMaxAmount, 0);         
         auto slot      = E_InventorySlotForItem(player, ammo);

         if(slot && slot->amount > maxamount)
            slot->amount = maxamount;
      }
   }
   
   return removed;
}

//
// E_GetMaxAmountForArtifact
//
// Get the max amount of an artifact that can be carried. There are some
// special cases for different token subtypes of artifact.
//
int E_GetMaxAmountForArtifact(player_t *player, itemeffect_t *artifact)
{
   if(!artifact)
      return 0;

   int subType = artifact->getInt(keyArtifactType, ARTI_NORMAL);

   switch(subType)
   {
   case ARTI_AMMO:
      // ammo may increase the max amount if the player is carrying a backpack
      if(E_PlayerHasBackpack(player))
         return artifact->getInt(keyBackpackMaxAmt, 0);
      break;
   default:
      break;
   }

   // The default case is to return the ordinary max amount.
   return artifact->getInt(keyMaxAmount, 1);
}

//
// E_GetItemOwnedAmount
//
// If you do not need the inventory slot for any other purpose, you can lookup
// the amount of an item owned in one step by using this function.
//
int E_GetItemOwnedAmount(player_t *player, itemeffect_t *artifact)
{
   auto slot = E_InventorySlotForItem(player, artifact);

   return (slot ? slot->amount : 0);
}

//
// E_GetItemOwnedAmountName
//
// As above, but also doing a lookup on name.
//
int E_GetItemOwnedAmountName(player_t *player, const char *name)
{
   auto slot = E_InventorySlotForItemName(player, name);

   return (slot ? slot->amount : 0);
}


//
// E_GiveInventoryItem
//
// Place an artifact effect into the player's inventory, if it will fit.
//
bool E_GiveInventoryItem(player_t *player, itemeffect_t *artifact, int amount)
{
   if(!artifact)
      return false;

   itemeffecttype_t  fxtype = artifact->getInt(keyClass, ITEMFX_NONE);
   inventoryitemid_t itemid = artifact->getInt(keyItemID, -1);

   // Not an artifact??
   if(fxtype != ITEMFX_ARTIFACT || itemid < 0)
      return false;
   
   inventoryindex_t newSlot = -1;
   int amountToGive = artifact->getInt(keyAmount, 1);
   int maxAmount    = E_GetMaxAmountForArtifact(player, artifact);

   // may override amount to give via parameter "amount", if > 0
   if(amount > 0)
      amountToGive = amount;

   // Does the player already have this item?
   inventoryslot_t *slot = E_InventorySlotForItemID(player, itemid);

   // If not, make a slot for it
   if(!slot)
   {
      if((newSlot = E_findInventorySlot(player->inventory)) < 0)
         return false; // internal error, actually... shouldn't happen
      slot = &player->inventory[newSlot];
   }
   
   // If must collect full amount, but it won't fit, return now.
   if(artifact->getInt(keyFullAmountOnly, 0) && 
      slot->amount + amountToGive > maxAmount)
      return false;

   // set the item type in case the slot is new, and increment the amount owned
   // by the amount this item gives, capping to the maximum allowed amount
   slot->item    = itemid;
   slot->amount += amountToGive;
   if(slot->amount > maxAmount)
      slot->amount = maxAmount;

   // sort if needed
   if(newSlot > 0)
      E_sortInventory(player, newSlot, artifact->getInt(keySortOrder, 0));

   return true;
}

//
// E_removeInventorySlot
//
// Remove a slot from the player's inventory.
//
static void E_removeInventorySlot(player_t *player, inventoryslot_t *slot)
{
   inventory_t inventory = player->inventory;

   for(inventoryindex_t idx = 0; idx < e_maxitemid; idx++)
   {
      if(slot == &inventory[idx])
      {
         // shift everything down
         for(inventoryindex_t down = idx; down < e_maxitemid - 1; down++)
            inventory[down] = inventory[down + 1];

         // clear the top slot
         inventory[e_maxitemid - 1].item   = -1;
         inventory[e_maxitemid - 1].amount =  0;

         return;
      }
   }
}

//
// E_RemoveInventoryItem
//
// Remove some amount of a specific item from the player's inventory, if
// possible. If amount is less than zero, then all of the item will be removed.
//
itemremoved_e E_RemoveInventoryItem(player_t *player, itemeffect_t *artifact, int amount)
{
   inventoryslot_t *slot = E_InventorySlotForItem(player, artifact);

   // don't have that item at all?
   if(!slot)
      return INV_NOTREMOVED;

   // a negative amount means to remove the full possessed amount
   if(amount < 0)
      amount = slot->amount;

   // don't own that many?
   if(slot->amount < amount)
      return INV_NOTREMOVED;

   itemremoved_e ret = INV_REMOVED;

   // subtract the amount requested
   slot->amount -= amount;

   // are they used up?
   if(slot->amount == 0)
   {
      // check for "keep depleted" flag to see if item stays even when we have
      // a zero amount of it.
      if(!artifact->getInt(keyKeepDepleted, 0))
      {
         // otherwise, we need to remove that item and collapse the player's 
         // inventory
         E_removeInventorySlot(player, slot);
         ret = INV_REMOVEDSLOT;
      }
   }

   return ret;
}

//
// E_InventoryEndHub
//
// At the end of a hub (or a level that is not part of a hub), call this 
// function to strip all inventory items that are not meant to remain across
// levels to their max hub amount.
//
void E_InventoryEndHub(player_t *player)
{
   for(inventoryindex_t i = 0; i < e_maxitemid; i++)
   {
      int amount = player->inventory[i].amount;
      itemeffect_t *item = E_EffectForInventoryIndex(player, i);

      if(item)
      {
         int interHubAmount = item->getInt(keyInterHubAmount, 0);
         
         // an interhubamount less than zero means no stripping occurs
         if(interHubAmount >= 0 && amount > interHubAmount)
         {
            auto code = E_RemoveInventoryItem(player, item, amount - interHubAmount);
            if(code == INV_REMOVEDSLOT)
               --i; // back up one slot, because the current one was removed
         }
      }
   }
}

//
// E_ClearInventory
//
// Completely clear a player's inventory.
//
void E_ClearInventory(player_t *player)
{
   for(inventoryindex_t i = 0; i < e_maxitemid; i++)
   {
      player->inventory[i].amount =  0;
      player->inventory[i].item   = -1;
   }
}

//
// E_GetInventoryAllocSize
//
// Return the allocated size of a player's inventory in number of slots.
// This has nothing to do with the number of items owned.
//
int E_GetInventoryAllocSize()
{
   return e_maxitemid;
}

//=============================================================================
//
// Global Processing
//

//
// E_ProcessInventory
//
// Main global function for performing all inventory-related EDF processing.
//
void E_ProcessInventory(cfg_t *cfg)
{
   // process item effects
   E_processItemEffects(cfg);

   // allocate inventory item IDs
   E_allocateInventoryItemIDs();

   // allocate player inventories
   E_allocatePlayerInventories();

   // collect special artifact type definitions
   E_collectAmmoTypes();
   E_collectKeyItems();

   // process pickup item bindings
   E_processPickupItems(cfg);

   // process lockdefs
   E_processLockDefs(cfg);

   // TODO: MOAR?
}

// EOF

