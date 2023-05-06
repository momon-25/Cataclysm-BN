#include "mapbuffer.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "cata_utility.h"
#include "coordinate_conversions.h"
#include "debug.h"
#include "distribution_grid.h"
#include "filesystem.h"
#include "fstream_utils.h"
#include "game.h"
#include "game_constants.h"
#include "json.h"
#include "map.h"
#include "output.h"
#include "popup.h"
#include "string_formatter.h"
#include "submap.h"
#include "translations.h"
#include "ui_manager.h"

static std::string find_quad_path( const std::string &dirname, const tripoint &om_addr )
{
    return string_format( "%s/%d.%d.%d.map", dirname, om_addr.x, om_addr.y, om_addr.z );
}

static std::string find_dirname( const tripoint &om_addr )
{
    const tripoint segment_addr = omt_to_seg_copy( om_addr );
    return string_format( "%s/maps/%d.%d.%d", g->get_world_base_save_path(), segment_addr.x,
                          segment_addr.y, segment_addr.z );
}

mapbuffer MAPBUFFER;

mapbuffer::mapbuffer() = default;

mapbuffer::~mapbuffer()
{
    reset();
}

void mapbuffer::reset()
{
    for( auto &elem : submaps ) {
        delete elem.second;
    }
    submaps.clear();
}

bool mapbuffer::add_submap( const tripoint &p, submap *sm )
{
    if( submaps.count( p ) != 0 ) {
        return false;
    }

    submaps[p] = sm;

    return true;
}

bool mapbuffer::add_submap( const tripoint &p, std::unique_ptr<submap> &sm )
{
    const bool result = add_submap( p, sm.get() );
    if( result ) {
        sm.release();
    }
    return result;
}

void mapbuffer::remove_submap( tripoint addr )
{
    auto m_target = submaps.find( addr );
    if( m_target == submaps.end() ) {
        debugmsg( "Tried to remove non-existing submap %s", addr.to_string() );
        return;
    }
    delete m_target->second;
    submaps.erase( m_target );
}

submap *mapbuffer::lookup_submap( const tripoint &p )
{
    const auto iter = submaps.find( p );
    if( iter == submaps.end() ) {
        try {
            return unserialize_submaps( p );
        } catch( const std::exception &err ) {
            debugmsg( "Failed to load submap %s: %s", p.to_string(), err.what() );
        }
        return nullptr;
    }

    return iter->second;
}

void mapbuffer::save( bool delete_after_save )
{
    assure_dir_exist( g->get_world_base_save_path() + "/maps" );

    int num_saved_submaps = 0;
    int num_total_submaps = submaps.size();

    map &here = get_map();
    const tripoint map_origin = sm_to_omt_copy( here.get_abs_sub() );
    const bool map_has_zlevels = g != nullptr && here.has_zlevels();

    static_popup popup;

    // A set of already-saved submaps, in global overmap coordinates.
    std::set<tripoint> saved_submaps;
    std::list<tripoint> submaps_to_delete;
    static constexpr std::chrono::milliseconds update_interval( 500 );
    auto last_update = std::chrono::steady_clock::now();

    for( auto &elem : submaps ) {
        auto now = std::chrono::steady_clock::now();
        if( last_update + update_interval < now ) {
            popup.message( _( "Please wait as the map saves [%d/%d]" ),
                           num_saved_submaps, num_total_submaps );
            ui_manager::redraw();
            refresh_display();
            inp_mngr.pump_events();
            last_update = now;
        }
        // Whatever the coordinates of the current submap are,
        // we're saving a 2x2 quad of submaps at a time.
        // Submaps are generated in quads, so we know if we have one member of a quad,
        // we have the rest of it, if that assumption is broken we have REAL problems.
        const tripoint om_addr = sm_to_omt_copy( elem.first );
        if( saved_submaps.count( om_addr ) != 0 ) {
            // Already handled this one.
            continue;
        }
        saved_submaps.insert( om_addr );

        // A segment is a chunk of 32x32 submap quads.
        // We're breaking them into subdirectories so there aren't too many files per directory.
        // Might want to make a set for this one too so it's only checked once per save().
        const std::string dirname = find_dirname( om_addr );
        const std::string quad_path = find_quad_path( dirname, om_addr );

        // delete_on_save deletes everything, otherwise delete submaps
        // outside the current map.
        const bool zlev_del = !map_has_zlevels && om_addr.z != g->get_levz();
        save_quad( dirname, quad_path, om_addr, submaps_to_delete,
                   delete_after_save || zlev_del ||
                   om_addr.x < map_origin.x || om_addr.y < map_origin.y ||
                   om_addr.x > map_origin.x + HALF_MAPSIZE ||
                   om_addr.y > map_origin.y + HALF_MAPSIZE );
        num_saved_submaps += 4;
    }
    for( auto &elem : submaps_to_delete ) {
        remove_submap( elem );
    }

    get_distribution_grid_tracker().on_saved();
}

void mapbuffer::save_quad( const std::string &dirname, const std::string &filename,
                           const tripoint &om_addr, std::list<tripoint> &submaps_to_delete,
                           bool delete_after_save )
{
    std::vector<point> offsets;
    std::vector<tripoint> submap_addrs;
    offsets.push_back( point_zero );
    offsets.push_back( point_south );
    offsets.push_back( point_east );
    offsets.push_back( point_south_east );

    bool all_uniform = true;
    for( auto &offsets_offset : offsets ) {
        tripoint submap_addr = omt_to_sm_copy( om_addr );
        submap_addr.x += offsets_offset.x;
        submap_addr.y += offsets_offset.y;
        submap_addrs.push_back( submap_addr );
        submap *sm = submaps[submap_addr];
        if( sm != nullptr && !sm->is_uniform ) {
            all_uniform = false;
        }
    }

    if( all_uniform ) {
        // Nothing to save - this quad will be regenerated faster than it would be re-read
        if( delete_after_save ) {
            for( auto &submap_addr : submap_addrs ) {
                if( submaps.count( submap_addr ) > 0 && submaps[submap_addr] != nullptr ) {
                    submaps_to_delete.push_back( submap_addr );
                }
            }
        }

        return;
    }

    if( disable_mapgen ) {
        return;
    }

    // Don't create the directory if it would be empty
    assure_dir_exist( dirname );
    write_to_file( filename, [&]( std::ostream & fout ) {
        JsonOut jsout( fout );
        jsout.start_array();
        for( auto &submap_addr : submap_addrs ) {
            if( submaps.count( submap_addr ) == 0 ) {
                continue;
            }

            submap *sm = submaps[submap_addr];

            if( sm == nullptr ) {
                continue;
            }

            jsout.start_object();

            jsout.member( "version", savegame_version );
            jsout.member( "coordinates" );

            jsout.start_array();
            jsout.write( submap_addr.x );
            jsout.write( submap_addr.y );
            jsout.write( submap_addr.z );
            jsout.end_array();

            sm->store( jsout );

            jsout.end_object();

            if( delete_after_save ) {
                submaps_to_delete.push_back( submap_addr );
            }
        }

        jsout.end_array();
    } );
}

// We're reading in way too many entities here to mess around with creating sub-objects and
// seeking around in them, so we're using the json streaming API.
submap *mapbuffer::unserialize_submaps( const tripoint &p )
{
    // Map the tripoint to the submap quad that stores it.
    const tripoint om_addr = sm_to_omt_copy( p );
    const std::string dirname = find_dirname( om_addr );
    std::string quad_path = find_quad_path( dirname, om_addr );

    if( !file_exist( quad_path ) ) {
        // Fix for old saves where the path was generated using std::stringstream, which
        // did format the number using the current locale. That formatting may insert
        // thousands separators, so the resulting path is "map/1,234.7.8.map" instead
        // of "map/1234.7.8.map".
        std::ostringstream buffer;
        buffer << dirname << "/" << om_addr.x << "." << om_addr.y << "." << om_addr.z << ".map";
        if( file_exist( buffer.str() ) ) {
            quad_path = buffer.str();
        }
    }

    using namespace std::placeholders;
    if( !read_from_file_optional_json( quad_path, std::bind( &mapbuffer::deserialize, this, _1 ) ) ) {
        // If it doesn't exist, trigger generating it.
        return nullptr;
    }
    if( submaps.count( p ) == 0 ) {
        debugmsg( "file %s did not contain the expected submap %d,%d,%d",
                  quad_path, p.x, p.y, p.z );
        return nullptr;
    }
    return submaps[ p ];
}

void mapbuffer::deserialize( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        std::unique_ptr<submap> sm;
        tripoint submap_coordinates;
        jsin.start_object();
        int version = 0;
        while( !jsin.end_object() ) {
            std::string submap_member_name = jsin.get_member_name();
            if( submap_member_name == "version" ) {
                version = jsin.get_int();
            } else if( submap_member_name == "coordinates" ) {
                jsin.start_array();
                int i = jsin.get_int();
                int j = jsin.get_int();
                int k = jsin.get_int();
                tripoint loc{ i, j, k };
                jsin.end_array();
                submap_coordinates = loc;
                sm = std::make_unique<submap>( sm_to_ms_copy( submap_coordinates ) );
            } else {
                if( !sm ) { //This whole thing is a nasty hack that relys on coordinates coming first...
                    debugmsg( "coordinates was not at the top of submap json" );
                }
                sm->load( jsin, submap_member_name, version, multiply_xy( submap_coordinates, 12 ) );
            }
        }

        if( !add_submap( submap_coordinates, sm ) ) {
            debugmsg( "submap %d,%d,%d was already loaded", submap_coordinates.x, submap_coordinates.y,
                      submap_coordinates.z );
        }
    }
}
