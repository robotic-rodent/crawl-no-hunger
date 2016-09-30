/**
 * @file
 * @brief Functions for eating and butchering.
**/

#ifndef FOOD_H
#define FOOD_H

#include "mon-enum.h"

bool eat_food(int slot = -1);

bool is_bad_food(const item_def &food);
bool is_mutagenic(const item_def &food);
bool is_noxious(const item_def &food);
bool is_inedible(const item_def &item);
bool is_preferred_food(const item_def &food);
bool is_forbidden_food(const item_def &food);
corpse_effect_type determine_chunk_effect(const item_def &carrion);
corpse_effect_type determine_chunk_effect(corpse_effect_type chunktype);
mon_intel_type corpse_intelligence(const item_def &corpse);

bool can_eat(const item_def &food, bool suppress_msg, bool check_hunger = true);

bool eat_item(item_def &food);
void finish_eating_item(item_def &food);

int prompt_eat_chunks(bool only_auto = false);

bool prompt_eat_item(int slot = -1);

void vampire_nutrition_per_turn(const item_def &corpse, int feeding = 0);

bool you_foodless(bool can_eat = false);
// Is the player always foodless or just because of a temporary change?
bool you_foodless_normally();
#endif
