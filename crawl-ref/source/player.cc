/*
 *  File:       player.cc
 *  Summary:    Player related functions.
 *  Written by: Linley Henzell
 *
 *  Modified for Crawl Reference by $Author$ on $Date$
 *
 *  Change History (most recent first):
 *
 * <6> 7/30/99 BWR   Added player_spell_levels()
 * <5> 7/13/99 BWR   Added player_res_electricity()
 *                   and player_hunger_rate()
 * <4> 6/22/99 BWR   Racial adjustments to stealth and Armour.
 * <3> 5/20/99 BWR   Fixed problems with random stat increases, added kobold
 *                   stat increase.  increased EV recovery for Armour.
 * <2> 5/08/99 LRH   display_char_status correctly handles magic_contamination.
 * <1> -/--/-- LRH   Created
 */

#include "AppHdr.h"
#include "player.h"

#ifdef DOS
#include <conio.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <sstream>

#include "externs.h"

#include "branch.h"
#include "cio.h"
#include "cloud.h"
#include "clua.h"
#include "delay.h"
#include "dgnevent.h"
#include "effects.h"
#include "fight.h"
#include "food.h"
#include "itemname.h"
#include "itemprop.h"
#include "items.h"
#include "Kills.h"
#include "macro.h"
#include "message.h"
#include "misc.h"
#include "monstuff.h"
#include "mon-util.h"
#include "mutation.h"
#include "notes.h"
#include "ouch.h"
#include "output.h"
#include "place.h"
#include "quiver.h"
#include "randart.h"
#include "religion.h"
#include "skills.h"
#include "skills2.h"
#include "spells1.h"
#include "spells3.h"
#include "spells4.h"
#include "spl-util.h"
#include "state.h"
#include "stuff.h"
#include "terrain.h"
#include "transfor.h"
#include "traps.h"
#include "travel.h"
#include "tutorial.h"
#include "view.h"
#include "tiles.h"
#include "xom.h"

std::string pronoun_you(description_level_type desc)
{
    switch (desc)
    {
    case DESC_NONE:
        return "";
    case DESC_CAP_A: case DESC_CAP_THE:
        return "You";
    case DESC_NOCAP_A: case DESC_NOCAP_THE:
    default:
        return "you";
    case DESC_CAP_YOUR:
        return "Your";
    case DESC_NOCAP_YOUR:
    case DESC_NOCAP_ITS:
        return "your";
    }
}

/* Contains functions which return various player state vars,
   and other stuff related to the player. */

static void _attribute_increase();

// Use this function whenever the player enters (or lands and thus re-enters)
// a grid.
//
// stepped     - normal walking moves
// allow_shift - allowed to scramble in any direction out of lava/water
// force       - ignore safety checks, move must happen (traps, lava/water).
bool move_player_to_grid( int x, int y, bool stepped, bool allow_shift,
                          bool force )
{
    ASSERT( in_bounds( x, y ) );

    int id;
    // assuming that entering the same square means coming from above (levitate)
    const bool from_above = (you.x_pos == x && you.y_pos == y);
    const dungeon_feature_type old_grid = (from_above) ? DNGN_FLOOR
                                            : grd[you.x_pos][you.y_pos];
    const dungeon_feature_type new_grid = grd[x][y];

    // Really must be clear.
    ASSERT( you.can_pass_through_feat( new_grid ) );

    // Better not be an unsubmerged monster either.
    ASSERT(mgrd[x][y] == NON_MONSTER
           || mons_is_submerged( &menv[ mgrd[x][y] ] ));

    const int cloud = env.cgrid[x][y];
    if (cloud != EMPTY_CLOUD)
    {
        const cloud_type ctype = env.cloud[ cloud ].type;
        // Don't prompt if already in a cloud of the same type.
        if (is_damaging_cloud(ctype, false)
            && (env.cgrid[you.x_pos][you.y_pos] == EMPTY_CLOUD
                || ctype != env.cloud[ env.cgrid[you.x_pos][you.y_pos] ].type))
        {
            std::string prompt = make_stringf(
                                    "Really step into that cloud of %s?",
                                    cloud_name(ctype).c_str());

            if (!yesno(prompt.c_str(), false, 'n'))
            {
                canned_msg(MSG_OK);
                you.turn_is_over = false;
                return (false);
            }
        }
    }

    // If we're walking along, give a chance to avoid traps.
    if (stepped && !force && !you.confused())
    {
        if (new_grid == DNGN_UNDISCOVERED_TRAP)
        {
            const int skill = 4 + you.skills[SK_TRAPS_DOORS]
                                + player_mutation_level(MUT_ACUTE_VISION)
                                - 2 * player_mutation_level(MUT_BLURRY_VISION);

            if (random2( skill ) > 6)
            {
                id = trap_at_xy( x, y );
                if (id != -1)
                    grd[x][y] = trap_category( env.trap[id].type );

                viewwindow(true, false);

                mprf( MSGCH_WARN,
                      "Wait a moment, %s!  Do you really want to step there?",
                      you.your_name );

                if (!you.running.is_any_travel())
                    more();

                exercise( SK_TRAPS_DOORS, 3 );

                you.turn_is_over = false;

                return (false);
            }
        }
        else if (new_grid == DNGN_TRAP_MAGICAL
#ifdef CLUA_BINDINGS
                 || new_grid == DNGN_TRAP_MECHANICAL
                    && Options.trap_prompt
#endif
                 || new_grid == DNGN_TRAP_NATURAL)
        {
            const trap_type type = trap_type_at_xy(x,y);
            if (type == TRAP_ZOT)
            {
                if (!yes_or_no("Do you really want to step into the Zot trap"))
                {
                    canned_msg(MSG_OK);
                    stop_running();
                    you.turn_is_over = false;
                    return (false);
                }
            }
            else if (new_grid != DNGN_TRAP_MAGICAL && you.airborne())
            {
                // No prompt (shaft and mechanical traps ineffective, if flying)
            }
            else
#ifdef CLUA_BINDINGS
                 // Prompt for any trap where you might not have enough hp
                 // as defined in init.txt (see trapwalk.lua)
                 if (new_grid != DNGN_TRAP_MECHANICAL
                     || !clua.callbooleanfn(false, "ch_cross_trap",
                                            "s", trap_name(x, y)))
#endif
            {
                std::string prompt = make_stringf(
                    "Really step %s that %s?",
                    (type == TRAP_ALARM) ? "onto" : "into",
                    feature_description(new_grid, type,
                                        false, DESC_BASENAME, false).c_str());

                if (!yesno(prompt.c_str(), true, 'n'))
                {
                    canned_msg(MSG_OK);
                    stop_running();
                    you.turn_is_over = false;
                    return (false);
                }
            }
        }
    }

    // Only consider terrain if player is not levitating.
    if (!player_is_airborne())
    {
        // XXX: at some point we're going to need to fix the swimming
        // code to handle burden states.
        if (is_grid_dangerous(new_grid))
        {
            // lava and dangerous deep water (ie not merfolk)
            int entry_x = (stepped) ? you.x_pos : x;
            int entry_y = (stepped) ? you.y_pos : y;

            if (stepped && !force && !you.duration[DUR_CONF])
            {
                canned_msg(MSG_UNTHINKING_ACT);
                return (false);
            }

            // Have to move now so fall_into_a_pool will work.
            you.moveto(x, y);

            viewwindow( true, false );

            // If true, we were shifted and so we're done.
            if (fall_into_a_pool( entry_x, entry_y, allow_shift, new_grid ))
                return (true);
        }
        else if (new_grid == DNGN_SHALLOW_WATER || new_grid == DNGN_DEEP_WATER)
        {
            // Safer water effects.
            if (you.species == SP_MERFOLK)
            {
                if (old_grid != DNGN_SHALLOW_WATER
                    && old_grid != DNGN_DEEP_WATER)
                {
                    if (stepped)
                        mpr("Your legs become a tail as you enter the water.");
                    else
                        mpr("Your legs become a tail as you dive into the water.");

                    merfolk_start_swimming();
                }
            }
            else if (!player_likes_water())
            {
                ASSERT( new_grid != DNGN_DEEP_WATER );

                if (!stepped)
                    noisy(SL_SPLASH, you.x_pos, you.y_pos, "Splash!");

                you.time_taken *= 13 + random2(8);
                you.time_taken /= 10;

                if (old_grid != DNGN_SHALLOW_WATER)
                {
                    mprf( "You %s the shallow water.",
                          (stepped ? "enter" : "fall into") );

                    mpr("Moving in this stuff is going to be slow.");

                    if (you.invisible())
                        mpr( "... and don't expect to remain undetected." );
                }
            }
        }
        else if (!grid_is_water(new_grid) && grid_is_water(old_grid)
                 && you.species == SP_MERFOLK)
        {
            you.redraw_evasion = true;
        }
    }

    // Move the player to new location.
    you.moveto(x, y);

    viewwindow( true, false );

    // Other Effects:
    // Clouds -- do we need this? (always seems to double up with acr.cc call)
    // if (is_cloud( you.x_pos, you.y_pos ))
    //     in_a_cloud();

    // Icy shield goes down over lava.
    if (new_grid == DNGN_LAVA)
        expose_player_to_element( BEAM_LAVA );

    // Traps go off.
    if (new_grid >= DNGN_TRAP_MECHANICAL && new_grid <= DNGN_UNDISCOVERED_TRAP)
    {
        id = trap_at_xy( you.x_pos, you.y_pos );

        if (id != -1)
        {
            bool trap_known = true;

            if (new_grid == DNGN_UNDISCOVERED_TRAP)
            {
                trap_known = false;

                const dungeon_feature_type type =
                    trap_category( env.trap[id].type );
                grd[you.x_pos][you.y_pos] = type;
                set_envmap_obj(you.x_pos, you.y_pos, type);
            }

            // It's not easy to blink onto a trap without setting it off.
            if (!stepped)
                trap_known = false;

            // mechanical traps and shafts cannot be set off if the
            // player is flying or levitating
            if (!player_is_airborne()
                || trap_category( env.trap[id].type ) == DNGN_TRAP_MAGICAL)
            {
                handle_traps(env.trap[id].type, id, trap_known);
            }
        }
    }

    return (true);
}

// Given an adjacent monster, returns true if the player can hit it (the
// monster should either not be submerged or submerged in shallow water,
// if the player has a polearm).
bool player_can_hit_monster(const monsters *mons)
{
    if (!mons_is_submerged(mons))
        return (true);

    if (grd(mons->pos()) != DNGN_SHALLOW_WATER)
        return (false);

    const item_def *weapon = you.weapon();
    return (weapon && weapon_skill(*weapon) == SK_POLEARMS);
}

bool player_can_swim()
{
    return you.can_swim();
}

bool is_grid_dangerous(int grid)
{
    return (!player_is_airborne()
            && (grid == DNGN_LAVA
                || (grid == DNGN_DEEP_WATER && !player_likes_water()) ));
}

bool player_in_mappable_area( void )
{
    return (!testbits(env.level_flags, LFLAG_NOT_MAPPABLE)
            && !testbits(get_branch_flags(), BFLAG_NOT_MAPPABLE));
}

bool player_in_branch( int branch )
{
    return (you.level_type == LEVEL_DUNGEON && you.where_are_you == branch);
}

bool player_in_hell( void )
{
    // No real reason except to draw someone's attention here if they
    // mess with the branch enum.
    COMPILE_CHECK(BRANCH_FIRST_HELL == BRANCH_DIS, a);
    COMPILE_CHECK(BRANCH_LAST_HELL  == BRANCH_THE_PIT, b);

    return (you.level_type == LEVEL_DUNGEON
            && you.where_are_you >= BRANCH_FIRST_HELL
            && you.where_are_you <= BRANCH_LAST_HELL
            && you.where_are_you != BRANCH_VESTIBULE_OF_HELL);
}

bool player_in_water(void)
{
    return (you.in_water());
}

bool player_likes_water(bool permanently)
{
    return (player_can_swim() || (!permanently && beogh_water_walk()));
}

bool player_is_swimming(void)
{
    return you.swimming();
}

bool player_under_penance(void)
{
    if (you.religion != GOD_NO_GOD)
        return (you.penance[you.religion]);
    else
        return (false);
}

bool player_genus(genus_type which_genus, species_type species)
{
    if (species == SP_UNKNOWN)
        species = you.species;

    switch (species)
    {
    case SP_RED_DRACONIAN:
    case SP_WHITE_DRACONIAN:
    case SP_GREEN_DRACONIAN:
    case SP_YELLOW_DRACONIAN:
    case SP_GREY_DRACONIAN:
    case SP_BLACK_DRACONIAN:
    case SP_PURPLE_DRACONIAN:
    case SP_MOTTLED_DRACONIAN:
    case SP_PALE_DRACONIAN:
    case SP_BASE_DRACONIAN:
        return (which_genus == GENPC_DRACONIAN);

    case SP_HIGH_ELF:
    case SP_GREY_ELF:
    case SP_DEEP_ELF:
    case SP_SLUDGE_ELF:
        return (which_genus == GENPC_ELVEN);

    case SP_MOUNTAIN_DWARF:
        return (which_genus == GENPC_DWARVEN);

    case SP_OGRE:
    case SP_OGRE_MAGE:
        return (which_genus == GENPC_OGRE);

    default:
        break;
    }

    return (false);
}

// If transform is true, compare with current transformation instead
// of (or in addition to) underlying species.
// (See mon-data.h for species/genus use.)
bool is_player_same_species(const int mon, bool transform)
{
    if (transform)
    {
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_AIR:
            return (false);
        // Unique monsters.
        case TRAN_BAT:
            return (mon == MONS_GIANT_BAT);
        case TRAN_ICE_BEAST:
            return (mon == MONS_ICE_BEAST);
        case TRAN_SERPENT_OF_HELL:
            return (mon == MONS_SERPENT_OF_HELL);
        // Compare with monster *species*.
        case TRAN_LICH:
            return (mons_species(mon) == MONS_LICH);
        // Compare with monster *genus*.
        case TRAN_SPIDER:
            return (mons_genus(mon) == MONS_WOLF_SPIDER);
        case TRAN_DRAGON:
            return (mons_genus(mon) == MONS_DRAGON); // Includes all drakes.
        default:
            break; // Check real (non-transformed) form.
        }
    }

    switch (you.species)
    {
        case SP_HUMAN:
            if (mons_genus(mon) == MONS_HUMAN)
                return (true);
            return (false);
        case SP_CENTAUR:
            if (mons_genus(mon) == MONS_CENTAUR)
                return (true);
            return (false);
        case SP_OGRE:
        case SP_OGRE_MAGE:
            if (mons_genus(mon) == MONS_OGRE)
                return (true);
            return (false);
        case SP_TROLL:
            if (mons_genus(mon) == MONS_TROLL)
                return (true);
            return (false);
        case SP_MUMMY:
            if (mons_genus(mon) == MONS_MUMMY)
                return (true);
            return (false);
        case SP_GHOUL: // Genus would include necrophage and rotting hulk.
            if (mons_species(mon) == MONS_GHOUL)
                return (true);
            return (false);
        case SP_VAMPIRE:
            if (mons_genus(mon) == MONS_VAMPIRE)
                return (true);
            return (false);
        case SP_MINOTAUR:
            if (mons_genus(mon) == MONS_MINOTAUR)
                return (true);
            return (false);
        case SP_NAGA:
            if (mons_genus(mon) == MONS_NAGA)
                return (true);
            return (false);
        case SP_HILL_ORC:
            if (mons_genus(mon) == MONS_ORC)
                return (true);
            return (false);
        case SP_MERFOLK:
            if (mons_genus(mon) == MONS_MERFOLK
                || mons_genus(mon) == MONS_MERMAID)
            {
                return (true);
            }
            return (false);

        case SP_GREY_ELF:
        case SP_HIGH_ELF:
        case SP_DEEP_ELF:
        case SP_SLUDGE_ELF:
            if (mons_genus(mon) == MONS_ELF)
                return (true);
            return (false);

        case SP_RED_DRACONIAN:
        case SP_WHITE_DRACONIAN:
        case SP_GREEN_DRACONIAN:
        case SP_YELLOW_DRACONIAN:
        case SP_GREY_DRACONIAN:
        case SP_BLACK_DRACONIAN:
        case SP_PURPLE_DRACONIAN:
        case SP_MOTTLED_DRACONIAN:
        case SP_PALE_DRACONIAN:
            if (mons_genus(mon) == MONS_DRACONIAN)
                return (true);
            return (false);

        case SP_KOBOLD:
            if (mons_genus(mon) == MONS_KOBOLD)
                return (true);
            return (false);
        default: // no monster equivalent
            return (false);

    }
}

// Checks whether the player's current species can
// use (usually wear) a given piece of equipment.
// Note that EQ_BODY_ARMOUR and EQ_HELMET only check
// the ill-fitting variant (i.e. not caps and robes).
// If special_armour is set to true, special cases
// such as bardings, light armour and caps are
// considered. Otherwise these simply return false.
// -------------------------------------------------
bool you_can_wear(int eq, bool special_armour)
{
    // These can be used by all.
    if (eq == EQ_LEFT_RING || eq == EQ_RIGHT_RING || eq == EQ_AMULET
        || eq == EQ_WEAPON || eq == EQ_CLOAK)
    {
        return (true);
    }

    // Anyone can wear caps/hats and robes and at least one of buckler/shield.
    if (special_armour
        && (eq == EQ_HELMET || eq == EQ_BODY_ARMOUR || eq == EQ_SHIELD))
    {
        return (true);
    }

    if (eq == EQ_BOOTS && (you.species == SP_NAGA || you.species == SP_CENTAUR))
        return (special_armour);

    // Of the remaining items, these races can't wear anything.
    if (you.species == SP_TROLL || you.species == SP_SPRIGGAN
        || player_genus(GENPC_OGRE) || player_genus(GENPC_DRACONIAN))
    {
        return (false);
    }

    if (you.species == SP_KENKU && (eq == EQ_HELMET || eq == EQ_BOOTS))
        return (false);

    if (player_mutation_level(MUT_HORNS) && eq == EQ_HELMET)
        return (special_armour);

    if (you.species == SP_GHOUL && eq == EQ_GLOVES)
        return (false);

    // Else, no problems.
    return (true);
}

bool player_has_feet()
{
   if (you.species == SP_NAGA || player_genus(GENPC_DRACONIAN))
       return (false);

   if (player_mutation_level(MUT_HOOVES) || player_mutation_level(MUT_TALONS))
       return (false);

   return (true);
}

bool you_tran_can_wear(int eq, bool check_mutation)
{
    // Not a transformation, but also temporary -> check first.
    if (check_mutation)
    {
        if (eq == EQ_GLOVES && you.has_claws(false) >= 3)
            return (false);

        if (eq == EQ_BOOTS
            && (player_is_swimming() && you.species == SP_MERFOLK
                || player_mutation_level(MUT_HOOVES)
                || player_mutation_level(MUT_TALONS)))
        {
            return (false);
        }
    }

    int transform = you.attribute[ATTR_TRANSFORMATION];

    // No further restrictions.
    if (transform == TRAN_NONE || transform == TRAN_LICH)
        return (true);

    // Bats cannot use anything, clouds obviously so.
    if (transform == TRAN_BAT || transform == TRAN_AIR)
        return (false);

    // Everyone else can wear jewellery, at least.
    if (eq == EQ_LEFT_RING || eq == EQ_RIGHT_RING || eq == EQ_AMULET)
        return (true);

    // These cannot use anything but jewellery.
    if (transform == TRAN_SPIDER || transform == TRAN_DRAGON
        || transform == TRAN_SERPENT_OF_HELL)
    {
        return (false);
    }

    if (transform == TRAN_BLADE_HANDS)
    {
        if (eq == EQ_WEAPON || eq == EQ_GLOVES || eq == EQ_SHIELD)
            return (false);
        return (true);
    }

    if (transform == TRAN_ICE_BEAST)
    {
        if (eq != EQ_CLOAK)
            return (false);
        return (true);
    }

    if (transform == TRAN_STATUE)
    {
        if (eq == EQ_BODY_ARMOUR || eq == EQ_GLOVES || eq == EQ_SHIELD)
            return (false);
        return (true);
    }

    return (true);
}

// Returns the item in the given equipment slot, NULL if the slot is empty.
// eq must be in [EQ_WEAPON, EQ_AMULET], or bad things will happen.
item_def *player_slot_item(equipment_type eq)
{
    return you.slot_item(eq);
}

// Returns the item in the player's weapon slot.
item_def *player_weapon()
{
    return you.weapon();
}

bool player_weapon_wielded()
{
    const int wpn = you.equip[EQ_WEAPON];

    if (wpn == -1)
        return (false);

    if (you.inv[wpn].base_type != OBJ_WEAPONS
        && you.inv[wpn].base_type != OBJ_STAVES)
    {
        return (false);
    }

    // FIXME: This needs to go in eventually.
    /*
    // should never have a bad "shape" weapon in hand
    ASSERT( check_weapon_shape( you.inv[wpn], false ) );

    if (!check_weapon_wieldable_size( you.inv[wpn], player_size() ))
        return (false);
     */

    return (true);
}

// Returns the you.inv[] index of our wielded weapon or -1 (no item, not wield)
int get_player_wielded_item()
{
    return (you.equip[EQ_WEAPON]);
}

int get_player_wielded_weapon()
{
    return (player_weapon_wielded()? get_player_wielded_item() : -1);
}

// Returns false if the player is wielding a weapon inappropriate for Berserk.
bool berserk_check_wielded_weapon()
{
    if (you.equip[EQ_WEAPON] == -1)
        return (true);

    const item_def weapon = you.inv[you.equip[EQ_WEAPON]];
    if (is_valid_item(weapon) && weapon.base_type != OBJ_STAVES
           && (weapon.base_type != OBJ_WEAPONS || is_range_weapon(weapon))
        || you.attribute[ATTR_WEAPON_SWAP_INTERRUPTED])
    {
        std::string prompt = "Do you really want to go berserk while "
                             "wielding " + weapon.name(DESC_NOCAP_YOUR)
                             + "? ";

        if (!yesno(prompt.c_str(), true, 'n'))
        {
            canned_msg(MSG_OK);
            return (false);
        }

        you.attribute[ATTR_WEAPON_SWAP_INTERRUPTED] = 0;
    }

    return (true);
}

// Looks in equipment "slot" to see if there is an equipped "sub_type".
// Returns number of matches (in the case of rings, both are checked)
int player_equip( equipment_type slot, int sub_type, bool calc_unid )
{
    int ret = 0;

    switch (slot)
    {
    case EQ_WEAPON:
        // Hands can have more than just weapons.
        if (you.equip[EQ_WEAPON] != -1
            && you.inv[you.equip[EQ_WEAPON]].base_type == OBJ_WEAPONS
            && you.inv[you.equip[EQ_WEAPON]].sub_type == sub_type)
        {
            ret++;
        }
        break;

    case EQ_STAFF:
        // Like above, but must be magical stave.
        if (you.equip[EQ_WEAPON] != -1
            && you.inv[you.equip[EQ_WEAPON]].base_type == OBJ_STAVES
            && you.inv[you.equip[EQ_WEAPON]].sub_type == sub_type
            && (calc_unid
                || item_type_known( you.inv[you.equip[EQ_WEAPON]] )))
        {
            ret++;
        }
        break;

    case EQ_RINGS:
        if (you.equip[EQ_LEFT_RING] != -1
            && you.inv[you.equip[EQ_LEFT_RING]].sub_type == sub_type
            && (calc_unid
                || item_type_known( you.inv[you.equip[EQ_LEFT_RING]] )))
        {
            ret++;
        }

        if (you.equip[EQ_RIGHT_RING] != -1
            && you.inv[you.equip[EQ_RIGHT_RING]].sub_type == sub_type
            && (calc_unid
                || item_type_known( you.inv[you.equip[EQ_RIGHT_RING]] )))
        {
            ret++;
        }
        break;

    case EQ_RINGS_PLUS:
        if (you.equip[EQ_LEFT_RING] != -1
            && you.inv[you.equip[EQ_LEFT_RING]].sub_type == sub_type)
        {
            ret += you.inv[you.equip[EQ_LEFT_RING]].plus;
        }

        if (you.equip[EQ_RIGHT_RING] != -1
            && you.inv[you.equip[EQ_RIGHT_RING]].sub_type == sub_type)
        {
            ret += you.inv[you.equip[EQ_RIGHT_RING]].plus;
        }
        break;

    case EQ_RINGS_PLUS2:
        if (you.equip[EQ_LEFT_RING] != -1
            && you.inv[you.equip[EQ_LEFT_RING]].sub_type == sub_type)
        {
            ret += you.inv[you.equip[EQ_LEFT_RING]].plus2;
        }

        if (you.equip[EQ_RIGHT_RING] != -1
            && you.inv[you.equip[EQ_RIGHT_RING]].sub_type == sub_type)
        {
            ret += you.inv[you.equip[EQ_RIGHT_RING]].plus2;
        }
        break;

    case EQ_ALL_ARMOUR:
        // Doesn't make much sense here... be specific. -- bwr
        break;

    default:
        if (you.equip[slot] != -1
            && you.inv[you.equip[slot]].sub_type == sub_type
            && (calc_unid ||
                item_type_known(you.inv[you.equip[slot]])))
        {
            ret++;
        }
        break;
    }

    return (ret);
}


// Looks in equipment "slot" to see if equipped item has "special" ego-type
// Returns number of matches (jewellery returns zero -- no ego type).
// [ds] There's no equivalent of calc_unid or req_id because as of now, weapons
// and armour type-id on wield/wear.
int player_equip_ego_type( int slot, int special )
{
    int ret = 0;
    int wpn;

    switch (slot)
    {
    case EQ_WEAPON:
        // This actually checks against the "branding", so it will catch
        // randart brands, but not fixed artefacts.  -- bwr

        // Hands can have more than just weapons.
        wpn = you.equip[EQ_WEAPON];
        if (wpn != -1
            && you.inv[wpn].base_type == OBJ_WEAPONS
            && get_weapon_brand( you.inv[wpn] ) == special)
        {
            ret++;
        }
        break;

    case EQ_LEFT_RING:
    case EQ_RIGHT_RING:
    case EQ_AMULET:
    case EQ_STAFF:
    case EQ_RINGS:
    case EQ_RINGS_PLUS:
    case EQ_RINGS_PLUS2:
        // no ego types for these slots
        break;

    case EQ_ALL_ARMOUR:
        // Check all armour slots:
        for (int i = EQ_CLOAK; i <= EQ_BODY_ARMOUR; i++)
        {
            if (you.equip[i] != -1
                && get_armour_ego_type( you.inv[you.equip[i]] ) == special)
            {
                ret++;
            }
        }
        break;

    default:
        // Check a specific armour slot for an ego type:
        if (you.equip[slot] != -1
            && get_armour_ego_type( you.inv[you.equip[slot]] ) == special)
        {
            ret++;
        }
        break;
    }

    return (ret);
}

int player_damage_type( void )
{
    return you.damage_type();
}

// returns band of player's melee damage
int player_damage_brand( void )
{
    return you.damage_brand();
}

int player_teleport(bool calc_unid)
{
    int tp = 0;

    // rings
    tp += 8 * player_equip( EQ_RINGS, RING_TELEPORTATION, calc_unid );

    // mutations
    tp += player_mutation_level(MUT_TELEPORT) * 3;

    // randart weapons only
    if (you.equip[EQ_WEAPON] != -1
        && you.inv[you.equip[EQ_WEAPON]].base_type == OBJ_WEAPONS
        && is_random_artefact( you.inv[you.equip[EQ_WEAPON]] ))
    {
        tp += scan_randarts(RAP_CAUSE_TELEPORTATION, calc_unid);
    }

    return tp;
}

int player_regen(void)
{
    int rr = you.hp_max / 3;

    if (rr > 20)
        rr = 20 + ((rr - 20) / 2);

    // rings
    rr += 40 * player_equip( EQ_RINGS, RING_REGENERATION );

    // spell
    if (you.duration[DUR_REGENERATION])
        rr += 100;

    // troll leather (except for trolls)
    if (player_equip( EQ_BODY_ARMOUR, ARM_TROLL_LEATHER_ARMOUR ) &&
        you.species != SP_TROLL)
        rr += 30;

    // fast heal mutation
    rr += player_mutation_level(MUT_REGENERATION) * 20;

    // Ghouls heal slowly.
    // Dematerialised people heal slowly.
    // Dematerialised ghouls shouldn't heal any more slowly. -- bwr
    if ((you.species == SP_GHOUL
            && (you.attribute[ATTR_TRANSFORMATION] == TRAN_NONE
                || you.attribute[ATTR_TRANSFORMATION] == TRAN_BLADE_HANDS))
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
    {
        rr /= 2;
    }

    // Healing depending on satiation.
    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
         case HS_STARVING:
            return (0); // No regeneration for starving vampires!
         case HS_NEAR_STARVING:
         case HS_VERY_HUNGRY:
         case HS_HUNGRY:
            return (rr / 2);
         case HS_SATIATED:
            return (rr);
         case HS_FULL:
         case HS_VERY_FULL:
            return (rr + 10);
         case HS_ENGORGED:
            return (rr + 20);
        }
    }

    if (rr < 1)
        rr = 1;

    return (rr);
}

int player_hunger_rate(void)
{
    int hunger = 3;

    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_BAT)
        return 1;

    // jmf: Hunger isn't fair while you can't eat.
    // Actually, it is since you can detransform any time you like -- bwr
    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
        return 0;

    if (you.species == SP_TROLL)
        hunger += 3;            // in addition to the +3 for fast metabolism

    if (you.duration[DUR_REGENERATION] > 0)
        hunger += 4;

    // Moved here from acr.cc... maintaining the >= 40 behaviour.
    if (you.hunger >= 40)
    {
        if (you.duration[DUR_INVIS] > 0)
            hunger += 5;

        // Berserk has its own food penalty -- excluding berserk haste.
        if (you.duration[DUR_HASTE] > 0 && !you.duration[DUR_BERSERKER])
            hunger += 5;
    }

    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
        case HS_STARVING:
        case HS_NEAR_STARVING:
            hunger -= 3;
            break;
        case HS_VERY_HUNGRY:
            hunger -= 2;
            break;
        case HS_HUNGRY:
            hunger--;
            break;
        case HS_SATIATED:
            break;
        case HS_FULL:
            hunger++;
            break;
        case HS_VERY_FULL:
            hunger += 2;
            break;
        case HS_ENGORGED:
            hunger += 3;
        }
    }
    else
    {
        hunger += player_mutation_level(MUT_FAST_METABOLISM);

        if (player_mutation_level(MUT_SLOW_METABOLISM) > 2)
            hunger -= 2;
        else if (player_mutation_level(MUT_SLOW_METABOLISM) > 0)
            hunger--;
    }

    // rings
    hunger += 2 * player_equip( EQ_RINGS, RING_REGENERATION );
    hunger += 4 * player_equip( EQ_RINGS, RING_HUNGER );
    hunger -= 2 * player_equip( EQ_RINGS, RING_SUSTENANCE );

    // weapon ego types
    if (you.species != SP_VAMPIRE)
    {
        hunger += 6 * player_equip_ego_type( EQ_WEAPON, SPWPN_VAMPIRICISM );
        hunger += 9 * player_equip_ego_type( EQ_WEAPON, SPWPN_VAMPIRES_TOOTH );
    }
    else
    {
        hunger += 1 * player_equip_ego_type( EQ_WEAPON, SPWPN_VAMPIRICISM );
        hunger += 2 * player_equip_ego_type( EQ_WEAPON, SPWPN_VAMPIRES_TOOTH );
    }

    // troll leather armour
    if (you.species != SP_TROLL)
        hunger += player_equip( EQ_BODY_ARMOUR, ARM_TROLL_LEATHER_ARMOUR );

    // randarts
    hunger += scan_randarts(RAP_METABOLISM);

    // burden
    hunger += you.burden_state;

    if (hunger < 1)
        hunger = 1;

    return (hunger);
}

int player_spell_levels(void)
{
    int sl = (you.experience_level - 1) + (you.skills[SK_SPELLCASTING] * 2);

    bool fireball = false;
    bool delayed_fireball = false;

    if (sl > 99)
        sl = 99;

    for (int i = 0; i < 25; i++)
    {
        if (you.spells[i] == SPELL_FIREBALL)
            fireball = true;
        else if (you.spells[i] == SPELL_DELAYED_FIREBALL)
            delayed_fireball = true;

        if (you.spells[i] != SPELL_NO_SPELL)
            sl -= spell_difficulty(you.spells[i]);
    }

    // Fireball is free for characters with delayed fireball
    if (fireball && delayed_fireball)
        sl += spell_difficulty( SPELL_FIREBALL );

    // Note: This can happen because of level drain.  Maybe we should
    // force random spells out when that happens. -- bwr
    if (sl < 0)
        sl = 0;

    return (sl);
}

bool player_knows_spell(int spell)
{
    for (int i = 0; i < 25; i++)
        if (you.spells[i] == spell)
            return (true);

    return (false);
}

int player_res_magic(void)
{
    int rm = 0;

    switch (you.species)
    {
    default:
        rm = you.experience_level * 3;
        break;
    case SP_HIGH_ELF:
    case SP_GREY_ELF:
    case SP_SLUDGE_ELF:
    case SP_DEEP_ELF:
    case SP_MOUNTAIN_DWARF:
    case SP_VAMPIRE:
    case SP_DEMIGOD:
        rm = you.experience_level * 4;
        break;
    case SP_NAGA:
    case SP_OGRE_MAGE:
        rm = you.experience_level * 5;
        break;
    case SP_PURPLE_DRACONIAN:
    case SP_GNOME:
        rm = you.experience_level * 6;
        break;
    case SP_SPRIGGAN:
        rm = you.experience_level * 7;
        break;
    }

    // randarts
    rm += scan_randarts(RAP_MAGIC);

    // armour
    rm += 30 * player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_MAGIC_RESISTANCE );

    // rings of magic resistance
    rm += 40 * player_equip( EQ_RINGS, RING_PROTECTION_FROM_MAGIC );

    // Enchantment skill
    rm += 2 * you.skills[SK_ENCHANTMENTS];

    // Mutations
    rm += 30 * player_mutation_level(MUT_MAGIC_RESISTANCE);

    // transformations
    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH)
        rm += 50;

    return rm;
}

// If temp is set to false, temporary sources or resistance won't be counted.
int player_res_steam(bool calc_unid, bool temp, bool items)
{
    int res = 0;

    if (you.species == SP_PALE_DRACONIAN && you.experience_level > 5)
        res += 2;

    if (items && player_equip(EQ_BODY_ARMOUR, ARM_STEAM_DRAGON_ARMOUR))
        res += 2;

    return (res + player_res_fire(calc_unid, temp, items) / 2);
}

bool player_can_smell()
{
    return (you.species != SP_MUMMY);
}

// If temp is set to false, temporary sources or resistance won't be counted.
int player_res_fire(bool calc_unid, bool temp, bool items)
{
    int rf = 0;

    if (items)
    {
        /* rings of fire resistance/fire */
        rf += player_equip( EQ_RINGS, RING_PROTECTION_FROM_FIRE, calc_unid );
        rf += player_equip( EQ_RINGS, RING_FIRE, calc_unid );

        /* rings of ice */
        rf -= player_equip( EQ_RINGS, RING_ICE, calc_unid );

        /* Staves */
        rf += player_equip( EQ_STAFF, STAFF_FIRE, calc_unid );

        // body armour:
        rf += 2 * player_equip( EQ_BODY_ARMOUR, ARM_DRAGON_ARMOUR );
        rf += player_equip( EQ_BODY_ARMOUR, ARM_GOLD_DRAGON_ARMOUR );
        rf -= player_equip( EQ_BODY_ARMOUR, ARM_ICE_DRAGON_ARMOUR );

        // ego armours
        rf += player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_FIRE_RESISTANCE );
        rf += player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_RESISTANCE );

        // randart weapons:
        rf += scan_randarts(RAP_FIRE, calc_unid);
    }

    // species:
    if (you.species == SP_MUMMY)
        rf--;

    // mutations:
    rf += player_mutation_level(MUT_HEAT_RESISTANCE);

    // spells:
    if (temp)
    {
        if (you.duration[DUR_RESIST_FIRE] > 0)
            rf++;

        if (you.duration[DUR_FIRE_SHIELD])
            rf += 2;

        // transformations:
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_ICE_BEAST:
            rf--;
            break;
        case TRAN_DRAGON:
            rf += 2;
            break;
        case TRAN_SERPENT_OF_HELL:
            rf += 2;
            break;
        case TRAN_AIR:
            rf -= 2;
            break;
        }
    }

    if (rf < -3)
        rf = -3;
    else if (rf > 3)
        rf = 3;

    return (rf);
}

int player_res_cold(bool calc_unid, bool temp, bool items)
{
    int rc = 0;

    if (temp)
    {
        // spells:
        if (you.duration[DUR_RESIST_COLD] > 0)
            rc++;

        if (you.duration[DUR_FIRE_SHIELD])
            rc -= 2;

        // transformations:
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_ICE_BEAST:
            rc += 3;
            break;
        case TRAN_DRAGON:
            rc--;
            break;
        case TRAN_LICH:
            rc++;
            break;
        case TRAN_AIR:
            rc -= 2;
            break;
        }


        if (you.species == SP_VAMPIRE)
        {
            if (you.hunger_state <= HS_NEAR_STARVING)
                rc += 2;
            else if (you.hunger_state < HS_SATIATED)
                rc++;
        }
    }

    if (items)
    {
        // rings of fire resistance/fire
        rc += player_equip( EQ_RINGS, RING_PROTECTION_FROM_COLD, calc_unid );
        rc += player_equip( EQ_RINGS, RING_ICE, calc_unid );

        // rings of ice
        rc -= player_equip( EQ_RINGS, RING_FIRE, calc_unid );

        // Staves
        rc += player_equip( EQ_STAFF, STAFF_COLD, calc_unid );

        // body armour:
        rc += 2 * player_equip( EQ_BODY_ARMOUR, ARM_ICE_DRAGON_ARMOUR );
        rc += player_equip( EQ_BODY_ARMOUR, ARM_GOLD_DRAGON_ARMOUR );
        rc -= player_equip( EQ_BODY_ARMOUR, ARM_DRAGON_ARMOUR );

        // ego armours
        rc += player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_COLD_RESISTANCE );
        rc += player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_RESISTANCE );

        // randart weapons:
        rc += scan_randarts(RAP_COLD, calc_unid);
    }

    // mutations:
    rc += player_mutation_level(MUT_COLD_RESISTANCE);

    if (player_mutation_level(MUT_SHAGGY_FUR) == 3)
        rc++;

    if (rc < -3)
        rc = -3;
    else if (rc > 3)
        rc = 3;

    return (rc);
}

int player_res_acid(bool calc_unid, bool items)
{
    int res = 0;
    if (!transform_changed_physiology())
    {
        if (you.species == SP_YELLOW_DRACONIAN
                    && you.experience_level >= 7)
            res += 2;

        res += player_mutation_level(MUT_YELLOW_SCALES) * 2 / 3;
    }

    if (items && wearing_amulet(AMU_RESIST_CORROSION, calc_unid))
        res++;

    return (res);
}

// Returns a factor X such that post-resistance acid damage can be calculated
// as pre_resist_damage * X / 100.
int player_acid_resist_factor()
{
    int res = player_res_acid();

    int factor = 100;

    if (res == 1)
        factor = 50;
    else if (res == 2)
        factor = 34;
    else if (res > 2)
    {
        factor = 30;
        while (res-- > 2 && factor >= 20)
            factor = factor * 90 / 100;
    }

    return (factor);
}

int player_res_electricity(bool calc_unid, bool temp, bool items)
{
    int re = 0;

    if (temp)
    {
        if (you.duration[DUR_INSULATION])
            re++;

        // transformations:
        if (you.attribute[ATTR_TRANSFORMATION] == TRAN_STATUE)
            re += 1;

        if (you.attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
            re += 2;  // multiple levels currently meaningless

        if (you.attribute[ATTR_DIVINE_LIGHTNING_PROTECTION])
            re = 3;
        else if (re > 1)
            re = 1;
    }

    if (items)
    {
        // staff
        re += player_equip( EQ_STAFF, STAFF_AIR, calc_unid );

        // body armour:
        re += player_equip( EQ_BODY_ARMOUR, ARM_STORM_DRAGON_ARMOUR );

        // randart weapons:
        re += scan_randarts(RAP_ELECTRICITY, calc_unid);
    }

    // species:
    if (you.species == SP_BLACK_DRACONIAN && you.experience_level > 17)
        re++;

    // mutations:
    if (player_mutation_level(MUT_SHOCK_RESISTANCE))
        re++;

    return (re);
}                               // end player_res_electricity()

// Does the player resist asphyxiation?
bool player_res_asphyx()
{
    // The undead are immune to asphyxiation, or so we'll assume.
    if (you.is_undead)
        return (true);

    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_LICH:
    case TRAN_STATUE:
    case TRAN_SERPENT_OF_HELL:
    case TRAN_AIR:
        return (true);
    }
    return (false);
}

bool player_control_teleport(bool calc_unid, bool temp, bool items)
{
    return ( (you.duration[DUR_CONTROL_TELEPORT] && temp)
             || (player_equip(EQ_RINGS, RING_TELEPORT_CONTROL, calc_unid)
                 && items)
             || player_mutation_level(MUT_TELEPORT_CONTROL) );
}

int player_res_torment(bool)
{
    return (player_mutation_level(MUT_TORMENT_RESISTANCE)
            || you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH
            || you.species == SP_VAMPIRE && you.hunger_state == HS_STARVING);
}

// Funny that no races are susceptible to poisons. {dlb}
// If temp is set to false, temporary sources or resistance won't be counted.
int player_res_poison(bool calc_unid, bool temp, bool items)
{
    int rp = 0;

    if (items)
    {
        // rings of poison resistance
        rp += player_equip( EQ_RINGS, RING_POISON_RESISTANCE, calc_unid );

        // Staves
        rp += player_equip( EQ_STAFF, STAFF_POISON, calc_unid );

        // the staff of Olgreb:
        if (you.equip[EQ_WEAPON] != -1
            && you.inv[you.equip[EQ_WEAPON]].base_type == OBJ_WEAPONS
            && you.inv[you.equip[EQ_WEAPON]].special == SPWPN_STAFF_OF_OLGREB)
        {
            rp++;
        }

        // ego armour:
        rp += player_equip_ego_type( EQ_ALL_ARMOUR, SPARM_POISON_RESISTANCE );

        // body armour:
        rp += player_equip( EQ_BODY_ARMOUR, ARM_GOLD_DRAGON_ARMOUR );
        rp += player_equip( EQ_BODY_ARMOUR, ARM_SWAMP_DRAGON_ARMOUR );

        // randart weapons:
        rp += scan_randarts(RAP_POISON, calc_unid);
    }

    // mutations:
    rp += player_mutation_level(MUT_POISON_RESISTANCE);

    if (temp)
    {
        // Only thirsty vampires are naturally poison resistant.
        if (you.species == SP_VAMPIRE && you.hunger_state < HS_SATIATED)
            rp++;

        // spells:
        if (you.duration[DUR_RESIST_POISON] > 0)
            rp++;

        // transformations:
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_LICH:
        case TRAN_ICE_BEAST:
        case TRAN_STATUE:
        case TRAN_DRAGON:
        case TRAN_SERPENT_OF_HELL:
        case TRAN_AIR:
            rp++;
            break;
        }
    }

    if (rp > 1)
        rp = 1;

    return (rp);
}                               // end player_res_poison()

int player_spec_death()
{
    int sd = 0;

    /* Staves */
    sd += player_equip( EQ_STAFF, STAFF_DEATH );

    // body armour:
    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI ))
        sd++;

    // species:
    if (you.species == SP_MUMMY)
    {
        if (you.experience_level >= 13)
            sd++;
        if (you.experience_level >= 26)
            sd++;
    }
    else if (you.species == SP_VAMPIRE)
    {
        // Vampires get bonus only when thirsty
        if (you.experience_level >= 13 && you.hunger_state < HS_SATIATED)
            sd++;
    }

    // transformations:
    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH)
        sd++;

    return sd;
}

int player_spec_holy()
{
    //if ( you.char_class == JOB_PRIEST || you.char_class == JOB_PALADIN )
    //  return 1;
    return 0;
}

int player_spec_fire()
{
    int sf = 0;

    // staves:
    sf += player_equip( EQ_STAFF, STAFF_FIRE );

    // rings of fire:
    sf += player_equip( EQ_RINGS, RING_FIRE );

    if (you.duration[DUR_FIRE_SHIELD])
        sf++;

    return sf;
}

int player_spec_cold()
{
    int sc = 0;

    // staves:
    sc += player_equip( EQ_STAFF, STAFF_COLD );

    // rings of ice:
    sc += player_equip( EQ_RINGS, RING_ICE );

    return sc;
}

int player_spec_earth()
{
    int se = 0;

    /* Staves */
    se += player_equip( EQ_STAFF, STAFF_EARTH );

    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
        se--;

    return se;
}

int player_spec_air()
{
    int sa = 0;

    /* Staves */
    sa += player_equip( EQ_STAFF, STAFF_AIR );

    //jmf: this was too good
    //if (you.attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
    //  sa++;
    return sa;
}

int player_spec_conj()
{
    int sc = 0;

    /* Staves */
    sc += player_equip( EQ_STAFF, STAFF_CONJURATION );

    // armour of the Archmagi
    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI ))
        sc++;

    return sc;
}

int player_spec_ench()
{
    int se = 0;

    /* Staves */
    se += player_equip( EQ_STAFF, STAFF_ENCHANTMENT );

    // armour of the Archmagi
    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI ))
        se++;

    return se;
}

int player_spec_summ()
{
    int ss = 0;

    /* Staves */
    ss += player_equip( EQ_STAFF, STAFF_SUMMONING );

    // armour of the Archmagi
    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI ))
        ss++;

    return ss;
}

int player_spec_poison()
{
    int sp = 0;

    /* Staves */
    sp += player_equip( EQ_STAFF, STAFF_POISON );

    if (you.equip[EQ_WEAPON] != -1
        && you.inv[you.equip[EQ_WEAPON]].base_type == OBJ_WEAPONS
        && you.inv[you.equip[EQ_WEAPON]].special == SPWPN_STAFF_OF_OLGREB)
    {
        sp++;
    }

    return sp;
}

int player_energy()
{
    int pe = 0;

    // Staves
    pe += player_equip( EQ_STAFF, STAFF_ENERGY );

    return pe;
}

// If temp is set to false, temporary sources of resistance won't be
// counted.
int player_prot_life(bool calc_unid, bool temp)
{
    int pl = 0;

    // Hunger is temporary, true, but that's something you can control,
    // especially as life protection only increases the hungrier you
    // get.
    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
        case HS_STARVING:
        case HS_NEAR_STARVING:
            pl = 3;
            break;
        case HS_VERY_HUNGRY:
        case HS_HUNGRY:
            pl = 2;
            break;
        case HS_SATIATED:
            pl = 1;
            break;
        default:
            break;
        }
    }

    // Same here.  Your piety status, and, hence, TSO's protection, is
    // something you can more or less control.
    if (you.religion == GOD_SHINING_ONE && you.piety > pl * 50)
        pl = you.piety / 50;

    if (temp)
    {
        // Now, transformations could stop at any time.
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_STATUE:
            pl++;
            break;
        case TRAN_SERPENT_OF_HELL:
            pl += 2;
            break;
        case TRAN_LICH:
            pl += 3;
            break;
        default:
           break;
        }
    }

    if (wearing_amulet(AMU_WARDING, calc_unid))
        pl++;

    // rings
    pl += player_equip(EQ_RINGS, RING_LIFE_PROTECTION, calc_unid);

    // armour (checks body armour only)
    pl += player_equip_ego_type(EQ_ALL_ARMOUR, SPARM_POSITIVE_ENERGY);

    // randart wpns
    pl += scan_randarts(RAP_NEGATIVE_ENERGY, calc_unid);

    // undead/demonic power
    pl += player_mutation_level(MUT_NEGATIVE_ENERGY_RESISTANCE);

    pl = std::min(3, pl);

    return (pl);
}

// Returns the movement speed for a player ghost. Note that this is a real
// speed, not a movement cost, so higher is better.
int player_ghost_base_movement_speed()
{
    int speed = you.species == SP_NAGA? 8 : 10;

    if (player_mutation_level(MUT_FAST))
        speed += player_mutation_level(MUT_FAST) + 1;

    if (player_equip_ego_type( EQ_BOOTS, SPARM_RUNNING ))
        speed += 2;

    // Cap speeds.
    if (speed < 6)
        speed = 6;

    if (speed > 13)
        speed = 13;

    return (speed);
}

// New player movement speed system... allows for a bit more that
// "player runs fast" and "player walks slow" in that the speed is
// actually calculated (allowing for centaurs to get a bonus from
// swiftness and other such things).  Levels of the mutation now
// also have meaning (before they all just meant fast).  Most of
// this isn't as fast as it used to be (6 for having anything), but
// even a slight speed advantage is very good... and we certainly don't
// want to go past 6 (see below). -- bwr
int player_movement_speed(void)
{
    int mv = 10;

    if (player_is_swimming())
    {
        // This is swimming... so it doesn't make sense to really
        // apply the other things (the mutation is "cover ground",
        // swiftness is an air spell, can't wear boots, can't be
        // transformed).
        mv = 6;
    }
    else
    {
        // transformations
        if (you.attribute[ATTR_TRANSFORMATION] == TRAN_SPIDER)
            mv = 8;
        else if (you.attribute[ATTR_TRANSFORMATION] == TRAN_BAT)
            mv = 5; // but allowed minimum is six

        // armour
        if (player_equip_ego_type( EQ_BOOTS, SPARM_RUNNING ))
            mv -= 2;

        if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_PONDEROUSNESS ))
            mv += 2;

        // In the air, can fly fast (should be lightly burdened).
        if (you.light_flight())
            mv--;

        // Swiftness is an Air spell, it doesn't work in water, but
        // flying players will move faster.
        if (you.duration[DUR_SWIFTNESS] > 0 && !player_in_water())
            mv -= (you.flight_mode() == FL_FLY ? 4 : 2);

        /* Mutations: -2, -3, -4, unless innate and shapechanged */
        if (player_mutation_level(MUT_FAST) > 0
            && (!you.demon_pow[MUT_FAST] || !player_is_shapechanged()) )
        {
            mv -= (player_mutation_level(MUT_FAST) + 1);
        }

        // Burden
        if (you.burden_state == BS_ENCUMBERED)
            mv += 1;
        else if (you.burden_state == BS_OVERLOADED)
            mv += 3;
    }

    // We'll use the old value of six as a minimum, with haste this could
    // end up as a speed of three, which is about as fast as we want
    // the player to be able to go (since 3 is 3.33x as fast and 2 is 5x,
    // which is a bit of a jump, and a bit too fast) -- bwr
    if (mv < 6)
        mv = 6;

    // Nagas move slowly:
    if (you.species == SP_NAGA && !player_is_shapechanged())
    {
        mv *= 14;
        mv /= 10;
    }

    return (mv);
}

// This function differs from the above in that it's used to set the
// initial time_taken value for the turn.  Everything else (movement,
// spellcasting, combat) applies a ratio to this value.
int player_speed(void)
{
    int ps = 10;

    if (you.duration[DUR_SLOW])
        ps *= 2;

    if (you.duration[DUR_HASTE])
        ps /= 2;

    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_STATUE:
        ps *= 15;
        ps /= 10;
        break;

    case TRAN_SERPENT_OF_HELL:
        ps *= 12;
        ps /= 10;
        break;

    default:
        break;
    }

    return ps;
}

static int _player_armour_racial_bonus(const item_def& item)
{
    if (item.base_type != OBJ_ARMOUR)
        return 0;

    int racial_bonus = 0;
    const unsigned long armour_race = get_equip_race(item);

    // get the armour race value that corresponds to the character's race:
    const unsigned long racial_type
                            = ((player_genus(GENPC_DWARVEN)) ? ISFLAG_DWARVEN :
                               (player_genus(GENPC_ELVEN))   ? ISFLAG_ELVEN :
                               (you.species == SP_HILL_ORC)  ? ISFLAG_ORCISH
                                                             : 0);

    // Dwarven armour is universally good -- bwr
    if (armour_race == ISFLAG_DWARVEN)
        racial_bonus += 4;

    if (racial_type && armour_race == racial_type)
    {
        // Elven armour is light, but still gives one level to elves.
        // Orcish and Dwarven armour are worth +2 to the correct
        // species, plus the plus that anyone gets with dwarven armour.
        // -- bwr
        if (racial_type == ISFLAG_ELVEN)
            racial_bonus += 2;
        else
            racial_bonus += 4;

        // an additional bonus for Beogh worshippers
        if (you.religion == GOD_BEOGH && !you.penance[GOD_BEOGH])
        {
            if (you.piety >= 185)
                racial_bonus += racial_bonus * 9 / 4;
            else if (you.piety >= 160)
                racial_bonus += racial_bonus * 2;
            else if (you.piety >= 120)
                racial_bonus += racial_bonus * 7 / 4;
            else if (you.piety >= 80)
                racial_bonus += racial_bonus * 5 / 4;
            else if (you.piety >= 40)
                racial_bonus += racial_bonus * 3 / 4;
            else
                racial_bonus += racial_bonus / 4;
        }
    }

    return racial_bonus;
}

int player_AC(void)
{
    int AC = 0;
    int i;                      // loop variable

    for (i = EQ_CLOAK; i <= EQ_BODY_ARMOUR; i++)
    {
        if ( i == EQ_SHIELD )
            continue;

        if ( you.equip[i] == -1 )
            continue;

        const item_def& item = you.inv[you.equip[i]];
        const int ac_value = property(item, PARM_AC ) * 100;
        int racial_bonus   = _player_armour_racial_bonus(item);

        AC += ac_value * (30 + 2 * you.skills[SK_ARMOUR] + racial_bonus) / 30;

        AC += item.plus * 100;

        // The deformed don't fit into body armour very well.
        // (This includes nagas and centaurs.)
        if (i == EQ_BODY_ARMOUR && player_mutation_level(MUT_DEFORMED))
            AC -= ac_value / 2;
    }

    AC += player_equip( EQ_RINGS_PLUS, RING_PROTECTION ) * 100;

    if (player_equip_ego_type( EQ_WEAPON, SPWPN_PROTECTION ))
        AC += 500;

    if (player_equip_ego_type( EQ_SHIELD, SPARM_PROTECTION ))
        AC += 300;

    AC += scan_randarts(RAP_AC) * 100;

    if (you.duration[DUR_ICY_ARMOUR])
        AC += 400 + 100 * you.skills[SK_ICE_MAGIC] / 3;         // max 13

    if (you.duration[DUR_STONEMAIL])
        AC += 500 + 100 * you.skills[SK_EARTH_MAGIC] / 2;       // max 18

    if (you.duration[DUR_STONESKIN])
        AC += 200 + 100 * you.skills[SK_EARTH_MAGIC] / 5;       // max 7

    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_NONE
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_BLADE_HANDS)
    {
        // Being a lich doesn't preclude the benefits of hide/scales -- bwr
        //
        // Note: Even though necromutation is a high level spell, it does
        // allow the character full armour (so the bonus is low). -- bwr
        if (you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH)
            AC += (300 + 100 * you.skills[SK_NECROMANCY] / 6);   // max 7

        //jmf: only give:
        if (player_genus(GENPC_DRACONIAN))
        {
            if (you.experience_level < 8)
                AC += 200;
            else if (you.species == SP_GREY_DRACONIAN)
                AC += 100 + 100 * (you.experience_level - 4) / 2;  // max 12
            else
                AC += 100 + (100 * you.experience_level / 4);      // max 7
        }
        else
        {
            switch (you.species)
            {
            case SP_NAGA:
                AC += 100 * you.experience_level / 3;              // max 9
                break;

            case SP_OGRE:
                AC += 100;
                break;

            case SP_TROLL:
            case SP_CENTAUR:
                AC += 300;
                break;

            default:
                break;
            }
        }

        // Scales -- some evil uses of the fact that boolean "true" == 1...
        // I'll spell things out with some comments -- bwr

        // mutations:
        // these give: +1, +2, +3
        AC += 100 * player_mutation_level(MUT_TOUGH_SKIN);
        AC += 100 * player_mutation_level(MUT_GREY_SCALES);
        AC += 100 * player_mutation_level(MUT_SHAGGY_FUR);
        AC += 100 * player_mutation_level(MUT_BLUE_SCALES);
        AC += 100 * player_mutation_level(MUT_SPECKLED_SCALES);
        AC += 100 * player_mutation_level(MUT_IRIDESCENT_SCALES);
        AC += 100 * player_mutation_level(MUT_PATTERNED_SCALES);

        // these give: +1, +3, +5
        if (player_mutation_level(MUT_GREEN_SCALES) > 0)
            AC += (player_mutation_level(MUT_GREEN_SCALES) * 200) - 100;
        if (player_mutation_level(MUT_NACREOUS_SCALES) > 0)
            AC += (player_mutation_level(MUT_NACREOUS_SCALES) * 200) - 100;
        if (player_mutation_level(MUT_BLACK2_SCALES) > 0)
            AC += (player_mutation_level(MUT_BLACK2_SCALES) * 200) - 100;
        if (player_mutation_level(MUT_WHITE_SCALES) > 0)
            AC += (player_mutation_level(MUT_WHITE_SCALES) * 200) - 100;

        // these give: +2, +4, +6
        AC += player_mutation_level(MUT_GREY2_SCALES) * 200;
        AC += player_mutation_level(MUT_YELLOW_SCALES) * 200;
        AC += player_mutation_level(MUT_PURPLE_SCALES) * 200;

        // black gives: +3, +6, +9
        AC += player_mutation_level(MUT_BLACK_SCALES) * 300;

        // boney plates give: +2, +3, +4
        if (player_mutation_level(MUT_BONEY_PLATES) > 0)
            AC += 100 * (player_mutation_level(MUT_BONEY_PLATES) + 1);

        // red gives +1, +2, +4
        AC += 100 * (player_mutation_level(MUT_RED_SCALES)
                     + (player_mutation_level(MUT_RED_SCALES) == 3));

        // indigo gives: +2, +3, +5
        if (player_mutation_level(MUT_INDIGO_SCALES) > 0)
        {
            AC += 100 * (1 + player_mutation_level(MUT_INDIGO_SCALES)
                         + (player_mutation_level(MUT_INDIGO_SCALES) == 3));
        }

        // brown gives: +2, +4, +5
        AC += 100 * ((player_mutation_level(MUT_BROWN_SCALES) * 2)
                     - (player_mutation_level(MUT_BROWN_SCALES) == 3));

        // orange gives: +1, +3, +4
        AC += 100 * (player_mutation_level(MUT_ORANGE_SCALES)
                     + (player_mutation_level(MUT_ORANGE_SCALES) > 1));

        // knobbly red gives: +2, +5, +7
        AC += 100 * ((player_mutation_level(MUT_RED2_SCALES) * 2)
                     + (player_mutation_level(MUT_RED2_SCALES) > 1));

        // metallic gives +3, +7, +10
        AC += 100 * (player_mutation_level(MUT_METALLIC_SCALES) * 3
                     + (player_mutation_level(MUT_METALLIC_SCALES) > 1));
    }
    else
    {
        // transformations:
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_NONE:
        case TRAN_BLADE_HANDS:
        case TRAN_LICH:  // can wear normal body armour (small bonus)
            break;

        case TRAN_SPIDER: // low level (small bonus), also gets EV
            AC += (200 + 100 * you.skills[SK_POISON_MAGIC] / 6); // max 6
            break;

        case TRAN_ICE_BEAST:
            AC += (500 + 100 * (you.skills[SK_ICE_MAGIC] + 1) / 4); // max 12

            if (you.duration[DUR_ICY_ARMOUR])
                AC += (100 + 100 * you.skills[SK_ICE_MAGIC] / 4);   // max +7
            break;

        case TRAN_DRAGON:
            AC += (700 + 100 * you.skills[SK_FIRE_MAGIC] / 3);      // max 16
            break;

        case TRAN_STATUE: // main ability is armour (high bonus)
            AC += (1700 + 100 * you.skills[SK_EARTH_MAGIC] / 2);    // max 30

            if (you.duration[DUR_STONESKIN] || you.duration[DUR_STONEMAIL])
                AC += (100 + 100 * you.skills[SK_EARTH_MAGIC] / 4); // max +7
            break;

        case TRAN_SERPENT_OF_HELL:
            AC += (1000 + 100 * you.skills[SK_FIRE_MAGIC] / 3);     // max 19
            break;

        case TRAN_AIR:    // air - scales & species ought to be irrelevant
            AC = (you.skills[SK_AIR_MAGIC] * 300) / 2;            // max 40
            break;

        default:
            break;
        }
    }

    return (AC / 100);
}

bool is_light_armour( const item_def &item )
{
    if (get_equip_race(item) == ISFLAG_ELVEN)
        return (true);

    switch (item.sub_type)
    {
    case ARM_ROBE:
    case ARM_ANIMAL_SKIN:
    case ARM_LEATHER_ARMOUR:
    case ARM_STEAM_DRAGON_HIDE:
    case ARM_STEAM_DRAGON_ARMOUR:
    case ARM_MOTTLED_DRAGON_HIDE:
    case ARM_MOTTLED_DRAGON_ARMOUR:
        return (true);

    default:
        return (false);
    }
}

bool player_light_armour(bool with_skill)
{
    const int arm = you.equip[EQ_BODY_ARMOUR];

    if (arm == -1)
        return (true);

    if (with_skill
        && property(you.inv[arm], PARM_EVASION) + you.skills[SK_ARMOUR]/3 >= 0)
    {
        return (true);
    }

    return (is_light_armour(you.inv[arm]));
}                               // end player_light_armour()

//
// This function returns true if the player has a radically different
// shape... minor changes like blade hands don't count, also note
// that lich transformation doesn't change the character's shape
// (so we end up with Naga-lichs, Spiggan-lichs, Minotaur-lichs)
// it just makes the character undead (with the benefits that implies). --bwr
//
bool player_is_shapechanged(void)
{
    if (you.attribute[ATTR_TRANSFORMATION] == TRAN_NONE
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_BLADE_HANDS
        || you.attribute[ATTR_TRANSFORMATION] == TRAN_LICH)
    {
        return (false);
    }

    return (true);
}

// psize defaults to PSIZE_TORSO, which checks the part of the body
// that wears armour and wields weapons (which is different for some hybrids).
// base defaults to "false", meaning consider our current size, not our
// natural one.
size_type player_size( int psize, bool base )
{
    return you.body_size(psize, base);
}

// New and improved 4.1 evasion model, courtesy Brent Ross.
int player_evasion()
{
    // XXX: player_size() implementations are incomplete, fix.
    const size_type size  = player_size(PSIZE_BODY);
    const size_type torso = player_size(PSIZE_TORSO);

    const int size_factor = SIZE_MEDIUM - size;
    int ev = 10 + 2 * size_factor;

    // Repulsion fields and size are all that matters when paralysed.
    if (you.cannot_move())
    {
        ev = 2 + size_factor;
        if (player_mutation_level(MUT_REPULSION_FIELD) > 0)
            ev += (player_mutation_level(MUT_REPULSION_FIELD) * 2) - 1;
        return ((ev < 1) ? 1 : ev);
    }

    // Calculate the base bonus here, but it may be reduced by heavy
    // armour below.
    int dodge_bonus = (you.skills[SK_DODGING] * you.dex + 7)
                      / (20 - size_factor);

    // Limit on bonus from dodging:
    const int max_bonus = (you.skills[SK_DODGING] * (7 + size_factor)) / 9;
    if (dodge_bonus > max_bonus)
        dodge_bonus = max_bonus;

    // Some lesser armours have small penalties now (shields, barding).
    for (int i = EQ_CLOAK; i < EQ_BODY_ARMOUR; i++)
    {
        if (you.equip[i] == -1)
            continue;

        int pen = property( you.inv[ you.equip[i] ], PARM_EVASION );

        // Reducing penalty of larger shields for larger characters.
        if (i == EQ_SHIELD && torso > SIZE_MEDIUM)
            pen += (torso - SIZE_MEDIUM);

        if (pen < 0)
            ev += pen;
    }

    // Handle main body armour penalty.
    if (you.equip[EQ_BODY_ARMOUR] != -1)
    {
        // XXX: magnify arm_penalty for weak characters?
        // Remember: arm_penalty and ev_change are negative.
        int arm_penalty = property( you.inv[ you.equip[EQ_BODY_ARMOUR] ],
                                          PARM_EVASION );

        // The very large races take a penalty to AC for not being able
        // to fully cover, and in compensation we give back some freedom
        // of movement here.  Likewise, the very small are extra encumbered
        // by armour (which partially counteracts their size bonus above).
        if (size < SIZE_SMALL || size > SIZE_LARGE)
        {
            arm_penalty -= ((size - SIZE_MEDIUM) * arm_penalty) / 4;

            if (arm_penalty > 0)
                arm_penalty = 0;
        }

        int ev_change = arm_penalty;
        ev_change += (you.skills[SK_ARMOUR] * you.strength) / 60;

        if (ev_change > arm_penalty / 2)
            ev_change = arm_penalty / 2;

        ev += ev_change;

        // This reduces dodging ability in heavy armour.
        if (!player_light_armour())
            dodge_bonus += (arm_penalty * 30 + 15) / you.strength;
    }

    if (dodge_bonus > 0)                // always a bonus
        ev += dodge_bonus;

    if (you.duration[DUR_FORESCRY])
        ev += 8;

    if (you.duration[DUR_STONEMAIL])
        ev -= 2;

    ev += player_equip( EQ_RINGS_PLUS, RING_EVASION );
    ev += scan_randarts( RAP_EVASION );

    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_PONDEROUSNESS ))
        ev -= 2;

    if (player_mutation_level(MUT_REPULSION_FIELD) > 0)
        ev += (player_mutation_level(MUT_REPULSION_FIELD) * 2) - 1;

    // transformation penalties/bonuses not covered by size alone:
    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_STATUE:
        ev -= 5;                // stiff
        break;

    case TRAN_AIR:
        ev += 20;               // vapourous
        break;

    default:
        break;
    }

    switch (you.species)
    {
    case SP_MERFOLK:
        // Merfolk get an evasion bonus in water.
        if (you.swimming())
        {
            const int ev_bonus = std::min(9, std::max(2, ev / 4));
            ev += ev_bonus;
        }
        break;

    case SP_KENKU:
        // Flying Kenku get an evasion bonus.
        if (you.flight_mode() == FL_FLY)
        {
            const int ev_bonus = std::min(9, std::max(1, ev / 5));
            ev += ev_bonus;
        }
        break;

    default:
        break;
    }

    return (ev);
}

// Obsolete evasion calc.
#if 0
int old_player_evasion(void)
{
    int ev = 10;

    int armour_ev_penalty;

    if (you.equip[EQ_BODY_ARMOUR] == -1)
        armour_ev_penalty = 0;
    else
    {
        armour_ev_penalty = property( you.inv[you.equip[EQ_BODY_ARMOUR]],
                                      PARM_EVASION );
    }

    // We return 2 here to give the player some chance of not being hit,
    // repulsion fields still work while paralysed
    if (you.duration[DUR_PARALYSIS])
        return (2 + player_mutation_level(MUT_REPULSION_FIELD) * 2);

    if (you.species == SP_CENTAUR)
        ev -= 3;

    if (you.equip[EQ_BODY_ARMOUR] != -1)
    {
        int ev_change = 0;

        ev_change = armour_ev_penalty;
        ev_change += you.skills[SK_ARMOUR] / 3;

        if (ev_change > armour_ev_penalty / 3)
            ev_change = armour_ev_penalty / 3;

        ev += ev_change;        /* remember that it's negative */
    }

    ev += player_equip( EQ_RINGS_PLUS, RING_EVASION );

    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_PONDEROUSNESS ))
        ev -= 2;

    if (you.duration[DUR_STONEMAIL])
        ev -= 2;

    if (you.duration[DUR_FORESCRY])
        ev += 8;                //jmf: is this a reasonable value?

    int emod = 0;

    if (!player_light_armour())
    {
        // meaning that the armour evasion modifier is often effectively
        // applied twice, but not if you're wearing elven armour
        emod += (armour_ev_penalty * 14) / 10;
    }

    emod += you.skills[SK_DODGING] / 2;

    if (emod > 0)
        ev += emod;

    if (player_mutation_level(MUT_REPULSION_FIELD) > 0)
        ev += (player_mutation_level(MUT_REPULSION_FIELD) * 2) - 1;

    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_DRAGON:
        ev -= 3;
        break;

    case TRAN_STATUE:
    case TRAN_SERPENT_OF_HELL:
        ev -= 5;
        break;

    case TRAN_SPIDER:
        ev += 3;
        break;

    case TRAN_BAT:
        ev += 20 + (you.experience_level - 10);
        break;

    case TRAN_AIR:
        ev += 20;
        break;

    default:
        break;
    }

    ev += scan_randarts(RAP_EVASION);

    return ev;
}                               // end player_evasion()
#endif

int player_magical_power( void )
{
    int ret = 0;

    ret += 13 * player_equip(  EQ_STAFF, STAFF_POWER );
    ret +=  9 * player_equip(  EQ_RINGS, RING_MAGICAL_POWER );
    ret +=      scan_randarts( RAP_MAGICAL_POWER );

    return (ret);
}

int player_mag_abil(bool is_weighted)
{
    int ma = 0;

    ma += 3 * player_equip( EQ_RINGS, RING_WIZARDRY );

    /* Staves */
    ma += 4 * player_equip( EQ_STAFF, STAFF_WIZARDRY );

    /* armour of the Archmagi (checks body armour only) */
    ma += 2 * player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI );

    return ((is_weighted) ? ((ma * you.intel) / 10) : ma);
}                               // end player_mag_abil()

// Returns the shield the player is wearing, or NULL if none.
item_def *player_shield()
{
    return you.shield();
}

int player_shield_class(void)   //jmf: changes for new spell
{
    int base_shield = 0;
    const int shield = you.equip[EQ_SHIELD];

    if (shield == -1)
    {
        if (you.duration[DUR_MAGIC_SHIELD])
            base_shield = 2 + you.skills[SK_EVOCATIONS] / 6;

        if (!you.duration[DUR_FIRE_SHIELD] &&
            you.duration[DUR_CONDENSATION_SHIELD])
            base_shield += 2 + (you.skills[SK_ICE_MAGIC] / 6);  // max 6
    }
    else
    {
        const item_def& item = you.inv[shield];

        switch (item.sub_type)
        {
        case ARM_BUCKLER:
            base_shield = 3;   // +3/20 per skill level    max 7
            break;
        case ARM_SHIELD:
            base_shield = 5;   // +5/20 per skill level    max 11
            break;
        case ARM_LARGE_SHIELD:
            base_shield = 7;   // +7/20 per skill level    max 16
            break;
        }

        int racial_bonus = _player_armour_racial_bonus(item) * 2 / 3;

        // bonus applied only to base, see above for effect:
        base_shield *= (20 + you.skills[SK_SHIELDS] + racial_bonus);
        base_shield /= 20;

        base_shield += item.plus;
    }

    if (you.duration[DUR_DIVINE_SHIELD])
        base_shield += you.attribute[ATTR_DIVINE_SHIELD];

    return (base_shield);
}                               // end player_shield_class()

int player_see_invis(bool calc_unid)
{
    int si = 0;

    si += player_equip( EQ_RINGS, RING_SEE_INVISIBLE, calc_unid );

    /* armour: (checks head armour only) */
    si += player_equip_ego_type( EQ_HELMET, SPARM_SEE_INVISIBLE );

    if (player_mutation_level(MUT_ACUTE_VISION) > 0)
        si += player_mutation_level(MUT_ACUTE_VISION);

    //jmf: added see_invisible spell
    if (you.duration[DUR_SEE_INVISIBLE] > 0)
        si++;

    /* randart wpns */
    int artefacts = scan_randarts(RAP_EYESIGHT, calc_unid);

    if (artefacts > 0)
        si += artefacts;

    if (si > 1)
        si = 1;

    return (si);
}

// This does NOT do line of sight!  It checks the monster's visibility
// with repect to the players perception, but doesn't do walls or range...
// to find if the square the monster is in los see mons_near().
bool player_monster_visible( const monsters *mon )
{
    if (mon->has_ench(ENCH_SUBMERGED)
        || mon->invisible() && !player_see_invis())
    {
        return (false);
    }

    return (true);
}

// Returns true if player is beheld by a given monster.
bool player_beheld_by( const monsters *mon )
{
    if (!you.duration[DUR_BEHELD])
        return (false);

    // Can this monster even behold you?
    if (mon->type != MONS_MERMAID)
        return (false);

#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "beheld_by.size: %d, DUR_BEHELD: %d, current mon: %d",
                            you.beheld_by.size(), you.duration[DUR_BEHELD],
                            monster_index(mon));
#endif

    if (you.beheld_by.empty()) // shouldn't happen
    {
        you.duration[DUR_BEHELD] = 0;
        return (false);
    }

    for (unsigned int i = 0; i < you.beheld_by.size(); i++)
    {
         unsigned int which_mon = you.beheld_by[i];
         if (monster_index(mon) == which_mon)
             return (true);
    }

    return (false);
}

// Removes a monster from the list of beholders if force == true
// (e.g. monster dead) or one of several cases is met.
void update_beholders(const monsters *mon, bool force)
{
    if (!player_beheld_by(mon)) // Not in list?
        return;

    // Is an update even necessary?
    if (force || !mons_near(mon) || mons_friendly(mon) || mon->submerged()
        || mon->has_ench(ENCH_CONFUSION) || mons_cannot_move(mon)
        || mon->asleep() || silenced(you.x_pos, you.y_pos)
        || silenced(mon->x, mon->y))
    {
        const std::vector<int> help = you.beheld_by;
        you.beheld_by.clear();

        for (unsigned int i = 0; i < help.size(); i++)
        {
             unsigned int which_mon = help[i];
             if (monster_index(mon) != which_mon)
                 you.beheld_by.push_back(i);
        }

        if (you.beheld_by.empty())
        {
            mpr("You are no longer entranced.", MSGCH_RECOVERY);
            you.duration[DUR_BEHELD] = 0;
        }
    }
}

void check_beholders()
{
    for (int i = you.beheld_by.size() - 1; i >= 0; i--)
    {
        const monsters* mon = &menv[you.beheld_by[i]];
        if (!mon->alive() || mon->type != MONS_MERMAID)
        {
#if DEBUG
            if (!mon->alive())
                mpr("Dead mermaid still beholding?", MSGCH_DIAGNOSTICS);
            else if (mon->type != MONS_MERMAID)
                mprf(MSGCH_DIAGNOSTICS, "Non-mermaid '%s' beholding?",
                     mon->name(DESC_PLAIN, true).c_str());
#endif

            you.beheld_by.erase(you.beheld_by.begin() + i);
            if (you.beheld_by.empty())
            {
                mpr("You are no longer entranced.", MSGCH_RECOVERY);
                you.duration[DUR_BEHELD] = 0;
                break;
            }
            continue;
        }
        const coord_def pos = mon->pos();
        int walls = num_feats_between(you.x_pos, you.y_pos,
                                      pos.x, pos.y, DNGN_UNSEEN,
                                      DNGN_MAXWALL);

        if (walls > 0)
        {
#if DEBUG
            mprf(MSGCH_DIAGNOSTICS, "%d walls between beholding '%s' "
                 "and player", walls, mon->name(DESC_PLAIN, true).c_str());
#endif
            you.beheld_by.erase(you.beheld_by.begin() + i);
            if (you.beheld_by.empty())
            {
                mpr("You are no longer entranced.", MSGCH_RECOVERY);
                you.duration[DUR_BEHELD] = 0;
                break;
            }
            continue;
        }
    }

    if (you.duration[DUR_BEHELD] > 0 && you.beheld_by.empty())
    {
#if DEBUG
        mpr("Beheld with no mermaids left?", MSGCH_DIAGNOSTICS);
#endif

        mpr("You are no longer entranced.", MSGCH_RECOVERY);
        you.duration[DUR_BEHELD] = 0;
    }
}

int player_sust_abil(bool calc_unid)
{
    int sa = 0;

    sa += player_equip( EQ_RINGS, RING_SUSTAIN_ABILITIES, calc_unid );

    return sa;
}                               // end player_sust_abil()

int carrying_capacity( burden_state_type bs )
{
    int cap = 3500 + (you.strength * 100) + (player_is_airborne() ? 1000 : 0);
    if ( bs == BS_UNENCUMBERED )
        return (cap * 5) / 6;
    else if ( bs == BS_ENCUMBERED )
        return (cap * 11) / 12;
    else
        return cap;
}

int burden_change(void)
{
    const burden_state_type old_burdenstate = you.burden_state;
    const bool was_flying_light = you.light_flight();

    you.burden = 0;

    if (you.duration[DUR_STONEMAIL])
        you.burden += 800;

    for (int bu = 0; bu < ENDOFPACK; bu++)
    {
        if (you.inv[bu].quantity < 1)
            continue;

        you.burden += item_mass( you.inv[bu] ) * you.inv[bu].quantity;
    }

    you.burden_state = BS_UNENCUMBERED;
    set_redraw_status( REDRAW_BURDEN );
    you.redraw_evasion = true;

    // changed the burdened levels to match the change to max_carried
    if (you.burden < carrying_capacity(BS_UNENCUMBERED))
    {
        you.burden_state = BS_UNENCUMBERED;

        // this message may have to change, just testing {dlb}
        if (old_burdenstate != you.burden_state)
            mpr("Your possessions no longer seem quite so burdensome.");
    }
    else if (you.burden < carrying_capacity(BS_ENCUMBERED))
    {
        you.burden_state = BS_ENCUMBERED;

        if (old_burdenstate != you.burden_state)
        {
            mpr("You are being weighed down by all of your possessions.");
            learned_something_new(TUT_HEAVY_LOAD);
        }
    }
    else
    {
        you.burden_state = BS_OVERLOADED;

        if (old_burdenstate != you.burden_state)
        {
            mpr("You are being crushed by all of your possessions.");
            learned_something_new(TUT_HEAVY_LOAD);
        }
    }

    // Stop travel if we get burdened (as from potions of might/levitation
    // wearing off).
    if (you.burden_state > old_burdenstate)
        interrupt_activity( AI_BURDEN_CHANGE );

    const bool is_flying_light = you.light_flight();

    if (is_flying_light != was_flying_light)
    {
        mpr(is_flying_light ? "You feel quicker in the air."
                            : "You feel heavier in the air.");
    }

    return you.burden;
}                               // end burden_change()

bool you_resist_magic(int power)
{
    int ench_power = stepdown_value( power, 30, 40, 100, 120 );

    int mrchance = 100 + player_res_magic();

    mrchance -= ench_power;

    int mrch2 = random2(100) + random2(101);

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "Power: %d, player's MR: %d, target: %d, roll: %d",
         ench_power, player_res_magic(), mrchance, mrch2 );
#endif

    if (mrch2 < mrchance)
        return (true);            // ie saved successfully

    return (false);
}

// force is true for forget_map command on level map.
void forget_map(unsigned char chance_forgotten, bool force)
{
    if (force && !yesno("Really forget level map?", true, 'n'))
        return;

    for (unsigned char xcount = 0; xcount < GXM; xcount++)
        for (unsigned char ycount = 0; ycount < GYM; ycount++)
        {
            if (!see_grid(xcount, ycount)
                && (force || x_chance_in_y(chance_forgotten, 100)))
            {
                env.map[xcount][ycount].clear();
            }
        }

#ifdef USE_TILE
    tiles.clear_minimap();
    tile_clear_buf();
#endif
}

void gain_exp( unsigned int exp_gained, unsigned int* actual_gain,
               unsigned int* actual_avail_gain)
{
    if (player_equip_ego_type( EQ_BODY_ARMOUR, SPARM_ARCHMAGI ))
        exp_gained = div_rand_round( exp_gained, 4 );

    const unsigned long old_exp   = you.experience;
    const int           old_avail = you.exp_available;

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "gain_exp: %d", exp_gained );
#endif

    if (you.experience + exp_gained > 8999999)
        you.experience = 8999999;
    else
        you.experience += exp_gained;

    if (you.duration[DUR_SAGE])
    {
        // Bonus skill training from Sage.
        you.exp_available =
            (exp_gained * you.sage_bonus_degree) / 100 + exp_gained / 2;
        exercise(you.sage_bonus_skill, 20);
        you.exp_available = old_avail;
        exp_gained /= 2;
    }

    if (you.exp_available + exp_gained > 20000)
        you.exp_available = 20000;
    else
        you.exp_available += exp_gained;

    level_change();

    // Increase tutorial time-out now that it's actually
    // become useful for a longer time.
    if (Options.tutorial_left && you.experience_level == 7)
        tutorial_finished();

    if (actual_gain != NULL)
        *actual_gain = you.experience - old_exp;

    if (actual_avail_gain != NULL)
        *actual_avail_gain = you.exp_available - old_avail;
}                               // end gain_exp()

void level_change(bool skip_attribute_increase)
{
    // necessary for the time being, as level_change() is called
    // directly sometimes {dlb}
    you.redraw_experience = true;

    while (you.experience_level < 27
           && you.experience > exp_needed(you.experience_level + 2))
    {
        bool skip_more = false;

        if (!skip_attribute_increase)
        {
            if (crawl_state.is_replaying_keys()
                || crawl_state.is_repeating_cmd())
            {
                crawl_state.cancel_cmd_repeat();
                crawl_state.cancel_cmd_again();
            }

            if (is_processing_macro())
                flush_input_buffer(FLUSH_ABORT_MACRO);
        }
        else if (crawl_state.is_replaying_keys()
                 || crawl_state.is_repeating_cmd() || is_processing_macro())
        {
            skip_more = true;
        }

        int hp_adjust = 0;
        int mp_adjust = 0;

        you.experience_level++;

        if (you.experience_level <= you.max_level)
        {
            mprf(MSGCH_INTRINSIC_GAIN,
                 "Welcome back to level %d!", you.experience_level );
            if (!skip_more)
                more();

            // Gain back the hp and mp we lose in lose_level().  -- bwr
            inc_hp( 4, true );
            inc_mp( 1, true );
        }
        else  // Character has gained a new level
        {
            if (you.experience_level == 27)
               mprf(MSGCH_INTRINSIC_GAIN, "You have reached level 27, the final one!");
            else
            {
               mprf(MSGCH_INTRINSIC_GAIN, "You have reached level %d!",
                    you.experience_level);
            }
            if (!skip_more)
                more();

            int brek = 0;

            if (you.experience_level > 21)
                brek = (coinflip() ? 3 : 2);
            else if (you.experience_level > 12)
                brek = 3 + random2(3);      // up from 2 + rand(3) -- bwr
            else
                brek = 4 + random2(4);      // up from 3 + rand(4) -- bwr

            inc_hp( brek, true );
            inc_mp( 1, true );

            if (!(you.experience_level % 3) && !skip_attribute_increase)
                _attribute_increase();

            switch (you.species)
            {
            case SP_HUMAN:
                if (!(you.experience_level % 5))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");
                break;

            case SP_HIGH_ELF:
                if (you.experience_level % 3)
                    hp_adjust--;

                if (!(you.experience_level % 2))
                    mp_adjust++;

                if (!(you.experience_level % 3))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }
                break;

            case SP_GREY_ELF:
                if (you.experience_level < 14)
                    hp_adjust--;

                if (you.experience_level % 3)
                    mp_adjust++;

                if (!(you.experience_level % 4))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }

                break;

            case SP_DEEP_ELF:
                if (you.experience_level < 17)
                    hp_adjust--;
                if (!(you.experience_level % 3))
                    hp_adjust--;

                mp_adjust++;

                if (!(you.experience_level % 4))
                    modify_stat(STAT_INTELLIGENCE, 1, false, "level gain");
                break;

            case SP_SLUDGE_ELF:
                if (you.experience_level % 3)
                    hp_adjust--;
                else
                    mp_adjust++;

                if (!(you.experience_level % 4))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }
                break;

            case SP_MOUNTAIN_DWARF:
                // lowered because of HD raise -- bwr
                // if (you.experience_level < 14)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (!(you.experience_level % 3))
                    mp_adjust--;

                if (!(you.experience_level % 4))
                    modify_stat(STAT_STRENGTH, 1, false, "level gain");
                break;

            case SP_HALFLING:
                if (!(you.experience_level % 5))
                    modify_stat(STAT_DEXTERITY, 1, false, "level gain");

                if (you.experience_level < 17)
                    hp_adjust--;

                if (!(you.experience_level % 2))
                    hp_adjust--;
                break;

            case SP_KOBOLD:
                if (!(you.experience_level % 5))
                {
                    modify_stat( (coinflip() ? STAT_STRENGTH
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }

                if (you.experience_level < 17)
                    hp_adjust--;

                if (!(you.experience_level % 2))
                    hp_adjust--;
                break;

            case SP_HILL_ORC:
                // lower because of HD raise -- bwr
                // if (you.experience_level < 17)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (!(you.experience_level % 3))
                    mp_adjust--;

                if (!(you.experience_level % 5))
                    modify_stat(STAT_STRENGTH, 1, false, "level gain");
                break;

            case SP_MUMMY:
                if (you.experience_level == 13 || you.experience_level == 26)
                {
                    mpr( "You feel more in touch with the powers of death.",
                         MSGCH_INTRINSIC_GAIN );
                }

                if (you.experience_level == 13)  // level 13 for now -- bwr
                {
                    mpr( "You can now infuse your body with magic to restore decomposition.", MSGCH_INTRINSIC_GAIN );
                }
                break;

            case SP_VAMPIRE:
                if (you.experience_level == 3)
                {
                    if (you.hunger_state > HS_SATIATED)
                    {
                        mpr( "If you weren't so full you could now transform "
                             "into a vampire bat.", MSGCH_INTRINSIC_GAIN );
                    }
                    else
                    {
                        mpr( "You can now transform into a vampire bat.",
                             MSGCH_INTRINSIC_GAIN );
                    }
                }
                else if (you.experience_level == 6)
                {
                    mpr("You can now bottle potions of blood from chopped up "
                        "corpses.");
                }
                else if (you.experience_level == 13)
                {
                    mprf( MSGCH_INTRINSIC_GAIN,
                          "You feel %sin touch with the powers of death.",
                          (you.hunger_state < HS_SATIATED ? "" : "strangely "));
                }
                break;
            case SP_NAGA:
                // lower because of HD raise -- bwr
                // if (you.experience_level < 14)
                //     hp_adjust++;

                hp_adjust++;

                if (!(you.experience_level % 4))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");

                if (!(you.experience_level % 3))
                {
                    mpr("Your skin feels tougher.", MSGCH_INTRINSIC_GAIN);
                    you.redraw_armour_class = true;
                }
                break;

            case SP_GNOME:
                if (you.experience_level < 13)
                    hp_adjust--;

                if (!(you.experience_level % 3))
                    hp_adjust--;

                if (!(you.experience_level % 4))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }
                break;

            case SP_OGRE:
            case SP_TROLL:
                hp_adjust++;

                // lowered because of HD raise -- bwr
                // if (you.experience_level < 14)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (you.experience_level % 3)
                    mp_adjust--;

                if (!(you.experience_level % 3))
                    modify_stat(STAT_STRENGTH, 1, false, "level gain");
                break;

            case SP_OGRE_MAGE:
                hp_adjust++;

                // lowered because of HD raise -- bwr
                // if (you.experience_level < 14)
                //     hp_adjust++;

                if (!(you.experience_level % 5))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                                 : STAT_STRENGTH), 1, false,
                                 "level gain");
                }
                break;

            case SP_RED_DRACONIAN:
            case SP_WHITE_DRACONIAN:
            case SP_GREEN_DRACONIAN:
            case SP_YELLOW_DRACONIAN:
            // Grey is handled later.
            case SP_BLACK_DRACONIAN:
            case SP_PURPLE_DRACONIAN:
            case SP_MOTTLED_DRACONIAN:
            case SP_PALE_DRACONIAN:
            case SP_BASE_DRACONIAN:
                if (you.experience_level == 7)
                {
                    switch (you.species)
                    {
                    case SP_RED_DRACONIAN:
                        mpr("Your scales start taking on a fiery red colour.",
                            MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_WHITE_DRACONIAN:
                        mpr("Your scales start taking on an icy white colour.",
                            MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_GREEN_DRACONIAN:
                        mpr("Your scales start taking on a green colour.",
                            MSGCH_INTRINSIC_GAIN);
                        // green dracos get this at level 7
                        perma_mutate(MUT_POISON_RESISTANCE, 1);
                        break;

                    case SP_YELLOW_DRACONIAN:
                        mpr("Your scales start taking on a golden yellow colour.", MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_BLACK_DRACONIAN:
                        mpr("Your scales start turning black.", MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_PURPLE_DRACONIAN:
                        mpr("Your scales start taking on a rich purple colour.", MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_MOTTLED_DRACONIAN:
                        mpr("Your scales start taking on a weird mottled pattern.", MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_PALE_DRACONIAN:
                        mpr("Your scales start fading to a pale grey colour.", MSGCH_INTRINSIC_GAIN);
                        break;
                    case SP_BASE_DRACONIAN:
                        mpr("");
                        break;

                    default:
                        break;
                    }
                    more();
                    redraw_screen();
                }

                if (you.experience_level == 14)
                {
                    switch (you.species)
                    {
                    case SP_RED_DRACONIAN:
                        perma_mutate(MUT_HEAT_RESISTANCE, 1);
                        break;
                    case SP_WHITE_DRACONIAN:
                        perma_mutate(MUT_COLD_RESISTANCE, 1);
                        break;
                    default:
                        break;
                    }
                }
                else if (you.species == SP_BLACK_DRACONIAN
                         && you.experience_level == 18)
                {
                    perma_mutate(MUT_SHOCK_RESISTANCE, 1);
                }

                if (!(you.experience_level % 3))
                    hp_adjust++;

                if (you.experience_level > 7 && !(you.experience_level % 4))
                {
                    mpr("Your scales feel tougher.", MSGCH_INTRINSIC_GAIN);
                    you.redraw_armour_class = true;
                    modify_stat(STAT_RANDOM, 1, false, "level gain");
                }
                break;

            case SP_GREY_DRACONIAN:
                if (you.experience_level == 7)
                {
                    mpr("Your scales start turning grey.", MSGCH_INTRINSIC_GAIN);
                    more();
                    redraw_screen();
                }

                if (!(you.experience_level % 3))
                {
                    hp_adjust++;
                    if (you.experience_level > 7)
                        hp_adjust++;
                }

                if (you.experience_level > 7 && !(you.experience_level % 2))
                {
                    mpr("Your scales feel tougher.", MSGCH_INTRINSIC_GAIN);
                    you.redraw_armour_class = true;
                }

                if ((you.experience_level > 7 && !(you.experience_level % 3))
                    || you.experience_level == 4 || you.experience_level == 7)
                {
                    modify_stat(STAT_RANDOM, 1, false, "level gain");
                }
                break;

            case SP_CENTAUR:
                if (!(you.experience_level % 4))
                {
                    modify_stat( (coinflip() ? STAT_DEXTERITY
                                             : STAT_STRENGTH), 1, false,
                                 "level gain");
                }

                // lowered because of HD raise -- bwr
                // if (you.experience_level < 17)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (!(you.experience_level % 3))
                    mp_adjust--;
                break;

            case SP_DEMIGOD:
                if (!(you.experience_level % 2))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");

                // lowered because of HD raise -- bwr
                // if (you.experience_level < 17)
                //    hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (you.experience_level % 3)
                    mp_adjust++;
                break;

            case SP_SPRIGGAN:
                if (you.experience_level < 17)
                    hp_adjust--;

                if (you.experience_level % 3)
                    hp_adjust--;

                mp_adjust++;

                if (!(you.experience_level % 5))
                {
                    modify_stat( (coinflip() ? STAT_INTELLIGENCE
                                             : STAT_DEXTERITY), 1, false,
                                 "level gain");
                }
                break;

            case SP_MINOTAUR:
                // lowered because of HD raise -- bwr
                // if (you.experience_level < 17)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (!(you.experience_level % 2))
                    mp_adjust--;

                if (!(you.experience_level % 4))
                {
                    modify_stat( (coinflip() ? STAT_DEXTERITY
                                             : STAT_STRENGTH), 1, false,
                                 "level gain");
                }
                break;

            case SP_DEMONSPAWN:
                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 0
                    && (you.experience_level == 4
                        || (you.experience_level < 4 && one_chance_in(3))))
                {
                    demonspawn();
                }

                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 1
                    && you.experience_level > 4
                    && (you.experience_level == 9
                        || (you.experience_level < 9 && one_chance_in(3))))
                {
                    demonspawn();
                }

                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 2
                    && you.experience_level > 9
                    && (you.experience_level == 14
                        || (you.experience_level < 14 && one_chance_in(3))))
                {
                    demonspawn();
                }

                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 3
                    && you.experience_level > 14
                    && (you.experience_level == 19
                        || (you.experience_level < 19 && one_chance_in(3))))
                {
                    demonspawn();
                }

                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 4
                    && you.experience_level > 19
                    && (you.experience_level == 24
                        || (you.experience_level < 24 && one_chance_in(3))))
                {
                    demonspawn();
                }

                if (you.attribute[ATTR_NUM_DEMONIC_POWERS] == 5
                    && you.experience_level == 27)
                {
                    demonspawn();
                }

                if (!(you.experience_level % 4))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");
                break;

            case SP_GHOUL:
                // lowered because of HD raise -- bwr
                // if (you.experience_level < 17)
                //     hp_adjust++;

                if (!(you.experience_level % 2))
                    hp_adjust++;

                if (!(you.experience_level % 3))
                    mp_adjust--;

                if (!(you.experience_level % 5))
                    modify_stat(STAT_STRENGTH, 1, false, "level gain");
                break;

            case SP_KENKU:
                if (you.experience_level < 17)
                    hp_adjust--;

                if (!(you.experience_level % 3))
                    hp_adjust--;

                if (!(you.experience_level % 4))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");

                if (you.experience_level == 5)
                {
                    mpr("You have gained the ability to fly.",
                        MSGCH_INTRINSIC_GAIN);
                }
                else if (you.experience_level == 15)
                    mpr("You can now fly continuously.", MSGCH_INTRINSIC_GAIN);
                break;

            case SP_MERFOLK:
                if (you.experience_level % 3)
                    hp_adjust++;

                if (!(you.experience_level % 5))
                    modify_stat(STAT_RANDOM, 1, false, "level gain");
                break;

            default:
                break;
            }
        }

        // add hp and mp adjustments - GDL
        inc_max_hp( hp_adjust );
        inc_max_mp( mp_adjust );

        deflate_hp( you.hp_max, false );

        you.magic_points = std::max(0, you.magic_points);

        {
            unwind_var<int> hpmax(you.hp_max);
            unwind_var<int> hp(you.hp);
            unwind_var<int> mpmax(you.max_magic_points);
            unwind_var<int> mp(you.magic_points);
            // calculate "real" values for note-taking, i.e. ignore Berserk,
            // transformations or equipped items
            calc_hp(true);
            calc_mp(true);

            char buf[200];
            sprintf(buf, "HP: %d/%d MP: %d/%d",
                    you.hp, you.hp_max, you.magic_points, you.max_magic_points);
            take_note(Note(NOTE_XP_LEVEL_CHANGE, you.experience_level, 0, buf));
        }

        // recalculate for game
        calc_hp();
        calc_mp();

        if (you.experience_level > you.max_level)
            you.max_level = you.experience_level;

        xom_is_stimulated(16);

        learned_something_new(TUT_NEW_LEVEL);

    }

    redraw_skill( you.your_name, player_title() );
}

// Here's a question for you: does the ordering of mods make a difference?
// (yes) -- are these things in the right order of application to stealth?
// - 12mar2000 {dlb}
int check_stealth(void)
{
#ifdef WIZARD
    // Extreme stealthiness can be enforced by wizmode stealth setting.
    if (you.skills[SK_STEALTH] > 27)
        return (1000);
#endif

    if (you.special_wield == SPWLD_SHADOW || you.duration[DUR_BERSERKER])
        return (0);

    int stealth = you.dex * 3;

    if (you.skills[SK_STEALTH])
    {
        if (player_genus(GENPC_DRACONIAN))
            stealth += (you.skills[SK_STEALTH] * 12);
        else
        {
            switch (you.species) // why not use body_size here?
            {
            case SP_TROLL:
            case SP_OGRE:
            case SP_OGRE_MAGE:
            case SP_CENTAUR:
                stealth += (you.skills[SK_STEALTH] * 9);
                break;
            case SP_MINOTAUR:
                stealth += (you.skills[SK_STEALTH] * 12);
                break;
            case SP_VAMPIRE:
                // Thirsty/bat-form vampires are (much) more stealthy
                if (you.hunger_state == HS_STARVING)
                    stealth += (you.skills[SK_STEALTH] * 21);
                else if (you.attribute[ATTR_TRANSFORMATION] == TRAN_BAT
                         || you.hunger_state <= HS_NEAR_STARVING)
                {
                    stealth += (you.skills[SK_STEALTH] * 20);
                }
                else if (you.hunger_state < HS_SATIATED)
                    stealth += (you.skills[SK_STEALTH] * 19);
                else
                    stealth += (you.skills[SK_STEALTH] * 18);
                break;
            case SP_GNOME:
            case SP_HALFLING:
            case SP_KOBOLD:
            case SP_SPRIGGAN:
            case SP_NAGA:       // not small but very good at stealth
                stealth += (you.skills[SK_STEALTH] * 18);
                break;
            default:
                stealth += (you.skills[SK_STEALTH] * 15);
                break;
            }
        }
    }

    if (you.burden_state > BS_UNENCUMBERED)
        stealth /= you.burden_state;

    if (you.duration[DUR_CONF])
        stealth /= 3;

    const int arm   = you.equip[EQ_BODY_ARMOUR];
    const int cloak = you.equip[EQ_CLOAK];
    const int boots = you.equip[EQ_BOOTS];

    if (arm != -1 && !player_light_armour())
        stealth -= (item_mass( you.inv[arm] ) / 10);

    if (cloak != -1 && get_equip_race(you.inv[cloak]) == ISFLAG_ELVEN)
        stealth += 20;

    if (boots != -1)
    {
        if (get_armour_ego_type( you.inv[boots] ) == SPARM_STEALTH)
            stealth += 50;

        if (get_equip_race(you.inv[boots]) == ISFLAG_ELVEN)
            stealth += 20;
    }

    if ( you.duration[DUR_STEALTH] )
        stealth += 80;

    stealth += scan_randarts( RAP_STEALTH );

    if (player_is_airborne())
        stealth += 10;
    else if (player_in_water())
    {
        // Merfolk can sneak up on monsters underwater -- bwr
        if (you.species == SP_MERFOLK)
            stealth += 50;
        else if ( !player_can_swim() )
            stealth /= 2;       // splashy-splashy
    }
    else if (player_mutation_level(MUT_HOOVES))
        stealth -= 10;  // clippety-clop

    // Radiating silence is the negative complement of shouting all the
    // time... a sudden change from background noise to no noise is going
    // to clue anything in to the fact that something is very wrong...
    // a personal silence spell would naturally be different, but this
    // silence radiates for a distance and prevents monster spellcasting,
    // which pretty much gives away the stealth game.
    if (you.duration[DUR_SILENCE])
        stealth -= 50;

    stealth = std::max(0, stealth);

    return (stealth);
}

static void _attribute_increase()
{
    mpr("Your experience leads to an increase in your attributes!",
        MSGCH_INTRINSIC_GAIN);

    learned_something_new(TUT_CHOOSE_STAT);
    more();
    mesclr();

    mpr("Increase (S)trength, (I)ntelligence, or (D)exterity? ", MSGCH_PROMPT);

    while (true)
    {
        const int keyin = getch();

        switch (keyin)
        {
        case 's':
        case 'S':
            modify_stat(STAT_STRENGTH, 1, false, "level gain");
            return;

        case 'i':
        case 'I':
            modify_stat(STAT_INTELLIGENCE, 1, false, "level gain");
            return;

        case 'd':
        case 'D':
            modify_stat(STAT_DEXTERITY, 1, false, "level gain");
            return;
        }
    }
}

static const char * _get_rotting_how()
{
    ASSERT(you.rotting > 0 || you.species == SP_GHOUL);

    if (you.rotting > 15)
        return (" before your eyes");
    if (you.rotting > 8)
        return (" away quickly");
    if (you.rotting > 4)
        return (" badly");

    if (you.species == SP_GHOUL)
        return (" faster than usual");

    return("");
}

void display_char_status()
{
    if (you.is_undead == US_SEMI_UNDEAD && you.hunger_state == HS_ENGORGED)
        mpr( "You feel almost alive." );
    else if (you.is_undead)
        mpr( "You are undead." );
    else if (you.duration[DUR_DEATHS_DOOR])
        mpr( "You are standing in death's doorway." );
    else
        mpr( "You are alive." );

    if (you.haloed())
    {
        int halo_size = halo_radius();
        if (halo_size > 6)
            mpr( "You are illuminated by a large divine halo." );
        else if (halo_size > 3)
            mpr( "You are illuminated by a divine halo." );
        else
            mpr( "You are illuminated by a small divine halo." );
    }

    if (you.species == SP_VAMPIRE)
    {
        std::string msg = "At your current hunger state you ";
        std::vector<std::string> attrib;

        switch (you.hunger_state)
        {
            case HS_STARVING:
                attrib.push_back("resist poison");
                attrib.push_back("significantly resist cold");
                attrib.push_back("strongly resist negative energy");
                attrib.push_back("resist torment");
                if (you.experience_level >= 13)
                    attrib.push_back("are in touch with the powers of death");
                attrib.push_back("do not heal!");
                break;
            case HS_NEAR_STARVING:
                attrib.push_back("resist poison");
                attrib.push_back("significantly resist cold");
                attrib.push_back("strongly resist negative energy");
                if (you.experience_level >= 13)
                    attrib.push_back("are in touch with the powers of death");
                attrib.push_back("have an extremely slow metabolism");
                attrib.push_back("heal slowly!");
                break;
            case HS_HUNGRY:
            case HS_VERY_HUNGRY:
                attrib.push_back("resist poison");
                attrib.push_back("resist cold");
                attrib.push_back("significantly resist negative energy");
                if (you.experience_level >= 13)
                    attrib.push_back("are in touch with the powers of death");
                if (you.hunger_state == HS_HUNGRY)
                    attrib.push_back("have a slow metabolism");
                else
                    attrib.push_back("have a very slow metabolism");
                attrib.push_back("heal slowly!");
                break;
            case HS_SATIATED:
                attrib.push_back("resist negative energy.");
                break;
            case HS_FULL:
                attrib.push_back("have a fast metabolism");
                attrib.push_back("heal quickly.");
                break;
            case HS_VERY_FULL:
                attrib.push_back("have a very fast metabolism");
                attrib.push_back("heal quickly.");
                break;
            case HS_ENGORGED:
                attrib.push_back("have an extremely fast metabolism");
                attrib.push_back("heal extremely quickly.");
                break;
        }

        if (!attrib.empty())
        {
            msg += comma_separated_line(attrib.begin(), attrib.end(),
                                        ", and ", ", ");
            mpr(msg.c_str());
        }
    }

    switch (you.attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_SPIDER:
        mpr( "You are in spider-form." );
        break;
    case TRAN_BAT:
        mprf( "You are in %sbat-form.",
              you.species == SP_VAMPIRE ? "vampire " : "" );
        break;
    case TRAN_BLADE_HANDS:
        mpr( "You have blades for hands." );
        break;
    case TRAN_STATUE:
        mpr( "You are a statue." );
        break;
    case TRAN_ICE_BEAST:
        mpr( "You are an ice creature." );
        break;
    case TRAN_DRAGON:
        mpr( "You are in dragon-form." );
        break;
    case TRAN_LICH:
        mpr( "You are in lich-form." );
        break;
    case TRAN_SERPENT_OF_HELL:
        mpr( "You are a huge demonic serpent." );
        break;
    case TRAN_AIR:
        mpr( "You are a cloud of diffuse gas." );
        break;
    }

    if (you.burden_state == BS_ENCUMBERED)
        mpr("You are burdened.");
    else if (you.burden_state == BS_OVERLOADED)
        mpr("You are overloaded with stuff.");

    if (you.duration[DUR_SAGE])
        mprf("You are studying %s.", skill_name(you.sage_bonus_skill));

    if (you.duration[DUR_BREATH_WEAPON])
        mpr( "You are short of breath." );

    if (you.duration[DUR_REPEL_UNDEAD])
        mpr( "You have a holy aura protecting you from undead." );

    if (you.duration[DUR_LIQUID_FLAMES])
        mpr( "You are covered in liquid flames." );

    if (you.duration[DUR_ICY_ARMOUR])
        mpr( "You are protected by an icy shield." );

    if (you.duration[DUR_REPEL_MISSILES])
        mpr( "You are protected from missiles." );

    if (you.duration[DUR_DEFLECT_MISSILES])
        mpr( "You deflect missiles." );

    if (you.duration[DUR_PRAYER])
        mpr( "You are praying." );

    if (you.disease && !you.duration[DUR_REGENERATION]
        && (you.species != SP_VAMPIRE || you.hunger_state != HS_STARVING))
    {
        mpr("You do not heal.");
    }

    if (you.duration[DUR_REGENERATION]
        && (you.species != SP_VAMPIRE || you.hunger_state != HS_STARVING))
    {
        if (you.disease)
            mpr("You are recuperating from your illness.");
        else
            mpr("You are regenerating.");
    }

    if (you.duration[DUR_SWIFTNESS])
        mpr( "You can move swiftly." );

    if (you.duration[DUR_INSULATION])
        mpr( "You are insulated." );

    if (you.duration[DUR_STONEMAIL])
        mpr( "You are covered in scales of stone." );

    if (you.duration[DUR_CONTROLLED_FLIGHT])
        mpr( "You can control your flight." );

    if (you.duration[DUR_TELEPORT])
        mpr( "You are about to teleport." );

    if (you.duration[DUR_CONTROL_TELEPORT])
        mpr( "You can control teleportation." );

    if (you.duration[DUR_DEATH_CHANNEL])
        mpr( "You are channeling the dead." );

    if (you.duration[DUR_FORESCRY])     //jmf: added 19mar2000
        mpr( "You are forewarned." );

    if (you.duration[DUR_SILENCE])      //jmf: added 27mar2000
        mpr( "You radiate silence." );

    if (you.duration[DUR_STONESKIN])
        mpr( "Your skin is tough as stone." );

    if (you.duration[DUR_SEE_INVISIBLE])
        mpr( "You can see invisible." );

    if (you.duration[DUR_INVIS])
        mpr( "You are invisible." );

    if (you.duration[DUR_CONF])
        mpr( "You are confused." );

    if (you.duration[DUR_BEHELD])
        mpr( "You are beheld." );

    // How exactly did you get to show the status?
    if (you.duration[DUR_PARALYSIS])
        mpr( "You are paralysed." );
    if (you.duration[DUR_PETRIFIED])
        mpr( "You are petrified." );
    if (you.duration[DUR_SLEEP])
        mpr( "You are asleep." );

    if (you.duration[DUR_EXHAUSTED])
        mpr( "You are exhausted." );

    if (you.duration[DUR_SLOW] && you.duration[DUR_HASTE])
        mpr( "You are under both slowing and hasting effects." );
    else if (you.duration[DUR_SLOW])
        mpr( "Your actions are slowed." );
    else if (you.duration[DUR_HASTE])
        mpr( "Your actions are hasted." );

    if (you.duration[DUR_MIGHT])
        mpr( "You are mighty." );

    if (you.duration[DUR_DIVINE_ROBUSTNESS])
        mpr( "You are divinely robust." );

    if (you.duration[DUR_DIVINE_STAMINA])
        mpr( "You are divinely fortified." );

    if (you.duration[DUR_BERSERKER])
        mpr( "You are possessed by a berserker rage." );

    if (player_is_airborne())
        mpr( "You are hovering above the floor." );

    if (you.attribute[ATTR_HELD])
        mpr( "You are held in a net." );

    if (you.duration[DUR_POISONING])
    {
        mprf("You are %s poisoned.",
             (you.duration[DUR_POISONING] > 10) ? "extremely" :
             (you.duration[DUR_POISONING] > 5)  ? "very" :
             (you.duration[DUR_POISONING] > 3)  ? "quite"
                                  : "mildly" );
    }

    if (you.disease)
    {
        mprf("You are %sdiseased.",
             (you.disease > 120) ? "badly " :
             (you.disease >  40) ? ""
                                 : "mildly " );
    }

    if (you.rotting || you.species == SP_GHOUL)
        mprf("Your flesh is rotting%s.", _get_rotting_how());

    // Prints a contamination message.
    contaminate_player( 0, false, true );

    if (you.duration[DUR_CONFUSING_TOUCH])
    {
        mprf("Your hands are glowing %s red.",
             (you.duration[DUR_CONFUSING_TOUCH] > 40) ? "an extremely bright" :
             (you.duration[DUR_CONFUSING_TOUCH] > 20) ? "bright"
                                                      : "a soft" );
    }

    if (you.duration[DUR_SURE_BLADE])
    {
        mprf("You have a %sbond with your blade.",
             (you.duration[DUR_SURE_BLADE] > 15) ? "strong " :
             (you.duration[DUR_SURE_BLADE] >  5) ? ""
                                                 : "weak " );
    }

    int move_cost = (player_speed() * player_movement_speed()) / 10;

    const bool water  = player_in_water();
    const bool swim   = player_is_swimming();

    const bool lev    = player_is_airborne();
    const bool fly    = (you.flight_mode() == FL_FLY);
    const bool swift  = (you.duration[DUR_SWIFTNESS] > 0);

    mprf( "Your %s speed is %s%s%s.",
          // order is important for these:
          (swim)    ? "swimming" :
          (water)   ? "wading" :
          (fly)     ? "flying" :
          (lev)     ? "levitating"
                    : "movement",

          (water && !swim)  ? "uncertain and " :
          (!water && swift) ? "aided by the wind" : "",

          (!water && swift) ? ((move_cost >= 10) ? ", but still "
                                                 : " and ")
                            : "",

          (move_cost <   8) ? "very quick" :
          (move_cost <  10) ? "quick" :
          (move_cost == 10) ? "average" :
          (move_cost <  13) ? "slow"
                            : "very slow" );

#if DEBUG_DIAGNOSTICS
    const int to_hit = calc_your_to_hit( false ) * 2;
    mprf("To-hit: %d", to_hit);
#endif
/*
    // Messages based largely on percentage chance of missing the
    // average EV 10 humanoid, and very agile EV 30 (pretty much
    // max EV for monsters currently).
    //
    // "awkward"    - need lucky hit (less than EV)
    // "difficult"  - worse than 2 in 3
    // "hard"       - worse than fair chance
    mprf("%s given your current equipment.",
         (to_hit <   1) ? "You are completely incapable of fighting" :
         (to_hit <   5) ? "Hitting even clumsy monsters is extremely awkward" :
         (to_hit <  10) ? "Hitting average monsters is awkward" :
         (to_hit <  15) ? "Hitting average monsters is difficult" :
         (to_hit <  20) ? "Hitting average monsters is hard" :
         (to_hit <  30) ? "Very agile monsters are a bit awkward to hit" :
         (to_hit <  45) ? "Very agile monsters are a bit difficult to hit" :
         (to_hit <  60) ? "Very agile monsters are a bit hard to hit" :
         (to_hit < 100) ? "You feel comfortable with your ability to fight"
                        : "You feel confident with your ability to fight" );
*/

    // magic resistance
    const int mr = player_res_magic();
    mprf("You are %s resistant to magic.",
         (mr <  10) ? "not" :
         (mr <  30) ? "slightly" :
         (mr <  60) ? "somewhat" :
         (mr <  90) ? "quite" :
         (mr < 120) ? "very" :
         (mr < 140) ? "extremely"
                    : "incredibly");

    // character evaluates their ability to sneak around:
    const int ustealth = check_stealth();

    mprf("You feel %sstealthy.",
         (ustealth <  10) ? "extremely un" :
         (ustealth <  30) ? "very un" :
         (ustealth <  60) ? "un" :
         (ustealth <  90) ? "fairly " :
         (ustealth < 120) ? "" :
         (ustealth < 160) ? "quite " :
         (ustealth < 220) ? "very " :
         (ustealth < 300) ? "extremely " :
         (ustealth < 400) ? "extraordinarily " :
         (ustealth < 520) ? "incredibly "
                          : "uncannily ");

#if DEBUG_DIAGNOSTICS
    mprf("stealth: %d", ustealth);
#endif
}                               // end display_char_status()

// Does a case-sensitive lookup of the species name supplied.
int str_to_species(const std::string &species)
{
    if (species.empty())
        return SP_HUMAN;

    // first look for full name (e.g. Green Draconian)
    for (int i = SP_HUMAN; i < NUM_SPECIES; ++i)
    {
        if (species == species_name(static_cast<species_type>(i), 10))
            return (i);
    }

    // nothing found, try again with plain name
    for (int i = SP_HUMAN; i < NUM_SPECIES; ++i)
    {
        if (species == species_name(static_cast<species_type>(i), 1))
            return (i);
    }

    return (SP_HUMAN);
}

std::string species_name(species_type speci, int level, bool genus, bool adj,
                         bool ogre)
// defaults:             false                          false       false
{
    std::string res;

    // If 'ogre' is true, the Ogre-Mage is described as like the Ogre.
    if (ogre && speci == SP_OGRE_MAGE)
        speci = SP_OGRE;

    if (player_genus( GENPC_DRACONIAN, speci ))
    {
        if (adj || genus)  // adj doesn't care about exact species
            res = "Draconian";
        else
        {
            if (level < 7)
                res = "Draconian";
            else
            {
                switch (speci)
                {
                case SP_RED_DRACONIAN:     res = "Red Draconian";     break;
                case SP_WHITE_DRACONIAN:   res = "White Draconian";   break;
                case SP_GREEN_DRACONIAN:   res = "Green Draconian";   break;
                case SP_YELLOW_DRACONIAN:  res = "Yellow Draconian";  break;
                case SP_GREY_DRACONIAN:    res = "Grey Draconian";    break;
                case SP_BLACK_DRACONIAN:   res = "Black Draconian";   break;
                case SP_PURPLE_DRACONIAN:  res = "Purple Draconian";  break;
                case SP_MOTTLED_DRACONIAN: res = "Mottled Draconian"; break;
                case SP_PALE_DRACONIAN:    res = "Pale Draconian";    break;

                case SP_BASE_DRACONIAN:
                default:
                    res = "Draconian";
                    break;
                }
            }
        }
    }
    else if (player_genus( GENPC_ELVEN, speci ))
    {
        if (adj)  // doesn't care about species/genus
            res = "Elven";
        else if (genus)
            res = "Elf";
        else
        {
            switch (speci)
            {
            case SP_HIGH_ELF:   res = "High Elf";   break;
            case SP_GREY_ELF:   res = "Grey Elf";   break;
            case SP_DEEP_ELF:   res = "Deep Elf";   break;
            case SP_SLUDGE_ELF: res = "Sludge Elf"; break;
            default:            res = "Elf";        break;
            }
        }
    }
    else if (player_genus( GENPC_DWARVEN, speci ))
    {
        if (adj)  // doesn't care about species/genus
            res = "Dwarven";
        else if (genus)
            res = "Dwarf";
        else
        {
            switch (speci)
            {
            case SP_MOUNTAIN_DWARF: res = "Mountain Dwarf"; break;
            default:                res = "Dwarf";          break;
            }
        }
    }
    else
    {
        switch (speci)
        {
        case SP_HUMAN:      res = "Human";                           break;
        case SP_HALFLING:   res = "Halfling";                        break;
        case SP_KOBOLD:     res = "Kobold";                          break;
        case SP_MUMMY:      res = "Mummy";                           break;
        case SP_NAGA:       res = "Naga";                            break;
        // We've previously declared that these are radically
        // different from Ogres... so we're not going to
        // refer to them as Ogres.  -- bwr
        case SP_OGRE_MAGE:  res = "Ogre-Mage";                       break;
        case SP_CENTAUR:    res = "Centaur";                         break;
        case SP_SPRIGGAN:   res = "Spriggan";                        break;
        case SP_MINOTAUR:   res = "Minotaur";                        break;
        case SP_KENKU:      res = "Kenku";                           break;
        case SP_VAMPIRE:    res = "Vampire";                         break;

        case SP_HILL_ORC:
            res = (adj ? "Orcish" : genus ? "Orc" : "Hill Orc");
            break;

        case SP_GNOME:      res = (adj ? "Gnomish"    : "Gnome");       break;
        case SP_OGRE:       res = (adj ? "Ogreish"    : "Ogre");        break;
        case SP_TROLL:      res = (adj ? "Trollish"   : "Troll");       break;
        case SP_DEMIGOD:    res = (adj ? "Divine"     : "Demigod");     break;
        case SP_DEMONSPAWN: res = (adj ? "Demonic"    : "Demonspawn" ); break;
        case SP_GHOUL:      res = (adj ? "Ghoulish"   : "Ghoul");       break;
        case SP_MERFOLK:    res = (adj ? "Merfolkian" : "Merfolk");     break;
        default:            res = (adj ? "Yakish"     : "Yak");         break;
        }
    }
    return res;
}

bool player_res_corrosion(bool calc_unid)
{
    return (player_equip(EQ_AMULET, AMU_RESIST_CORROSION, calc_unid)
            || player_equip_ego_type(EQ_CLOAK, SPARM_PRESERVATION));
}

bool player_item_conserve(bool calc_unid)
{
    return (player_equip(EQ_AMULET, AMU_CONSERVATION, calc_unid)
            || player_equip_ego_type(EQ_CLOAK, SPARM_PRESERVATION));
}

int player_mental_clarity(bool calc_unid, bool items)
{
    const int ret = (3 * player_equip(EQ_AMULET, AMU_CLARITY, calc_unid)
                       * items)
                        + player_mutation_level(MUT_CLARITY);

    return ((ret > 3) ? 3 : ret);
}

bool wearing_amulet(char amulet, bool calc_unid)
{
    if (amulet == AMU_CONTROLLED_FLIGHT
        && (you.duration[DUR_CONTROLLED_FLIGHT]
            || player_genus(GENPC_DRACONIAN)
            || you.attribute[ATTR_TRANSFORMATION] == TRAN_DRAGON
            || you.attribute[ATTR_TRANSFORMATION] == TRAN_BAT))
    {
        return (true);
    }

    if (amulet == AMU_CLARITY && player_mutation_level(MUT_CLARITY))
        return (true);

    if (amulet == AMU_RESIST_CORROSION || amulet == AMU_CONSERVATION)
    {
        // XXX: this is hackish {dlb}
        if (player_equip_ego_type( EQ_CLOAK, SPARM_PRESERVATION ))
            return (true);
    }

    if (you.equip[EQ_AMULET] == -1)
        return (false);

    if (you.inv[you.equip[EQ_AMULET]].sub_type == amulet
        && ( calc_unid || item_type_known(you.inv[you.equip[EQ_AMULET]]) ))
    {
        return (true);
    }

    return (false);
}

bool player_is_airborne(void)
{
    return you.airborne();
}

bool player_has_spell( spell_type spell )
{
    return you.has_spell(spell);
}

static int _species_exp_mod(species_type species)
{
    if (player_genus(GENPC_DRACONIAN, species))
        return 14;
    else if (player_genus(GENPC_DWARVEN, species))
        return 13;
    switch (species)
    {
        case SP_HUMAN:
        case SP_HALFLING:
        case SP_HILL_ORC:
        case SP_KOBOLD:
            return 10;
        case SP_GNOME:
            return 11;
        case SP_SLUDGE_ELF:
        case SP_NAGA:
        case SP_GHOUL:
        case SP_MERFOLK:
            return 12;
        case SP_SPRIGGAN:
        case SP_KENKU:
            return 13;
        case SP_GREY_ELF:
        case SP_DEEP_ELF:
        case SP_OGRE:
        case SP_CENTAUR:
        case SP_MINOTAUR:
        case SP_DEMONSPAWN:
            return 14;
        case SP_HIGH_ELF:
        case SP_MUMMY:
        case SP_VAMPIRE:
        case SP_TROLL:
        case SP_OGRE_MAGE:
            return 15;
        case SP_DEMIGOD:
            return 16;
        default:
            return 0;
    }
}

unsigned long exp_needed(int lev)
{
    lev--;

    unsigned long level = 0;

    // Basic plan:
    // Section 1: levels  1- 5, second derivative goes 10-10-20-30.
    // Section 2: levels  6-13, second derivative is exponential/doubling.
    // Section 3: levels 14-27, second derivative is constant at 6000.
    //
    // Section three is constant so we end up with high levels at about
    // their old values (level 27 at 850k), without delta2 ever decreasing.
    // The values that are considerably different (ie level 13 is now 29000,
    // down from 41040 are because the second derivative goes from 9040 to
    // 1430 at that point in the original, and then slowly builds back
    // up again).  This function smoothes out the old level 10-15 area
    // considerably.

    // Here's a table:
    //
    // level      xp      delta   delta2
    // =====   =======    =====   ======
    //   1           0        0       0
    //   2          10       10      10
    //   3          30       20      10
    //   4          70       40      20
    //   5         140       70      30
    //   6         270      130      60
    //   7         520      250     120
    //   8        1010      490     240
    //   9        1980      970     480
    //  10        3910     1930     960
    //  11        7760     3850    1920
    //  12       15450     7690    3840
    //  13       29000    13550    5860
    //  14       48500    19500    5950
    //  15       74000    25500    6000
    //  16      105500    31500    6000
    //  17      143000    37500    6000
    //  18      186500    43500    6000
    //  19      236000    49500    6000
    //  20      291500    55500    6000
    //  21      353000    61500    6000
    //  22      420500    67500    6000
    //  23      494000    73500    6000
    //  24      573500    79500    6000
    //  25      659000    85500    6000
    //  26      750500    91500    6000
    //  27      848000    97500    6000


    switch (lev)
    {
    case 1:
        level = 1;
        break;
    case 2:
        level = 10;
        break;
    case 3:
        level = 30;
        break;
    case 4:
        level = 70;
        break;

    default:
        if (lev < 13)
        {
            lev -= 4;
            level = 10 + 10 * lev + (60 << lev);
        }
        else
        {
            lev -= 12;
            level = 15500 + 10500 * lev + 3000 * lev * lev;
        }
        break;
    }

    return ((level - 1) * _species_exp_mod(you.species) / 10);
}                               // end exp_needed()

// returns bonuses from rings of slaying, etc.
int slaying_bonus(char which_affected)
{
    int ret = 0;

    if (which_affected == PWPN_HIT)
    {
        ret += player_equip( EQ_RINGS_PLUS, RING_SLAYING );
        ret += scan_randarts(RAP_ACCURACY);
    }
    else if (which_affected == PWPN_DAMAGE)
    {
        ret += player_equip( EQ_RINGS_PLUS2, RING_SLAYING );
        ret += scan_randarts(RAP_DAMAGE);
    }

    ret += std::min(you.duration[DUR_SLAYING] / 13, 6);

    return (ret);
}                               // end slaying_bonus()

// Checks each equip slot for an evokable item (jewellery or randart).
// Returns true if any of these has the same ability as the one handed in.
bool items_give_ability(const int slot, randart_prop_type abil)
{
    for (int i = EQ_WEAPON; i < NUM_EQUIP; i++)
    {
        const int eq = you.equip[i];

        if (eq == -1)
            continue;

        // skip item to compare with
        if (eq == slot)
            continue;

        // only weapons give their effects when in our hands
        if (i == EQ_WEAPON && you.inv[ eq ].base_type != OBJ_WEAPONS)
            continue;

        if (eq == EQ_LEFT_RING || eq == EQ_RIGHT_RING)
        {
            if (abil == RAP_LEVITATE && you.inv[eq].sub_type == RING_LEVITATION)
                return (true);
            if (abil == RAP_CAN_TELEPORT && you.inv[eq].sub_type == RING_TELEPORTATION)
                return (true);
            if (abil == RAP_INVISIBLE && you.inv[eq].sub_type == RING_INVISIBILITY)
                return (true);
        }
        else if (eq == EQ_AMULET)
        {
            if (abil == RAP_BERSERK && you.inv[eq].sub_type == AMU_RAGE)
                return (true);
        }

        // other items are not evokable
        if (!is_random_artefact( you.inv[ eq ] ))
            continue;

        if (randart_wpn_property(you.inv[ eq ], abil))
            return (true);
    }

    // none of the equipped items possesses this ability
    return (false);
}                               // end items_give_ability()

/* Checks each equip slot for a randart, and adds up all of those with
   a given property. Slow if any randarts are worn, so avoid where possible. */
int scan_randarts(randart_prop_type which_property, bool calc_unid)
{
    int i = 0;
    int retval = 0;

    for (i = EQ_WEAPON; i < NUM_EQUIP; i++)
    {
        const int eq = you.equip[i];

        if (eq == -1)
            continue;

        // Only weapons give their effects when in our hands.
        if (i == EQ_WEAPON && you.inv[ eq ].base_type != OBJ_WEAPONS)
            continue;

        if (!is_random_artefact( you.inv[ eq ] ))
            continue;

        // Ignore unidentified items [TileCrawl dump enhancements].
        if (!item_ident(you.inv[ eq ], ISFLAG_KNOW_PROPERTIES)
            && !calc_unid)
        {
            continue;
        }

        retval += randart_wpn_property( you.inv[ eq ], which_property );
    }

    return (retval);
}

void modify_stat(stat_type which_stat, char amount, bool suppress_msg,
                 const char *cause, bool see_source)
{
    char *ptr_stat = NULL;
    char *ptr_stat_max = NULL;
    bool *ptr_redraw = NULL;

    kill_method_type kill_type = NUM_KILLBY;

    // sanity - is non-zero amount?
    if (amount == 0)
        return;

    // Stop delays if a stat drops.
    if (amount < 0)
        interrupt_activity( AI_STAT_CHANGE );

    std::string msg = "You feel ";

    if (which_stat == STAT_RANDOM)
        which_stat = static_cast<stat_type>(random2(NUM_STATS));

    switch (which_stat)
    {
    case STAT_STRENGTH:
        ptr_stat     = &you.strength;
        ptr_stat_max = &you.max_strength;
        ptr_redraw   = &you.redraw_strength;
        kill_type    = KILLED_BY_WEAKNESS;
        msg += ((amount > 0) ? "stronger." : "weaker.");
        break;

    case STAT_INTELLIGENCE:
        ptr_stat     = &you.intel;
        ptr_stat_max = &you.max_intel;
        ptr_redraw   = &you.redraw_intelligence;
        kill_type    = KILLED_BY_STUPIDITY;
        msg += ((amount > 0) ? "clever." : "stupid.");
        break;

    case STAT_DEXTERITY:
        ptr_stat     = &you.dex;
        ptr_stat_max = &you.max_dex;
        ptr_redraw   = &you.redraw_dexterity;
        kill_type    = KILLED_BY_CLUMSINESS;
        msg += ((amount > 0) ? "agile." : "clumsy.");
        break;

    default:
        break;
    }

    if (!suppress_msg && amount != 0)
        mpr( msg.c_str(), (amount > 0) ? MSGCH_INTRINSIC_GAIN : MSGCH_WARN );

    *ptr_stat     += amount;
    *ptr_stat_max += amount;
    *ptr_redraw    = 1;

    if (amount < 0 && *ptr_stat < 1)
    {
        if (cause == NULL)
            ouch(INSTANT_DEATH, 0, kill_type);
        else
            ouch(INSTANT_DEATH, 0, kill_type, cause, see_source);
    }

    if (ptr_stat == &you.strength)
        burden_change();

    return;
}                               // end modify_stat()

void modify_stat(stat_type which_stat, char amount, bool suppress_msg,
                 const std::string& cause, bool see_source)
{
    modify_stat(which_stat, amount, suppress_msg, cause.c_str(), see_source);
}

void modify_stat(stat_type which_stat, char amount, bool suppress_msg,
                 const monsters* cause)
{
    if (cause == NULL || invalid_monster(cause))
    {
        modify_stat(which_stat, amount, suppress_msg, NULL, true);
        return;
    }

    bool        vis  = mons_near(cause) && player_monster_visible(cause);
    std::string name = cause->name(DESC_NOCAP_A, true);

    if (cause->has_ench(ENCH_SHAPESHIFTER))
        name += " (shapeshifter)";
    else if (cause->has_ench(ENCH_GLOWING_SHAPESHIFTER))
        name += " (glowing shapeshifter)";

    modify_stat(which_stat, amount, suppress_msg, name, vis);
}

void modify_stat(stat_type which_stat, char amount, bool suppress_msg,
                 const item_def &cause, bool removed)
{
    std::string name = cause.name(DESC_NOCAP_THE, false, true, false, false,
                                  ISFLAG_KNOW_CURSE | ISFLAG_KNOW_PLUSES);
    std::string verb;

    switch(cause.base_type)
    {
    case OBJ_ARMOUR:
    case OBJ_JEWELLERY:
        if (removed)
            verb = "removing";
        else
            verb = "wearing";
        break;

    case OBJ_WEAPONS:
    case OBJ_STAVES:
        if (removed)
            verb = "unwielding";
        else
            verb = "wielding";
        break;

    case OBJ_WANDS:   verb = "zapping";  break;
    case OBJ_FOOD:    verb = "eating";   break;
    case OBJ_SCROLLS: verb = "reading";  break;
    case OBJ_POTIONS: verb = "drinking"; break;
    default:          verb = "using";
    }

    modify_stat(which_stat, amount, suppress_msg,
                verb + " " + name, true);
}

void dec_hp(int hp_loss, bool fatal, const char *aux)
{
    if (!fatal && you.hp < 1)
        you.hp = 1;

    if (!fatal && hp_loss >= you.hp)
        hp_loss = you.hp - 1;

    if (hp_loss < 1)
        return;

    // If it's not fatal, use ouch() so that notes can be taken. If it IS
    // fatal, somebody else is doing the bookkeeping, and we don't want to mess
    // with that.
    if (!fatal && aux)
        ouch(hp_loss, -1, KILLED_BY_SOMETHING, aux);
    else
        you.hp -= hp_loss;

    you.redraw_hit_points = true;
}

void dec_mp(int mp_loss)
{
    if (mp_loss < 1)
        return;

    you.magic_points -= mp_loss;

    if (you.magic_points < 0)
        you.magic_points = 0;

    if (Options.magic_point_warning
        && you.magic_points < (you.max_magic_points
                               * Options.magic_point_warning) / 100)
    {
        mpr( "* * * LOW MAGIC WARNING * * *", MSGCH_DANGER );
    }

    take_note(Note(NOTE_MP_CHANGE, you.magic_points, you.max_magic_points));
    you.redraw_magic_points = true;
}

bool enough_hp(int minimum, bool suppress_msg)
{
    // We want to at least keep 1 HP. -- bwr
    if (you.hp < minimum + 1)
    {
        if (!suppress_msg)
            mpr("You haven't enough vitality at the moment.");

        crawl_state.cancel_cmd_again();
        crawl_state.cancel_cmd_repeat();
        return (false);
    }

    return (true);
}

bool enough_mp(int minimum, bool suppress_msg)
{
    if (you.magic_points < minimum)
    {
        if (!suppress_msg)
            mpr("You haven't enough magic at the moment.");

        crawl_state.cancel_cmd_again();
        crawl_state.cancel_cmd_repeat();
        return (false);
    }

    return (true);
}

// Note that "max_too" refers to the base potential, the actual
// resulting max value is subject to penalties, bonuses, and scalings.
void inc_mp(int mp_gain, bool max_too)
{
    if (mp_gain < 1)
        return;

    bool wasnt_max = (you.magic_points < you.max_magic_points);

    you.magic_points += mp_gain;

    if (max_too)
        inc_max_mp( mp_gain );

    if (you.magic_points > you.max_magic_points)
        you.magic_points = you.max_magic_points;

    if (wasnt_max && you.magic_points == you.max_magic_points)
        interrupt_activity(AI_FULL_MP);

    take_note(Note(NOTE_MP_CHANGE, you.magic_points, you.max_magic_points));
    you.redraw_magic_points = true;
}

// Note that "max_too" refers to the base potential, the actual
// resulting max value is subject to penalties, bonuses, and scalings.
// To avoid message spam, don't take notes when HP increases.
void inc_hp(int hp_gain, bool max_too)
{
    if (hp_gain < 1)
        return;

    bool wasnt_max = (you.hp < you.hp_max);

    you.hp += hp_gain;

    if (max_too)
        inc_max_hp( hp_gain );

    if (you.hp > you.hp_max)
        you.hp = you.hp_max;

    if (wasnt_max && you.hp == you.hp_max)
        interrupt_activity(AI_FULL_HP);

    you.redraw_hit_points = true;
}

void rot_hp( int hp_loss )
{
    you.base_hp -= hp_loss;
    calc_hp();

    you.redraw_hit_points = true;
}

void unrot_hp( int hp_recovered )
{
    if (hp_recovered >= 5000 - you.base_hp)
        you.base_hp = 5000;
    else
        you.base_hp += hp_recovered;

    calc_hp();

    you.redraw_hit_points = true;
}

int player_rotted( void )
{
    return (5000 - you.base_hp);
}

void rot_mp( int mp_loss )
{
    you.base_magic_points -= mp_loss;
    calc_mp();

    you.redraw_magic_points = true;
}

void inc_max_hp( int hp_gain )
{
    you.base_hp2 += hp_gain;
    calc_hp();

    take_note(Note(NOTE_MAXHP_CHANGE, you.hp_max));
    you.redraw_hit_points = true;
}

void dec_max_hp( int hp_loss )
{
    you.base_hp2 -= hp_loss;
    calc_hp();

    take_note(Note(NOTE_MAXHP_CHANGE, you.hp_max));
    you.redraw_hit_points = true;
}

void inc_max_mp( int mp_gain )
{
    you.base_magic_points2 += mp_gain;
    calc_mp();

    take_note(Note(NOTE_MAXMP_CHANGE, you.max_magic_points));
    you.redraw_magic_points = true;
}

void dec_max_mp( int mp_loss )
{
    you.base_magic_points2 -= mp_loss;
    calc_mp();

    take_note(Note(NOTE_MAXMP_CHANGE, you.max_magic_points));
    you.redraw_magic_points = true;
}

// Use of floor: false = hp max, true = hp min. {dlb}
void deflate_hp(int new_level, bool floor)
{
    if (floor && you.hp < new_level)
        you.hp = new_level;
    else if (!floor && you.hp > new_level)
        you.hp = new_level;

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_hit_points = true;
}

// Note that "max_too" refers to the base potential, the actual
// resulting max value is subject to penalties, bonuses, and scalings.
void set_hp(int new_amount, bool max_too)
{
    if (you.hp != new_amount)
        you.hp = new_amount;

    if (max_too && you.hp_max != new_amount)
    {
        you.base_hp2 = 5000 + new_amount;
        calc_hp();
    }

    if (you.hp > you.hp_max)
        you.hp = you.hp_max;

    if (max_too)
        take_note(Note(NOTE_MAXHP_CHANGE, you.hp_max));

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_hit_points = true;
}

// Note that "max_too" refers to the base potential, the actual
// resulting max value is subject to penalties, bonuses, and scalings.
void set_mp(int new_amount, bool max_too)
{
    if (you.magic_points != new_amount)
        you.magic_points = new_amount;

    if (max_too && you.max_magic_points != new_amount)
    {
        // Note that this gets scaled down for values > 18.
        you.base_magic_points2 = 5000 + new_amount;
        calc_mp();
    }

    if (you.magic_points > you.max_magic_points)
        you.magic_points = you.max_magic_points;

    take_note(Note(NOTE_MP_CHANGE, you.magic_points, you.max_magic_points));
    if (max_too)
        take_note(Note(NOTE_MAXMP_CHANGE, you.max_magic_points));

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_magic_points = true;
}

// If trans is true, being berserk and/or transformed is taken into account
// here. Else, the base hp is calculated. If rotted is true, calculate the
// real max hp you'd have if the rotting was cured.
int get_real_hp(bool trans, bool rotted)
{
    int hitp;

    hitp  = (you.base_hp - 5000) + (you.base_hp2 - 5000);
    hitp += (you.experience_level * you.skills[SK_FIGHTING]) / 5;

    // Being berserk makes you resistant to damage. I don't know why.
    if (trans && you.duration[DUR_BERSERKER])
    {
        hitp *= 15;
        hitp /= 10;
    }

    if (trans)
    {
        // Some transformations give you extra hp.
        switch (you.attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_STATUE:
            hitp *= 15;
            hitp /= 10;
            break;
        case TRAN_ICE_BEAST:
            hitp *= 12;
            hitp /= 10;
            break;
        case TRAN_DRAGON:
            hitp *= 16;
            hitp /= 10;
            break;
        }
    }

    if (rotted)
        hitp += player_rotted();

    // Frail and robust mutations, and divine robustness.
    hitp *= (10 + player_mutation_level(MUT_ROBUST)
                + you.attribute[ATTR_DIVINE_ROBUSTNESS]
                - player_mutation_level(MUT_FRAIL));
    hitp /= 10;

    return (hitp);
}

static int _get_contamination_level()
{
    const int glow = you.magic_contamination;

    if (glow > 60)
        return (glow / 20 + 2);
    if (glow > 40)
        return 4;
    if (glow > 25)
        return 3;
    if (glow > 15)
        return 2;
    if (glow > 5)
        return 1;

    return 0;
}

// controlled is true if the player actively did something to cause
// contamination (such as drink a known potion of resistance),
// status_only is true only for the status output
void contaminate_player(int change, bool controlled, bool status_only)
{
    // get current contamination level
    int old_level = _get_contamination_level();
    int new_level = 0;
#if DEBUG_DIAGNOSTICS
    if (change > 0 || (change < 0 && you.magic_contamination))
    {
        mprf(MSGCH_DIAGNOSTICS, "change: %d  radiation: %d",
             change, change + you.magic_contamination );
    }
#endif

    // make the change
    if (change + you.magic_contamination < 0)
        you.magic_contamination = 0;
    else
    {
        if (change + you.magic_contamination > 250)
            you.magic_contamination = 250;
        else
            you.magic_contamination += change;
    }

    // figure out new level
    new_level = _get_contamination_level();

    if (status_only || (new_level >= 1 && old_level == 0))
    {
        if (new_level > 0)
        {
            if (new_level > 3)
            {
                mpr( (new_level == 4) ?
                     "Your entire body has taken on an eerie glow!" :
                     "You are engulfed in a nimbus of crackling magics!");
            }
            else
            {
                mprf("You are %s with residual magics%s",
                     (new_level == 3) ? "practically glowing" :
                     (new_level == 2) ? "heavily infused"
                                      : "contaminated",
                     (new_level == 3) ? "!" : ".");
            }
        }
    }
    else if (new_level != old_level)
    {
        mprf((change > 0) ? MSGCH_WARN : MSGCH_RECOVERY,
             "You feel %s contaminated with magical energies.",
             (change > 0) ? "more" : "less" );
    }

    if (new_level >= 1)
        learned_something_new(TUT_GLOWING);

    if (status_only)
        return;

    // Zin doesn't like mutations or mutagenic radiation.
    if (you.religion == GOD_ZIN)
    {
        // Whenever the glow status is first reached, give a warning message.
        if (old_level < 1 && new_level >= 1)
            did_god_conduct(DID_CAUSE_GLOWING, 0, false);
        // If the player actively did something to increase glowing,
        // Zin is displeased.
        else if (controlled && change > 0 && old_level > 0)
            did_god_conduct(DID_CAUSE_GLOWING, 1 + new_level, true);
    }
}

bool poison_player( int amount, bool force )
{
    if (!force && player_res_poison() || amount <= 0)
        return (false);

    const int old_value = you.duration[DUR_POISONING];
    you.duration[DUR_POISONING] += amount;

    if (you.duration[DUR_POISONING] > 40)
        you.duration[DUR_POISONING] = 40;

    if (you.duration[DUR_POISONING] > old_value)
    {
        mprf(MSGCH_WARN, "You are %spoisoned.",
             (old_value > 0) ? "more " : "" );
        learned_something_new(TUT_YOU_POISON);
    }
    return (true);
}

void reduce_poison_player( int amount )
{
    if (you.duration[DUR_POISONING] == 0 || amount <= 0)
        return;

    you.duration[DUR_POISONING] -= amount;

    if (you.duration[DUR_POISONING] <= 0)
    {
        you.duration[DUR_POISONING] = 0;
        mpr( "You feel better.", MSGCH_RECOVERY );
    }
    else
        mpr( "You feel a little better.", MSGCH_RECOVERY );
}

bool confuse_player( int amount, bool resistable )
{
    if (amount <= 0)
        return (false);

    if (resistable && wearing_amulet(AMU_CLARITY))
    {
        mpr( "You feel momentarily confused." );
        return (false);
    }

    const int old_value = you.duration[DUR_CONF];
    you.duration[DUR_CONF] += amount;

    if (you.duration[DUR_CONF] > 40)
        you.duration[DUR_CONF] = 40;

    if (you.duration[DUR_CONF] > old_value)
    {
        you.check_awaken(500);
        mprf(MSGCH_WARN, "You are %sconfused.",
             (old_value > 0) ? "more " : "" );
        learned_something_new(TUT_YOU_ENCHANTED);

        xom_is_stimulated(you.duration[DUR_CONF] - old_value);
    }
    return (true);
}

void reduce_confuse_player( int amount )
{
    if (you.duration[DUR_CONF] == 0 || amount <= 0)
        return;

    you.duration[DUR_CONF] -= amount;

    if (you.duration[DUR_CONF] <= 0)
    {
        you.duration[DUR_CONF] = 0;
        mpr( "You feel less confused." );
    }
}

bool slow_player( int amount )
{
    if (amount <= 0)
        return (false);

    if (wearing_amulet( AMU_RESIST_SLOW ))
    {
        mpr("You feel momentarily lethargic.");

        // Identify amulet.
        item_def amu = you.inv[you.equip[EQ_AMULET]];
        if (!item_type_known(amu))
            set_ident_type( amu, ID_KNOWN_TYPE );

        return (false);
    }
    else if (you.duration[DUR_SLOW] >= 100)
        mpr( "You already are as slow as you could be." );
    else
    {
        if (you.duration[DUR_SLOW] == 0)
            mpr( "You feel yourself slow down." );
        else
            mpr( "You feel as though you will be slow longer." );

        you.duration[DUR_SLOW] += amount;

        if (you.duration[DUR_SLOW] > 100)
            you.duration[DUR_SLOW] = 100;
        learned_something_new(TUT_YOU_ENCHANTED);
    }
    return (true);
}

void dec_slow_player( void )
{
    if (you.duration[DUR_SLOW] > 1)
    {
        // BCR - Amulet of resist slow affects slow counter
        if (wearing_amulet(AMU_RESIST_SLOW))
        {
            you.duration[DUR_SLOW] -= 5;
            if (you.duration[DUR_SLOW] < 1)
                you.duration[DUR_SLOW] = 1;
        }
        else
            you.duration[DUR_SLOW]--;
    }
    else if (you.duration[DUR_SLOW] == 1)
    {
        mpr("You feel yourself speed up.", MSGCH_DURATION);
        you.duration[DUR_SLOW] = 0;
    }
}

void haste_player( int amount )
{
    bool amu_eff = wearing_amulet( AMU_RESIST_SLOW );

    if (amount <= 0)
        return;

    if (amu_eff)
    {
        mpr( "Your amulet glows brightly." );
        item_def *amulet = you.slot_item(EQ_AMULET);
        if (amulet && !item_type_known(*amulet))
            set_ident_type( *amulet, ID_KNOWN_TYPE );
    }

    if (you.duration[DUR_HASTE] == 0)
        mpr( "You feel yourself speed up." );
    else if (you.duration[DUR_HASTE] > 80 + 20 * amu_eff)
        mpr( "You already have as much speed as you can handle." );
    else
    {
        mpr( "You feel as though your hastened speed will last longer." );
        contaminate_player(1, true); // always deliberate
    }

    you.duration[DUR_HASTE] += amount;

    if (you.duration[DUR_HASTE] > 80 + 20 * amu_eff)
        you.duration[DUR_HASTE] = 80 + 20 * amu_eff;

    did_god_conduct( DID_STIMULANTS, 4 + random2(4) );
}

void dec_haste_player( void )
{
    if (you.duration[DUR_HASTE] > 1)
    {
        // BCR - Amulet of resist slow affects haste counter
        if (!wearing_amulet(AMU_RESIST_SLOW) || coinflip())
            you.duration[DUR_HASTE]--;

        if (you.duration[DUR_HASTE] == 6)
        {
            mpr( "Your extra speed is starting to run out.", MSGCH_DURATION );
            if (coinflip())
                you.duration[DUR_HASTE]--;
        }
    }
    else if (you.duration[DUR_HASTE] == 1)
    {
        mpr( "You feel yourself slow down.", MSGCH_DURATION );
        you.duration[DUR_HASTE] = 0;
    }
}

bool disease_player( int amount )
{
    return you.sicken(amount);
}

void dec_disease_player( void )
{
    if (you.disease > 0)
    {
        you.disease--;

        // kobolds and regenerators recuperate quickly
        if (you.disease > 5
            && (you.species == SP_KOBOLD
                || you.duration[ DUR_REGENERATION ]
                || player_mutation_level(MUT_REGENERATION) == 3))
        {
            you.disease -= 2;
        }

        if (!you.disease)
            mpr("You feel your health improve.", MSGCH_RECOVERY);
    }
}

bool rot_player( int amount )
{
    if (amount <= 0)
        return (false);

    if (you.res_rotting() > 0)
    {
        mpr( "You feel terrible." );
        return (false);
    }

    if (you.rotting < 40)
    {
        // Either this, or the actual rotting message should probably
        // be changed so that they're easier to tell apart. -- bwr
        mprf(MSGCH_WARN, "You feel your flesh %s away!",
             (you.rotting) ? "rotting" : "start to rot" );

        you.rotting += amount;

        learned_something_new(TUT_YOU_ROTTING);
    }
    return (true);
}

int count_worn_ego( int which_ego )
{
    int result = 0;
    for (int slot = EQ_CLOAK; slot <= EQ_BODY_ARMOUR; ++slot)
    {
        if (you.equip[slot] != -1
            && get_armour_ego_type(you.inv[you.equip[slot]]) == which_ego)
        {
            result++;
        }
    }

    return (result);
}

static int _strength_modifier()
{
    int result = 0;

    if (you.duration[DUR_MIGHT])
        result += 5;

    if (you.duration[DUR_DIVINE_STAMINA])
        result += you.attribute[ATTR_DIVINE_STAMINA];

    // ego items of strength
    result += 3 * count_worn_ego(SPARM_STRENGTH);

    // rings of strength
    result += player_equip( EQ_RINGS_PLUS, RING_STRENGTH );

    // randarts of strength
    result += scan_randarts(RAP_STRENGTH);

    // mutations
    result += player_mutation_level(MUT_STRONG)
              - player_mutation_level(MUT_WEAK);
    result += player_mutation_level(MUT_STRONG_STIFF)
              - player_mutation_level(MUT_FLEXIBLE_WEAK);

    // transformations
    switch ( you.attribute[ATTR_TRANSFORMATION] )
    {
    case TRAN_STATUE:          result +=  2; break;
    case TRAN_DRAGON:          result += 10; break;
    case TRAN_LICH:            result +=  3; break;
    case TRAN_SERPENT_OF_HELL: result += 13; break;
    case TRAN_BAT:             result -=  5; break;
    default:                                 break;
    }

    return (result);
}

static int _int_modifier()
{
    int result = 0;

    if (you.duration[DUR_DIVINE_STAMINA])
        result += you.attribute[ATTR_DIVINE_STAMINA];

    // ego items of intelligence
    result += 3 * count_worn_ego(SPARM_INTELLIGENCE);

    // rings of intelligence
    result += player_equip( EQ_RINGS_PLUS, RING_INTELLIGENCE );

    // randarts of intelligence
    result += scan_randarts(RAP_INTELLIGENCE);

    // mutations
    result += player_mutation_level(MUT_CLEVER)
              - player_mutation_level(MUT_DOPEY);

    return (result);
}

static int _dex_modifier()
{
    int result = 0;

    if (you.duration[DUR_DIVINE_STAMINA])
        result += you.attribute[ATTR_DIVINE_STAMINA];

    // ego items of dexterity
    result += 3 * count_worn_ego(SPARM_DEXTERITY);

    // rings of dexterity
    result += player_equip( EQ_RINGS_PLUS, RING_DEXTERITY );

    // randarts of dexterity
    result += scan_randarts(RAP_DEXTERITY);

    // mutations
    result += player_mutation_level(MUT_AGILE)
              - player_mutation_level(MUT_CLUMSY);
    result += player_mutation_level(MUT_FLEXIBLE_WEAK)
              - player_mutation_level(MUT_STRONG_STIFF);
    result -= player_mutation_level(MUT_BLACK_SCALES);
    result -= player_mutation_level(MUT_BONEY_PLATES);

    const int grey2_modifier[]    = { 0, -1, -1, -2 };
    const int metallic_modifier[] = { 0, -2, -3, -4 };
    const int yellow_modifier[]   = { 0,  0, -1, -2 };
    const int red2_modifier[]     = { 0,  0, -1, -2 };

    result -= grey2_modifier[player_mutation_level(MUT_GREY2_SCALES)];
    result -= metallic_modifier[player_mutation_level(MUT_METALLIC_SCALES)];
    result -= yellow_modifier[player_mutation_level(MUT_YELLOW_SCALES)];
    result -= red2_modifier[player_mutation_level(MUT_RED2_SCALES)];

    // transformations
    switch ( you.attribute[ATTR_TRANSFORMATION] )
    {
    case TRAN_SPIDER: result +=  5; break;
    case TRAN_STATUE: result -=  2; break;
    case TRAN_AIR:    result +=  8; break;
    case TRAN_BAT:    result +=  5; break;
    default:                        break;
    }

    return (result);
}

int stat_modifier( stat_type stat )
{
    switch (stat)
    {
    case STAT_STRENGTH:     return _strength_modifier();
    case STAT_INTELLIGENCE: return _int_modifier();
    case STAT_DEXTERITY:    return _dex_modifier();
    default:
        mprf(MSGCH_ERROR, "Bad stat: %d", stat);
        return 0;
    }
}


//////////////////////////////////////////////////////////////////////////////
// actor

actor::~actor()
{
}

bool actor::has_equipped(equipment_type eq, int sub_type) const
{
    const item_def *item = slot_item(eq);
    return (item && item->sub_type == sub_type);
}

bool actor::will_trigger_shaft() const
{
    return (!airborne() && total_weight() > 0 && is_valid_shaft_level());
}

level_id actor::shaft_dest() const
{
    return generic_shaft_dest(pos());
}

bool actor::airborne() const
{
    return (is_levitating() || (flight_mode() == FL_FLY && !cannot_move()));
}

bool actor::can_pass_through(int x, int y) const
{
    return can_pass_through_feat(grd[x][y]);
}

bool actor::can_pass_through(const coord_def &c) const
{
    return can_pass_through_feat(grd(c));
}

//////////////////////////////////////////////////////////////////////////////
// player

player::player()
    : m_quiver(0)
{
    init();
    kills = new KillMaster();   // why isn't this done in init()?
}

player::player(const player &other)
    : m_quiver(0)
{
    init();

    // why doesn't this do a copy_from?
    player_quiver* saved_quiver = m_quiver;
    *this = other;
    m_quiver = saved_quiver;

    kills = new KillMaster(*(other.kills));
    *m_quiver = *(other.m_quiver);
}

// why is this not called "operator="?
void player::copy_from(const player &other)
{
    if (this == &other)
        return;

    KillMaster *saved_kills = kills;
    player_quiver* saved_quiver = m_quiver;

    *this = other;

    kills  = saved_kills;
    *kills = *(other.kills);
    m_quiver = saved_quiver;
    *m_quiver = *(other.m_quiver);
}


// player struct initialization
void player::init()
{
    birth_time       = time( NULL );
    real_time        = 0;
    num_turns        = 0L;
    last_view_update = 0L;

#ifdef WIZARD
    wizard = (Options.wiz_mode == WIZ_YES) ? true : false;
#else
    wizard = false;
#endif

    your_name[0] = 0;

    banished = false;
    banished_by.clear();

    entering_level = false;
    lava_in_sight  = -1;
    water_in_sight = -1;
    transit_stair  = DNGN_UNSEEN;

    berserk_penalty = 0;
    disease         = 0;
    elapsed_time    = 0;
    rotting         = 0;
    special_wield   = SPWLD_NONE;
    synch_time      = 0;

    magic_contamination = 0;

    base_hp  = 5000;
    base_hp2 = 5000;
    hp_max   = 0;
    base_magic_points  = 5000;
    base_magic_points2 = 5000;
    max_magic_points   = 0;

    magic_points_regeneration = 0;
    strength         = 0;
    max_strength     = 0;
    intel            = 0;
    max_intel        = 0;
    dex              = 0;
    max_dex          = 0;
    experience       = 0;
    experience_level = 1;
    max_level        = 1;
    char_class       = JOB_UNKNOWN;
    species          = SP_UNKNOWN;

    hunger       = 6000;
    hunger_state = HS_SATIATED;

    wield_change            = false;
    redraw_quiver           = false;
    received_weapon_warning = false;

    gold = 0;
    // speed = 10;             // 0.75;  // unused

    burden       = 0;
    burden_state = BS_UNENCUMBERED;

    spell_no = 0;

    your_level      = 0;
    level_type      = LEVEL_DUNGEON;
    entry_cause     = EC_SELF_EXPLICIT;
    entry_cause_god = GOD_NO_GOD;
    where_are_you   = BRANCH_MAIN_DUNGEON;
    char_direction  = GDT_DESCENDING;

    prev_targ  = MHITNOT;
    pet_target = MHITNOT;

    prev_grd_targ = coord_def(0, 0);

    x_pos = 0;
    y_pos = 0;

    prev_move_x = 0;
    prev_move_y = 0;

    running.clear();
    travel_x = 0;
    travel_y = 0;

    religion         = GOD_NO_GOD;
    piety            = 0;
    piety_hysteresis = 0;

    gift_timeout     = 0;

    penance.init(0);
    worshipped.init(0);
    num_gifts.init(0);

    equip.init(-1);

    spells.init(SPELL_NO_SPELL);

    spell_letter_table.init(-1);
    ability_letter_table.init(ABIL_NON_ABILITY);

    mutation.init(0);
    demon_pow.init(0);

    had_book.init(false);

    unique_items.init(UNIQ_NOT_EXISTS);

    for (int i = 0; i < NO_UNRANDARTS; i++)
        set_unrandart_exist(i, false);

    skills.init(0);
    skill_points.init(0);
    skill_order.init(MAX_SKILL_ORDER);
    practise_skill.init(true);

    skill_cost_level   = 1;
    total_skill_points = 0;

    attribute.init(0);
    quiver.init(ENDOFPACK);
    sacrifice_value.init(0);

    for (int i = 0; i < ENDOFPACK; i++)
    {
        inv[i].quantity  = 0;
        inv[i].base_type = OBJ_WEAPONS;
        inv[i].sub_type  = WPN_CLUB;
        inv[i].plus      = 0;
        inv[i].plus2     = 0;
        inv[i].special   = 0;
        inv[i].colour    = 0;
        set_ident_flags( inv[i], ISFLAG_IDENT_MASK );

        inv[i].x = -1;
        inv[i].y = -1;
        inv[i].link = i;
    }

    duration.init(0);

    exp_available = 25;

    global_info.make_global();
    global_info.assert_validity();

    for (int i = 0; i < NUM_BRANCHES; i++)
    {
        branch_info[i].level_type = LEVEL_DUNGEON;
        branch_info[i].branch     = i;
        branch_info[i].assert_validity();
    }

    for (int i = 0; i < (NUM_LEVEL_AREA_TYPES - 1); i++)
    {
        non_branch_info[i].level_type = i + 1;
        non_branch_info[i].branch     = -1;
        non_branch_info[i].assert_validity();
    }

    if (m_quiver)
        delete m_quiver;
    m_quiver = new player_quiver;
}

player_save_info player_save_info::operator=(const player& rhs)
{
    name = rhs.your_name;
    experience = rhs.experience;
    experience_level = rhs.experience_level;
    wizard = rhs.wizard;
    species = rhs.species;
    class_name = rhs.class_name;
    return *this;
}

bool player_save_info::operator<(const player_save_info& rhs) const
{
    return experience < rhs.experience ||
        (experience == rhs.experience && name < rhs.name);
}

std::string player_save_info::short_desc() const
{
    std::ostringstream desc;
    desc << name << ", a level " << experience_level << ' '
         << species_name(species, experience_level) << ' '
         << class_name;

#ifdef WIZARD
    if (wizard)
        desc << " (WIZ)";
#endif

    return desc.str();
}

player::~player()
{
    delete kills;
}

coord_def player::pos() const
{
    return coord_def(x_pos, y_pos);
}

bool player::is_levitating() const
{
    return (duration[DUR_LEVITATION]);
}

bool player::in_water() const
{
    return (!airborne() && !beogh_water_walk()
            && grid_is_water(grd[you.x_pos][you.y_pos]));
}

bool player::can_swim() const
{
    return (species == SP_MERFOLK);
}

bool player::swimming() const
{
    return in_water() && can_swim();
}

bool player::submerged() const
{
    return (false);
}

bool player::has_spell(spell_type spell) const
{
    for (int i = 0; i < 25; i++)
    {
        if (spells[i] == spell)
            return (true);
    }

    return (false);
}

bool player::floundering() const
{
    return in_water() && !can_swim();
}

bool player::can_pass_through_feat(dungeon_feature_type grid) const
{
    return !grid_is_solid(grid);
}

size_type player::body_size(int psize, bool base) const
{
    size_type ret = (base) ? SIZE_CHARACTER
                           : transform_size( psize );

    if (ret == SIZE_CHARACTER)
    {
        // Transformation has size of character's species.
        switch (species)
        {
        case SP_OGRE:
        case SP_OGRE_MAGE:
        case SP_TROLL:
            ret = SIZE_LARGE;
            break;

        case SP_NAGA:
            // Most of their body is on the ground giving them a low profile.
            if (psize == PSIZE_TORSO || psize == PSIZE_PROFILE)
                ret = SIZE_MEDIUM;
            else
                ret = SIZE_LARGE;
            break;

        case SP_CENTAUR:
            ret = (psize == PSIZE_TORSO) ? SIZE_MEDIUM : SIZE_LARGE;
            break;

        case SP_SPRIGGAN:
            ret = SIZE_LITTLE;
            break;

        case SP_HALFLING:
        case SP_GNOME:
        case SP_KOBOLD:
            ret = SIZE_SMALL;
            break;

        default:
            ret = SIZE_MEDIUM;
            break;
        }
    }

    return (ret);
}

int player::body_weight() const
{
    if (attribute[ATTR_TRANSFORMATION] == TRAN_AIR)
        return 0;

    int weight = 0;
    switch(body_size(PSIZE_BODY))
    {
    case SIZE_TINY:
        weight = 150;
        break;
    case SIZE_LITTLE:
        weight = 300;
        break;
    case SIZE_SMALL:
        weight = 425;
        break;
    case SIZE_MEDIUM:
        weight = 550;
        break;
    case SIZE_LARGE:
        weight = 1300;
        break;
    case SIZE_BIG:
        weight = 1500;
        break;
    case SIZE_GIANT:
        weight = 1800;
        break;
    case SIZE_HUGE:
        weight = 2200;
        break;
    default:
        mpr("ERROR: invalid player body weight");
        perror("player::body_weight(): invalid player body weight");
        end(0);
    }

    switch(attribute[ATTR_TRANSFORMATION])
    {
    case TRAN_STATUE:
        weight *= 2;
        break;
    case TRAN_LICH:
        weight /= 2;
        break;
    default:
        break;
    }

    return (weight);
}

int player::total_weight() const
{
    return (body_weight() + burden);
}

bool player::cannot_speak() const
{
    if (silenced(x_pos, y_pos))
        return (true);

    if (you.cannot_move()) // we allow talking during sleep ;)
        return (true);

    // No transform that prevents the player from speaking yet.
    return (false);
}

std::string player::shout_verb() const
{
    const int transform = attribute[ATTR_TRANSFORMATION];
    switch (transform)
    {
    case TRAN_DRAGON:
    case TRAN_SERPENT_OF_HELL:
        return "roar";
    case TRAN_SPIDER:
        return "hiss";
    case TRAN_BAT:
        return "squeak";
    case TRAN_AIR:
        return "__NONE";
    default: // depends on SCREAM mutation
        int level = player_mutation_level(MUT_SCREAM);
        if ( level <= 1)
            return "shout";
        else if (level == 2)
            return "yell";
        else // level == 3
            return "scream";
    }
}

int player::damage_type(int)
{
    const int wpn = equip[ EQ_WEAPON ];

    if (wpn != -1)
    {
        return (get_vorpal_type(inv[wpn]));
    }
    else if (equip[EQ_GLOVES] == -1
             && attribute[ATTR_TRANSFORMATION] == TRAN_BLADE_HANDS)
    {
        return (DVORP_SLICING);
    }
    else if (has_usable_claws())
    {
        return (DVORP_CLAWING);
    }

    return (DVORP_CRUSHING);
}

int player::damage_brand(int)
{
    int ret = SPWPN_NORMAL;
    const int wpn = equip[ EQ_WEAPON ];

    if (wpn != -1)
    {
        if ( !is_range_weapon(inv[wpn]) )
            ret = get_weapon_brand( inv[wpn] );
    }
    else if (duration[DUR_CONFUSING_TOUCH])
        ret = SPWPN_CONFUSE;
    else if (mutation[MUT_DRAIN_LIFE])
        ret = SPWPN_DRAINING;
    else
    {
        switch (attribute[ATTR_TRANSFORMATION])
        {
        case TRAN_SPIDER:
            ret = SPWPN_VENOM;
            break;

        case TRAN_ICE_BEAST:
            ret = SPWPN_FREEZING;
            break;

        case TRAN_LICH:
            ret = SPWPN_DRAINING;
            break;

        case TRAN_BAT:
            if (you.species == SP_VAMPIRE && one_chance_in(8))
                ret = SPWPN_VAMPIRICISM;
            break;

        default:
            break;
        }
    }

    return (ret);
}

item_def *player::slot_item(equipment_type eq)
{
    ASSERT(eq >= EQ_WEAPON && eq <= EQ_AMULET);

    const int item = equip[eq];
    return (item == -1? NULL : &inv[item]);
}

// Returns the item in the player's weapon slot.
item_def *player::weapon(int /* which_attack */)
{
    return slot_item(EQ_WEAPON);
}

item_def *player::shield()
{
    return slot_item(EQ_SHIELD);
}

std::string player::name(description_level_type type) const
{
    return (pronoun_you(type));
}

std::string player::pronoun(pronoun_type pro) const
{
    switch (pro)
    {
    default:
    case PRONOUN_CAP:   return "You";
    case PRONOUN_NOCAP: return "you";
    case PRONOUN_CAP_POSSESSIVE: return "Your";
    case PRONOUN_NOCAP_POSSESSIVE: return "your";
    case PRONOUN_REFLEXIVE: return "yourself";
    }
}

std::string player::conj_verb(const std::string &verb) const
{
    return (verb);
}

int player::id() const
{
    return (-1);
}

int player::get_experience_level() const
{
    return (experience_level);
}

bool player::alive() const
{
    // Simplistic, but if the player dies the game is over anyway, so
    // nobody can ask further questions.
    return (true);
}

bool player::fumbles_attack(bool verbose)
{
    // Fumbling in shallow water.
    if (floundering())
    {
        if (x_chance_in_y(4, dex) || one_chance_in(5))
        {
            if (verbose)
                mpr("Unstable footing causes you to fumble your attack.");
            return (true);
        }
    }
    return (false);
}

bool player::cannot_fight() const
{
    return (false);
}

// If you have a randart equipped that has the RAP_ANGRY property,
// there's a 1/20 chance of it becoming activated whenever you
// attack a monster. (Same as the berserk mutation at level 1.)
// The probabilities for actually going berserk are cumulative!
static bool _equipment_make_berserk()
{
    for (int eq = EQ_WEAPON; eq < NUM_EQUIP; eq++)
    {
         const item_def *item = you.slot_item((equipment_type) eq);
         if (!item)
             continue;

         if (!is_random_artefact(*item))
             continue;

         if (one_chance_in(20)
             && randart_wpn_property(*item,
                  static_cast<randart_prop_type>(RAP_ANGRY)))
         {
             return (true);
         }
    }

    // nothing found
    return (false);
}

void player::attacking(actor *other)
{
    if (other && other->atype() == ACT_MONSTER)
    {
        const monsters *mon = dynamic_cast<monsters*>(other);
        if (!mons_friendly(mon) && !mons_neutral(mon))
            pet_target = monster_index(mon);
    }

    if (player_mutation_level(MUT_BERSERK)
            && x_chance_in_y(player_mutation_level(MUT_BERSERK) * 10 - 5, 100)
        || _equipment_make_berserk())
    {
        go_berserk(false);
    }
}

void player::go_berserk(bool intentional)
{
    ::go_berserk(intentional);
}

bool player::can_go_berserk() const
{
    return can_go_berserk(false);
}

bool player::can_go_berserk(bool verbose) const
{
    if (you.duration[DUR_BERSERKER])
    {
        if (verbose)
            mpr("You're already berserk!");
        // or else you won't notice -- no message here.
        return (false);
    }

    if (you.duration[DUR_EXHAUSTED])
    {
        if (verbose)
            mpr("You're too exhausted to go berserk.");

        // or else they won't notice -- no message here
        return (false);
    }

    if (you.is_undead
        && (you.is_undead != US_SEMI_UNDEAD || you.hunger_state <= HS_SATIATED))
    {
        if (verbose)
            mpr("You cannot raise a blood rage in your lifeless body.");

        // or else you won't notice -- no message here
        return (false);
    }

    return (true);
}

void player::god_conduct(conduct_type thing_done, int level)
{
    ::did_god_conduct(thing_done, level);
}

void player::banish(const std::string &who)
{
    banished    = true;
    banished_by = who;
}

void player::make_hungry(int hunger_increase, bool silent)
{
    if (hunger_increase > 0)
        ::make_hungry(hunger_increase, silent);
    else if (hunger_increase < 0)
        ::lessen_hunger(-hunger_increase, silent);
}

// For semi-undead species (Vampire!) reduce food cost for spells and abilities
// to 50% (hungry, very hungry) or zero (near starving, starving).
int calc_hunger(int food_cost)
{
    if (you.is_undead == US_SEMI_UNDEAD && you.hunger_state < HS_SATIATED)
    {
        if (you.hunger_state <= HS_NEAR_STARVING)
            return 0;

        return (food_cost/2);
    }
    return (food_cost);
}

int player::holy_aura() const
{
    return (duration[DUR_REPEL_UNDEAD]? piety : 0);
}

int player::warding() const
{
    if (wearing_amulet(AMU_WARDING))
        return (30);

    if (religion == GOD_VEHUMET && !player_under_penance()
        && piety >= piety_breakpoint(2))
    {
        // Clamp piety at 160 and scale that down to a max of 30.
        const int wardpiety = piety > 160? 160 : piety;
        return (wardpiety * 3 / 16);
    }

    return (0);
}

bool player::paralysed() const
{
    return (duration[DUR_PARALYSIS]);
}

bool player::cannot_move() const
{
    return (duration[DUR_PARALYSIS] || duration[DUR_PETRIFIED]);
}

bool player::confused() const
{
    return (duration[DUR_CONF]);
}

bool player::caught() const
{
    return (attribute[ATTR_HELD]);
}

int player::shield_block_penalty() const
{
    return (5 * shield_blocks * shield_blocks);
}

int player::shield_bonus() const
{
    const int shield_class = player_shield_class();
    if (!shield_class)
        return (-100);

    // Note that 0 is not quite no-blocking.
    if (incapacitated())
        return (0);

    int stat = 0;
    if (const item_def *sh = const_cast<player*>(this)->shield())
    {
        stat =
            sh->sub_type == ARM_BUCKLER?      dex :
            sh->sub_type == ARM_LARGE_SHIELD? (3 * strength + dex) / 4:
            (dex + strength) / 2;
    }
    else
    {
        // Condensation shield is guided by the mind.
        stat = intel / 2;
    }

    return (random2(shield_class)
            + (random2(stat) / 4)
            + (random2(skill_bump(SK_SHIELDS)) / 4)
            - 1);
}

int player::shield_bypass_ability(int tohit) const
{
    return (15 + tohit / 2);
}

void player::shield_block_succeeded()
{
    shield_blocks++;
    if (coinflip())
        exercise(SK_SHIELDS, 1);
}

bool player::wearing_light_armour(bool with_skill) const
{
    return (player_light_armour(with_skill));
}

void player::exercise(skill_type sk, int qty)
{
    ::exercise(sk, qty);
}

int player::skill(skill_type sk, bool bump) const
{
    return (bump? skill_bump(sk) : skills[sk]);
}

int player::armour_class() const
{
    return (player_AC());
}

int player::melee_evasion(const actor *act) const
{
    return (player_evasion()
            - (act->visible()? 0 : 10)
            - (you_are_delayed()? 5 : 0));
}

void player::heal(int amount, bool max_too)
{
    ::inc_hp(amount, max_too);
}

mon_holy_type player::holiness() const
{
    if (is_undead)
        return (MH_UNDEAD);

    if (species == SP_DEMONSPAWN)
        return (MH_DEMONIC);

    return (MH_NATURAL);
}

// Output active level of player mutation.
// Might be lower than real mutation for non-"Alive" Vampires.
int player_mutation_level(mutation_type mut)
{
    const int mlevel = you.mutation[mut];

    if (mutation_is_fully_active(mut))
        return (mlevel);

    // For now, dynamic mutations only apply to semi-undead.
    ASSERT(you.is_undead == US_SEMI_UNDEAD);

    // Assumption: stat mutations are physical, and thus always fully active.
    switch (you.hunger_state)
    {
    case HS_ENGORGED:
        return (mlevel);
    case HS_VERY_FULL:
    case HS_FULL:
        return (std::min(mlevel, 2));
    case HS_SATIATED:
        return (std::min(mlevel, 1));
    }
    return (0);
}

int player::res_fire() const
{
    return (player_res_fire());
}

int player::res_steam() const
{
    return (player_res_steam());
}

int player::res_cold() const
{
    return (player_res_cold());
}

int player::res_elec() const
{
    return (player_res_electricity() * 2);
}

int player::res_poison() const
{
    return (player_res_poison());
}

int player::res_negative_energy() const
{
    return (player_prot_life());
}

int player::res_rotting() const
{
    if (you.is_undead
        && (you.is_undead != US_SEMI_UNDEAD || you.hunger_state < HS_SATIATED))
    {
        return 1;
    }
    return 0;
}

bool player::confusable() const
{
    return (player_mental_clarity() == 0);
}

bool player::slowable() const
{
    return (!wearing_amulet(AMU_RESIST_SLOW));
}

bool player::omnivorous() const
{
    return (species == SP_TROLL || species == SP_OGRE);
}

flight_type player::flight_mode() const
{
    if (attribute[ATTR_TRANSFORMATION] == TRAN_DRAGON
        || attribute[ATTR_TRANSFORMATION] == TRAN_BAT)
    {
        return FL_FLY;
    }
    else if (is_levitating())
    {
        return (you.duration[DUR_CONTROLLED_FLIGHT]
                || wearing_amulet(AMU_CONTROLLED_FLIGHT) ? FL_FLY
                                                         : FL_LEVITATE);
    }
    else
        return (FL_NONE);
}

bool player::permanent_levitation() const
{
    // Boots of levitation keep you with DUR_LEVITATION >= 2 at
    // all times. This is so that you can evoke stop-levitation
    // in order to actually cancel levitation (by setting
    // DUR_LEVITATION to 1.) Note that antimagic() won't do this.
    return (airborne() && player_equip_ego_type(EQ_BOOTS, SPARM_LEVITATION)
               && you.duration[DUR_LEVITATION] > 1);
}

bool player::permanent_flight() const
{
    return (airborne() && wearing_amulet( AMU_CONTROLLED_FLIGHT )
            && you.species == SP_KENKU && you.experience_level >= 15);
}

bool player::light_flight() const
{
    // Only Kenku get perks for flying light.
    return (species == SP_KENKU
            && flight_mode() == FL_FLY && travelling_light());
}

bool player::travelling_light() const
{
    return (you.burden < carrying_capacity(BS_UNENCUMBERED) * 70 / 100);
}

int player::mons_species() const
{
    if (player_genus(GENPC_DRACONIAN))
        return (MONS_DRACONIAN);

    switch (species)
    {
    case SP_HILL_ORC:
        return (MONS_ORC);
    case SP_HIGH_ELF: case SP_GREY_ELF:
    case SP_DEEP_ELF: case SP_SLUDGE_ELF:
        return (MONS_ELF);

    default:
        return (MONS_HUMAN);
    }
}

void player::poison(actor*, int amount)
{
    ::poison_player(amount);
}

void player::expose_to_element(beam_type element, int st)
{
    ::expose_player_to_element(element, st);
}

void player::blink()
{
    random_blink(true);
}

void player::teleport(bool now, bool abyss_shift)
{
    if (now)
        you_teleport_now(true, abyss_shift);
    else
        you_teleport();
}

int player::hurt(const actor *agent, int amount)
{
    const monsters *mon = dynamic_cast<const monsters*>(agent);
    if (agent->atype() == ACT_MONSTER)
    {
        ouch(amount, monster_index( mon ),
             KILLED_BY_MONSTER, "", player_monster_visible(mon));
    }
    else
    {
        // Should never happen!
        ASSERT(false);
        ouch(amount, 0, KILLED_BY_SOMETHING);
    }
    return (amount);
}

void player::drain_stat(int stat, int amount, actor* attacker)
{
    if (attacker == NULL)
        lose_stat(stat, amount, false, "");
    else if (attacker->atype() == ACT_MONSTER)
        lose_stat(stat, amount, dynamic_cast<monsters*>(attacker), false);
    else if (attacker->atype() == ACT_PLAYER)
        lose_stat(stat, amount, false, "suicide");
    else
        lose_stat(stat, amount, false, "");
}

void player::rot(actor *who, int rotlevel, int immed_rot)
{
    if (res_rotting() > 0)
        return;

    if (rotlevel)
        rot_player(rotlevel);

    if (immed_rot)
        rot_hp(immed_rot);

    if (rotlevel && one_chance_in(4))
        disease_player( 50 + random2(100) );
}

void player::confuse(int str)
{
    confuse_player(str);
}

void player::paralyse(int str)
{
    int &paralysis(duration[DUR_PARALYSIS]);

    mprf( "You %s the ability to move!",
          paralysis ? "still haven't" : "suddenly lose" );

    if (str > paralysis && (paralysis < 3 || one_chance_in(paralysis)))
        paralysis = str;

    if (paralysis > 13)
        paralysis = 13;
}

void player::petrify(int str)
{
    int &petrif(duration[DUR_PETRIFIED]);

    mprf( "You %s the ability to move!",
          petrif ? "still haven't" : "suddenly lose" );

    if (str > petrif && (petrif < 3 || one_chance_in(petrif)))
        petrif = str;

    petrif = std::min(13, petrif);
}

void player::slow_down(int str)
{
    ::slow_player( str );
}

int player::has_claws(bool allow_tran) const
{
    if (allow_tran)
    {
        // these transformations bring claws with them
        if (attribute[ATTR_TRANSFORMATION] == TRAN_DRAGON
            || attribute[ATTR_TRANSFORMATION] == TRAN_SERPENT_OF_HELL)
        {
            return 3;
        }

        // transformations other than these will override claws
        if (attribute[ATTR_TRANSFORMATION] != TRAN_NONE
            && attribute[ATTR_TRANSFORMATION] != TRAN_STATUE
            && attribute[ATTR_TRANSFORMATION] != TRAN_LICH)
        {
            return 0;
        }
    }

    // these are the only other sources for claws
    if (species == SP_TROLL || species == SP_GHOUL)
        return 3;
    else
        return mutation[MUT_CLAWS];
}

bool player::has_usable_claws(bool allow_tran) const
{
    return (equip[EQ_GLOVES] == -1 && has_claws(allow_tran));
}

god_type player::deity() const
{
    return (religion);
}

kill_category player::kill_alignment() const
{
    return (KC_YOU);
}

bool player::sicken(int amount)
{
    if (is_undead || amount <= 0)
        return (false);

    mpr( "You feel ill." );

    const int tmp = disease + amount;
    disease = (tmp > 210) ? 210 : tmp;
    learned_something_new(TUT_YOU_SICK);

    return (true);
}

bool player::can_see_invisible() const
{
    return (player_see_invis() > 0);
}

bool player::invisible() const
{
    return (duration[DUR_INVIS] && !backlit());
}

bool player::visible_to(const actor *looker) const
{
    if (this == looker)
        return (!invisible() || can_see_invisible());

    const monsters* mon = dynamic_cast<const monsters*>(looker);
    return mons_player_visible(mon);
}

bool player::can_see(const actor *target) const
{
    if (this == target)
        return visible_to(target);

    const monsters* mon = dynamic_cast<const monsters*>(target);
    return (mons_near(mon) && target->visible_to(this));
}

bool player::backlit(bool check_haloed) const
{
    return (_get_contamination_level() >= 1 || duration[DUR_BACKLIGHT]
        || ((check_haloed) ? haloed() : false));
}

bool player::haloed() const
{
    return (halo_radius());
}

bool player::can_mutate() const
{
    return (true);
}

bool player::can_safely_mutate() const
{
    if (!can_mutate())
        return (false);

    return (!you.is_undead
            || you.is_undead == US_SEMI_UNDEAD
               && you.hunger_state == HS_ENGORGED);
}

bool player::mutate()
{
    if (!can_mutate())
        return (false);

    if (one_chance_in(5))
    {
        if (::mutate(RANDOM_MUTATION))
        {
            learned_something_new(TUT_YOU_MUTATED);
            return (true);
        }
    }

    return (give_bad_mutation());
}

bool player::is_icy() const
{
    return (attribute[ATTR_TRANSFORMATION] == TRAN_ICE_BEAST);
}

void player::moveto(int x, int y)
{
    moveto(coord_def(x, y));
}

void player::moveto(const coord_def &c)
{
    const bool real_move = (c != pos());
    x_pos = c.x;
    y_pos = c.y;
    crawl_view.set_player_at(c);

    if (real_move)
    {
        you.reset_prev_move();
        dungeon_events.fire_position_event(DET_PLAYER_MOVED, c);

        // Reset lava/water nearness check to unknown, so it'll be
        // recalculated for the next monster that tries to reach us.
        you.lava_in_sight = you.water_in_sight = -1;
    }
}

coord_def player::prev_move() const
{
    return coord_def(prev_move_x, prev_move_y);
}

void player::reset_prev_move()
{
    prev_move_x = 0;
    prev_move_y = 0;
}

bool player::asleep() const
{
    return (duration[DUR_SLEEP] > 0);
}

bool player::cannot_act() const
{
    return (asleep() || cannot_move());
}

bool player::can_throw_rocks() const
{
    return (species == SP_OGRE || species == SP_TROLL
            || species == SP_OGRE_MAGE);
}

void player::put_to_sleep(int)
{
    if (duration[DUR_BERSERKER] || asleep())   // No cumulative sleeps.
        return;

    // Not all species can be put to sleep. Check holiness.
    const mon_holy_type holy = holiness();
    if (holy == MH_UNDEAD || holy == MH_NONLIVING)
    {
        mpr("You feel weary for a moment.");
        return;
    }

    mpr("You fall asleep.");
    stop_delay();
    you.flash_colour = DARKGREY;
    viewwindow(true, false);

    // Do this *after* redrawing the view, or viewwindow() will no-op.
    duration[DUR_SLEEP] = 3 + random2avg(5, 2);
}

void player::awake()
{
    duration[DUR_SLEEP] = 0;
    mpr("You wake up.");
    you.flash_colour = BLACK;
    viewwindow(true, false);
}

void player::check_awaken(int disturbance)
{
    if (asleep() && x_chance_in_y(disturbance + 1, 50))
        awake();
}

////////////////////////////////////////////////////////////////////////////

PlaceInfo::PlaceInfo()
    : level_type(-2), branch(-2), num_visits(0),
      levels_seen(0), mon_kill_exp(0), mon_kill_exp_avail(0),
      turns_total(0), turns_explore(0), turns_travel(0), turns_interlevel(0),
      turns_resting(0), turns_other(0), elapsed_total(0.0),
      elapsed_explore(0.0), elapsed_travel(0.0), elapsed_interlevel(0.0),
      elapsed_resting(0.0), elapsed_other(0.0)
{
    for (int i = 0; i < KC_NCATEGORIES; i++)
        mon_kill_num[i] = 0;
}

bool PlaceInfo::is_global() const
{
    return (level_type == -1 && branch == -1);
}

void PlaceInfo::make_global()
{
    level_type = -1;
    branch     = -1;
}

void PlaceInfo::assert_validity() const
{
    // Check that level_type and branch match up.
    ASSERT(is_global()
           || level_type == LEVEL_DUNGEON && branch >= BRANCH_MAIN_DUNGEON
              && branch < NUM_BRANCHES
           || level_type > LEVEL_DUNGEON && level_type < NUM_LEVEL_AREA_TYPES
              && branch == -1);

    // Can't have visited a place without seeing any of its levels, and
    // vice versa.
    ASSERT(num_visits == 0 && levels_seen == 0
           || num_visits > 0 && levels_seen > 0);

    if (level_type == LEVEL_LABYRINTH || level_type == LEVEL_ABYSS)
        ASSERT(num_visits == levels_seen);
    else if (level_type == LEVEL_PANDEMONIUM)
        ASSERT(num_visits <= levels_seen);
    else if (level_type == LEVEL_DUNGEON && branches[branch].depth > 0)
        ASSERT(levels_seen <= (unsigned long) branches[branch].depth);

    ASSERT(turns_total == (turns_explore + turns_travel + turns_interlevel
                           + turns_resting + turns_other));

    ASSERT(elapsed_total == (elapsed_explore + elapsed_travel
                             + elapsed_interlevel + elapsed_resting
                             + elapsed_other));
}

const std::string PlaceInfo::short_name() const
{
    if (level_type == LEVEL_DUNGEON)
        return branches[branch].shortname;
    else
    {
        switch (level_type)
        {
        case LEVEL_ABYSS:
            return "Abyss";

        case LEVEL_PANDEMONIUM:
            return "Pandemonium";

        case LEVEL_LABYRINTH:
            return "Labyrinth";

        case LEVEL_PORTAL_VAULT:
            return "Portal Chamber";

        default:
            return "Bug";
        }
    }
}

const PlaceInfo &PlaceInfo::operator += (const PlaceInfo &other)
{
    num_visits  += other.num_visits;
    levels_seen += other.levels_seen;

    mon_kill_exp       += other.mon_kill_exp;
    mon_kill_exp_avail += other.mon_kill_exp_avail;

    for (int i = 0; i < KC_NCATEGORIES; i++)
        mon_kill_num[i] += other.mon_kill_num[i];

    turns_total      += other.turns_total;
    turns_explore    += other.turns_explore;
    turns_travel     += other.turns_travel;
    turns_interlevel += other.turns_interlevel;
    turns_resting    += other.turns_resting;
    turns_other      += other.turns_other;

    elapsed_total      += other.elapsed_total;
    elapsed_explore    += other.elapsed_explore;
    elapsed_travel     += other.elapsed_travel;
    elapsed_interlevel += other.elapsed_interlevel;
    elapsed_resting    += other.elapsed_resting;
    elapsed_other      += other.elapsed_other;

    return (*this);
}

const PlaceInfo &PlaceInfo::operator -= (const PlaceInfo &other)
{
    num_visits  -= other.num_visits;
    levels_seen -= other.levels_seen;

    mon_kill_exp       -= other.mon_kill_exp;
    mon_kill_exp_avail -= other.mon_kill_exp_avail;

    for (int i = 0; i < KC_NCATEGORIES; i++)
        mon_kill_num[i] -= other.mon_kill_num[i];

    turns_total      -= other.turns_total;
    turns_explore    -= other.turns_explore;
    turns_travel     -= other.turns_travel;
    turns_interlevel -= other.turns_interlevel;
    turns_resting    -= other.turns_resting;
    turns_other      -= other.turns_other;

    elapsed_total      -= other.elapsed_total;
    elapsed_explore    -= other.elapsed_explore;
    elapsed_travel     -= other.elapsed_travel;
    elapsed_interlevel -= other.elapsed_interlevel;
    elapsed_resting    -= other.elapsed_resting;
    elapsed_other      -= other.elapsed_other;

    return (*this);
}

PlaceInfo PlaceInfo::operator + (const PlaceInfo &other) const
{
    PlaceInfo copy = *this;
    copy += other;
    return copy;
}

PlaceInfo PlaceInfo::operator - (const PlaceInfo &other) const
{
    PlaceInfo copy = *this;
    copy -= other;
    return copy;
}

PlaceInfo& player::get_place_info() const
{
    return get_place_info(where_are_you, level_type);
}

PlaceInfo& player::get_place_info(branch_type branch) const
{
    return get_place_info(branch, LEVEL_DUNGEON);
}

PlaceInfo& player::get_place_info(level_area_type level_type2) const
{
    return get_place_info(NUM_BRANCHES, level_type2);
}

PlaceInfo& player::get_place_info(branch_type branch,
                                  level_area_type level_type2) const
{
    ASSERT(level_type2 == LEVEL_DUNGEON && branch >= BRANCH_MAIN_DUNGEON
                && branch < NUM_BRANCHES
           || level_type2 > LEVEL_DUNGEON && level_type < NUM_LEVEL_AREA_TYPES);

    if (level_type2 == LEVEL_DUNGEON)
        return (PlaceInfo&) branch_info[branch];
    else
        return (PlaceInfo&) non_branch_info[level_type2 - 1];
}

void player::set_place_info(PlaceInfo place_info)
{
    place_info.assert_validity();

    if (place_info.is_global())
        global_info = place_info;
    else if (place_info.level_type == LEVEL_DUNGEON)
        branch_info[place_info.branch] = place_info;
    else
        non_branch_info[place_info.level_type - 1] = place_info;
}

std::vector<PlaceInfo> player::get_all_place_info(bool visited_only,
                                                  bool dungeon_only) const
{
    std::vector<PlaceInfo> list;

    for (int i = 0; i < NUM_BRANCHES; i++)
    {
        if (visited_only && branch_info[i].num_visits == 0
            || dungeon_only && branch_info[i].level_type != LEVEL_DUNGEON)
        {
            continue;
        }
        list.push_back(branch_info[i]);
    }

    for (int i = 0; i < (NUM_LEVEL_AREA_TYPES - 1); i++)
    {
        if (visited_only && non_branch_info[i].num_visits == 0
            || dungeon_only && non_branch_info[i].level_type != LEVEL_DUNGEON)
        {
            continue;
        }
        list.push_back(non_branch_info[i]);
    }

    return list;
}

bool player::do_shaft()
{
    dungeon_feature_type force_stair = DNGN_UNSEEN;

    if (!is_valid_shaft_level())
        return (false);

    // Handle instances of do_shaft() being invoked magically when
    // the player isn't standing over a shaft.
    if (trap_type_at_xy(x_pos, y_pos) != TRAP_SHAFT)
    {
        switch(grd[x_pos][y_pos])
        {
        case DNGN_FLOOR:
        case DNGN_OPEN_DOOR:
        case DNGN_TRAP_MECHANICAL:
        case DNGN_TRAP_MAGICAL:
        case DNGN_TRAP_NATURAL:
        case DNGN_UNDISCOVERED_TRAP:
        case DNGN_ENTER_SHOP:
            break;

        default:
            return (false);
        }

        if (airborne() || total_weight() == 0)
        {
            mpr("A shaft briefly opens up underneath you!");
            handle_items_on_shaft(you.x_pos, you.y_pos, false);
            return (true);
        }

        force_stair = DNGN_TRAP_NATURAL;
    }

    down_stairs(your_level, force_stair);

    return (true);
}
