/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2011 Jean-Pierre Charras, <jp.charras@wanadoo.fr>
 * Copyright (C) 2013-2016 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 * Copyright (C) 1992-2017 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <footprint_info_impl.h>

#include <class_module.h>
#include <common.h>
#include <fctsys.h>
#include <footprint_info.h>
#include <fp_lib_table.h>
#include <html_messagebox.h>
#include <io_mgr.h>
#include <kiface_ids.h>
#include <kiway.h>
#include <lib_id.h>
#include <macros.h>
#include <make_unique.h>
#include <pgm_base.h>
#include <wildcards_and_files_ext.h>
#include <widgets/progress_reporter.h>

#include <thread>


void FOOTPRINT_INFO_IMPL::load()
{
    FP_LIB_TABLE* fptable = m_owner->GetTable();

    wxASSERT( fptable );

    std::unique_ptr<MODULE> footprint( fptable->LoadEnumeratedFootprint( m_nickname, m_fpname ) );
    if( footprint.get() == NULL ) // Should happen only with malformed/broken libraries
    {
        m_pad_count = 0;
        m_unique_pad_count = 0;
    }
    else
    {
        m_pad_count = footprint->GetPadCount( DO_NOT_INCLUDE_NPTH );
        m_unique_pad_count = footprint->GetUniquePadCount( DO_NOT_INCLUDE_NPTH );
        m_keywords = footprint->GetKeywords();
        m_doc = footprint->GetDescription();

        // tell ensure_loaded() I'm loaded.
        m_loaded = true;
    }
}


bool FOOTPRINT_LIST_IMPL::CatchErrors( const std::function<void()>& aFunc )
{
    try
    {
        aFunc();
    }
    catch( const IO_ERROR& ioe )
    {
        m_errors.move_push( std::make_unique<IO_ERROR>( ioe ) );
        return false;
    }
    catch( const std::exception& se )
    {
        // This is a round about way to do this, but who knows what THROW_IO_ERROR()
        // may be tricked out to do someday, keep it in the game.
        try
        {
            THROW_IO_ERROR( se.what() );
        }
        catch( const IO_ERROR& ioe )
        {
            m_errors.move_push( std::make_unique<IO_ERROR>( ioe ) );
        }
        return false;
    }

    return true;
}


void FOOTPRINT_LIST_IMPL::loader_job()
{
    wxString nickname;

    while( m_queue_in.pop( nickname ) && !m_cancelled )
    {
        CatchErrors( [this, &nickname]() {
            m_lib_table->PrefetchLib( nickname );
            m_queue_out.push( nickname );
        } );

        m_count_finished.fetch_add( 1 );

        if( m_progress_reporter )
            m_progress_reporter->AdvanceProgress();
    }
}


bool FOOTPRINT_LIST_IMPL::ReadFootprintFiles( FP_LIB_TABLE* aTable, const wxString* aNickname,
                                              WX_PROGRESS_REPORTER* aProgressReporter )
{
    long long libraryTimestamp = aTable->GenerateTimestamp( aNickname );
    if( m_list_timestamp == libraryTimestamp )
        return true;

    m_progress_reporter = aProgressReporter;
    m_cancelled = false;

    FOOTPRINT_ASYNC_LOADER loader;

    loader.SetList( this );
    loader.Start( aTable, aNickname );

    if( m_progress_reporter )
    {
        m_progress_reporter->SetMaxProgress( m_queue_in.size() );
        m_progress_reporter->Report( _( "Fetching Footprint Libraries" ) );
    }

    while( !m_cancelled && loader.GetProgress() < 100 )
    {
        if( m_progress_reporter )
            m_cancelled = !m_progress_reporter->KeepRefreshing();
        else
            wxMilliSleep( 20 );
    }

    if( m_progress_reporter )
        m_progress_reporter->AdvancePhase();

    if( !m_cancelled )
    {
        if( m_progress_reporter )
        {
            m_progress_reporter->SetMaxProgress( m_queue_out.size() );
            m_progress_reporter->Report( _( "Loading Footprints" ) );
        }

        loader.Join();
    }

    if( m_progress_reporter )
        m_progress_reporter->AdvancePhase();

    if( m_cancelled )
    {
        m_errors.move_push( std::make_unique<IO_ERROR>
                    ( _( "Loading incomplete; cancelled by user." ), nullptr, nullptr, 0 ) );
    }

    m_list_timestamp = libraryTimestamp;
    m_progress_reporter = nullptr;

    return m_errors.empty();
}


void FOOTPRINT_LIST_IMPL::StartWorkers( FP_LIB_TABLE* aTable, wxString const* aNickname,
        FOOTPRINT_ASYNC_LOADER* aLoader, unsigned aNThreads )
{
    m_loader = aLoader;
    m_lib_table = aTable;

    // Clear data before reading files
    m_count_finished.store( 0 );
    m_errors.clear();
    m_list.clear();
    m_threads.clear();
    m_queue_in.clear();
    m_queue_out.clear();

    if( aNickname )
        m_queue_in.push( *aNickname );
    else
    {
        for( auto const& nickname : aTable->GetLogicalLibs() )
            m_queue_in.push( nickname );
    }

    m_loader->m_total_libs = m_queue_in.size();

    for( unsigned i = 0; i < aNThreads; ++i )
    {
        m_threads.push_back( std::thread( &FOOTPRINT_LIST_IMPL::loader_job, this ) );
    }
}

bool FOOTPRINT_LIST_IMPL::JoinWorkers()
{
    for( auto& i : m_threads )
        i.join();

    m_threads.clear();
    m_queue_in.clear();
    m_count_finished.store( 0 );

    size_t total_count = m_queue_out.size();

    LOCALE_IO toggle_locale;

    // Parse the footprints in parallel. WARNING! This requires changing the locale, which is
    // GLOBAL. It is only threadsafe to construct the LOCALE_IO before the threads are created,
    // destroy it after they finish, and block the main (GUI) thread while they work. Any deviation
    // from this will cause nasal demons.
    //
    // TODO: blast LOCALE_IO into the sun

    SYNC_QUEUE<std::unique_ptr<FOOTPRINT_INFO>> queue_parsed;
    std::vector<std::thread>                    threads;

    for( size_t ii = 0; ii < std::thread::hardware_concurrency() + 1; ++ii )
    {
        threads.push_back( std::thread( [this, &queue_parsed]() {
            wxString nickname;

            while( this->m_queue_out.pop( nickname ) && !m_cancelled )
            {
                wxArrayString fpnames;

                try
                {
                    m_lib_table->FootprintEnumerate( fpnames, nickname );
                }
                catch( const IO_ERROR& ioe )
                {
                    m_errors.move_push( std::make_unique<IO_ERROR>( ioe ) );
                }
                catch( const std::exception& se )
                {
                    // This is a round about way to do this, but who knows what THROW_IO_ERROR()
                    // may be tricked out to do someday, keep it in the game.
                    try
                    {
                        THROW_IO_ERROR( se.what() );
                    }
                    catch( const IO_ERROR& ioe )
                    {
                        m_errors.move_push( std::make_unique<IO_ERROR>( ioe ) );
                    }
                }

                for( unsigned jj = 0; jj < fpnames.size() && !m_cancelled; ++jj )
                {
                    wxString fpname = fpnames[jj];
                    FOOTPRINT_INFO* fpinfo = new FOOTPRINT_INFO_IMPL( this, nickname, fpname );
                    queue_parsed.move_push( std::unique_ptr<FOOTPRINT_INFO>( fpinfo ) );
                }

                if( m_progress_reporter )
                    m_progress_reporter->AdvanceProgress();

                m_count_finished.fetch_add( 1 );
            }
        } ) );
    }

    while( !m_cancelled && m_count_finished.load() < total_count )
    {
        if( m_progress_reporter )
            m_cancelled = !m_progress_reporter->KeepRefreshing();
        else
            wxMilliSleep( 20 );
    }

    for( auto& thr : threads )
        thr.join();

    std::unique_ptr<FOOTPRINT_INFO> fpi;

    while( queue_parsed.pop( fpi ) )
        m_list.push_back( std::move( fpi ) );

    std::sort( m_list.begin(), m_list.end(),
            []( std::unique_ptr<FOOTPRINT_INFO> const&     lhs,
                    std::unique_ptr<FOOTPRINT_INFO> const& rhs ) -> bool { return *lhs < *rhs; } );

    return m_errors.empty();
}


size_t FOOTPRINT_LIST_IMPL::CountFinished()
{
    return m_count_finished.load();
}


FOOTPRINT_LIST_IMPL::FOOTPRINT_LIST_IMPL() :
    m_loader( nullptr ),
    m_count_finished( 0 ),
    m_list_timestamp( 0 ),
    m_progress_reporter( nullptr ),
    m_cancelled( false )
{
}


FOOTPRINT_LIST_IMPL::~FOOTPRINT_LIST_IMPL()
{
    for( auto& i : m_threads )
        i.join();
}
