#include "global.h"
#include "MemoryCardManager.h"
#include "arch/MemoryCard/MemoryCardDriver.h"	// for UsbStorageDevice
#include "ScreenManager.h"
#include "ThemeManager.h"
#include "PrefsManager.h"
#include "RageLog.h"
#include "RageFileManager.h"
#include "RageFileDriver.h"
#include "ScreenManager.h"
#include "ProfileManager.h"
#include "Foreach.h"
#include "GameState.h"

MemoryCardManager*	MEMCARDMAN = NULL;	// global and accessable from anywhere in our program

const CString MEM_CARD_MOUNT_POINT[NUM_PLAYERS] =
{
	/* @ is importast; see RageFileManager LoadedDriver::GetPath */
	"@mc1/",
	"@mc2/",
};

static const CString MEM_CARD_MOUNT_POINT_INTERNAL[NUM_PLAYERS] =
{
	/* @ is importast; see RageFileManager LoadedDriver::GetPath */
	"@mc1int/",
	"@mc2int/",
};

MemoryCardManager::MemoryCardManager()
{
	m_pDriver = MakeMemoryCardDriver();
	m_bCardsLocked = false;
	FOREACH_PlayerNumber( p )
	{
		m_bTooLate[p] = false;
		m_bMounted[p] = false;
	}
	
	/* These can play at any time.  Preload them, so we don't cause a skip in gameplay. */
	m_soundReady.Load( THEME->GetPathToS("MemoryCardManager ready"), true );
	m_soundError.Load( THEME->GetPathToS("MemoryCardManager error"), true );
	m_soundTooLate.Load( THEME->GetPathToS("MemoryCardManager too late"), true );
	m_soundDisconnect.Load( THEME->GetPathToS("MemoryCardManager disconnect"), true );
}

MemoryCardManager::~MemoryCardManager()
{
	delete m_pDriver;

	FOREACH_PlayerNumber( pn )
	{
		FILEMAN->Unmount( "", "", MEM_CARD_MOUNT_POINT[pn] );
		FILEMAN->Unmount( "", "", MEM_CARD_MOUNT_POINT_INTERNAL[pn] );
	}
}

void MemoryCardManager::Update( float fDelta )
{
	if( m_pDriver->StorageDevicesChanged() )
	{
		vector<UsbStorageDevice> vOld = m_vStorageDevices;	// copy
		m_pDriver->GetStorageDevices( m_vStorageDevices );
		vector<UsbStorageDevice> &vNew = m_vStorageDevices;
		vector<UsbStorageDevice> vConnects;	// fill these in below
		vector<UsbStorageDevice> vDisconnects;	// fill these in below
		
		// check for disconnects
		FOREACH( UsbStorageDevice, vOld, old )
		{
			vector<UsbStorageDevice>::iterator iter = find( vNew.begin(), vNew.end(), *old );
			if( iter == vNew.end() )	// card no longer present
			{
				LOG->Trace( "Disconnected bus %d port %d device %d path %s", old->iBus, old->iPort, old->iLevel, old->sOsMountDir.c_str() );
				vDisconnects.push_back( *old );
			}
		}
		
		// check for connects
		FOREACH( UsbStorageDevice, vNew, newd )
		{
			vector<UsbStorageDevice>::iterator iter = find( vOld.begin(), vOld.end(), *newd );
			if( iter == vOld.end() )	// card wasn't present last update
			{
				LOG->Trace( "Connected bus %d port %d device %d path %s", newd->iBus, newd->iPort, newd->iLevel, newd->sOsMountDir.c_str() );
				vConnects.push_back( *newd );
			}
		}
		
		// unassign cards that were disconnected
		FOREACH_PlayerNumber( p )
		{
			UsbStorageDevice &assigned_device = m_Device[p];
			if( assigned_device.IsBlank() )	// not assigned a card
				continue;
			
			vector<UsbStorageDevice>::iterator iter = find( vDisconnects.begin(), vDisconnects.end(), assigned_device );
			if( iter != vDisconnects.end() )
			{
				UnmountCard( p );
				
				assigned_device.MakeBlank();
				m_soundDisconnect.Play();
				
				if( PROFILEMAN->ProfileWasLoadedFromMemoryCard(p) )
					PROFILEMAN->UnloadProfile( p );
			}
		}
		
		// Update the status of already-assigned cards.  It may contain updated info 
		// like WriteTest results and a new sName.
		FOREACH_PlayerNumber( p )
		{
			UsbStorageDevice &assigned_device = m_Device[p];
			if( assigned_device.IsBlank() )     // no card assigned to this player
				continue;
			
			vector<UsbStorageDevice>::iterator iter = find( m_vStorageDevices.begin(), m_vStorageDevices.end(), assigned_device );
			if( iter != m_vStorageDevices.end() )
			{
				// play write test error sound if write test failed since we last checked
				if( assigned_device.bNeedsWriteTest && !iter->bNeedsWriteTest && !iter->bWriteTestSucceeded )
					m_soundError.Play();
				assigned_device = *iter;
			}
		}
		
		// make a list of unassigned
		vector<UsbStorageDevice> vUnassignedDevices = m_vStorageDevices;        // copy
		
		// remove cards that are already assigned
		FOREACH_PlayerNumber( p )
		{
			UsbStorageDevice &assigned_device = m_Device[p];
			if( assigned_device.IsBlank() )     // no card assigned to this player
				continue;
			
			FOREACH( UsbStorageDevice, vUnassignedDevices, d )
			{
				if( *d == assigned_device )
				{
					vUnassignedDevices.erase( d );
					break;
				}
			}
		}
		
		// try to assign each device to a player
		FOREACH_PlayerNumber( p )
		{
			LOG->Trace( "Looking for a card for Player %d", p+1 );
			
			UsbStorageDevice &assigned_device = m_Device[p];		    
			if( !assigned_device.IsBlank() )    // they already have an assigned card
			{
				LOG->Trace( "Player %d already has a card: '%s'", p+1, assigned_device.sOsMountDir.c_str() );
				continue;       // skip
			}
			
			FOREACH( UsbStorageDevice, vUnassignedDevices, d )
			{
				// search for card dir match
				if( !PREFSMAN->m_sMemoryCardOsMountPoint[p].empty() &&
					d->sOsMountDir.CompareNoCase(PREFSMAN->m_sMemoryCardOsMountPoint[p]) )
					continue;      // not a match
				
				// search for USB bus match
				if( PREFSMAN->m_iMemoryCardUsbBus[p] != -1 &&
					PREFSMAN->m_iMemoryCardUsbBus[p] != d->iBus )
					continue;       // not a match
				
				if( PREFSMAN->m_iMemoryCardUsbPort[p] != -1 &&
					PREFSMAN->m_iMemoryCardUsbPort[p] != d->iPort )
					continue;       // not a match
				
				if( PREFSMAN->m_iMemoryCardUsbLevel[p] != -1 &&
					PREFSMAN->m_iMemoryCardUsbLevel[p] != d->iLevel )
					continue;       // not a match
				
				LOG->Trace( "device match:  iScsiIndex: %d, iBus: %d, iLevel: %d, iPort: %d, sOsMountDir: %s",
					d->iScsiIndex, d->iBus, d->iLevel, d->iPort, d->sOsMountDir.c_str() );
				
				assigned_device = *d;    // save a copy
				vUnassignedDevices.erase( d );       // remove the device so we don't match it for another player
				m_bTooLate[p] = GAMESTATE->m_bPlayersFinalized;    // the device is too late if inserted when cards were locked
				
				// play sound
				if( m_bTooLate[p] )
					m_soundTooLate.Play();
				else if( !d->bNeedsWriteTest && !d->bWriteTestSucceeded )
					m_soundError.Play();
				else
					m_soundReady.Play();
				
				break;
			}
		}
		
		SCREENMAN->RefreshCreditsMessages();
	}
}

MemoryCardState MemoryCardManager::GetCardState( PlayerNumber pn )
{
	UsbStorageDevice &d = m_Device[pn];
	if( d.IsBlank() )
		return MEMORY_CARD_STATE_NO_CARD;
	else if( m_bTooLate[pn] )
		return MEMORY_CARD_STATE_TOO_LATE;
	else if( !d.bNeedsWriteTest && !d.bWriteTestSucceeded )
		return MEMORY_CARD_STATE_WRITE_ERROR;
	else
		return MEMORY_CARD_STATE_READY;
}

void MemoryCardManager::LockCards()
{
	if( m_bCardsLocked )
		return;

	m_bCardsLocked = true;
}

void MemoryCardManager::UnlockCards()
{
	m_bCardsLocked = false;
	
	// clear too late flag
	FOREACH_PlayerNumber( p )
		m_bTooLate[p] = false;
	
	m_pDriver->SetMountThreadState( MemoryCardDriver::detect_and_mount );
}

/* Called in EndGame just before writing the profile.  Should block. */
void MemoryCardManager::MountUsedCard( PlayerNumber pn )
{
	if( m_Device[pn].IsBlank() )	// they don't have an assigned card
		return;
	
	MountCard( pn );
}

void MemoryCardManager::MountCard( PlayerNumber pn )
{
	ASSERT( !m_Device[pn].IsBlank() );

	if( !m_Device[pn].bWriteTestSucceeded || m_bTooLate[pn] )
		return;

	/* Pause the mounting thread when we mount the first drive. */
	bool bStartingMemoryCardAccess = true;
	FOREACH_PlayerNumber( p )
		if( m_bMounted[p] )
			bStartingMemoryCardAccess = false; /* already did */
	if( bStartingMemoryCardAccess )
	{
		/* We're starting to do stuff to the memory cards. */
		this->PauseMountingThread();
	}

	if( !m_pDriver->MountAndTestWrite(&m_Device[pn]) )
	{
		if( bStartingMemoryCardAccess )
			this->UnPauseMountingThread();

		return;
	}

	m_bMounted[p] = true;

	/* If this is the first time we're mounting the device, mount the VFS drivers.
	 * Simply mounting our VFS on a directory doesn't actually touch the directory,
	 * so this isn't a timeout risk.  (This is important for other reasons; for example,
	 * if we mount a CDROM, we should not spin up the disc simply by mounting it.) */
	RageFileDriver *pDriver = FILEMAN->GetFileDriver( MEM_CARD_MOUNT_POINT_INTERNAL[pn] );
	if( pDriver == NULL )
	{
		FILEMAN->Mount( "dir", m_Device[pn].sOsMountDir, MEM_CARD_MOUNT_POINT_INTERNAL[pn] );
		FILEMAN->Mount( "timeout", MEM_CARD_MOUNT_POINT_INTERNAL[pn], MEM_CARD_MOUNT_POINT[pn] );
	}
	else
	{
		/* It's already mounted.  We don't want to unmount the timeout FS.  Instead, just
		 * move the target. */
		pDriver->Remount( m_Device[pn].sOsMountDir );
		FILEMAN->ReleaseFileDriver( pDriver );
	}
}

/* Called in EndGame just after writing the profile.  Called by PlayersFinalized just after
 * reading the profile.  Should never block; use FlushAndReset to block until writes complete. */
void MemoryCardManager::UnmountCard( PlayerNumber pn )
{
	ASSERT( !m_Device[pn].IsBlank() );

	if( !m_bMounted[pn] )
		return;

	/* Leave our own filesystem drivers mounted. */
	m_pDriver->Unmount( &m_Device[pn] );

	m_bMounted[pn] = false;

	/* Unpause the mounting thread when we unmount the last drive. */
	bool bNeedUnpause = true;
	FOREACH_PlayerNumber( p )
		if( m_bMounted[p] )
			bNeedUnpause = false;
	if( bNeedUnpause )
		this->UnPauseMountingThread();
}

void MemoryCardManager::FlushAndReset()
{
	FOREACH_PlayerNumber( p )
	{
		UsbStorageDevice &d = m_Device[p];
		if( d.IsBlank() )	// no card assigned
			continue;	// skip
		if( (!d.bNeedsWriteTest && !d.bWriteTestSucceeded) || m_bTooLate[p] )
			continue;	// skip
		m_pDriver->Flush(&m_Device[p]);
	}
	
	m_pDriver->ResetUsbStorage();	// forces cards to be re-detected
}

bool MemoryCardManager::PathIsMemCard( CString sDir ) const
{
	FOREACH_PlayerNumber( p )
		if( !sDir.Left(MEM_CARD_MOUNT_POINT[p].size()).CompareNoCase( MEM_CARD_MOUNT_POINT[p] ) )
			return true;
	return false;
}

bool MemoryCardManager::IsNameAvailable( PlayerNumber pn ) const
{
	return m_Device[pn].bIsNameAvailable;
}

CString MemoryCardManager::GetName( PlayerNumber pn ) const
{
	return m_Device[pn].sName;
}

void MemoryCardManager::PauseMountingThread()
{
	m_pDriver->SetMountThreadState( MemoryCardDriver::paused );
}

void MemoryCardManager::UnPauseMountingThread()
{
	m_pDriver->SetMountThreadState( 
		m_bCardsLocked ? 
		MemoryCardDriver::detect_and_dont_mount : 
	MemoryCardDriver::detect_and_mount );
}

bool IsAnyPlayerUsingMemoryCard()
{
	FOREACH_HumanPlayer( pn )
	{
		if( MEMCARDMAN->GetCardState(pn) == MEMORY_CARD_STATE_READY )
			return true;
	}
	return false;
}

#include "LuaFunctions.h"
LuaFunction_NoArgs( IsAnyPlayerUsingMemoryCard,		IsAnyPlayerUsingMemoryCard() )

/*
 * (c) 2003-2004 Chris Danford
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
