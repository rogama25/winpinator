#include "page_hostlist.hpp"

#include "../../win32/resource.h"
#include "../globals.hpp"
#include "utils.hpp"

#include <wx/tooltip.h>

namespace gui
{

wxDEFINE_EVENT( EVT_NO_HOSTS_IN_TIME, wxCommandEvent );

const wxString HostListPage::s_details = _(
    "Below is a list of currently available computers. "
    "Select the one you want to transfer your files to." );

const wxString HostListPage::s_detailsWrapped = _(
    "Below is a list of currently available computers.\n"
    "Select the one you want to transfer your files to." );

const int HostListPage::NO_HOSTS_TIMEOUT_MILLIS = 15000;

HostListPage::HostListPage( wxWindow* parent )
    : wxPanel( parent, wxID_ANY )
    , m_header( nullptr )
    , m_details( nullptr )
    , m_refreshBtn( nullptr )
    , m_hostlist( nullptr )
    , m_fwdBtn( nullptr )
    , m_progLbl( nullptr )
    , m_refreshBmp( wxNullBitmap )
    , m_timer( nullptr )
{
    wxBoxSizer* margSizer = new wxBoxSizer( wxHORIZONTAL );

    margSizer->AddSpacer( FromDIP( 20 ) );
    margSizer->AddStretchSpacer();

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    wxBoxSizer* headingSizerH = new wxBoxSizer( wxHORIZONTAL );
    wxBoxSizer* headingSizerV = new wxBoxSizer( wxVERTICAL );

    m_header = new wxStaticText( this, wxID_ANY,
        _( "Select where to send your files" ) );
    m_header->SetFont( Utils::get()->getHeaderFont() );
    m_header->SetForegroundColour( Utils::get()->getHeaderColor() );
    headingSizerV->Add( m_header, 0, wxEXPAND | wxTOP, FromDIP( 25 ) );

    m_details = new wxStaticText( this, wxID_ANY, HostListPage::s_details,
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END );
    headingSizerV->Add( m_details, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP( 4 ) );

    headingSizerH->Add( headingSizerV, 1, wxEXPAND | wxRIGHT, FromDIP( 15 ) );

    m_refreshBtn = new ToolButton( this, wxID_ANY, wxEmptyString );
    m_refreshBtn->SetToolTip( new wxToolTip( _( "Refresh list" ) ) );
    m_refreshBtn->SetWindowVariant( wxWindowVariant::wxWINDOW_VARIANT_LARGE );
    loadIcon();
    m_refreshBtn->SetWindowStyle( wxBU_EXACTFIT );
    m_refreshBtn->SetBitmapMargins( FromDIP( 1 ), FromDIP( 1 ) );
    headingSizerH->Add( m_refreshBtn, 0, wxALIGN_BOTTOM );

    mainSizer->Add( headingSizerH, 0, wxEXPAND );

    m_hostlist = new HostListbox( this );
    m_hostlist->SetWindowStyle( wxBORDER_THEME );
    mainSizer->Add( m_hostlist, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP( 10 ) );

    wxBoxSizer* bottomBar = new wxBoxSizer( wxHORIZONTAL );

    m_progLbl = new ProgressLabel( this, 
        _( "Searching for computers on your network..." ) );
    bottomBar->Add( m_progLbl, 0, wxALIGN_CENTER_VERTICAL, 0 );

    bottomBar->AddStretchSpacer();

    m_fwdBtn = new wxButton( this, wxID_ANY, _( "&Next >" ) );
    m_fwdBtn->SetMinSize( wxSize( m_fwdBtn->GetSize().x * 1.5, FromDIP( 25 ) ) );
    m_fwdBtn->Disable();
    bottomBar->Add( m_fwdBtn, 0, wxALIGN_CENTER_VERTICAL, 0 );

    mainSizer->Add( bottomBar, 0, wxEXPAND | wxBOTTOM, FromDIP( 25 ) );

    margSizer->Add( mainSizer, 10, wxEXPAND );

    margSizer->AddStretchSpacer();
    margSizer->AddSpacer( FromDIP( 20 ) );

    SetSizer( margSizer );

    m_timer = new wxTimer( this );
    refreshAll();

    observeService( Globals::get()->getWinpinatorServiceInstance() );

    // Events

    Bind( wxEVT_DPI_CHANGED, &HostListPage::onDpiChanged, this );
    Bind( wxEVT_SIZE, &HostListPage::onLabelResized, this );
    Bind( wxEVT_BUTTON, &HostListPage::onRefreshClicked, this );
    Bind( wxEVT_TIMER, &HostListPage::onTimerTicked, this );
    Bind( wxEVT_THREAD, &HostListPage::onManipulateList, this );
}

void HostListPage::refreshAll()
{
    m_timer->StartOnce( HostListPage::NO_HOSTS_TIMEOUT_MILLIS );

    // Get current service state (host list)
    auto serv = Globals::get()->getWinpinatorServiceInstance();

    m_trackedRemotes = serv->getRemoteManager()->generateCurrentHostList();
    
    m_hostlist->clear();

    for ( srv::RemoteInfoPtr remote : m_trackedRemotes )
    {
        m_hostlist->addItem( convertRemoteInfoToHostItem( remote ) );
    }
}

void HostListPage::onDpiChanged( wxDPIChangedEvent& event )
{
    loadIcon();
}

void HostListPage::onLabelResized( wxSizeEvent& event )
{
    int textWidth = m_details->GetTextExtent( HostListPage::s_details ).x;

    if ( textWidth > m_details->GetSize().x )
    {
        m_details->SetLabel( HostListPage::s_detailsWrapped );
    }
    else
    {
        m_details->SetLabel( HostListPage::s_details );
    }

    event.Skip();
}

void HostListPage::onRefreshClicked( wxCommandEvent& event )
{
    srv::Event srvEvent;
    srvEvent.type = srv::EventType::REPEAT_MDNS_QUERY;

    Globals::get()->getWinpinatorServiceInstance()->postEvent( srvEvent );
}

void HostListPage::onTimerTicked( wxTimerEvent& event )
{
    auto serv = Globals::get()->getWinpinatorServiceInstance();

    if ( serv->getRemoteManager()->getVisibleHostsCount() == 0 )
    {
        wxCommandEvent evt( EVT_NO_HOSTS_IN_TIME );
        wxPostEvent( this, evt );
    }
}

void HostListPage::onManipulateList( wxThreadEvent& event )
{
    switch ( (ThreadEventType)event.GetInt() )
    {
    case ThreadEventType::ADD:
    {
        srv::RemoteInfoPtr info = event.GetPayload<srv::RemoteInfoPtr>();

        for ( const srv::RemoteInfoPtr& ptr : m_trackedRemotes )
        {
            if ( info->id == ptr->id )
            {
                return;
            }
        }

        m_trackedRemotes.push_back( info );
        m_hostlist->addItem( convertRemoteInfoToHostItem( info ) );

        break;
    }
    case ThreadEventType::RESET:
    {
        m_trackedRemotes.clear();
        m_hostlist->clear();

        break;
    }
    
    }
    
}

void HostListPage::loadIcon()
{
    wxBitmap original;
    original.LoadFile( Utils::makeIntResource( IDB_REFRESH ),
        wxBITMAP_TYPE_PNG_RESOURCE );

    wxImage toScale = original.ConvertToImage();

    int size = FromDIP( 24 );

    m_refreshBmp = toScale.Scale( size, size, wxIMAGE_QUALITY_BICUBIC );
    m_refreshBtn->SetBitmap( m_refreshBmp, wxDirection::wxWEST );
}

HostItem HostListPage::convertRemoteInfoToHostItem( srv::RemoteInfoPtr rinfo )
{
    HostItem result;
    result.id = rinfo->id;

    if ( rinfo->shortName.empty() )
    {
        result.hostname = rinfo->hostname;
    }
    else
    {
        result.hostname = rinfo->shortName + '@' + rinfo->hostname;
    }

    result.ipAddress = rinfo->ips.ipv4;

    if ( result.ipAddress.empty() )
    {
        result.ipAddress = rinfo->ips.ipv6;
    }

    result.os = rinfo->os;
    result.profileBmp = std::make_shared<wxBitmap>( wxNullBitmap );
    result.profilePic = wxNullImage;
    result.state = rinfo->state;

    if ( rinfo->state == srv::RemoteStatus::ONLINE )
    {
        result.username = rinfo->fullName;
    }
    else if ( rinfo->state == srv::RemoteStatus::UNREACHABLE
        || rinfo->state == srv::RemoteStatus::OFFLINE )
    {
        result.username = _( "Data unavailable" );
    }
    else
    {
        result.username = _( "Loading..." );
    }

    return result;
}

void HostListPage::onStateChanged()
{
    auto serv = Globals::get()->getWinpinatorServiceInstance();

    if ( !serv->isOnline() )
    {
        wxThreadEvent evnt;
        evnt.SetInt( (int)ThreadEventType::RESET );

        wxQueueEvent( this, evnt.Clone() );
    }
}

void HostListPage::onAddHost( srv::RemoteInfoPtr info )
{
    wxThreadEvent evnt;
    evnt.SetInt( (int)ThreadEventType::ADD );
    evnt.SetPayload( info );

    wxQueueEvent( this, evnt.Clone() );
}

};
