#include "remote_manager.hpp"

#include "auth_manager.hpp"

#include <wx/log.h>
#include <wx/socket.h>
#include <wx/wx.h>

#include <chrono>

#include <stdlib.h>

namespace srv
{

const std::string RemoteManager::FALLBACK_OS = "Linux";
const std::string RemoteManager::REQUEST = "REQUEST";

RemoteManager::RemoteManager( ObservableService* service )
    : m_srv( service )
    , m_srvType( "" )
{
}

RemoteManager::~RemoteManager()
{
    stop();
}

void RemoteManager::stop()
{
    std::lock_guard<std::mutex> guard( m_mutex );

    for ( std::shared_ptr<RemoteInfo>& info : m_hosts )
    {
        std::lock_guard<std::mutex> lock( info->mutex );

        info->stopping = true;
        info->stopVar.notify_all();
    }

    for ( std::shared_ptr<RemoteInfo>& info : m_hosts )
    {
        info->thread.join();
    }
}

size_t RemoteManager::getTotalHostsCount()
{
    std::lock_guard<std::mutex> guard( m_mutex );

    return m_hosts.size();
}

size_t RemoteManager::getVisibleHostsCount()
{
    std::lock_guard<std::mutex> guard( m_mutex );
    size_t count = 0;

    for ( const std::shared_ptr<RemoteInfo>& info : m_hosts )
    {
        if ( info->visible )
        {
            count++;
        }
    }

    return count;
}

void RemoteManager::setServiceType( const std::string& serviceType )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    m_srvType = serviceType;

    if ( m_srvType[m_srvType.length() - 1] != '.' )
    {
        m_srvType += '.';
    }
}

const std::string& RemoteManager::getServiceType()
{
    std::lock_guard<std::mutex> guard( m_mutex );

    return m_srvType;
}

void RemoteManager::processAddHost( const zc::MdnsServiceData& data )
{
    std::lock_guard<std::mutex> guard( m_mutex );

    // Check if our peer is a 'real' type host
    // Otherwise ignore it
    if ( data.txtRecords.find( "type" ) == data.txtRecords.end() )
    {
        return;
    }

    if ( data.txtRecords.find( "hostname" ) == data.txtRecords.end() )
    {
        return;
    }

    if ( data.txtRecords.at( "type" ) != "real" )
    {
        return;
    }

    // Fill RemoteInfo struct
    std::shared_ptr<RemoteInfo> info = std::make_shared<RemoteInfo>();
    info->visible = false;
    info->id = stripServiceFromIdent( data.name );
    info->ips.valid = true;
    info->ips.ipv4 = data.ipv4;
    info->ips.ipv6 = data.ipv6;
    info->port = data.port;
    info->hostname = data.txtRecords.at( "hostname" );

    if ( data.txtRecords.find( "api-version" ) == data.txtRecords.end() )
    {
        info->apiVersion = 1;
    }
    else
    {
        info->apiVersion = atoi( data.txtRecords.at( "api-version" ).c_str() );
        if ( info->apiVersion < 1 )
        {
            info->apiVersion = 1;
        }
        if ( info->apiVersion > 2 )
        {
            info->apiVersion = 2;
        }
    }

    if ( data.txtRecords.find( "auth-port" ) == data.txtRecords.end() )
    {
        info->authPort = info->port;
    }
    else
    {
        info->authPort = (uint16_t)atoi(
            data.txtRecords.at( "auth-port" ).c_str() );
    }

    if ( data.txtRecords.find( "os" ) == data.txtRecords.end() )
    {
        info->os = RemoteManager::FALLBACK_OS;
    }
    else
    {
        info->os = data.txtRecords.at( "os" );
    }

    // Start remote handler thread
    info->thread = std::thread( std::bind(
        &RemoteManager::remoteThreadEntry, this, info ) );

    m_hosts.push_back( info );
}

void RemoteManager::processRemoveHost( const std::string& id )
{
    std::lock_guard<std::mutex> guard( m_mutex );
    // Currently not implemented
}

std::string RemoteManager::stripServiceFromIdent(
    const std::string& identStr ) const
{
    if ( identStr.length() <= m_srvType.length() )
    {
        return identStr;
    }

    size_t subOffset = identStr.length() - m_srvType.length();

    if ( identStr.substr( subOffset ) == m_srvType )
    {
        return identStr.substr( 0, subOffset );
    }

    return identStr;
}

int RemoteManager::remoteThreadEntry( std::shared_ptr<RemoteInfo> serviceInfo )
{
    if ( serviceInfo->apiVersion == 1 )
    {
        if( !doRegistrationV1( serviceInfo.get() ) )
        {
            wxLogDebug( "Unable to register with %s (%s:%d) - api version 1",
                serviceInfo->hostname, serviceInfo->ips.ipv4, 
                serviceInfo->port );

            return EXIT_SUCCESS;
        }
    }
    else if ( serviceInfo->apiVersion == 2 )
    {
        if( !doRegistrationV2( serviceInfo.get() ) )
        {
            wxLogDebug( "Unable to register with %s (%s:%d) - api version 2",
                serviceInfo->hostname, serviceInfo->ips.ipv4,
                serviceInfo->authPort );

            return EXIT_SUCCESS;
        }
    }

    if ( serviceInfo->stopping )
    {
        return EXIT_SUCCESS;
    }

    return EXIT_SUCCESS;
}

bool RemoteManager::doRegistrationV1( RemoteInfo* serviceInfo )
{
    wxLogDebug( "Registering with %s (%s:%d) - api version 1",
        serviceInfo->hostname, serviceInfo->ips.ipv4, serviceInfo->port );

    do
    {
        wxLogDebug( "Requesting cert from %s...", serviceInfo->hostname );
        int tryCount = 0;

        wxIPV4address address;
        address.AnyAddress();

        wxIPV4address destination;
        destination.Hostname( serviceInfo->ips.ipv4 );
        destination.Service( serviceInfo->port );

        while ( tryCount < 3 )
        {
            wxDatagramSocket sock( address, wxSOCKET_BLOCK );
            sock.SetTimeout( 1 );
            sock.SendTo( destination, RemoteManager::REQUEST.data(),
                RemoteManager::REQUEST.size() );

            wxIPV4address replyAddr;
            char replyBuf[1500];

            sock.RecvFrom( replyAddr, replyBuf, 1500 );
            size_t length = sock.LastReadCount();

            if ( length == 0 ) // Timeout
            {
                tryCount++;
                continue;
            }

            if ( replyAddr == destination )
            {
                wxLogDebug( "Got remote cert from %s", serviceInfo->hostname );

                std::string msg( replyBuf, length );

                return AuthManager::get()->processRemoteCert( 
                    serviceInfo->hostname, serviceInfo->ips, msg );
            }
        }

        wxLogDebug( "Can't get cert from %s. Retry limit (3) exceeded. "
                    "Waiting 30s.",
            serviceInfo->hostname );

        std::unique_lock<std::mutex> lock( serviceInfo->mutex );
        if ( serviceInfo->stopping )
        {
            return false;
        }
        serviceInfo->stopVar.wait_for( lock, std::chrono::seconds( 30 ) );

    } while ( !serviceInfo->stopping );

    return false;
}

bool RemoteManager::doRegistrationV2( RemoteInfo* serviceInfo )
{
    wxLogDebug( "Registering with %s (%s:%d) - api version 1",
        serviceInfo->id, serviceInfo->ips.ipv4, serviceInfo->authPort );

    return false;
}

};
