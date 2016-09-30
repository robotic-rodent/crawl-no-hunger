/**
 * @file
 * @brief Functions for eating.
**/

#include "AppHdr.h"

#include "food.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "butcher.h"
#include "chardump.h"
#include "database.h"
#include "delay.h"
#include "env.h"
#include "godabil.h"
#include "godconduct.h"
#include "hints.h"
#include "invent.h"
#include "itemprop.h"
#include "items.h"
#include "item_use.h"
#include "libutil.h"
#include "macro.h"
#include "message.h"
#include "misc.h"
#include "mutation.h"
#include "nearby-danger.h"
#include "notes.h"
#include "options.h"
#include "output.h"
#include "religion.h"
#include "rot.h"
#include "state.h"
#include "stepdown.h"
#include "stringutil.h"
#include "travel.h"
#include "transform.h"
#include "xom.h"

static void _eat_chunk(item_def& food);
static bool _vampire_consume_corpse(item_def& corpse);
static void _heal_from_food(int hp_amt);

bool you_foodless(bool can_eat)
{
    return you.undead_state() == US_UNDEAD
#if TAG_MAJOR_VERSION == 34
        || you.species == SP_DJINNI && !can_eat
#endif
        ;
}

bool you_foodless_normally()
{
    return you.undead_state(false) == US_UNDEAD
#if TAG_MAJOR_VERSION == 34
        || you.species == SP_DJINNI
#endif
        ;
}

bool prompt_eat_item(int slot)
{
    // There's nothing in inventory that a vampire can 'e', and floor corpses
    // are handled by prompt_eat_chunks.
    if (you.species == SP_VAMPIRE)
        return false;

    item_def* item = nullptr;
    if (slot == -1)
    {
        item = use_an_item(OBJ_FOOD, OPER_EAT, "Eat which item?");
        if (!item)
            return false;
    }
    else
        item = &you.inv[slot];

    eat_item(*item);

    return true;
}

static bool _eat_check(bool check_hunger = true, bool silent = false)
{
    if (you_foodless(true))
    {
        if (!silent)
        {
            mpr("You can't eat.");
            crawl_state.zero_turns_taken();
        }
        return false;
    }

    return true;
}

// [ds] Returns true if something was eaten.
bool eat_food(int slot)
{
    if (!_eat_check())
        return false;

    // Skip the prompts if we already know what we're eating.
    if (slot == -1)
    {
        int result = prompt_eat_chunks();
        if (result == 1)
            return true;
        else if (result == -1)
            return false;
    }

    if (you.species == SP_VAMPIRE)
        mpr("There's nothing here to drain!");

    return prompt_eat_item(slot);
}

bool eat_item(item_def &food)
{
    if (food.is_type(OBJ_CORPSES, CORPSE_BODY))
    {
        if (you.species != SP_VAMPIRE)
            return false;

        if (_vampire_consume_corpse(food))
        {
            count_action(CACT_EAT, -1); // subtype Corpse
            you.turn_is_over = true;
            return true;
        }
        else
            return false;
    }
    else
        start_delay<EatDelay>(food_turns(food) - 1, food);

    mprf("You start eating %s%s.", food.quantity > 1 ? "one of " : "",
                                   food.name(DESC_THE).c_str());
    you.turn_is_over = true;
    return true;
}

// Handle messaging at the end of eating.
// Some food types may not get a message.
static void _finished_eating_message(food_type type)
{
    bool herbivorous = player_mutation_level(MUT_HERBIVOROUS) > 0;
    bool carnivorous = player_mutation_level(MUT_CARNIVOROUS) > 0;

    if (herbivorous)
    {
        if (food_is_meaty(type))
        {
            mpr("Blech - you need greens!");
            return;
        }
    }
    else
    {
        switch (type)
        {
        case FOOD_MEAT_RATION:
            mpr("That meat ration really hit the spot!");
            return;
        case FOOD_BEEF_JERKY:
            mprf("That beef jerky was %s!",
                 one_chance_in(4) ? "jerk-a-riffic"
                                  : "delicious");
            return;
        default:
            break;
        }
    }

    if (carnivorous)
    {
        if (food_is_veggie(type))
        {
            mpr("Blech - you need meat!");
            return;
        }
    }
    else
    {
        switch (type)
        {
        case FOOD_BREAD_RATION:
            mpr("That bread ration really hit the spot!");
            return;
        case FOOD_FRUIT:
        {
            string taste = getMiscString("eating_fruit");
            if (taste.empty())
                taste = "Eugh, buggy fruit.";
            mpr(taste);
            break;
        }
        default:
            break;
        }
    }

    switch (type)
    {
    case FOOD_ROYAL_JELLY:
        mpr("That royal jelly was delicious!");
        break;
    case FOOD_PIZZA:
    {
        if (!Options.pizzas.empty())
        {
            const string za = Options.pizzas[random2(Options.pizzas.size())];
            mprf("Mmm... %s.", trimmed_string(za).c_str());
            break;
        }

        const string taste = getMiscString("eating_pizza");
        if (taste.empty())
        {
            mpr("Bleh, bug pizza.");
            break;
        }

        mprf("%s", taste.c_str());
        break;
    }
    default:
        break;
    }
}


void finish_eating_item(item_def& food)
{
    if (food.sub_type == FOOD_CHUNK)
        _eat_chunk(food);
    else
    {
        int value = food_value(food);
        ASSERT(value > 0);
        _finished_eating_message(static_cast<food_type>(food.sub_type));
    }

    count_action(CACT_EAT, food.sub_type);

    if (is_perishable_stack(food)) // chunks
        remove_oldest_perishable_item(food);
    if (in_inventory(food))
        dec_inv_item_quantity(food.link, 1);
    else
        dec_mitm_item_quantity(food.index(), 1);
}

// Returns which of two food items is older (true for first, else false).
static bool _compare_by_freshness(const item_def *food1, const item_def *food2)
{
    ASSERT(food1->base_type == OBJ_CORPSES || food1->base_type == OBJ_FOOD);
    ASSERT(food2->base_type == OBJ_CORPSES || food2->base_type == OBJ_FOOD);
    ASSERT(food1->base_type == food2->base_type);

    if (is_inedible(*food1))
        return false;

    if (is_inedible(*food2))
        return true;

    // Permafood can last longest, skip it if possible.
    if (food1->base_type == OBJ_FOOD && food1->sub_type != FOOD_CHUNK)
        return false;
    if (food2->base_type == OBJ_FOOD && food2->sub_type != FOOD_CHUNK)
        return true;

    // At this point, we know both are corpses or chunks, edible

    // Always offer poisonous/mutagenic chunks last.
    if (is_bad_food(*food1) && !is_bad_food(*food2))
        return false;
    if (is_bad_food(*food2) && !is_bad_food(*food1))
        return true;

    return food1->freshness < food2->freshness;
}

/** Make the prompt for chunk eating/corpse draining.
 *
 *  @param only_auto Don't actually make a prompt: if there are
 *                   things to auto_eat, eat them, and exit otherwise.
 *  @returns -1 for cancel, 1 for eaten, 0 for not eaten,
 */
int prompt_eat_chunks(bool only_auto)
{
    // Full herbivores cannot eat chunks.
    if (player_mutation_level(MUT_HERBIVOROUS) == 3)
        return 0;

    // If we *know* the player can eat chunks, doesn't have the gourmand
    // effect and isn't hungry, don't prompt for chunks.
    if (you.species != SP_VAMPIRE)
    {
        return 0;
    }

    bool found_valid = false;
    vector<item_def *> chunks;

    for (stack_iterator si(you.pos(), true); si; ++si)
    {
        if (you.species == SP_VAMPIRE)
        {
            if (si->base_type != OBJ_CORPSES || si->sub_type != CORPSE_BODY)
                continue;

            if (!mons_has_blood(si->mon_type))
                continue;
        }
        else if (si->base_type != OBJ_FOOD
                 || si->sub_type != FOOD_CHUNK
                 || is_bad_food(*si))
        {
            continue;
        }

        found_valid = true;
        chunks.push_back(&(*si));
    }

    // Then search through the inventory.
    for (auto &item : you.inv)
    {
        if (!item.defined())
            continue;

        // Vampires can't eat anything in their inventory.
        if (you.species == SP_VAMPIRE)
            continue;

        if (item.base_type != OBJ_FOOD || item.sub_type != FOOD_CHUNK)
            continue;

        // Don't prompt for bad food types.
        if (is_bad_food(item))
            continue;

        found_valid = true;
        chunks.push_back(&item);
    }

    const bool easy_eat = Options.easy_eat_chunks || only_auto;

    if (found_valid)
    {
        sort(chunks.begin(), chunks.end(), _compare_by_freshness);
        for (item_def *item : chunks)
        {
            bool autoeat = false;
            string item_name = menu_colour_item_name(*item, DESC_A);

            const bool bad = is_bad_food(*item);

            // Allow undead to use easy_eat, but not auto_eat, since the player
            // might not want to drink blood as a vampire and might want to save
            // chunks as a ghoul. Ghouls can auto_eat if they have rotted hp.
            const bool no_auto = you.undead_state()
                && !(you.species == SP_GHOUL && player_rotted());

            // If this chunk is safe to eat, just do so without prompting.
            if (easy_eat && !bad && i_feel_safe() && !(only_auto && no_auto))
                autoeat = true;
            else if (only_auto)
                return 0;
            else
            {
                mprf(MSGCH_PROMPT, "%s %s%s? (ye/n/q)",
                     (you.species == SP_VAMPIRE ? "Drink blood from" : "Eat"),
                     ((item->quantity > 1) ? "one of " : ""),
                     item_name.c_str());
            }

            int keyin = autoeat ? 'y' : toalower(getchm(KMC_CONFIRM));
            switch (keyin)
            {
            case 'q':
            CASE_ESCAPE
                canned_msg(MSG_OK);
                return -1;
            case 'i':
            case '?':
                // Skip ahead to the inventory.
                return 0;
            case 'e':
            case 'y':
                if (can_eat(*item, false))
                {
                    if (autoeat)
                    {
                        mprf("%s %s%s.",
                             (you.species == SP_VAMPIRE ? "Drinking blood from"
                                                        : "Eating"),
                             ((item->quantity > 1) ? "one of " : ""),
                             item_name.c_str());
                    }

                    return eat_item(*item) ? 1 : 0;
                }
                break;
            default:
                // Else no: try next one.
                break;
            }
        }
    }

    return 0;
}

static const char *_chunk_flavour_phrase(bool likes_chunks)
{
    const char *phrase = "tastes terrible.";

    if (you.species == SP_GHOUL)
        phrase = "tastes great!";
    else if (likes_chunks)
        phrase = "tastes great.";
    else
    {
        const int gourmand = you.duration[DUR_GOURMAND];
        if (gourmand >= GOURMAND_MAX)
        {
            phrase = one_chance_in(1000) ? "tastes like chicken!"
                                         : "tastes great.";
        }
        else if (gourmand > GOURMAND_MAX * 75 / 100)
            phrase = "tastes very good.";
        else if (gourmand > GOURMAND_MAX * 50 / 100)
            phrase = "tastes good.";
        else if (gourmand > GOURMAND_MAX * 25 / 100)
            phrase = "is not very appetising.";
    }

    return phrase;
}

/**
 * How intelligent was the monster that the given corpse came from?
 *
 * @param   The corpse being examined.
 * @return  The mon_intel_type of the monster that the given corpse was
 *          produced from.
 */
mon_intel_type corpse_intelligence(const item_def &corpse)
{
    // An optimising compiler can assume an enum value is in range, so
    // check the range on the uncast value.
    const bool bad = corpse.orig_monnum < 0
                     || corpse.orig_monnum >= NUM_MONSTERS;
    const monster_type orig_mt = static_cast<monster_type>(corpse.orig_monnum);
    const monster_type type = bad || invalid_monster_type(orig_mt)
                                ? corpse.mon_type
                                : orig_mt;
    return mons_class_intel(type);
}

// Never called directly - chunk_effect values must pass
// through food:determine_chunk_effect() first. {dlb}:
static void _eat_chunk(item_def& food)
{
    const corpse_effect_type chunk_effect = determine_chunk_effect(food);

    int likes_chunks  = player_likes_chunks(true);

    switch (chunk_effect)
    {
    case CE_MUTAGEN:
        mpr("This meat tastes really weird.");
        mutate(RANDOM_MUTATION, "mutagenic meat");
        did_god_conduct(DID_DELIBERATE_MUTATING, 10);
        xom_is_stimulated(100);
        break;

    case CE_CLEAN:
    {
        if (you.species == SP_GHOUL)
        {
            const int hp_amt = 1 + random2avg(5 + you.experience_level, 3);
            _heal_from_food(hp_amt);
        }

        mprf("This raw flesh %s", _chunk_flavour_phrase(likes_chunks));
        break;
    }

    case CE_NOXIOUS:
    case CE_NOCORPSE:
        mprf(MSGCH_ERROR, "This flesh (%d) tastes buggy!", chunk_effect);
        break;
    }
}

void vampire_nutrition_per_turn(const item_def &corpse, int feeding)
{
    mprf("Sblood, just remove me already!");
}

bool is_bad_food(const item_def &food)
{
    return is_mutagenic(food) || is_forbidden_food(food) || is_noxious(food);
}

// Returns true if a food item (or corpse) is mutagenic.
bool is_mutagenic(const item_def &food)
{
    if (food.base_type != OBJ_FOOD && food.base_type != OBJ_CORPSES)
        return false;

    return determine_chunk_effect(food) == CE_MUTAGEN;
}

// Returns true if a food item (or corpse) is totally inedible.
bool is_noxious(const item_def &food)
{
    if (food.base_type != OBJ_FOOD && food.base_type != OBJ_CORPSES)
        return false;

    return determine_chunk_effect(food) == CE_NOXIOUS;
}

// Returns true if an item of basetype FOOD or CORPSES cannot currently
// be eaten (respecting species and mutations set).
bool is_inedible(const item_def &item)
{
    // Mummies and liches don't eat.
    if (you_foodless(true))
        return true;

    if (item.base_type == OBJ_FOOD
        && !can_eat(item, true, false))
    {
        return true;
    }

    if (item.base_type == OBJ_CORPSES)
    {
        if (item.sub_type == CORPSE_SKELETON)
            return true;

        if (you.species == SP_VAMPIRE)
        {
            if (!mons_has_blood(item.mon_type))
                return true;
        }
        else
        {
            item_def chunk = item;
            chunk.base_type = OBJ_FOOD;
            chunk.sub_type  = FOOD_CHUNK;
            if (is_inedible(chunk))
                return true;
        }
    }

    return false;
}

// As we want to avoid autocolouring the entire food selection, this should
// be restricted to the absolute highlights, even though other stuff may
// still be edible or even delicious.
bool is_preferred_food(const item_def &food)
{
    // Mummies and liches don't eat.
    if (you_foodless(true))
        return false;

    // Vampires don't really have a preferred food type, but they really
    // like blood potions.
    if (you.species == SP_VAMPIRE)
        return is_blood_potion(food);

#if TAG_MAJOR_VERSION == 34
    if (food.is_type(OBJ_POTIONS, POT_PORRIDGE)
        && item_type_known(food)
        && you.species != SP_DJINNI
        )
    {
        return !player_mutation_level(MUT_CARNIVOROUS);
    }
#endif

    if (food.base_type != OBJ_FOOD)
        return false;

    // Poisoned, mutagenic, etc. food should never be marked as "preferred".
    if (is_bad_food(food))
        return false;

    if (player_mutation_level(MUT_CARNIVOROUS) == 3)
        return food_is_meaty(food.sub_type);

    if (player_mutation_level(MUT_HERBIVOROUS) == 3)
        return food_is_veggie(food.sub_type);

    // No food preference.
    return false;
}

/**
 * Is the given food item forbidden to the player by their god?
 *
 * @param food  The food item in question.
 * @return      Whether your god hates you eating it.
 */
bool is_forbidden_food(const item_def &food)
{
    // no food is forbidden to the player who does not yet exist
    if (!crawl_state.need_save)
        return false;

    // Only corpses are only forbidden, now.
    if (food.base_type != OBJ_CORPSES)
        return false;

    // Specific handling for intelligent monsters like Gastronok and Xtahua
    // of a normally unintelligent class.
    if (you_worship(GOD_ZIN) && corpse_intelligence(food) >= I_HUMAN)
        return true;

    return god_hates_eating(you.religion, food.mon_type);
}

/** Can the player eat this item?
 *
 *  @param food the item (must be a corpse or food item)
 *  @param suppress_msg whether to print why you can't eat it
 *  @param check_hunger whether to check how hungry you are currently
 */
bool can_eat(const item_def &food, bool suppress_msg, bool check_hunger)
{
#define FAIL(msg) { if (!suppress_msg) mpr(msg); return false; }
    ASSERT(food.base_type == OBJ_FOOD || food.base_type == OBJ_CORPSES);

    // special case mutagenic chunks to skip hunger checks, as they don't give
    // nutrition and player can get hungry by using spells etc. anyway
    if (is_mutagenic(food))
        check_hunger = false;

    // [ds] These redundant checks are now necessary - Lua might be calling us.
    if (!_eat_check(check_hunger, suppress_msg))
        return false;

    if (is_noxious(food))
        FAIL("It is completely inedible.");

    if (you.species == SP_VAMPIRE)
    {
        if (food.is_type(OBJ_CORPSES, CORPSE_BODY))
            return true;

        FAIL("Blech - you need blood!")
    }
    else if (food.base_type == OBJ_CORPSES)
        return false;

    if (food_is_veggie(food))
    {
        if (player_mutation_level(MUT_CARNIVOROUS) == 3)
            FAIL("Sorry, you're a carnivore.")
        else
            return true;
    }
    else if (food_is_meaty(food))
    {
        if (player_mutation_level(MUT_HERBIVOROUS) == 3)
            FAIL("Sorry, you're a herbivore.")
        else if (food.sub_type == FOOD_CHUNK)
        {
            if (!check_hunger
                || player_likes_chunks())
            {
                return true;
            }

            FAIL("You aren't quite hungry enough to eat that!")
        }
    }

    // Any food types not specifically handled until here (e.g. meat
    // rations for non-herbivores) are okay.
    return true;
}

/**
 * Determine the 'effective' chunk type for a given piece of carrion (chunk or
 * corpse), for the player.
 * E.g., ghouls treat rotting and mutagenic chunks as normal chunks.
 *
 * @param carrion       The actual chunk or corpse.
 * @return              A chunk type corresponding to the effect eating the
 *                      given item will have on the player.
 */
corpse_effect_type determine_chunk_effect(const item_def &carrion)
{
    return determine_chunk_effect(mons_corpse_effect(carrion.mon_type));
}

/**
 * Determine the 'effective' chunk type for a given input for the player.
 * E.g., ghouls/vampires treat rotting and mutagenic chunks as normal chunks.
 *
 * @param chunktype     The actual chunk type.
 * @return              A chunk type corresponding to the effect eating a chunk
 *                      of the given type will have on the player.
 */
corpse_effect_type determine_chunk_effect(corpse_effect_type chunktype)
{
    switch (chunktype)
    {
    case CE_NOXIOUS:
    case CE_MUTAGEN:
        if (you.species == SP_GHOUL || you.species == SP_VAMPIRE)
            chunktype = CE_CLEAN;
        break;

    default:
        break;
    }

    return chunktype;
}

static bool _vampire_consume_corpse(item_def& corpse)
{
    ASSERT(you.species == SP_VAMPIRE);
    ASSERT(corpse.base_type == OBJ_CORPSES);
    ASSERT(corpse.sub_type == CORPSE_BODY);

    if (!mons_has_blood(corpse.mon_type))
    {
        mpr("There is no blood in this body!");
        return false;
    }

    // The delay for eating a chunk (mass 1000) is 2
    // Here the base nutrition value equals that of chunks,
    // but the delay should be smaller.
    const int max_chunks = max_corpse_chunks(corpse.mon_type);
    int duration = 1 + max_chunks / 3;
    duration = stepdown_value(duration, 6, 6, 12, 12);

    // Get some nutrition right away, in case we're interrupted.
    // (-1 for the starting message.)
    vampire_nutrition_per_turn(corpse, -1);

    // The draining delay doesn't have a start action, and we only need
    // the continue/finish messages if it takes longer than 1 turn.
    start_delay<FeedVampireDelay>(duration, corpse);

    return true;
}

static void _heal_from_food(int hp_amt)
{
    if (hp_amt > 0)
        inc_hp(hp_amt);

    if (player_rotted())
    {
        mpr("You feel more resilient.");
        unrot_hp(1);
    }

    calc_hp();
    calc_mp();
}
