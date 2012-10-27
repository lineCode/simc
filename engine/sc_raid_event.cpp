// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"

// ==========================================================================
// Raid Events
// ==========================================================================

namespace { // UNNAMED NAMESPACE

struct adds_event_t : public raid_event_t
{
  unsigned count;
  double health;
  std::string master_str;
  std::string name_str;
  player_t* master;
  std::vector< pet_t* > adds;

  adds_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "adds" ),
    count( 1 ), health( 100000 ), master_str( "Fluffy_Pillow" ), name_str( "Add" ),
    master( 0 )
  {
    option_t options[] =
    {
      { "name", OPT_STRING, &name_str },
      { "master", OPT_STRING, &master_str },
      { "count", OPT_INT, &count },
      { "health", OPT_FLT, &health },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );

    master = sim -> find_player( master_str );
    assert( master );

    double overlap = 1;
    timespan_t min_cd = cooldown;
    
    if ( cooldown_stddev != timespan_t::zero() )
    {
      min_cd -= cooldown_stddev * 6;

      if ( min_cd <= timespan_t::zero() )
      {
        sim -> errorf( "The standard deviation of %.3f seconds is too large, creating a too short minimum cooldown (%.3f seconds)", cooldown_stddev.total_seconds(), min_cd.total_seconds() );
        cooldown_stddev = timespan_t::zero();
      }
    }

    if ( min_cd > timespan_t::zero() )
      overlap = duration / min_cd;

    if ( overlap > 1 )
    {
      sim -> errorf( "Simc does not support overlapping add spawning in a single raid event (duration of %.3fs > reasonable minimum cooldown of %.3fs).", duration.total_seconds(), min_cd.total_seconds() );
      overlap = 1;
      duration = min_cd - timespan_t::from_seconds( 0.001 );
    }
    
    for ( int i = 0; i < std::ceil( overlap ); i++ )
    {
      for ( unsigned add = 0; add < count; add++ )
      {
        std::string add_name_str = name_str;
        add_name_str += util::to_string( add + 1 );

        pet_t* p = master -> create_pet( add_name_str );
        p -> resources.base[ RESOURCE_HEALTH ] = health;

        adds.push_back( p );
      }
    }
  }

  virtual void _start()
  {
    for ( size_t i = 0; i < adds.size(); i++ )
      adds[ i ] -> summon();
  }

  virtual void _finish()
  {
    for ( size_t i = 0; i < adds.size(); i++ ) 
      adds[ i ] -> dismiss();
  }
};

// Casting ==================================================================

struct casting_event_t : public raid_event_t
{
  casting_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "casting" )
  {
    parse_options( NULL, options_str );
  }

  virtual void _start()
  {
    sim -> target -> debuffs.casting -> increment();
    for ( size_t i = 0; i < sim -> player_list.size(); ++i )
    {
      player_t* p = sim -> player_list[ i ];
      if ( p -> current.sleeping ) continue;
      p -> interrupt();
    }
  }

  virtual void _finish()
  {
    sim -> target -> debuffs.casting -> decrement();
  }
};

// Distraction ==============================================================

struct distraction_event_t : public raid_event_t
{
  double skill;

  distraction_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "distraction" ),
    skill( 0.2 )
  {
    option_t options[] =
    {
      { "skill", OPT_FLT, &skill },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      p -> current.skill -= skill;
    }
  }

  virtual void _finish()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      p -> current.skill += skill;
    }
  }
};

// Invulnerable =============================================================

struct invulnerable_event_t : public raid_event_t
{
  invulnerable_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "invulnerable" )
  {
    parse_options( NULL, options_str );
  }

  virtual void _start()
  {
    sim -> target -> debuffs.invulnerable -> increment();

    for ( size_t i = 0; i < sim -> player_list.size(); ++i )
    {
      player_t* p = sim -> player_list[ i ];
      if ( p -> current.sleeping ) continue;
      p -> in_combat = true; // FIXME? this is done to ensure we don't end up in infinite loops of non-harmful actions with gcd=0
      p -> halt();
    }

    sim -> target -> clear_debuffs(); // FIXME! this is really just clearing DoTs at the moment
  }

  virtual void _finish()
  {
    sim -> target -> debuffs.invulnerable -> decrement();

    if ( ! sim -> target -> debuffs.invulnerable -> check() )
    {
      // FIXME! restoring optimal_raid target debuffs?
    }
  }
};

// Flying =============================================================

struct flying_event_t : public raid_event_t
{
  flying_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "flying" )
  {
    parse_options( NULL, options_str );
  }

  virtual void _start()
  {
    sim -> target -> debuffs.flying -> increment();
  }

  virtual void _finish()
  {
    sim -> target -> debuffs.flying -> decrement();
  }
};


// Movement =================================================================

struct movement_event_t : public raid_event_t
{
  double move_to;
  double move_distance;
  int players_only;
  bool is_distance;

  movement_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "movement" ), move_to( -2 ), move_distance( 0 ), players_only( 0 ), is_distance( 0 )
  {
    option_t options[] =
    {
      { "to",           OPT_FLT,  &move_to       },
      { "distance",     OPT_FLT,  &move_distance },
      { "players_only", OPT_BOOL, &players_only  },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );
    is_distance = move_distance || ( move_to >= -1 && duration == timespan_t::zero() );
    if ( is_distance ) name_str = "movement_distance";
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> is_pet() && players_only ) continue;
      double my_duration;
      if ( is_distance )
      {
        double my_move_distance = move_distance;
        if ( move_to >= -1 )
        {
          double new_distance = ( move_to < 0 ) ? p -> initial.distance : move_to;
          if ( ! my_move_distance )
            my_move_distance = fabs( new_distance - p -> current.distance );
          p -> current.distance = new_distance;
        }
        my_duration = my_move_distance / p -> composite_movement_speed();
      }
      else
      {
        my_duration = saved_duration.total_seconds();
      }
      if ( my_duration > 0 )
      {
        p -> buffs.raid_movement -> buff_duration = timespan_t::from_seconds( my_duration );
        p -> buffs.raid_movement -> trigger();
      }
      if ( p -> current.sleeping ) continue;
      if ( p -> buffs.stunned -> check() ) continue;
      p -> in_combat = true; // FIXME? this is done to ensure we don't end up in infinite loops of non-harmful actions with gcd=0
      p -> moving();
    }
  }

  virtual void _finish()
  {}
};

// Stun =====================================================================

struct stun_event_t : public raid_event_t
{
  stun_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "stun" )
  {
    parse_options( NULL, options_str );
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> current.sleeping ) continue;
      p -> buffs.stunned -> increment();
      p -> in_combat = true; // FIXME? this is done to ensure we don't end up in infinite loops of non-harmful actions with gcd=0
      p -> stun();
    }
  }

  virtual void _finish()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> current.sleeping ) continue;
      p -> buffs.stunned -> decrement();
      if ( ! p -> buffs.stunned -> check() )
      {
        // Don't schedule_ready players who are already working, like pets auto-summoned during the stun event ( ebon imp ).
        if ( ! p -> channeling && ! p -> executing && ! p -> readying && ! p -> current.sleeping )
          p -> schedule_ready();
      }
    }
  }
};

// Interrupt ================================================================

struct interrupt_event_t : public raid_event_t
{
  interrupt_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "interrupt" )
  {
    parse_options( NULL, options_str );
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> current.sleeping ) continue;
      p -> interrupt();
    }
  }

  virtual void _finish()
  {}
};

// Damage ===================================================================

struct damage_event_t : public raid_event_t
{
  double amount;
  double amount_range;
  spell_t* raid_damage;

  damage_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "damage" ),
    amount( 1 ), amount_range( 0 ), raid_damage( 0 )
  {
    std::string type_str = "holy";
    option_t options[] =
    {
      { "amount",        OPT_FLT, &amount        },
      { "amount_range",  OPT_FLT, &amount_range  },
      { "type",          OPT_STRING, &type_str   },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );

    assert( duration == timespan_t::zero() );

    name_str = "raid_damage_" + type_str;

    struct raid_damage_t : public spell_t
    {
      raid_damage_t( const char* n, player_t* player, school_e s ) :
        spell_t( n, player, spell_data_t::nil() )
      {
        school = s;
        may_crit = false;
        background = true;
        trigger_gcd = timespan_t::zero();
      }
    };

    raid_damage = new raid_damage_t( name_str.c_str(), sim -> target, util::parse_school_type( type_str ) );
    raid_damage -> init();
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> current.sleeping ) continue;
      raid_damage -> base_dd_min = raid_damage -> base_dd_max = rng -> range( amount - amount_range, amount + amount_range );
      raid_damage -> target = p;
      raid_damage -> execute();
    }
  }

  virtual void _finish()
  {}
};

// Heal =====================================================================

struct heal_event_t : public raid_event_t
{
  double amount;
  double amount_range;

  heal_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "heal" ), amount( 1 ), amount_range( 0 )
  {
    option_t options[] =
    {
      { "amount",       OPT_FLT, &amount       },
      { "amount_range", OPT_FLT, &amount_range },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );

    assert( duration == timespan_t::zero() );
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> current.sleeping ) continue;

      double x = rng -> range( amount - amount_range, amount + amount_range );
      if ( sim -> log ) sim -> output( "%s takes %.0f raid heal.", p -> name(), x );
      p -> resource_gain( RESOURCE_HEALTH, x );
    }
  }

  virtual void _finish()
  {}
};

// Vulnerable ===============================================================

struct vulnerable_event_t : public raid_event_t
{
  double multiplier;
  vulnerable_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "vulnerable" ), multiplier( 2.0 )
  {
    option_t options[] =
    {
      { "multiplier", OPT_FLT, &multiplier },
      { NULL, OPT_UNKNOWN, NULL }
    };
    parse_options( options, options_str );
  }

  virtual void _start()
  {
    sim -> target -> debuffs.vulnerable -> increment( 1, multiplier );
  }

  virtual void _finish()
  {
    sim -> target -> debuffs.vulnerable -> decrement();
  }
};

// Position Switch ===============================================================

struct position_event_t : public raid_event_t
{

  position_event_t( sim_t* s, const std::string& options_str ) :
    raid_event_t( s, "position_switch" )
  {
    parse_options( 0, options_str );
  }

  virtual void _start()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];
      if ( p -> position() == POSITION_BACK )
        p -> change_position( POSITION_FRONT );
      else if ( p -> position() == POSITION_RANGED_BACK )
        p -> change_position( POSITION_RANGED_FRONT );
    }
  }

  virtual void _finish()
  {
    for ( size_t i = 0, num_affected = affected_players.size(); i < num_affected; ++i )
    {
      player_t* p = affected_players[ i ];

      p -> change_position( p -> initial.position );
    }
  }
};

} // UNNAMED NAMESPACE

// raid_event_t::raid_event_t ===============================================

raid_event_t::raid_event_t( sim_t* s, const std::string& n ) :
  sim( s ),
  name_str( n ),
  num_starts( 0 ),
  first( timespan_t::zero() ),
  last( timespan_t::zero() ),
  cooldown( timespan_t::zero() ),
  cooldown_stddev( timespan_t::zero() ),
  cooldown_min( timespan_t::zero() ),
  cooldown_max( timespan_t::zero() ),
  duration( timespan_t::zero() ),
  duration_stddev( timespan_t::zero() ),
  duration_min( timespan_t::zero() ),
  duration_max( timespan_t::zero() ),
  distance_min( 0 ),
  distance_max( 0 ),
  saved_duration( timespan_t::zero() ),
  rng( s -> default_rng() )
{}

// raid_event_t::cooldown_time ==============================================

timespan_t raid_event_t::cooldown_time()
{
  timespan_t time;

  if ( num_starts == 0 )
  {
    time = timespan_t::zero();

    if ( first > timespan_t::zero() )
    {
      time = first;
    }
    else
    {
      time = timespan_t::from_millis( 10 );
    }
  }
  else
  {
    time = rng -> gauss( cooldown, cooldown_stddev );

    if ( time < cooldown_min ) time = cooldown_min;
    if ( time > cooldown_max ) time = cooldown_max;
  }

  return time;
}

// raid_event_t::duration_time ==============================================

timespan_t raid_event_t::duration_time()
{
  timespan_t time = rng -> gauss( duration, duration_stddev );

  if ( time < duration_min ) time = duration_min;
  if ( time > duration_max ) time = duration_max;

  return time;
}

// raid_event_t::start ======================================================

void raid_event_t::start()
{
  if ( sim -> log )
    sim -> output( "Raid event %s starts.", name_str.c_str() );

  num_starts++;

  affected_players.clear();

  for ( size_t i = 0; i < sim -> player_list.size(); ++i )
  {
    player_t* p = sim -> player_list[ i ];
    if ( distance_min &&
         distance_min > p -> current.distance )
      continue;

    if ( distance_max &&
         distance_max < p -> current.distance )
      continue;

    affected_players.push_back( p );
  }

  _start();
}

// raid_event_t::finish =====================================================

void raid_event_t::finish()
{
  _finish();

  if ( sim -> log )
    sim -> output( "Raid event %s finishes.", name_str.c_str() );
}

// raid_event_t::schedule ===================================================

void raid_event_t::schedule()
{
  if ( sim -> debug ) sim -> output( "Scheduling raid event: %s", name_str.c_str() );

  struct duration_event_t : public event_t
  {
    raid_event_t* raid_event;

    duration_event_t( sim_t* s, raid_event_t* re, timespan_t time ) : event_t( s, 0, re -> name() ), raid_event( re )
    {
      sim -> add_event( this, time );
    }

    virtual void execute()
    {
      raid_event -> finish();
    }
  };

  struct cooldown_event_t : public event_t
  {
    raid_event_t* raid_event;

    cooldown_event_t( sim_t* s, raid_event_t* re, timespan_t time ) : event_t( s, 0, re -> name() ), raid_event( re )
    {
      sim -> add_event( this, time );
    }

    virtual void execute()
    {
      raid_event -> saved_duration = raid_event -> duration_time();
      raid_event -> start();

      timespan_t ct = raid_event -> cooldown_time();

      if ( ct <= raid_event -> saved_duration ) ct = raid_event -> saved_duration + timespan_t::from_seconds( 0.01 );

      if ( raid_event -> saved_duration > timespan_t::zero() )
      {
        new ( sim ) duration_event_t( sim, raid_event, raid_event -> saved_duration );
      }
      else raid_event -> finish();

      if ( raid_event -> last <= timespan_t::zero() ||
           raid_event -> last > ( sim -> current_time + ct ) )
      {
        new ( sim ) cooldown_event_t( sim, raid_event, ct );
      }
    }
  };

  new ( sim ) cooldown_event_t( sim, this, cooldown_time() );
}

// raid_event_t::reset ======================================================

void raid_event_t::reset()
{
  num_starts = 0;

  if ( cooldown_min == timespan_t::zero() ) cooldown_min = cooldown * 0.5;
  if ( cooldown_max == timespan_t::zero() ) cooldown_max = cooldown * 1.5;

  if ( duration_min == timespan_t::zero() ) duration_min = duration * 0.5;
  if ( duration_max == timespan_t::zero() ) duration_max = duration * 1.5;

  affected_players.clear();
}

// raid_event_t::parse_options ==============================================

void raid_event_t::parse_options( option_t*          options,
                                  const std::string& options_str )
{
  if ( options_str.empty() ) return;

  option_t base_options[] =
  {
    { "first",           OPT_TIMESPAN, &first           },
    { "last",            OPT_TIMESPAN, &last            },
    { "period",          OPT_TIMESPAN, &cooldown        },
    { "cooldown",        OPT_TIMESPAN, &cooldown        },
    { "cooldown_stddev", OPT_TIMESPAN, &cooldown_stddev },
    { "cooldown>",       OPT_TIMESPAN, &cooldown_min    },
    { "cooldown<",       OPT_TIMESPAN, &cooldown_max    },
    { "duration",        OPT_TIMESPAN, &duration        },
    { "duration_stddev", OPT_TIMESPAN, &duration_stddev },
    { "duration>",       OPT_TIMESPAN, &duration_min    },
    { "duration<",       OPT_TIMESPAN, &duration_max    },
    { "distance>",       OPT_FLT, &distance_min    },
    { "distance<",       OPT_FLT, &distance_max    },
    { NULL, OPT_UNKNOWN, NULL }
  };

  std::vector<option_t> merged_options;

  if ( options )
    for ( unsigned i = 0; options[ i ].name; i++ )
      merged_options.push_back( options[ i ] );

  for ( unsigned i = 0; base_options[ i ].name; i++ )
    merged_options.push_back( base_options[ i ] );

  if ( ! option_t::parse( sim, name_str.c_str(), merged_options, options_str ) )
  {
    sim -> errorf( "Raid Event %s: Unable to parse options str '%s'.\n", name_str.c_str(), options_str.c_str() );
    sim -> cancel();
  }

}

// raid_event_t::create =====================================================

raid_event_t* raid_event_t::create( sim_t* sim,
                                    const std::string& name,
                                    const std::string& options_str )
{
  if ( name == "adds"         ) return new         adds_event_t( sim, options_str );
  if ( name == "casting"      ) return new      casting_event_t( sim, options_str );
  if ( name == "distraction"  ) return new  distraction_event_t( sim, options_str );
  if ( name == "invul"        ) return new invulnerable_event_t( sim, options_str );
  if ( name == "invulnerable" ) return new invulnerable_event_t( sim, options_str );
  if ( name == "interrupt"    ) return new    interrupt_event_t( sim, options_str );
  if ( name == "movement"     ) return new     movement_event_t( sim, options_str );
  if ( name == "moving"       ) return new     movement_event_t( sim, options_str );
  if ( name == "damage"       ) return new       damage_event_t( sim, options_str );
  if ( name == "heal"         ) return new         heal_event_t( sim, options_str );
  if ( name == "stun"         ) return new         stun_event_t( sim, options_str );
  if ( name == "vulnerable"   ) return new   vulnerable_event_t( sim, options_str );
  if ( name == "position_switch" ) return new  position_event_t( sim, options_str );
  if ( name == "flying" )       return new  flying_event_t( sim, options_str );

  return 0;
}

// raid_event_t::init =======================================================

void raid_event_t::init( sim_t* sim )
{
  std::vector<std::string> splits;
  size_t num_splits = util::string_split( splits, sim -> raid_events_str, "/\\" );

  for ( size_t i = 0; i < num_splits; i++ )
  {
    std::string name = splits[ i ];
    std::string options = "";

    if ( sim -> debug ) sim -> output( "Creating raid event: %s", name.c_str() );

    std::string::size_type cut_pt = name.find_first_of( "," );

    if ( cut_pt != name.npos )
    {
      options = name.substr( cut_pt + 1 );
      name    = name.substr( 0, cut_pt );
    }

    raid_event_t* e = create( sim, name, options );

    if ( ! e )
    {
      sim -> errorf( "Unknown raid event: %s\n", splits[ i ].c_str() );
      sim -> cancel();
      continue;
    }

    assert( e -> cooldown > timespan_t::zero() );
    assert( e -> cooldown > e -> cooldown_stddev );

    sim -> raid_events.push_back( e );
  }
}

// raid_event_t::reset ======================================================

void raid_event_t::reset( sim_t* sim )
{
  for ( size_t i = 0; i < sim -> raid_events.size(); i++ )
  {
    sim -> raid_events[ i ] -> reset();
  }
}

// raid_event_t::combat_begin ===============================================

void raid_event_t::combat_begin( sim_t* sim )
{
  for ( size_t i = 0; i < sim -> raid_events.size(); i++ )
  {
    sim -> raid_events[ i ] -> schedule();
  }
}
