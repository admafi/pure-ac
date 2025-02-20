//Anything above this #include will be ignored by the compiler
#include "../qcommon/exe_headers.h"

// sv_client.c -- server code for dealing with clients

#include "server.h"
#include "../qcommon/stringed_ingame.h"
#include "../RMG/RM_Headers.h"
#include "../zlib32/zip.h"

static void SV_CloseDownload( client_t *cl );
/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.
=================
*/
void SV_GetChallenge( netadr_t from ) {
	int		i;
	int		oldest;
	int		oldestTime;
	challenge_t	*challenge;

	// ignore if we are in single player
	/*
	if ( Cvar_VariableValue( "g_gametype" ) == GT_SINGLE_PLAYER || Cvar_VariableValue("ui_singlePlayerActive")) {
		return;
	}
	*/
	if (Cvar_VariableValue("ui_singlePlayerActive"))
	{
		return;
	}

	oldest = 0;
	oldestTime = 0x7fffffff;

	// see if we already have a challenge for this ip
	challenge = &svs.challenges[0];
	for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
		if ( !challenge->connected && NET_CompareAdr( from, challenge->adr ) ) {
			break;
		}
		if ( challenge->time < oldestTime ) {
			oldestTime = challenge->time;
			oldest = i;
		}
	}

	if (i == MAX_CHALLENGES) {
		// this is the first time this client has asked for a challenge
		challenge = &svs.challenges[oldest];

		challenge->challenge = ( (rand() << 16) ^ rand() ) ^ svs.time;
		challenge->adr = from;
		challenge->firstTime = svs.time;
		challenge->time = svs.time;
		challenge->connected = qfalse;
		i = oldest;
	}

	// if they are on a lan address, send the challengeResponse immediately
	if ( Sys_IsLANAddress( from ) ) {
		challenge->pingTime = svs.time;
		NET_OutOfBandPrint( NS_SERVER, from, "challengeResponse %i", challenge->challenge );
		return;
	}

#ifdef USE_CD_KEY
	// look up the authorize server's IP
	if ( !svs.authorizeAddress.ip[0] && svs.authorizeAddress.type != NA_BAD ) {
		Com_Printf( "Resolving %s\n", AUTHORIZE_SERVER_NAME );
		if ( !NET_StringToAdr( AUTHORIZE_SERVER_NAME, &svs.authorizeAddress ) ) {
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs.authorizeAddress.port = BigShort( PORT_AUTHORIZE );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", AUTHORIZE_SERVER_NAME,
			svs.authorizeAddress.ip[0], svs.authorizeAddress.ip[1],
			svs.authorizeAddress.ip[2], svs.authorizeAddress.ip[3],
			BigShort( svs.authorizeAddress.port ) );
	}

	// if they have been challenging for a long time and we
	// haven't heard anything from the authoirze server, go ahead and
	// let them in, assuming the id server is down
	if ( svs.time - challenge->firstTime > AUTHORIZE_TIMEOUT ) {
		Com_DPrintf( "authorize server timed out\n" );

		challenge->pingTime = svs.time;
		NET_OutOfBandPrint( NS_SERVER, challenge->adr,
			"challengeResponse %i", challenge->challenge );
		return;
	}

	// otherwise send their ip to the authorize server
	if ( svs.authorizeAddress.type != NA_BAD ) {
		cvar_t	*fs;
		char	game[1024];

		game[0] = 0;
		fs = Cvar_Get ("fs_game", "", CVAR_INIT|CVAR_SYSTEMINFO );
		if (fs && fs->string[0] != 0) {
			strcpy(game, fs->string);
		}
		Com_DPrintf( "sending getIpAuthorize for %s\n", NET_AdrToString( from ));
		fs = Cvar_Get ("sv_allowAnonymous", "0", CVAR_SERVERINFO);

		NET_OutOfBandPrint( NS_SERVER, svs.authorizeAddress,
			"getIpAuthorize %i %i.%i.%i.%i %s %s",  svs.challenges[i].challenge,
			from.ip[0], from.ip[1], from.ip[2], from.ip[3], game, fs->integer );
	}
#else
	challenge->pingTime = svs.time;
	NET_OutOfBandPrint( NS_SERVER, challenge->adr, "challengeResponse %i", challenge->challenge );
#endif	// USE_CD_KEY
}

/*
====================
SV_AuthorizeIpPacket

A packet has been returned from the authorize server.
If we have a challenge adr for that ip, send the
challengeResponse to it
====================
*/
#ifndef _XBOX	// No authorization on Xbox
void SV_AuthorizeIpPacket( netadr_t from ) {
	int		challenge;
	int		i;
	char	*s;
	char	*r;
	char	ret[1024];

	if ( !NET_CompareBaseAdr( from, svs.authorizeAddress ) ) {
		Com_Printf( "SV_AuthorizeIpPacket: not from authorize server\n" );
		return;
	}

	challenge = atoi( Cmd_Argv( 1 ) );

	for (i = 0 ; i < MAX_CHALLENGES ; i++) {
		if ( svs.challenges[i].challenge == challenge ) {
			break;
		}
	}
	if ( i == MAX_CHALLENGES ) {
		Com_Printf( "SV_AuthorizeIpPacket: challenge not found\n" );
		return;
	}

	// send a packet back to the original client
	svs.challenges[i].pingTime = svs.time;
	s = Cmd_Argv( 2 );
	r = Cmd_Argv( 3 );			// reason

	if ( !Q_stricmp( s, "demo" ) ) {
		if ( Cvar_VariableValue( "fs_restrict" ) ) {
			// a demo client connecting to a demo server
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr,
				"challengeResponse %i", svs.challenges[i].challenge );
			return;
		}
		// they are a demo client trying to connect to a real server
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nServer is not a demo server\n" );
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}
	if ( !Q_stricmp( s, "accept" ) ) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr,
			"challengeResponse %i", svs.challenges[i].challenge );
		return;
	}
	if ( !Q_stricmp( s, "unknown" ) ) {
		if (!r) {
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nAwaiting CD key authorization\n" );
		} else {
			sprintf(ret, "print\n%s\n", r);
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, ret );
		}
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}

	// authorization failed
	if (!r) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nSomeone is using this CD Key\n" );
	} else {
		sprintf(ret, "print\n%s\n", r);
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, ret );
	}

	// clear the challenge record so it won't timeout and let them through
	Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
}
#endif

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/
void SV_DirectConnect( netadr_t from ) {
	char		userinfo[MAX_INFO_STRING];
	int			i;
	client_t	*cl, *newcl;
	MAC_STATIC client_t	temp;
	sharedEntity_t *ent;
	int			clientNum;
	int			version;
	int			qport;
	int			challenge;
	char		*password;
	int			startIndex;
	char		*denied;
	int			count;
	bool		reconnect = false;

	Com_DPrintf ("SVC_DirectConnect ()\n");

	Q_strncpyz( userinfo, Cmd_Argv(1), sizeof(userinfo) );

	version = atoi( Info_ValueForKey( userinfo, "protocol" ) );
	if ( version != PROTOCOL_VERSION ) {
		NET_OutOfBandPrint( NS_SERVER, from, "print\nServer uses protocol version %i.\n", PROTOCOL_VERSION );
		Com_DPrintf ("    rejected connect from version %i\n", version);
		return;
	}

	challenge = atoi( Info_ValueForKey( userinfo, "challenge" ) );
	qport = atoi( Info_ValueForKey( userinfo, "qport" ) );

	// quick reject
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

/* This was preventing sv_reconnectlimit from working.  It seems like commenting this
   out has solved the problem.  HOwever, if there is a future problem then it could
   be this.

		if ( cl->state == CS_FREE ) {
			continue;
		}
*/

		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			if (( svs.time - cl->lastConnectTime)
				< (sv_reconnectlimit->integer * 1000)) {
				NET_OutOfBandPrint( NS_SERVER, from, "print\nReconnect rejected : too soon\n" );
				Com_DPrintf ("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
				return;
			}
			break;
		}
	}

	// see if the challenge is valid (LAN clients don't need to challenge)
	if ( !NET_IsLocalAddress (from) ) {
		int		ping;

		for (i=0 ; i<MAX_CHALLENGES ; i++) {
			if (NET_CompareAdr(from, svs.challenges[i].adr)) {
				if ( challenge == svs.challenges[i].challenge ) {
					break;		// good
				}
			}
		}
		if (i == MAX_CHALLENGES) {
			NET_OutOfBandPrint( NS_SERVER, from, "print\nNo or bad challenge for address.\n" );
			return;
		}
		// force the IP key/value pair so the game can filter based on ip
		Info_SetValueForKey( userinfo, "ip", NET_AdrToString( from ) );

		ping = svs.time - svs.challenges[i].pingTime;
		Com_Printf( SE_GetString("MP_SVGAME", "CLIENT_CONN_WITH_PING"), i, ping);//"Client %i connecting with %i challenge ping\n", i, ping );
		svs.challenges[i].connected = qtrue;

		// never reject a LAN client based on ping
		if ( !Sys_IsLANAddress( from ) ) {
			if ( sv_minPing->value && ping < sv_minPing->value ) {
				// don't let them keep trying until they get a big delay
				NET_OutOfBandPrint( NS_SERVER, from, va("print\n%s\n", SE_GetString("MP_SVGAME", "SERVER_FOR_HIGH_PING")));//Server is for high pings only\n" );
				Com_DPrintf (SE_GetString("MP_SVGAME", "CLIENT_REJECTED_LOW_PING"), i);//"Client %i rejected on a too low ping\n", i);
				// reset the address otherwise their ping will keep increasing
				// with each connect message and they'd eventually be able to connect
				svs.challenges[i].adr.port = 0;
				return;
			}
			if ( sv_maxPing->value && ping > sv_maxPing->value ) {
				NET_OutOfBandPrint( NS_SERVER, from, va("print\n%s\n", SE_GetString("MP_SVGAME", "SERVER_FOR_LOW_PING")));//Server is for low pings only\n" );
				Com_DPrintf (SE_GetString("MP_SVGAME", "CLIENT_REJECTED_HIGH_PING"), i);//"Client %i rejected on a too high ping\n", i);
				return;
			}
		}
	} else {
		// force the "ip" info key to "localhost"
		Info_SetValueForKey( userinfo, "ip", "localhost" );
	}

	newcl = &temp;
	Com_Memset (newcl, 0, sizeof(client_t));

	// if there is already a slot for this ip, reuse it
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( cl->state == CS_FREE ) {
			continue;
		}
		if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
			&& ( cl->netchan.qport == qport
			|| from.port == cl->netchan.remoteAddress.port ) ) {
			Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
			newcl = cl;
			reconnect = true;
			// VVFIXME - both SOF2 and Wolf remove this call, claiming it blows away the user's info
			// disconnect the client from the game first so any flags the
			// player might have are dropped
			VM_Call( gvm, GAME_CLIENT_DISCONNECT, newcl - svs.clients );
			//
			goto gotnewcl;
		}
	}

	// find a client slot
	// if "sv_privateClients" is set > 0, then that number
	// of client slots will be reserved for connections that
	// have "password" set to the value of "sv_privatePassword"
	// Info requests will report the maxclients as if the private
	// slots didn't exist, to prevent people from trying to connect
	// to a full server.
	// This is to allow us to reserve a couple slots here on our
	// servers so we can play without having to kick people.

	// check for privateClient password
	password = Info_ValueForKey( userinfo, "password" );
#ifdef _XBOX	// We don't do private slots quite the same
	startIndex = 0;
#else
	if ( !strcmp( password, sv_privatePassword->string ) ) {
		startIndex = 0;
	} else {
		// skip past the reserved slots
		startIndex = sv_privateClients->integer;
	}
#endif

	newcl = NULL;
	for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
		cl = &svs.clients[i];
		if (cl->state == CS_FREE) {
			newcl = cl;
			break;
		}
	}

	if ( !newcl ) {
		if ( NET_IsLocalAddress( from ) ) {
			count = 0;
			for ( i = startIndex; i < sv_maxclients->integer ; i++ ) {
				cl = &svs.clients[i];
				if (cl->netchan.remoteAddress.type == NA_BOT) {
					count++;
				}
			}
			// if they're all bots
			if (count >= sv_maxclients->integer - startIndex) {
				SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
				newcl = &svs.clients[sv_maxclients->integer - 1];
			}
			else {
				Com_Error( ERR_FATAL, "server is full on local connect\n" );
				return;
			}
		}
		else {
			const char *SV_GetStringEdString(char *refSection, char *refName);
			NET_OutOfBandPrint( NS_SERVER, from, va("print\n%s\n", SV_GetStringEdString("MP_SVGAME","SERVER_IS_FULL")));
			Com_DPrintf ("Rejected a connection.\n");
			return;
		}
	}

	// we got a newcl, so reset the reliableSequence and reliableAcknowledge
	cl->reliableAcknowledge = 0;
	cl->reliableSequence = 0;

gotnewcl:

#ifdef _XBOX
	// OK. We used to manually search for a spot in the xbOnlineInfo player list now.
	// But we MUST keep svs.clients in sync with the player list, so that clients
	// can keep their cgs.clientinfo in sync as well. So...
	int index = newcl - svs.clients;

	// Sanity check
	assert( index >= 0 && index < MAX_ONLINE_PLAYERS );

	// Avoid a nasty bug situation: if the client is not logged on, they didn't send
	// an XUID. Don't let live clients on syslink servers or vice-versa. Localhost
	// doesn't need this check.
	const char *sxuid = Info_ValueForKey( userinfo, "xuid" );
	if ( !NET_IsLocalAddress(from) && ((logged_on && !*sxuid) || (!logged_on && *sxuid)) )
	{
		// We may not be able to reply to them - we might not have their address registered
		NET_OutOfBandPrint( NS_SERVER, from, "print\nLive/SystemLink mismatch\n" );
		return;
	}

	if ( reconnect )
	{
		// This is a reconnect message. They're going into the same slot as before.

		// We don't need to grab much off the net here. Everything should still be valid.
		// In particular, keep the old refIndex. If they sent an XUID, we'll update that,
		// but if not we want to continue using their fake one from earlier.
		if (logged_on)
		{
			StringToXUID(&xbOnlineInfo.xbPlayerList[index].xuid, sxuid);
		}

		// Ensure the client is active
		xbOnlineInfo.xbPlayerList[index].isActive = true;
	}
	else
	{
		// OK. New client. First, sanity check that player and client lists are in sync:
		assert( !xbOnlineInfo.xbPlayerList[index].isActive );

		// Again, we don't have a full PlayerInfo like SOF2, but this is fine
		XBPlayerInfo *pPlayer = &xbOnlineInfo.xbPlayerList[index];

		// Zero it out to start with
		memset(pPlayer, 0, sizeof(XBPlayerInfo));

		// Copy XNADDR
		StringToXnAddr(&pPlayer->xbAddr, Info_ValueForKey( userinfo, "xnaddr" ));

		// Copy XUID
		if(logged_on)
		{
			StringToXUID(&pPlayer->xuid, sxuid);
		}
		else
		{
			// Users have no XUID if not logged on. We make one up.
			pPlayer->xuid.qwUserID = svs.clientRefNum;
		}

		pPlayer->refIndex = svs.clientRefNum++;
		pPlayer->isActive = true;
	}

	// One more thing we need - the client will send an xbps "1" if they can
	// use a private slot (XBox Private Slot). Just check for its existence...
	bool usePrivateSlot = (strcmp( Info_ValueForKey( userinfo, "xbps" ), "1" ) == 0);

	// Set our refIndex for later. We also remember if they were a private slot
	// candidate when they joined, so we can free up the right type of slot when
	// they leave later.
	temp.refIndex = xbOnlineInfo.xbPlayerList[index].refIndex;
	temp.usePrivateSlot = usePrivateSlot;

	// Remove any mute flags as they are local to the client.
	xbOnlineInfo.xbPlayerList[index].flags &= ~MUTED_PLAYER;
	xbOnlineInfo.xbPlayerList[index].flags &= ~REMOTE_MUTED;

	// Disable new player's voice until they send their VOICESTATE
	xbOnlineInfo.xbPlayerList[index].flags &= ~VOICE_CAN_RECV;
	xbOnlineInfo.xbPlayerList[index].flags &= ~VOICE_CAN_SEND;

	// Now send the new client's info to all other relevant clients
	for (int j = 0; j < sv_maxclients->integer; j++) {
		if ( svs.clients[j].state < CS_PRIMED ) {
			continue;
		}
		if (svs.clients[j].netchan.remoteAddress.type == NA_BOT) {
			continue;
		}
		SV_SendClientNewPeer( &svs.clients[j], &xbOnlineInfo.xbPlayerList[index] );
	}
#endif

	// build a new connection
	// accept the new client
	// this is the only place a client_t is ever initialized
	*newcl = temp;
	clientNum = newcl - svs.clients;
	ent = SV_GentityNum( clientNum );
	newcl->gentity = ent;

	// save the challenge
	newcl->challenge = challenge;

	// save the address
	Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);

	// save the userinfo
	Q_strncpyz( newcl->userinfo, userinfo, sizeof(newcl->userinfo) );

	// get the game a chance to reject this connection or modify the userinfo
	denied = (char *)VM_Call( gvm, GAME_CLIENT_CONNECT, clientNum, qtrue, qfalse ); // firstTime = qtrue
	if ( denied ) {
		// we can't just use VM_ArgPtr, because that is only valid inside a VM_Call
		denied = (char *)VM_ExplicitArgPtr( gvm, (int)denied );

		NET_OutOfBandPrint( NS_SERVER, from, "print\n%s\n", denied );
		Com_DPrintf ("Game rejected a connection: %s.\n", denied);
		return;
	}

	SV_UserinfoChanged( newcl );

	// update the advertised session
    //
#ifdef _XBOX
	if (!reconnect)
	    XBL_MM_AddPlayer( usePrivateSlot );
#endif

	// send the connect packet to the client
	NET_OutOfBandPrint( NS_SERVER, from, "connectResponse" );

	Com_DPrintf( "Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name );

	newcl->state = CS_CONNECTED;
	newcl->nextSnapshotTime = svs.time;
	newcl->lastPacketTime = svs.time;
	newcl->lastConnectTime = svs.time;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	newcl->gamestateMessageNum = -1;

	newcl->lastUserInfoChange = 0; //reset the delay
	newcl->lastUserInfoCount = 0; //reset the count

	// if this was the first client on the server, or the last client
	// the server can hold, send a heartbeat to the master.
	count = 0;
	for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			count++;
		}
	}
	if ( count == 1 || count == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	challenge_t	*challenge;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if ( !drop->gentity || !(drop->gentity->r.svFlags & SVF_BOT) ) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
			if ( NET_CompareAdr( drop->netchan.remoteAddress, challenge->adr ) ) {
				challenge->connected = qfalse;
				break;
			}
		}
	}

#ifdef _XBOX
	// Tells all clients to remove the dropped player from their list, if not a bot
	if ( drop->netchan.remoteAddress.type != NA_BOT )
	{
		int index = drop - svs.clients;
		for (int j = 0; j < sv_maxclients->integer ; j++) {
			if ( svs.clients[j].state < CS_PRIMED ) {
				continue;
			}
			if ( svs.clients[j].netchan.remoteAddress.type == NA_BOT ) {
				continue;
			}
			SV_SendClientRemovePeer(&svs.clients[j], index);
		}

		// update the advertised session
		XBL_MM_RemovePlayer( drop->usePrivateSlot );
	}
#endif

	// Kill any download
#ifndef _XBOX	// No downloads on Xbox
	SV_CloseDownload( drop );
#endif

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
	drop->state = CS_ZOMBIE;		// become free in a few seconds

#ifndef _XBOX	// No downloads on Xbox
	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}
#endif

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, va("disconnect \"%s\"", reason ) );

	if ( drop->netchan.remoteAddress.type == NA_BOT ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
#ifndef _XBOX	// No master on Xbox
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}
	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
#endif
}

void SV_WriteRMGAutomapSymbols ( msg_t* msg )
{
	int count = TheRandomMissionManager->GetAutomapSymbolCount ( );
	int i;

	MSG_WriteShort ( msg, count );

	for ( i = 0; i < count; i ++ )
	{
		rmAutomapSymbol_t* symbol = TheRandomMissionManager->GetAutomapSymbol ( i );

		MSG_WriteByte ( msg, symbol->mType );
		MSG_WriteByte ( msg, symbol->mSide );
		MSG_WriteLong ( msg, (long)symbol->mOrigin[0] );
		MSG_WriteLong ( msg, (long)symbol->mOrigin[1] );
	}
}

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	int			start;
	entityState_t	*base, nullstate;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	// MW - my attempt to fix illegible server message errors caused by
	// packet fragmentation of initial snapshot.
	while(client->state&&client->netchan.unsentFragments)
	{
		// send additional message fragments if the last message
		// was too large to send at once

		Com_Printf ("[ISM]SV_SendClientGameState() [2] for %s, writing out old fragments\n", client->name);
		SV_Netchan_TransmitNextFragment(&client->netchan);
	}

	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->pureAuthentic = 0;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteBigString( &msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( &msg, svc_EOF );

	MSG_WriteLong( &msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( &msg, sv.checksumFeed);

	//rwwRMG - send info for the terrain
	if ( TheRandomMissionManager )
	{
		z_stream zdata;

		// Send the height map
		memset(&zdata, 0, sizeof(z_stream));
		deflateInit ( &zdata, Z_MAX_COMPRESSION );

		unsigned char heightmap[15000];
		zdata.next_out = (unsigned char*)heightmap;
		zdata.avail_out = 15000;
		zdata.next_in = TheRandomMissionManager->GetLandScape()->GetHeightMap();
		zdata.avail_in = TheRandomMissionManager->GetLandScape()->GetRealArea();
		deflate(&zdata, Z_SYNC_FLUSH);

		MSG_WriteShort ( &msg, (unsigned short)zdata.total_out );
		MSG_WriteBits ( &msg, 1, 1 );
		MSG_WriteData ( &msg, heightmap, zdata.total_out);

		deflateEnd(&zdata);

		// Send the flatten map
		memset(&zdata, 0, sizeof(z_stream));
		deflateInit ( &zdata, Z_MAX_COMPRESSION );

		zdata.next_out = (unsigned char*)heightmap;
		zdata.avail_out = 15000;
		zdata.next_in = TheRandomMissionManager->GetLandScape()->GetFlattenMap();
		zdata.avail_in = TheRandomMissionManager->GetLandScape()->GetRealArea();
		deflate(&zdata, Z_SYNC_FLUSH);

		MSG_WriteShort ( &msg, (unsigned short)zdata.total_out );
		MSG_WriteBits ( &msg, 1, 1 );
		MSG_WriteData ( &msg, heightmap, zdata.total_out);

		deflateEnd(&zdata);

		// Seed is needed for misc ents and noise
		MSG_WriteLong ( &msg, TheRandomMissionManager->GetLandScape()->get_rand_seed ( ) );

		SV_WriteRMGAutomapSymbols ( &msg );
	}
	else
	{
		MSG_WriteShort ( &msg, 0 );
	}

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}


void SV_SendClientMapChange( client_t *client )
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_mapchange );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

#ifdef _XBOX	// Utilities to notify clients of various XBL state changes
/*
	SV_SendClientNewPeer - tell a client to add a player to his xbOnlineInfo.xbPlayerList
*/
void SV_SendClientNewPeer(client_t *client, XBPlayerInfo* info)
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the command
	MSG_WriteByte( &msg, svc_newpeer );

	// We now write the specific player number as well, so the clients know where
	// to put this info. (That keeps cgs.clientinfo in sync with xbPlayerList)
	MSG_WriteLong( &msg, info - xbOnlineInfo.xbPlayerList );
	MSG_WriteData(&msg, info, sizeof(XBPlayerInfo));

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

/*
	SV_SendClientRemovePeer - tell a client to remove a player from his xbOnlineInfo.xbPlayerList
*/
void SV_SendClientRemovePeer(client_t *client, int index)
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the command
	MSG_WriteByte( &msg, svc_removepeer );

	// All clients have IDENTICAL ordering within xbPlayerList, so just
	// send the index (rather than the XUID, like we did before).
	MSG_WriteLong( &msg, index );

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}

/*
	SV_SendClientXbInfo - Sends the server's xbOnlineInfo to a given client
*/
void SV_SendClientXbInfo(client_t *client)
{
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the command
	MSG_WriteByte( &msg, svc_xbInfo );

	MSG_WriteData(&msg, &(xbOnlineInfo), sizeof(XBOnlineInfo));

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}
#endif	// _XBOX

/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

#ifdef _XBOX
	//update XbOnlineInfo with client
	SV_SendClientXbInfo(client);
#endif

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	client->gentity = ent;

	client->lastUserInfoChange = 0; //reset the delay
	client->lastUserInfoCount = 0; //reset the count

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately
	client->lastUsercmd = *cmd;

	if (client->state < CS_ACTIVE || !cmd || !cmd->serverTime)
	{
		SV_SendServerCommand(client, "print \"^7Welcome to EpicBase - %s!\n", __DATE__);
	}

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
#ifndef _XBOX	// No downloads on Xbox
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
void SV_StopDownload_f( client_t *cl ) {
	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", cl - svs.clients, cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
void SV_DoneDownload_f( client_t *cl ) {
	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", cl - svs.clients, block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", cl - svs.clients, cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
void SV_BeginDownload_f( client_t *cl ) {

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data
==================
*/
void SV_WriteDownloadToClient( client_t *cl , msg_t *msg )
{
	int curindex;
	int rate;
	int blockspersnap;
	int idPack, missionPack;
	char errorMessage[1024];

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if (!cl->download) {
		// We open the file here

		Com_Printf( "clientDownload: %d : begining \"%s\"\n", cl - svs.clients, cl->downloadName );

		missionPack = FS_idPak(cl->downloadName, "missionpack");
		idPack = missionPack || FS_idPak(cl->downloadName, "base");

		if ( !sv_allowDownload->integer || idPack ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) <= 0 ) {
			// cannot auto-download file
			if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", cl - svs.clients, cl->downloadName);
				if (missionPack) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			} else if ( !sv_allowDownload->integer ) {
				Com_Printf("clientDownload: %d : \"%s\" download disabled", cl - svs.clients, cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"Set autodownload to No in your settings and you might be "
										"able to connect if you do have the file.\n", cl->downloadName);
				}
			} else {
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", cl - svs.clients, cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;
			return;
		}

		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = (unsigned char *)Z_Malloc( MAX_DOWNLOAD_BLKSIZE, TAG_DOWNLOAD, qtrue );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );

		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", cl - svs.clients, cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}
#endif	// Xbox	- No downloads on Xbox

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
const char *SV_GetStringEdString(char *refSection, char *refName);
static void SV_Disconnect_f( client_t *cl ) {
//	SV_DropClient( cl, "disconnected" );
	SV_DropClient( cl, SV_GetStringEdString("MP_SVGAME","DISCONNECTED") );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
#ifndef _XBOX
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		//dlls are valid too now -rww
		if (Cvar_VariableValue( "vm_cgame" ))
		{
			bGood = (qboolean)(FS_FileIsInPAK("vm/cgame.qvm", &nChkSum1) == 1);
		}
		else
		{
			bGood = (qboolean)(FS_FileIsInPAK("cgamex86.dll", &nChkSum1) == 1);
		}

		if (bGood)
		{
			if (Cvar_VariableValue( "vm_ui" ))
			{
				bGood = (qboolean)(FS_FileIsInPAK("vm/ui.qvm", &nChkSum2) == 1);
			}
			else
			{
				bGood = (qboolean)(FS_FileIsInPAK("uix86.dll", &nChkSum2) == 1);
			}
		}

		nClientPaks = Cmd_Argc();

		// start at arg 1 ( skip cl_paks )
		nCurArg = 1;

		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		if (bGood) {
			cl->pureAuthentic = 1;
		}
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
#endif
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val;
	int		i;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 ) {
		cl->rate = 99999;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val)) {
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000) {
				cl->rate = 1000;
			} else if (cl->rate > 90000) {
				cl->rate = 90000;
			}
		} else {
			cl->rate = 3000;
		}
	}
	val = Info_ValueForKey (cl->userinfo, "handicap");
	if (strlen(val)) {
		i = atoi(val);
		if (i<=0 || i>100 || strlen(val) > 4) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	// snaps command
	val = Info_ValueForKey (cl->userinfo, "snaps");
	if (strlen(val)) {
		i = atoi(val);
		if ( i < 1 ) {
			i = 1;
		} else if ( i > 30 ) {
			i = 30;
		}
		cl->snapshotMsec = 1000/i;
	} else {
		cl->snapshotMsec = 50;
	}
}

#define INFO_CHANGE_MIN_INTERVAL	6000 //6 seconds is reasonable I suppose
#define INFO_CHANGE_MAX_COUNT		3 //only allow 3 changes within the 6 seconds

/*
==================
SV_UpdateUserinfo_f
==================
*/
static void SV_UpdateUserinfo_f( client_t *cl ) {
	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );

#ifdef FINAL_BUILD
	if (cl->lastUserInfoChange > svs.time)
	{
		cl->lastUserInfoCount++;

		if (cl->lastUserInfoCount >= INFO_CHANGE_MAX_COUNT)
		{
		//	SV_SendServerCommand(cl, "print \"Warning: Too many info changes, last info ignored\n\"\n");
			SV_SendServerCommand(cl, "print \"@@@TOO_MANY_INFO\n\"\n");
			return;
		}
	}
	else
#endif
	{
		cl->lastUserInfoCount = 0;
		cl->lastUserInfoChange = svs.time + INFO_CHANGE_MIN_INTERVAL;
	}

	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );
}

typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
#ifndef _XBOX	// No downloads on Xbox
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},
#endif

	{NULL, NULL}
};



/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {
	ucmd_t	*u;

	Cmd_TokenizeString( s );

	const char *cmd;
	const char *arg1;
	const char *arg2;
	qboolean sayCmd = qfalse;



	cmd = Cmd_Argv(0);
	arg1 = Cmd_Argv(1);
	arg2 = Cmd_Argv(2);

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			break;
		}
	}

	// Fix: buffer overflow
	if (!Q_stricmpn(cmd, "say", 3) || !Q_stricmpn(cmd, "say_team", 8) || !Q_stricmpn(cmd, "tell", 4))
	{
		// 256 because we don't need more, the chat can handle 150 max char
		// and allowing 256 prevent a message to not be sent instead of being truncated
		// if this is a bit more than 150
		if (strlen(Cmd_Args()) > 256)
		{
			clientOK = qfalse;
		}
	}

	// Fix: callvote fraglimit/timelimit with negative value
	if (!Q_stricmpn(cmd, "callvote", 8) && (!Q_stricmpn(arg1, "fraglimit", 9) || !Q_stricmpn(arg1, "timelimit", 9)) && atoi(arg2) < 0)
	{
		clientOK = qfalse;
	}

	if (clientOK) {
		// pass unknown strings to the game
		if (!u->name && sv.state == SS_GAME && (cl->state == CS_ACTIVE || cl->state == CS_PRIMED)) {
			Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, "\n\r", "  " );
			if (!sayCmd) {
					Cmd_Args_Sanitize( MAX_CVAR_VALUE_STRING, ";", " " );
				}
			VM_Call( gvm, GAME_CLIENT_COMMAND, cl - svs.clients );
		}
	}
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name,
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer &&
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer &&
		svs.time < cl->nextReliableTime ) {
		// ignore any other text messages from this client but let them keep playing
		clientOk = qfalse;
		Com_DPrintf( "client text ignored for %s\n", cl->name );
		//return qfalse;	// stop processing
	}

	// don't allow another command for one second
	cl->nextReliableTime = svs.time + 1000;

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );
}

/*
==================
SV_UserMove

The message usually contains all the movement commands
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
int numgbtfls = 0;
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;
	playerState_t* ps;
	animation_t	bgHumanoidAnimations[MAX_TOTALANIMATIONS];
	ps = SV_GameClientNum(cl - svs.clients);
	float currentbtflvelocity = 0;
	float currentvelocity = sqrt(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);


	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= Com_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		oldcmd = cmd;
	}

	// save time for ping calculation
	cl->frames[ cl->messageAcknowledge & PACKET_MASK ].messageAcked = svs.time;

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
	}
	//
#ifndef _XBOX	// No pure on Xbox
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0) {
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}
#endif

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		numgbtfls = 0;
		return;
	}

//NEW STUFF//

//display speed
//SV_SendServerCommand(NULL, "print\"%s ^7has current velocity %f.\n", cl->name, currentvelocity);

//check for butterflies
if (sv_gameplayfixes->integer >= 2 && ps->saberMove == LS_JUMPATTACK_STAFF_RIGHT && ps->fd.saberAnimLevel == SS_STAFF && ps->torsoTimer > 0 && ps->pm_flags & PMF_JUMP_HELD) //&& ps->groundEntityNum != ENTITYNUM_NONE
{
	currentbtflvelocity = sqrt(ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1]);
  int   currentbtflvelocityint = static_cast<int>(currentbtflvelocity);

	if ( ps->velocity[2] < 100 && ps->groundEntityNum != ENTITYNUM_NONE)
	{
		SV_SendServerCommand(NULL, "print\"%s ^1performed a ground-butterfly with a velocity of %i\n", cl->name, currentbtflvelocityint);
		//numgbtfls++;
		//SV_SendServerCommand(NULL, "print\"%s ^7did %i ground-butterflies.\n", cl->name, numgbtfls);
	}
	else
	{
	SV_SendServerCommand(NULL, "print\"%s ^1performed a butterfly with a velocity of %i\n", cl->name, currentbtflvelocityint);
	}
}

//check if someone opens the chat/console/menu while doing a btfl to prevent pmove->cmd.forwardmove going to zero
if (sv_gameplayfixes->integer && cmd->buttons & BUTTON_TALK && ps->saberMove == LS_JUMPATTACK_STAFF_RIGHT)
{
	cmd->buttons &= ~ BUTTON_TALK;
}
//
//END of new stuff

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (cl, &cmds[ i ]);
	}
}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
		//SV_DropClient( cl, "illegible client message" );
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	//
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
#ifdef _XBOX	// No downloads on Xbox
	if ( serverId != sv.serverId ) {
#else
	if ( serverId != sv.serverId && !*cl->downloadName ) {
#endif
		if ( serverId == sv.restartedServerId ) {
			// they just haven't caught the map_restart yet
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->state != CS_ACTIVE && cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );
		if ( c == clc_EOF ) {
			break;
		}
		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", cl - svs.clients );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}
