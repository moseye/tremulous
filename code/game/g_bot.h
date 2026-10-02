/* Tremulous server-side bots. GPL-2.0-or-later; see GPL. */
#ifndef G_BOT_H
#define G_BOT_H

typedef enum { BOT_ATTACK, BOT_DEFEND, BOT_BUILD } botRole_t;

typedef struct
{
  qboolean active;
  team_t team;
  int skill;
  botRole_t role;
  int target;
  int nextThink;
  int nextSpawn;
  int nextEnemyScan;
  int nextEconomyTime;
  int nextShotTime;
  int aimTime;
  int spawnCount;
  vec3_t lastOrigin;
  int stuckTime;
  usercmd_t cmd;
} botState_t;

extern botState_t g_botStates[ MAX_CLIENTS ];
extern vmCvar_t g_botThink, g_botSkill, g_botBuild, g_botDebug;

void G_BotInit( void );
void G_BotFrame( void );
void G_BotDisconnect( int clientNum );
void G_BotShutdown( void );
qboolean G_BotConsoleCommand( void );
qboolean G_BotIsBot( int clientNum );
void G_BotAim( gentity_t *ent, usercmd_t *cmd, const vec3_t point );
gentity_t *G_BotFindBuildable( gentity_t *ent, buildable_t type, float range );

void G_BotNavInit( void );
void G_BotNavFrame( void );
void G_BotNavMove( gentity_t *ent, botState_t *bot, const vec3_t goal,
                   usercmd_t *cmd, qboolean faceGoal );
void G_BotNavReset( int clientNum );
void G_BotNavStatus( void );
void G_BotNavSafeMove( gentity_t *ent, usercmd_t *cmd );
qboolean G_BotNavConsoleCommand( const char *command );

qboolean G_BotCombatThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd );
gentity_t *G_BotEconomyThink( gentity_t *ent, botState_t *bot );
void G_BotChooseSpawn( gentity_t *ent, botState_t *bot );

void G_BotBuildInit( void );
void G_BotBuildReset( int clientNum );
qboolean G_BotBuildThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd );

#endif
