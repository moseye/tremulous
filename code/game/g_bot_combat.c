/*
 * Server-side bot combat and shopping. GPL-2.0-or-later; see GPL.
 * Actions go through player movement, weapons and the normal shop/evolve checks.
 */
#include "g_local.h"
#include "g_bot.h"

/* Also used by their corresponding console command wrappers in g_cmds.c. */
void G_ChangeClass( gentity_t *ent, const char *name );
void G_BuyItem( gentity_t *ent, const char *name );
void G_SellItem( gentity_t *ent, const char *name );
void Cmd_Reload_f( gentity_t *ent );
qboolean G_RoomForClassChange( gentity_t *ent, class_t class, vec3_t newOrigin );

static qboolean BotEnemy( gentity_t *ent, gentity_t *other )
{
  if( !other->inuse || other == ent || other->health <= 0 ||
      ( other->flags & FL_NOTARGET ) )
    return qfalse;

  if( other->client )
    return other->client->pers.connected == CON_CONNECTED &&
           other->client->sess.spectatorState == SPECTATOR_NOT &&
           other->client->pers.teamSelection != TEAM_NONE &&
           other->client->pers.teamSelection != ent->client->pers.teamSelection;

  return other->s.eType == ET_BUILDABLE &&
         other->buildableTeam != ent->client->pers.teamSelection;
}

static void BotTargetPoint( gentity_t *target, vec3_t point )
{
  int i;

  /* The centre of the actual collision bounds works for wall-climbing clients. */
  for( i = 0; i < 3; i++ )
    point[ i ] = target->r.currentOrigin[ i ] +
                 ( target->r.mins[ i ] + target->r.maxs[ i ] ) * 0.5f;
}

static qboolean BotVisible( gentity_t *ent, gentity_t *target )
{
  vec3_t eye, point;
  trace_t tr;

  BG_GetClientViewOrigin( &ent->client->ps, eye );
  BotTargetPoint( target, point );
  trap_Trace( &tr, eye, NULL, NULL, point, ent->s.number, MASK_SHOT );
  return tr.fraction == 1.0f || tr.entityNum == target->s.number;
}

static gentity_t *BotSelectTarget( gentity_t *ent, botState_t *bot )
{
  int i;
  float distance, score, bestScore = -100000.0f;
  float range = bot->role == BOT_BUILD ? 600.0f : 2200.0f;
  vec3_t delta;
  gentity_t *other, *best = NULL;

  if( level.time < bot->nextEnemyScan )
  {
    if( bot->target >= 0 && bot->target < level.num_entities )
    {
      other = &g_entities[ bot->target ];
      if( BotEnemy( ent, other ) && BotVisible( ent, other ) )
        return other;
    }
    bot->target = -1;
    return NULL;
  }

  bot->nextEnemyScan = level.time + 250;
  for( i = 0; i < level.num_entities; i++ )
  {
    other = &g_entities[ i ];
    if( !BotEnemy( ent, other ) )
      continue;

    VectorSubtract( other->r.currentOrigin, ent->r.currentOrigin, delta );
    distance = VectorLength( delta );
    if( distance > range || !BotVisible( ent, other ) )
      continue;

    score = -distance;
    if( other->client )
      score += 450.0f;
    else if( BG_Buildable( other->s.modelindex )->turretRange > 0 )
      score += 200.0f;
    else if( other->s.modelindex == BA_A_OVERMIND ||
             other->s.modelindex == BA_H_REACTOR )
      score += 150.0f;

    /* Keep a target until another is clearly more urgent. */
    if( i == bot->target )
      score += 180.0f;
    if( score > bestScore )
    {
      bestScore = score;
      best = other;
    }
  }

  if( best && bot->target != best->s.number )
    bot->aimTime = level.time + 80 + ( 10 - bot->skill ) * 45;
  bot->target = best ? best->s.number : -1;
  return best;
}

static float BotMeleeRange( weapon_t weapon )
{
  switch( weapon )
  {
    case WP_ALEVEL0: return LEVEL0_BITE_RANGE;
    case WP_ALEVEL1: return LEVEL1_CLAW_RANGE;
    case WP_ALEVEL1_UPG: return LEVEL1_CLAW_U_RANGE;
    case WP_ALEVEL2: return LEVEL2_CLAW_RANGE;
    case WP_ALEVEL2_UPG: return LEVEL2_CLAW_U_RANGE;
    case WP_ALEVEL3: return LEVEL3_CLAW_RANGE;
    case WP_ALEVEL3_UPG: return LEVEL3_CLAW_UPG_RANGE;
    case WP_ALEVEL4: return LEVEL4_CLAW_RANGE;
    case WP_PAIN_SAW: return PAINSAW_RANGE;
    case WP_ABUILD: return ABUILDER_CLAW_RANGE;
    case WP_ABUILD2: return ABUILDER_CLAW_RANGE;
    default: return 0.0f;
  }
}

static float BotProjectileSpeed( weapon_t weapon, qboolean barb )
{
  if( barb )
    return weapon == WP_ABUILD2 ? ABUILDER_BLOB_SPEED : LEVEL3_BOUNCEBALL_SPEED;
  switch( weapon )
  {
    case WP_BLASTER: return BLASTER_SPEED;
    case WP_PULSE_RIFLE: return PRIFLE_SPEED;
    case WP_FLAMER: return FLAMER_SPEED;
    case WP_LUCIFER_CANNON: return LCANNON_SPEED;
    default: return 0.0f;
  }
}

static float BotPreferredRange( weapon_t weapon )
{
  switch( weapon )
  {
    case WP_SHOTGUN: return 200.0f;
    case WP_FLAMER: return 190.0f;
    case WP_CHAINGUN: return 350.0f;
    case WP_MASS_DRIVER: return 650.0f;
    case WP_LUCIFER_CANNON: return 550.0f;
    default: return 450.0f;
  }
}

static qboolean BotSafeShot( gentity_t *ent, gentity_t *enemy,
                             usercmd_t *cmd, float range )
{
  vec3_t eye, angles, forward, end, axis[ 3 ], rotated[ 3 ];
  trace_t tr;
  gentity_t *hit;
  int i;

  BG_GetClientViewOrigin( &ent->client->ps, eye );
  for( i = 0; i < 3; i++ )
    angles[ i ] = SHORT2ANGLE( cmd->angles[ i ] +
                              ent->client->ps.delta_angles[ i ] );
  AnglesToAxis( angles, axis );
  if( ( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) &&
      BG_RotateAxis( ent->client->ps.grapplePoint, axis, rotated, qfalse,
                     ent->client->ps.eFlags & EF_WALLCLIMBCEILING ) )
    VectorCopy( rotated[ 0 ], forward );
  else
    VectorCopy( axis[ 0 ], forward );
  VectorMA( eye, range, forward, end );
  trap_Trace( &tr, eye, NULL, NULL, end, ent->s.number, MASK_SHOT );

  if( tr.entityNum < 0 || tr.entityNum >= level.num_entities )
    return qtrue;
  hit = &g_entities[ tr.entityNum ];
  if( hit == enemy )
    return qtrue;
  if( hit->client &&
      hit->client->pers.teamSelection == ent->client->pers.teamSelection )
    return qfalse;
  if( hit->s.eType == ET_BUILDABLE &&
      hit->buildableTeam == ent->client->pers.teamSelection )
    return qfalse;
  return tr.fraction == 1.0f || BotEnemy( ent, hit );
}

qboolean G_BotCombatThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd )
{
  gentity_t *enemy;
  playerState_t *ps = &ent->client->ps;
  weapon_t weapon = ps->stats[ STAT_WEAPON ];
  vec3_t eye, point, aim, delta;
  float distance, edgeDistance, melee, speed, time, error, preferred;
  qboolean barb = qfalse, canFire;
  int i, attack = BUTTON_ATTACK;

  enemy = BotSelectTarget( ent, bot );
  if( !enemy )
    return qfalse;

  if( bot->team == TEAM_HUMANS )
  {
    if( weapon == WP_HBUILD ||
        ( !BG_Weapon( weapon )->infiniteAmmo && ps->ammo == 0 && ps->clips == 0 ) )
      weapon = WP_BLASTER;
    if( ps->weapon != weapon && ps->weaponstate != WEAPON_DROPPING &&
        ps->weaponstate != WEAPON_RAISING && BG_PlayerCanChangeWeapon( ps ) )
      G_ForceWeaponChange( ent, weapon );
  }
  else
    weapon = ps->weapon;
  cmd->weapon = weapon;

  if( weapon == WP_ABUILD || weapon == WP_ABUILD2 )
    attack = BUTTON_ATTACK2;

  BG_GetClientViewOrigin( ps, eye );
  BotTargetPoint( enemy, point );
  VectorSubtract( point, eye, delta );
  distance = VectorLength( delta );
  /* Melee range is measured to the surface, rather than an HQ's centre. */
  edgeDistance = distance;
  for( i = 0; i < 3; i++ )
  {
    float lo = enemy->r.currentOrigin[ i ] + enemy->r.mins[ i ];
    float hi = enemy->r.currentOrigin[ i ] + enemy->r.maxs[ i ];
    if( eye[ i ] < lo ) delta[ i ] = lo - eye[ i ];
    else if( eye[ i ] > hi ) delta[ i ] = eye[ i ] - hi;
    else delta[ i ] = 0;
  }
  edgeDistance = VectorLength( delta );
  melee = BotMeleeRange( weapon );

  if( ( weapon == WP_ALEVEL3_UPG && ps->ammo > 0 &&
        distance > 220.0f && distance < 1100.0f ) ||
      ( weapon == WP_ABUILD2 && distance > 110.0f && distance < 600.0f ) )
  {
    barb = qtrue;
    attack = BUTTON_USE_HOLDABLE;
  }

  VectorCopy( point, aim );
  speed = BotProjectileSpeed( weapon, barb );
  if( speed > 0.0f && enemy->client )
  {
    time = distance / speed;
    if( time > 1.2f ) time = 1.2f;
    VectorMA( point, time * ( 0.55f + bot->skill * 0.045f ),
              enemy->client->ps.velocity, aim );
  }
  if( barb )
  {
    time = distance / speed;
    if( time > 1.2f ) time = 1.2f;
    aim[ 2 ] += 0.5f * g_gravity.value * time * time;
  }
  error = distance * ( 11 - bot->skill ) * 0.0018f;
  if( error > 45.0f ) error = 45.0f;
  for( i = 0; i < 3; i++ )
    aim[ i ] += crandom( ) * error;
  G_BotAim( ent, cmd, aim );

  G_BotNavMove( ent, bot, enemy->r.currentOrigin, cmd, qfalse );
  if( melee > 0.0f && !barb )
  {
    if( edgeDistance < melee * 0.55f )
      cmd->forwardmove = 0;
  }
  else
  {
    preferred = BotPreferredRange( weapon );
    if( distance < preferred * 1.25f )
    {
      /* A visible opponent permits simple strafing without path detours. */
      cmd->forwardmove = distance < preferred * 0.55f ? -80 : 0;
      cmd->rightmove = ( ( level.time / 1600 + ent->s.number ) & 1 ) ? 80 : -80;
      cmd->buttons &= ~BUTTON_SPRINT;
    }
  }

  canFire = level.time >= bot->aimTime &&
            BotSafeShot( ent, enemy, cmd, distance + 100.0f );
  if( weapon == WP_ALEVEL2_UPG && edgeDistance < LEVEL2_AREAZAP_RANGE )
  {
    attack = BUTTON_ATTACK2;
    melee = LEVEL2_AREAZAP_RANGE;
  }
  /* Cloud affects human players only. Keep claws against structures and at
   * melee range, and do not repeatedly refresh an existing cloud effect. */
  if( weapon == WP_ALEVEL1_UPG && enemy->client &&
      enemy->client->pers.teamSelection == TEAM_HUMANS &&
      edgeDistance >= melee && edgeDistance < LEVEL1_PCLOUD_RANGE &&
      !( enemy->client->ps.eFlags & EF_POISONCLOUDED ) &&
      level.time >= bot->nextShotTime && ps->weaponTime <= 0 )
  {
    attack = BUTTON_ATTACK2;
    melee = LEVEL1_PCLOUD_RANGE;
  }
  if( melee > 0.0f && !barb )
    canFire = canFire && edgeDistance < melee;
  if( weapon == WP_FLAMER && distance > FLAMER_SPEED * FLAMER_LIFETIME / 1000.0f )
    canFire = qfalse;

  /* Ability buttons are handled by Pmove just as they are for real clients. */
  if( weapon == WP_ALEVEL3 || weapon == WP_ALEVEL3_UPG )
  {
    if( !barb && distance > 130.0f && distance < 700.0f &&
        level.time >= bot->aimTime && ps->weaponTime <= 0 )
    {
      if( ps->stats[ STAT_MISC ] <
          ( weapon == WP_ALEVEL3 ? LEVEL3_POUNCE_TIME : LEVEL3_POUNCE_TIME_UPG ) * 0.8f )
        cmd->buttons |= BUTTON_ATTACK2;
      /* Releasing the button launches the pounce. */
    }
  }
  else if( weapon == WP_ALEVEL4 && distance > 150.0f && distance < 900.0f )
  {
    cmd->forwardmove = 127;
    cmd->rightmove = 0;
    if( !( ps->stats[ STAT_STATE ] & SS_CHARGING ) &&
        ps->stats[ STAT_MISC ] < LEVEL4_TRAMPLE_CHARGE_MAX )
      cmd->buttons |= BUTTON_ATTACK2;
  }
  else if( ( weapon == WP_ALEVEL2 || weapon == WP_ALEVEL2_UPG ) &&
           distance > 140.0f && distance < 500.0f &&
           ps->groundEntityNum != ENTITYNUM_NONE &&
           ( ( level.time / 700 + ent->s.number ) & 1 ) )
    cmd->upmove = 127;

  if( canFire )
  {
    if( weapon == WP_LUCIFER_CANNON )
    {
      /* Release below the warning threshold, avoiding perpetual overcharge. */
      if( ps->stats[ STAT_MISC ] < LCANNON_CHARGE_TIME_WARN - 100 )
        cmd->buttons |= attack;
    }
    else
    {
      cmd->buttons |= attack;
      if( weapon == WP_ALEVEL1_UPG && attack == BUTTON_ATTACK2 )
        bot->nextShotTime = level.time + LEVEL1_PCLOUD_REPEAT;
    }
  }
  return qtrue;
}

static qboolean BotWeaponAllowed( weapon_t weapon )
{
  return BG_Weapon( weapon )->purchasable &&
         BG_Weapon( weapon )->team == TEAM_HUMANS &&
         BG_WeaponAllowedInStage( weapon, g_humanStage.integer ) &&
         BG_WeaponIsAllowed( weapon );
}

static qboolean BotUpgradeAllowed( gentity_t *ent, upgrade_t upgrade )
{
  return !BG_InventoryContainsUpgrade( upgrade, ent->client->ps.stats ) &&
         BG_Upgrade( upgrade )->purchasable &&
         BG_Upgrade( upgrade )->team == TEAM_HUMANS &&
         BG_UpgradeAllowedInStage( upgrade, g_humanStage.integer ) &&
         BG_UpgradeIsAllowed( upgrade ) &&
         BG_Upgrade( upgrade )->price <= ent->client->pers.credit &&
         !( BG_Upgrade( upgrade )->slots & BG_SlotsForInventory( ent->client->ps.stats ) );
}

static int BotWeaponValue( weapon_t weapon, botState_t *bot )
{
  switch( weapon )
  {
    case WP_MACHINEGUN: return 10;
    case WP_SHOTGUN: return 20;
    case WP_LAS_GUN: return 30;
    case WP_MASS_DRIVER: return bot->skill >= 7 ? 42 : 25;
    case WP_CHAINGUN: return 45;
    case WP_FLAMER: return 35;
    case WP_PULSE_RIFLE: return 50;
    case WP_LUCIFER_CANNON: return bot->skill >= 6 ? 60 : 40;
    default: return 0;
  }
}

static weapon_t BotDesiredWeapon( gentity_t *ent, botState_t *bot )
{
  weapon_t current = ent->client->ps.stats[ STAT_WEAPON ], desired = current;
  int i, budget = ent->client->pers.credit;
  int best = BotWeaponValue( current, bot );

  if( bot->role == BOT_BUILD )
    return BG_WeaponIsAllowed( WP_HBUILD ) ? WP_HBUILD : current;
  if( BG_Weapon( current )->purchasable )
    budget += BG_Weapon( current )->price;
  for( i = WP_MACHINEGUN; i <= WP_LUCIFER_CANNON; i++ )
  {
    if( BotWeaponAllowed( i ) && BG_Weapon( i )->price <= budget &&
        BotWeaponValue( i, bot ) > best )
    {
      desired = i;
      best = BotWeaponValue( i, bot );
    }
  }
  return desired;
}

static void BotShop( gentity_t *ent, botState_t *bot )
{
  weapon_t current = ent->client->ps.stats[ STAT_WEAPON ];
  weapon_t desired = BotDesiredWeapon( ent, bot );
  vec3_t suitOrigin;
  int i, suitBudget = ent->client->pers.credit;
  static const upgrade_t suitConflicts[ ] =
  {
    UP_LIGHTARMOUR, UP_HELMET, UP_BATTPACK, UP_JETPACK
  };

  if( desired != current && BotWeaponAllowed( desired ) &&
      BG_PlayerCanChangeWeapon( &ent->client->ps ) )
  {
    if( current != WP_NONE )
      G_SellItem( ent, BG_Weapon( current )->name );
    /* A build delay or other player rule may have refused the sale. */
    if( ent->client->ps.stats[ STAT_WEAPON ] == WP_NONE )
      G_BuyItem( ent, BG_Weapon( desired )->name );
  }

  suitBudget = ent->client->pers.credit;
  for( i = 0; i < sizeof( suitConflicts ) / sizeof( suitConflicts[ 0 ] ); i++ )
    if( BG_InventoryContainsUpgrade( suitConflicts[ i ], ent->client->ps.stats ) )
      suitBudget += BG_Upgrade( suitConflicts[ i ] )->price;
  if( !BG_InventoryContainsUpgrade( UP_BATTLESUIT, ent->client->ps.stats ) &&
      BG_UpgradeAllowedInStage( UP_BATTLESUIT, g_humanStage.integer ) &&
      BG_UpgradeIsAllowed( UP_BATTLESUIT ) &&
      suitBudget >= BG_Upgrade( UP_BATTLESUIT )->price + 100 &&
      G_RoomForClassChange( ent, PCL_HUMAN_BSUIT, suitOrigin ) )
  {
    /* Test room before selling gear; purchases still use all player checks. */
    for( i = 0; i < sizeof( suitConflicts ) / sizeof( suitConflicts[ 0 ] ); i++ )
      if( BG_InventoryContainsUpgrade( suitConflicts[ i ], ent->client->ps.stats ) )
        G_SellItem( ent, BG_Upgrade( suitConflicts[ i ] )->name );
    G_BuyItem( ent, BG_Upgrade( UP_BATTLESUIT )->name );
  }
  if( BotUpgradeAllowed( ent, UP_LIGHTARMOUR ) )
    G_BuyItem( ent, BG_Upgrade( UP_LIGHTARMOUR )->name );
  if( BotUpgradeAllowed( ent, UP_HELMET ) )
    G_BuyItem( ent, BG_Upgrade( UP_HELMET )->name );
  if( BG_Weapon( ent->client->ps.stats[ STAT_WEAPON ] )->usesEnergy &&
      BotUpgradeAllowed( ent, UP_BATTPACK ) )
    G_BuyItem( ent, BG_Upgrade( UP_BATTPACK )->name );
  if( BotUpgradeAllowed( ent, UP_MEDKIT ) )
    G_BuyItem( ent, BG_Upgrade( UP_MEDKIT )->name );
  G_BuyItem( ent, BG_Upgrade( UP_AMMO )->name );
}

static void BotEvolve( gentity_t *ent, botState_t *bot )
{
  static const class_t attackClasses[ ] =
  {
    PCL_ALIEN_LEVEL4, PCL_ALIEN_LEVEL3_UPG, PCL_ALIEN_LEVEL3,
    PCL_ALIEN_LEVEL2_UPG, PCL_ALIEN_LEVEL2, PCL_ALIEN_LEVEL1_UPG,
    PCL_ALIEN_LEVEL1
  };
  class_t current = ent->client->pers.classSelection, desired;
  int i;

  if( bot->role == BOT_BUILD )
  {
    desired = PCL_ALIEN_BUILDER0_UPG;
    if( current != desired && BG_ClassIsAllowed( desired ) &&
        BG_ClassCanEvolveFromTo( current, desired, ent->client->pers.credit,
                                g_alienStage.integer, 0 ) >= 0 )
      G_ChangeClass( ent, BG_Class( desired )->name );
    return;
  }

  for( i = 0; i < sizeof( attackClasses ) / sizeof( attackClasses[ 0 ] ); i++ )
  {
    desired = attackClasses[ i ];
    /* Don't spend a life repeatedly switching between equally good classes. */
    if( desired == current )
      return;
    if( BG_ClassIsAllowed( desired ) &&
        BG_ClassCanEvolveFromTo( current, desired, ent->client->pers.credit,
                                g_alienStage.integer, 0 ) >= 0 )
    {
      G_ChangeClass( ent, BG_Class( desired )->name );
      if( ent->client->pers.classSelection == desired )
        return;
      /* A larger class may not fit; try the next affordable class. */
    }
  }
}

gentity_t *G_BotEconomyThink( gentity_t *ent, botState_t *bot )
{
  playerState_t *ps = &ent->client->ps;
  gentity_t *service;
  weapon_t weapon = ps->stats[ STAT_WEAPON ];
  qboolean needAmmo, needShopping;

  if( bot->team == TEAM_ALIENS )
  {
    if( level.time >= bot->nextEconomyTime &&
        !( ps->eFlags & EF_WALLCLIMB ) )
    {
      bot->nextEconomyTime = level.time + 2500;
      BotEvolve( ent, bot );
    }
    if( ent->health < ps->stats[ STAT_MAX_HEALTH ] * 0.4f )
    {
      service = G_BotFindBuildable( ent, BA_A_BOOSTER, 0 );
      if( !service ) service = G_BotFindBuildable( ent, BA_A_OVERMIND, 0 );
      return service;
    }
    return NULL;
  }

  if( ( ent->health < ps->stats[ STAT_MAX_HEALTH ] * 0.65f ||
        ( ps->stats[ STAT_STATE ] & SS_POISONED ) ) &&
      !( ps->stats[ STAT_STATE ] & SS_HEALING_2X ) &&
      BG_InventoryContainsUpgrade( UP_MEDKIT, ps->stats ) )
    BG_ActivateUpgrade( UP_MEDKIT, ps->stats );

  if( ps->ammo == 0 && ps->clips > 0 && ps->weapon == weapon )
    Cmd_Reload_f( ent );
  if( bot->role == BOT_BUILD && weapon == WP_HBUILD &&
      ps->weapon == WP_BLASTER && bot->target < 0 &&
      ps->weaponstate != WEAPON_DROPPING && ps->weaponstate != WEAPON_RAISING &&
      BG_PlayerCanChangeWeapon( ps ) )
    G_ForceWeaponChange( ent, WP_HBUILD );
  needAmmo = !BG_Weapon( weapon )->infiniteAmmo && ps->clips == 0 &&
             ps->ammo < BG_Weapon( weapon )->maxAmmo / 3;
  needShopping = BotDesiredWeapon( ent, bot ) != weapon ||
                 BotUpgradeAllowed( ent, UP_LIGHTARMOUR ) ||
                 BotUpgradeAllowed( ent, UP_HELMET ) ||
                 ( BG_Weapon( weapon )->usesEnergy &&
                   BotUpgradeAllowed( ent, UP_BATTPACK ) ) ||
                 ( !BG_InventoryContainsUpgrade( UP_BATTLESUIT, ps->stats ) &&
                   BG_UpgradeAllowedInStage( UP_BATTLESUIT, g_humanStage.integer ) &&
                   BG_UpgradeIsAllowed( UP_BATTLESUIT ) &&
                   ent->client->pers.credit >= BG_Upgrade( UP_BATTLESUIT )->price + 100 );

  if( G_BuildableRange( ps->origin, 100.0f, BA_H_ARMOURY ) &&
      level.time >= bot->nextEconomyTime )
  {
    bot->nextEconomyTime = level.time + 2000;
    BotShop( ent, bot );
    needAmmo = qfalse;
    needShopping = qfalse;
  }
  else if( needAmmo && BG_Weapon( weapon )->usesEnergy &&
           level.time >= bot->nextEconomyTime &&
           ( G_BuildableRange( ps->origin, 100.0f, BA_H_REACTOR ) ||
             G_BuildableRange( ps->origin, 100.0f, BA_H_REPEATER ) ) )
  {
    bot->nextEconomyTime = level.time + 2000;
    G_BuyItem( ent, BG_Upgrade( UP_AMMO )->name );
    needAmmo = qfalse;
  }

  if( ent->health < ps->stats[ STAT_MAX_HEALTH ] * 0.45f &&
      !( ps->stats[ STAT_STATE ] & SS_HEALING_2X ) )
  {
    service = G_BotFindBuildable( ent, BA_H_MEDISTAT, 0 );
    if( service ) return service;
  }
  if( !BG_InventoryContainsUpgrade( UP_MEDKIT, ps->stats ) &&
      ent->health < ps->stats[ STAT_MAX_HEALTH ] * 0.85f )
  {
    service = G_BotFindBuildable( ent, BA_H_MEDISTAT, 0 );
    if( service ) return service;
  }
  if( needAmmo || needShopping )
  {
    service = G_BotFindBuildable( ent, BA_H_ARMOURY, 0 );
    if( service )
      return G_BuildableRange( ps->origin, 100.0f, BA_H_ARMOURY ) ? NULL : service;
    if( needAmmo && BG_Weapon( weapon )->usesEnergy )
    {
      service = G_BotFindBuildable( ent, BA_H_REACTOR, 0 );
      if( !service ) service = G_BotFindBuildable( ent, BA_H_REPEATER, 0 );
      if( service && G_BuildableRange( ps->origin, 100.0f, service->s.modelindex ) )
        return NULL;
      return service;
    }
  }
  return NULL;
}

void G_BotChooseSpawn( gentity_t *ent, botState_t *bot )
{
  class_t alienClass = bot->role == BOT_BUILD ? PCL_ALIEN_BUILDER0 : PCL_ALIEN_LEVEL0;
  weapon_t humanWeapon = bot->role == BOT_BUILD ? WP_HBUILD : WP_MACHINEGUN;
  int i;
  static const class_t spawnClasses[ ] =
  {
    PCL_ALIEN_LEVEL0, PCL_ALIEN_BUILDER0, PCL_ALIEN_BUILDER0_UPG
  };

  if( bot->team == TEAM_HUMANS )
  {
    if( !BG_WeaponIsAllowed( humanWeapon ) )
      humanWeapon = humanWeapon == WP_HBUILD ? WP_MACHINEGUN : WP_HBUILD;
    if( !BG_WeaponIsAllowed( humanWeapon ) )
      humanWeapon = WP_NONE;
    ent->client->pers.humanItemSelection = humanWeapon;
    ent->client->pers.classSelection = humanWeapon == WP_NONE ? PCL_NONE : PCL_HUMAN;
    ent->client->ps.stats[ STAT_CLASS ] = ent->client->pers.classSelection;
    return;
  }

  if( bot->role == BOT_BUILD && BG_ClassIsAllowed( PCL_ALIEN_BUILDER0_UPG ) &&
      BG_ClassAllowedInStage( PCL_ALIEN_BUILDER0_UPG, g_alienStage.integer ) )
    alienClass = PCL_ALIEN_BUILDER0_UPG;
  if( !BG_ClassIsAllowed( alienClass ) ||
      !BG_ClassAllowedInStage( alienClass, g_alienStage.integer ) )
  {
    alienClass = PCL_NONE;
    for( i = 0; i < sizeof( spawnClasses ) / sizeof( spawnClasses[ 0 ] ); i++ )
    {
      if( BG_ClassIsAllowed( spawnClasses[ i ] ) &&
          BG_ClassAllowedInStage( spawnClasses[ i ], g_alienStage.integer ) )
      {
        alienClass = spawnClasses[ i ];
        break;
      }
    }
  }
  ent->client->pers.classSelection = alienClass;
  ent->client->ps.stats[ STAT_CLASS ] = alienClass;
}
