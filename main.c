/*
 WarPlan can be used for estimating outcomes of WarFish attack plans.

 There are two modes of operation.  If you run this program and specify 0 bonus armies on the
 command line, then the attack vectors you provided will simply be simulated and the estimated
 outcomes printed.

 If, however, you specify non-zero bonus armies, WarPlan will simulate attack strategies attempting
 to find the optimum allocation of bonus units.  WarPlan will print out the best looking course of
 action for you to take.

 When using the application below, it is important to understand the concept of an attack vector.
 An attack vector specifies the number of units on the territory you'll be attacking from, followed
 by the number of enemy units contained in the sequence of territories you plan to attack.

 For instance, suppose your units are positioned in northern Scotland, and you wish to attack south
 towards London.  In Scotland you have 10 armies, and you plan to attack York, which contains 3
 armies, then Oxford, which contains 2, followed by London, containing 99.

 Your attack vector would then be written: 10:3,2,99


 Example command lines can be seen below:

 Just simulate a single attack vector, no planning:
     ./warplan 1000 0 0 7:3,3,1

 Simulate multiple attack vectors, no planning:
     ./warplan 1000 0 0 7:1,1,2 4:5,1\n"

 Given 10 bonus armies, plan an attack across multiple vectors requiring a win likelihood of 0.8:
     ./warplan 1000 10 0.8 3:2,2 4:1,1,1,1 2:2,1,2
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/param.h>
#include <limits.h>


enum program_args
{
    program_arg_binary_name,
    program_arg_sim_iterations,
    program_arg_bonus_units,
    program_arg_likelihood_threshold,
    program_arg_attack_vector
};

enum program_flags
{
    enable_debugging = 0x01
};

enum combinations_state
{
    combinations_exhausted,
    combinations_remain
};

#define DEBUG_ENV_NAME "DEBUG_WARPLAN"

#define MAX_TERRITORY_VECTOR_SIZE 128
#define MAX_ATTACK_VECTORS        16
#define MAX_DICE_STRING_SIZE      512

#define MIN_TERRITORY_UNITS   1
#define MAX_DICE_COUNT        3
#define MAX_ATTACK_DICE_COUNT 3
#define MAX_DEFEND_DICE_COUNT 2

#define DICE_SIDES          6
#define MAX_DICE_RAND_VALUE ((UINT_MAX/DICE_SIDES)*DICE_SIDES)

#define PLANS_SIZE_INCREMENT 100000


struct territory_def
{
    unsigned int units;
};

struct attack_vector_def
{
    char* def_string;

    unsigned int units_on_front;

    struct territory_def territory_vector[MAX_TERRITORY_VECTOR_SIZE];
    unsigned int         territory_count;
};

struct attack_result
{
    unsigned int conquered_territory_count;
    unsigned int units_on_front;
    unsigned int enemy_units_on_front;
};

struct attack_prediction
{
    float win_likelihood;
    float estimated_remaining_units_if_win;
    float estimated_remaining_enemies_if_loss;
    float estimated_remaining_territories_if_loss;

    unsigned int win_count;
    unsigned int loss_count;
};

struct attack_setup
{
    struct attack_prediction  prediction;
    struct attack_vector_def* attack_vector;
    unsigned int              bonus;
    float                     score;
};

struct attack_plan
{
    float total_score;

    struct attack_setup* setups[MAX_ATTACK_VECTORS];
    size_t               setup_count;
};


static enum program_flags run_flags;


static inline void
Abort (char* reason)
{
    printf("Aborting: %s\n", reason);
    exit(EXIT_FAILURE);
}

static inline void
Debug (char* format_string, ...)
{
    va_list arg_list;

    if(!(run_flags&enable_debugging))
        return;

    va_start(arg_list, format_string);
    vprintf(format_string, arg_list);
    va_end(arg_list);
}

static inline void
ParseAttackVector (char* def_string, struct attack_vector_def* attack_vector)
{
    char*                 param;
    char*                 dup_def_string;
    struct territory_def* territory_vector;
    unsigned int          territory_count;

    dup_def_string = strdup(def_string);
    if(dup_def_string == NULL)
        Abort("Failed to alloc memory");

    param = strtok(dup_def_string, ":");
    if(param == NULL)
        goto invalid_def_string;

    attack_vector->units_on_front = (unsigned int)atoi(param);

    territory_vector = attack_vector->territory_vector;
    territory_count  = 0;

    while((param = strtok(NULL, ",")) != NULL)
    {
        territory_vector[territory_count].units = (unsigned int)atoi(param);
        territory_count++;
    }

    if(territory_count == 0)
        goto invalid_def_string;

    attack_vector->def_string      = def_string;
    attack_vector->territory_count = territory_count;

    free(dup_def_string);

    return;

invalid_def_string:
    Abort("Malformed attack vector string, see usage");
}

static inline void
PrintPrediction (char* vector_def_string, struct attack_prediction* prediction)
{
    printf(
           "Attack vector '%s' prediction\n"
           "\tWin count: %u Loss count: %u\n",
           vector_def_string,
           prediction->win_count,
           prediction->loss_count
          );

    if(prediction->win_count > 0)
    {
        printf(
               "\tWin likelihood: %.2f with %.2f units remaning\n",
               prediction->win_likelihood,
               prediction->estimated_remaining_units_if_win
              );
    }
    else
        printf("\tWin likelihood: 0 this is a debo move\n");

    if(prediction->loss_count > 0)
    {
        printf(
               "\t\tIf loss, %.2f remaining territories with %.2f enemies total\n",
               prediction->estimated_remaining_enemies_if_loss,
               prediction->estimated_remaining_territories_if_loss
              );
    }
}

static inline void
PrintSetup (struct attack_setup* setup)
{
    char* def_string;

    def_string = setup->attack_vector->def_string;

    printf(
           "%u bonus armies to attack vector '%s'\n",
           setup->bonus,
           def_string
          );
    PrintPrediction(def_string, &setup->prediction);
}

static inline void
DiceToString (unsigned int* dice, unsigned int count, char* string)
{
    char*        cursor;
    unsigned int comma_sep_count;

    cursor          = string;
    comma_sep_count = count-1;

    for(unsigned int index = 0; index < comma_sep_count; index++)
        cursor += sprintf(cursor, "%u, ", dice[index]);

    sprintf(cursor, "%u", dice[comma_sep_count]);
}

static int
CompareDice (const void* left, const void* right)
{
    unsigned int left_value;
    unsigned int right_value;
    int          delta;

    left_value  = *(unsigned int*)left;
    right_value = *(unsigned int*)right;

    delta = (int)right_value-(int)left_value;

    return delta;
}

static int
ComparePlan (const void* left, const void* right)
{
    const struct attack_plan* left_plan;
    const struct attack_plan* right_plan;

    left_plan  = left;
    right_plan = right;

    if(right_plan->total_score > left_plan->total_score)
        return 1;
    else if(right_plan->total_score < left_plan->total_score)
        return -1;

    return 0;
}

static inline unsigned int
UniformDiceRoll (void)
{
    unsigned int rand_value;
    unsigned int dice_value;

    do
    {
        rand_value = rand();
    }while(rand_value >= MAX_DICE_RAND_VALUE);

    dice_value = (rand_value%DICE_SIDES)+1;

    return dice_value;
}

static inline void
RollDice (unsigned int* dice, unsigned int count)
{
    for(size_t index = count; index-- > 0;)
        dice[index] = UniformDiceRoll();

    qsort(dice, count, sizeof(unsigned int), &CompareDice);
}

static inline void
SingleAttack (
              unsigned int  units_on_front,
              unsigned int  territory_units,
              unsigned int* remaining_units_on_front,
              unsigned int* remaining_territory_units
             )
{
    unsigned int attack_dice[MAX_DICE_COUNT];
    unsigned int defend_dice[MAX_DICE_COUNT];
    unsigned int attack_unit_count;
    unsigned int attack_dice_count;
    unsigned int defend_unit_count;
    unsigned int defend_dice_count;
    unsigned int compare_dice_count;
    unsigned int lost_attack_units;
    unsigned int lost_defend_units;

    attack_unit_count = units_on_front-MIN_TERRITORY_UNITS;
    attack_dice_count = MIN(attack_unit_count, MAX_ATTACK_DICE_COUNT);

    defend_unit_count = territory_units;
    defend_dice_count = MIN(defend_unit_count, MAX_DEFEND_DICE_COUNT);

    RollDice(attack_dice, attack_dice_count);
    RollDice(defend_dice, defend_dice_count);

    compare_dice_count = MIN(attack_dice_count, defend_dice_count);
    lost_attack_units  = 0;
    lost_defend_units  = 0;

    for(unsigned int index = 0; index < compare_dice_count; index++)
    {
        if(attack_dice[index] > defend_dice[index])
            lost_defend_units++;
        else
            lost_attack_units++;
    }

    if(run_flags&enable_debugging)
    {
        char attack_dice_string[MAX_DICE_STRING_SIZE];
        char defend_dice_string[MAX_DICE_STRING_SIZE];

        DiceToString(attack_dice, attack_dice_count, attack_dice_string);
        DiceToString(defend_dice, defend_dice_count, defend_dice_string);

        Debug(
              "%u [%s] vs %u [%s] = %u front units lost and %u defending units lost\n",
              units_on_front,
              attack_dice_string,
              defend_unit_count,
              defend_dice_string,
              lost_attack_units,
              lost_defend_units
             );
    }

    *remaining_units_on_front  = units_on_front-lost_attack_units;
    *remaining_territory_units = territory_units-lost_defend_units;
}

static inline void
AttackTerritory (
                 unsigned int          units_on_front,
                 struct territory_def* territory,
                 unsigned int*         remaining_units_on_front,
                 unsigned int*         remaining_territory_units
                )
{
    unsigned int front_units;
    unsigned int territory_units;

    front_units     = units_on_front;
    territory_units = territory->units;

    while(front_units > MIN_TERRITORY_UNITS && territory_units > 0)
    {
        SingleAttack(
                     front_units,
                     territory_units,
                     &front_units,
                     &territory_units
                    );
    }

    *remaining_units_on_front  = front_units;
    *remaining_territory_units = territory_units;
}

static inline void
SimAttack (
           struct attack_vector_def* attack_vector,
           unsigned int              bonus_units,
           struct attack_result*     result
          )
{
    struct territory_def* territory_cursor;
    struct territory_def* territory_vector;
    unsigned int          units_on_front;
    unsigned int          remaining_units_on_front;
    unsigned int          remaining_territory_units;

    units_on_front   = attack_vector->units_on_front+bonus_units;
    territory_vector = attack_vector->territory_vector;

    territory_cursor = territory_vector;

    for(unsigned int size = attack_vector->territory_count; size-- > 0;)
    {
        Debug(
              "Attacking %u vs %u\n"
              "------------------\n",
              units_on_front,
              territory_cursor->units
             );

        AttackTerritory(
                        units_on_front,
                        territory_cursor,
                        &remaining_units_on_front,
                        &remaining_territory_units
                       );

        Debug("\n");

        if(remaining_territory_units == 0)
            units_on_front = remaining_units_on_front-MIN_TERRITORY_UNITS;
        else
        {
            units_on_front = remaining_units_on_front;

            Debug(
                  "Attack failed with %u vs %u remaining\n\n",
                  units_on_front,
                  remaining_territory_units
                 );

            break;
        }

        territory_cursor++;
    }

    result->conquered_territory_count = territory_cursor-territory_vector;
    result->units_on_front            = units_on_front;
    result->enemy_units_on_front      = remaining_territory_units;
}

static inline void
PredictAttack (
               struct attack_vector_def* attack_vector,
               unsigned int              bonus_units,
               unsigned int              sim_iterations,
               struct attack_prediction* prediction
              )
{
    unsigned int win_count;
    unsigned int total_units_on_front;
    unsigned int loss_count;
    unsigned int total_enemy_units_remaining;
    unsigned int total_territories_remaining;

    win_count                   = 0;
    total_units_on_front        = 0;
    loss_count                  = 0;
    total_enemy_units_remaining = 0;
    total_territories_remaining = 0;

    for(size_t remaining = sim_iterations; remaining-- > 0;)
    {
        struct attack_result result;

        Debug(
              "Beginning simulation of attack vector '%s'\n"
              "------------------------------------------\n",
              attack_vector->def_string
             );

        SimAttack(attack_vector, bonus_units, &result);

        if(result.enemy_units_on_front == 0)
        {
            win_count++;
            total_units_on_front += result.units_on_front;
        }
        else
        {
            struct territory_def* territories;
            unsigned int          enemy_units_remaining;
            unsigned int          territory_count;

            territories           = attack_vector->territory_vector;
            territory_count       = attack_vector->territory_count;
            enemy_units_remaining = result.enemy_units_on_front;

            for(
                unsigned int index = result.conquered_territory_count+1;
                index < territory_count;
                index++
               )
            {
                enemy_units_remaining += territories[index].units;
            }

            loss_count++;
            total_enemy_units_remaining += enemy_units_remaining;
            total_territories_remaining += territory_count-result.conquered_territory_count;
        }
    }

    prediction->win_likelihood                          = (float)win_count/(float)(win_count+loss_count);
    prediction->estimated_remaining_units_if_win        = (float)total_units_on_front/(float)win_count;
    prediction->estimated_remaining_enemies_if_loss     = (float)total_enemy_units_remaining/(float)loss_count;
    prediction->estimated_remaining_territories_if_loss = (float)total_territories_remaining/(float)loss_count;
    prediction->win_count                               = win_count;
    prediction->loss_count                              = loss_count;
}

static inline void
SimWar (
        struct attack_vector_def* attack_vectors,
        size_t                    count,
        unsigned int              bonus_units,
        unsigned int              sim_iterations
       )
{
    for(size_t index = 0; index < count; index++)
    {
        struct attack_prediction  prediction;
        struct attack_vector_def* attack_vector;

        attack_vector = &attack_vectors[index];

        PredictAttack(attack_vector, bonus_units, sim_iterations, &prediction);

        printf("\n");
        PrintPrediction(attack_vector->def_string, &prediction);
    }
}

static inline void
InitCombinations (unsigned int* indices, size_t index_count)
{
    for(size_t index = index_count; index-- > 0;)
        indices[index] = 0;
}

static inline enum combinations_state
NextCombination (unsigned int* indices, size_t index_count, unsigned int last_index)
{
    size_t index;

    index = 0;

    do
    {
        indices[index]++;
        if(indices[index] <= last_index)
            return combinations_remain;

        indices[index] = 0;

        index++;
    }while(index < index_count);

    return combinations_exhausted;
}

static inline void
PlanWar (
         struct attack_vector_def* attack_vectors,
         size_t                    attack_vector_count,
         unsigned int              bonus_units,
         float                     likelihood_threshold,
         unsigned int              sim_iterations
        )
{
    struct attack_setup setups[attack_vector_count][bonus_units+1];
    unsigned int        bonus_indices[attack_vector_count];
    struct attack_plan* plans;
    struct attack_plan* cursor;
    size_t              plans_size;
    size_t              plans_count;

    plans_count = 0;
    plans_size  = PLANS_SIZE_INCREMENT;
    plans       = malloc(plans_size*sizeof(struct attack_plan));
    if(plans == NULL)
        Abort("Memory alloc for plans failed");

    for(size_t index = 0; index < attack_vector_count; index++)
    {
        struct attack_vector_def* attack_vector;

        attack_vector = &attack_vectors[index];
        for(unsigned int bonus = 0; bonus <= bonus_units; bonus++)
        {
            struct attack_setup* setup;
            float                win_likelihood;

            setup = &setups[index][bonus];

            setup->attack_vector = attack_vector;
            setup->bonus         = bonus;

            PredictAttack(
                          attack_vector,
                          bonus,
                          sim_iterations,
                          &setup->prediction
                         );

            win_likelihood = setup->prediction.win_likelihood;

            if(win_likelihood >= likelihood_threshold)
                setup->score = win_likelihood;
            else
                setup->score = 0;
        }
    }

    InitCombinations(bonus_indices, attack_vector_count);

    cursor = plans;

    do
    {
        unsigned int total_bonus;

        total_bonus = 0;

        cursor->total_score = 0;
        cursor->setup_count = attack_vector_count;

        for(size_t index = attack_vector_count; index-- > 0;)
        {
            struct attack_setup* setup;
            unsigned int         bonus;

            bonus = bonus_indices[index];
            setup = &setups[index][bonus];

            total_bonus += bonus;

            cursor->setups[index]  = setup;
            cursor->total_score   += setup->score;
        }

        if(total_bonus != bonus_units)
            continue;

        plans_count++;
        if(plans_count >= plans_size)
        {
            plans_size += PLANS_SIZE_INCREMENT;
            plans       = realloc(plans, plans_size*sizeof(struct attack_plan));
            if(plans == NULL)
                Abort("Error reallocing space for attack plans");
        }

        cursor = &plans[plans_count];
    }while(NextCombination(bonus_indices, attack_vector_count, bonus_units) == combinations_remain);

    qsort(plans, plans_count, sizeof(struct attack_plan), &ComparePlan);

    printf("Highest scoring setup is below\n");

    for(size_t index = 0; index < attack_vector_count; index++)
        PrintSetup(plans[0].setups[index]);
}

int
main (int arg_count, char** args)
{
    struct attack_vector_def attack_vectors[MAX_ATTACK_VECTORS];
    char*                    debug_env;
    size_t                   attack_vector_count;
    float                    likelihood_threshold;
    unsigned int             sim_iterations;
    unsigned int             bonus_units;

    run_flags = 0;

    debug_env = getenv(DEBUG_ENV_NAME);
    if(debug_env != NULL)
        run_flags |= enable_debugging;

    if(arg_count <= program_arg_attack_vector)
        goto print_usage;

    sim_iterations       = (unsigned int)atoi(args[program_arg_sim_iterations]);
    bonus_units          = (unsigned int)atoi(args[program_arg_bonus_units]);
    likelihood_threshold = (float)atof(args[program_arg_likelihood_threshold]);

    attack_vector_count = 0;
    for(size_t index = program_arg_attack_vector; index < arg_count; index++)
    {
        ParseAttackVector(args[index], &attack_vectors[attack_vector_count]);
        attack_vector_count++;
    }

    if(bonus_units == 0)
    {
        printf("Simulating simple war and printing predictions\n\n");

        SimWar(attack_vectors, attack_vector_count, bonus_units, sim_iterations);
    }
    else
    {
        printf("Attempting to plan war for specified vectors\n\n");

        PlanWar(
                attack_vectors,
                attack_vector_count,
                bonus_units,
                likelihood_threshold,
                sim_iterations
               );
    }

    return EXIT_SUCCESS;

print_usage:
    printf(
           "Usage: warplan [simulation iterations] [bonus units] [win threshold] [attack vectors]\n"
           "\n"
           "Attack vectors are formatted as: "
           "[units on front]:[enemy territory 1 units],[enemy territory n units]\n"
           "\n"
           "Examples:\n\n"
           "\tJust simulate a single attack vector, no planning:\n"
           "\t\twarplan 1000 0 0 7:3,3,1\n"
           "\n"
           "\tSimulate multiple attack vectors, no planning:\n"
           "\t\twarplan 1000 0 0 7:1,1,2 4:5,1\n"
           "\n"
           "\tGiven 10 bonus armies, plan an attack across multiple vectors requiring a win likelihood of 0.8:\n"
           "\t\twarplan 1000 10 0.8 3:2,2 4:1,1,1,1 2:2,1,2\n"
          );

    return EXIT_FAILURE;
}
